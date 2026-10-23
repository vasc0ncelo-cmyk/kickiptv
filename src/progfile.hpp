#pragma once
// Filme/serie: baixa o arquivo para o disco EM PROGRESSO e libera a reproducao assim que o COMECO e o INDICE do arquivo chegaram.
//  - Antes o app baixava o filme inteiro (centenas de MB ou GB) antes de dar play: a tela ficava so "carregando".
//  - O arquivo e criado ja com o tamanho final (esparso). O AvPlayer abre o caminho local normalmente (unica forma que funciona no PS4)
//    e le o que ja chegou; o resto continua baixando em segundo plano, bem mais rapido do que o filme e consumido.
//  - MP4/MOV com o indice (moov) NO FINAL do arquivo: o indice e baixado primeiro e gravado na posicao certa.
//  - MKV/outros: baixa tambem o final do arquivo (onde ficam o indice/Cues).
// Independente de plataforma: a rede entra pelo parametro 'Fetch' (testavel no PC).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace progfile {

// Baixa os bytes [from, to] (inclusive). Retorna o status HTTP: 206 = ok, 416 = alem do fim, 200 = ignorou o Range, <= 0 = erro de rede.
using Fetch = std::function<int(uint64_t from, uint64_t to, std::string &body)>;

static const uint64_t kBlock = 1u << 20;          // 1 MB por requisicao (a conexao e reaproveitada)
static const uint64_t kHead = 5u << 20;           // o que precisa estar no disco antes de tocar
static const uint64_t kTail = 4u << 20;           // MKV/outros: final do arquivo (indice)
static const uint64_t kMaxIndex = 160u << 20;     // MP4 com "moov" maior que isso: desiste

struct Range { uint64_t a, b; };                  // [a, b)

class Downloader {
public:
    Downloader(Fetch f, std::string path, uint64_t total) : fetch_(std::move(f)), path_(std::move(path)), total_(total) {}
    ~Downloader() { join(); }

    void start() { th_ = std::thread([this] { try { run(); } catch (...) { fail("sem memoria ou erro interno ao baixar"); } }); }
    void abort() { stop_ = true; }
    void join() { abort(); if (th_.joinable()) th_.join(); if (fp_) { fclose(fp_); fp_ = nullptr; } }

    int state() const { return state_.load(); }                  // 0 preparando | 1 pronto (baixando) | 2 falhou | 3 completo
    uint64_t total() const { return total_; }
    uint64_t frontier() { std::lock_guard<std::mutex> l(m_); return frontier_; }   // bytes contiguos desde o inicio
    uint64_t have() { std::lock_guard<std::mutex> l(m_); uint64_t n = 0; for (auto &r : have_) n += r.b - r.a; return n; }
    std::string error() { std::lock_guard<std::mutex> l(m_); return err_; }
    const std::string &path() const { return path_; }

private:
    void fail(const std::string &e) { { std::lock_guard<std::mutex> l(m_); if (state_ != 2) err_ = e; } state_ = 2; }
    bool sleepMs(int ms) { for (int i = 0; i < ms / 20 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return !stop_; }

    // baixa [a, b) e grava no lugar certo. Tenta de novo em erro de rede.
    bool getInto(uint64_t a, uint64_t b, std::string *keep = nullptr) {
        uint64_t off = a;
        while (off < b) {
            if (stop_) return false;
            uint64_t to = std::min<uint64_t>(b, off + kBlock) - 1;
            std::string body; int st = 0;
            for (int attempt = 1;; attempt++) {
                if (stop_) return false;
                body.clear();
                st = fetch_(off, to, body);
                if (st == 206 && body.size() == (size_t)(to - off + 1)) break;
                if (st == 200) { fail("o servidor nao aceita Range (nao da para tocar enquanto baixa)"); return false; }
                if (attempt >= 6) { fail("o servidor parou de enviar o arquivo (HTTP " + std::to_string(st) + ")"); return false; }
                if (!sleepMs(250 * attempt)) return false;
            }
            if (fseeko(fp_, (off_t)off, SEEK_SET) != 0 || fwrite(body.data(), 1, body.size(), fp_) != body.size() || fflush(fp_) != 0) {
                fail("nao consegui gravar no disco (cheio?)"); return false;
            }
            if (keep) keep->append(body);
            addHave(off, off + body.size());
            off += body.size();
        }
        return true;
    }
    void addHave(uint64_t a, uint64_t b) {
        std::lock_guard<std::mutex> l(m_);
        have_.push_back({a, b});
        std::sort(have_.begin(), have_.end(), [](const Range &x, const Range &y) { return x.a < y.a; });
        std::vector<Range> m;
        for (auto &r : have_) { if (!m.empty() && r.a <= m.back().b) m.back().b = std::max(m.back().b, r.b); else m.push_back(r); }
        have_.swap(m);
        frontier_ = (!have_.empty() && have_[0].a == 0) ? have_[0].b : 0;
    }
    bool hasAll(uint64_t a, uint64_t b) {
        std::lock_guard<std::mutex> l(m_);
        for (auto &r : have_) if (r.a <= a && r.b >= b) return true;
        return false;
    }

    static uint64_t be32(const uint8_t *p) { return ((uint64_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
    static uint64_t be64(const uint8_t *p) { return (be32(p) << 32) | be32(p + 4); }

    // MP4/MOV: percorre as caixas do nivel superior ate achar o "moov". Retorna false em falha de rede.
    // moovOff/moovSize ficam em 0 se nao achou.
    bool findMoov(const std::string &head, uint64_t &moovOff, uint64_t &moovSize) {
        moovOff = moovSize = 0;
        uint64_t off = 0;
        for (int guard = 0; guard < 200 && off + 8 <= total_; guard++) {
            std::string hb;
            if (off + 16 <= head.size()) hb = head.substr((size_t)off, 16);
            else {
                std::string b;
                int st = 0;
                for (int a = 1; a <= 4; a++) { b.clear(); st = fetch_(off, std::min<uint64_t>(total_, off + 16) - 1, b); if (st == 206 && b.size() >= 8) break; if (stop_) return false; if (!sleepMs(200 * a)) return false; }
                if (st != 206 || b.size() < 8) { fail("nao consegui ler o indice do filme (HTTP " + std::to_string(st) + ")"); return false; }
                hb = b;
            }
            const uint8_t *p = (const uint8_t *)hb.data();
            uint64_t size = be32(p);
            if (size == 1 && hb.size() >= 16) size = be64(p + 8);
            else if (size == 0) size = total_ - off;
            if (size < 8) return true;                               // lixo: para
            if (memcmp(p + 4, "moov", 4) == 0) { moovOff = off; moovSize = std::min<uint64_t>(size, total_ - off); return true; }
            off += size;
        }
        return true;
    }

    void run() {
        fp_ = fopen(path_.c_str(), "w+b");
        if (!fp_) { fail("nao consegui criar o arquivo em /data"); return; }
        if (total_ == 0) { fail("arquivo vazio"); return; }
        if (fseeko(fp_, (off_t)(total_ - 1), SEEK_SET) != 0 || fputc(0, fp_) == EOF || fflush(fp_) != 0) { fail("sem espaco no disco para o arquivo"); return; }

        // 1) comeco do arquivo
        uint64_t headEnd = std::min<uint64_t>(total_, kHead);
        std::string head;
        if (!getInto(0, headEnd, &head)) return;

        // 2) indice
        bool mp4 = head.size() >= 12 && memcmp(head.data() + 4, "ftyp", 4) == 0;
        if (mp4) {
            uint64_t mo = 0, ms = 0;
            if (!findMoov(head, mo, ms)) return;
            if (ms > kMaxIndex) { fail("indice do filme grande demais"); return; }
            if (ms && !hasAll(mo, mo + ms) && !getInto(mo, mo + ms)) return;
        } else if (total_ > headEnd) {
            uint64_t ta = std::max<uint64_t>(headEnd, total_ > kTail ? total_ - kTail : 0);
            if (ta < total_ && !getInto(ta, total_)) return;
        }
        state_ = 1;                                                  // pode tocar

        // 3) resto do arquivo, em ordem
        for (uint64_t off = headEnd; off < total_ && !stop_;) {
            uint64_t end = std::min<uint64_t>(total_, off + 2 * kBlock);
            if (hasAll(off, end)) { off = end; continue; }
            // pula o que ja existe (indice/final): baixa so o trecho que falta
            uint64_t a = off;
            { std::lock_guard<std::mutex> l(m_); for (auto &r : have_) if (r.a <= a && r.b > a) a = r.b; }
            if (a >= end) { off = a; continue; }
            if (!getInto(a, end)) return;
            off = end;
        }
        if (!stop_) state_ = 3;
    }

    Fetch fetch_;
    std::string path_;
    uint64_t total_;
    FILE *fp_ = nullptr;
    std::thread th_;
    std::mutex m_;
    std::vector<Range> have_;
    uint64_t frontier_ = 0;
    std::string err_;
    std::atomic<int> state_{0};
    std::atomic<bool> stop_{false};
};

}  // namespace progfile
