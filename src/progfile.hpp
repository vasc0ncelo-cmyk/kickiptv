#pragma once
// Filme/serie: baixa o arquivo para o disco EM PROGRESSO e libera a reproducao assim que o COMECO e o INDICE do arquivo chegaram.
//  - Antes o app baixava o filme inteiro (centenas de MB ou GB) antes de dar play: a tela ficava so "carregando".
//  - O arquivo e criado ja com o tamanho final (esparso). O AvPlayer abre o caminho local normalmente (unica forma que funciona no PS4)
//    e le o que ja chegou; o resto continua baixando em segundo plano, bem mais rapido do que o filme e consumido.
//  - MP4/MOV com o indice (moov) NO FINAL do arquivo (comum em filmes de painel IPTV): o indice e baixado primeiro e o arquivo e
//    reorganizado ao gravar ("faststart"): ftyp + moov + dados. O AvPlayer do PS4 nao consegue ler o indice no fim de arquivos
//    grandes (o filme de 3 GB abria o player mas nunca mostrava imagem). Os enderecos (stco/co64) sao corrigidos na gravacao.
//  - MKV/outros: baixa tambem o final do arquivo (onde ficam o indice/Cues).
//  - Le o indice (moov) e descreve o filme (codec, resolucao, audio) para a tela de diagnostico.
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
static const uint64_t kMaxIndex = 48u << 20;      // MP4 com "moov" maior que isso: desiste (precisa caber na RAM)

struct Range { uint64_t a, b; };                  // [a, b)

// ---------------------------------------------------------------------------------------------------------------------
// Leitura/ajuste do indice MP4 (moov)
namespace mp4 {

inline uint32_t rd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint64_t rd64(const uint8_t *p) { return ((uint64_t)rd32(p) << 32) | rd32(p + 4); }
inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
inline void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

struct Child { char type[5]; size_t payload, end; };      // [payload, end) dentro do buffer

// Lista as caixas filhas de [a, b). Para ao encontrar uma caixa invalida.
inline std::vector<Child> children(const std::string &s, size_t a, size_t b) {
    std::vector<Child> v;
    const uint8_t *d = (const uint8_t *)s.data();
    if (b > s.size()) b = s.size();
    while (a + 8 <= b) {
        uint64_t sz = rd32(d + a); size_t hdr = 8;
        if (sz == 1) { if (a + 16 > b) break; sz = rd64(d + a + 8); hdr = 16; }
        else if (sz == 0) sz = b - a;
        if (sz < hdr || a + sz > b) break;
        Child c; memcpy(c.type, d + a + 4, 4); c.type[4] = 0; c.payload = a + hdr; c.end = a + (size_t)sz;
        v.push_back(c);
        a += (size_t)sz;
    }
    return v;
}
inline bool isContainer(const char *t) { return !strcmp(t, "moov") || !strcmp(t, "trak") || !strcmp(t, "mdia") || !strcmp(t, "minf") || !strcmp(t, "stbl"); }

// Soma 'delta' a todos os enderecos de dados (stco/co64) do moov (em 's'). false = nao coube em 32 bits.
inline bool patchOffsets(std::string &s, size_t a, size_t b, uint64_t delta, int depth = 0) {
    if (depth > 8) return true;
    for (auto &c : children(s, a, b)) {
        if (isContainer(c.type)) { if (!patchOffsets(s, c.payload, c.end, delta, depth + 1)) return false; continue; }
        bool is32 = !strcmp(c.type, "stco"), is64 = !strcmp(c.type, "co64");
        if (!is32 && !is64) continue;
        uint8_t *d = (uint8_t *)&s[0];
        if (c.payload + 8 > c.end) continue;
        uint64_t n = rd32(d + c.payload + 4);
        size_t w = is32 ? 4 : 8;
        if (c.payload + 8 + n * w > c.end) n = (c.end - c.payload - 8) / w;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t *p = d + c.payload + 8 + i * w;
            if (is32) { uint64_t v = rd32(p) + delta; if (v > 0xFFFFFFFFull) return false; wr32(p, (uint32_t)v); }
            else wr64(p, rd64(p) + delta);
        }
    }
    return true;
}

struct Info {
    std::string vcodec, acodec;                 // quatro letras: avc1, hvc1, mp4a, ac-3...
    int w = 0, h = 0, profile = 0, level = 0, ach = 0, asr = 0, vtracks = 0, atracks = 0;
};

inline const char *profileName(int p) {
    switch (p) { case 66: return "Baseline"; case 77: return "Main"; case 88: return "Extended"; case 100: return "High"; case 110: return "High10";
                 case 122: return "High422"; case 244: return "High444"; default: return ""; }
}

// Descreve as trilhas (primeira de video e primeira de audio).
inline Info parseInfo(const std::string &moov) {
    Info in;
    const uint8_t *d = (const uint8_t *)moov.data();
    auto top = children(moov, 0, moov.size());
    for (auto &m : top) {
        if (strcmp(m.type, "moov")) continue;
        for (auto &trak : children(moov, m.payload, m.end)) {
            if (strcmp(trak.type, "trak")) continue;
            for (auto &mdia : children(moov, trak.payload, trak.end)) {
                if (strcmp(mdia.type, "mdia")) continue;
                char handler[5] = "";
                std::vector<Child> mc = children(moov, mdia.payload, mdia.end);
                for (auto &x : mc) if (!strcmp(x.type, "hdlr") && x.payload + 12 <= x.end) { memcpy(handler, d + x.payload + 8, 4); handler[4] = 0; }
                for (auto &minf : mc) {
                    if (strcmp(minf.type, "minf")) continue;
                    for (auto &stbl : children(moov, minf.payload, minf.end)) {
                        if (strcmp(stbl.type, "stbl")) continue;
                        for (auto &stsd : children(moov, stbl.payload, stbl.end)) {
                            if (strcmp(stsd.type, "stsd") || stsd.payload + 16 > stsd.end) continue;
                            size_t e = stsd.payload + 8;                       // primeira entrada: size(4) tipo(4) ...
                            uint32_t esz = rd32(d + e);
                            if (esz < 16 || e + esz > stsd.end) continue;
                            char fcc[5]; memcpy(fcc, d + e + 4, 4); fcc[4] = 0;
                            size_t pl = e + 8;                                 // inicio do conteudo da entrada
                            if (!strcmp(handler, "vide")) {
                                in.vtracks++;
                                if (in.vcodec.empty()) {
                                    in.vcodec = fcc;
                                    if (pl + 28 <= e + esz) { in.w = (d[pl + 24] << 8) | d[pl + 25]; in.h = (d[pl + 26] << 8) | d[pl + 27]; }
                                    if (pl + 78 < e + esz)
                                        for (auto &cc : children(moov, pl + 78, e + esz))
                                            if (!strcmp(cc.type, "avcC") && cc.payload + 4 <= cc.end) { in.profile = d[cc.payload + 1]; in.level = d[cc.payload + 3]; }
                                }
                            } else if (!strcmp(handler, "soun")) {
                                in.atracks++;
                                if (in.acodec.empty()) {
                                    in.acodec = fcc;
                                    if (pl + 28 <= e + esz) { in.ach = (d[pl + 16] << 8) | d[pl + 17]; in.asr = (int)(rd32(d + pl + 24) >> 16); }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return in;
}

inline std::string describe(const Info &in) {
    std::string s;
    if (in.vcodec.empty()) s = "sem video";
    else {
        s = "video " + in.vcodec;
        if (in.profile) { s += std::string(" ") + profileName(in.profile); char b[24]; snprintf(b, sizeof b, "@%d.%d", in.level / 10, in.level % 10); s += b; }
        if (in.w) s += " " + std::to_string(in.w) + "x" + std::to_string(in.h);
    }
    s += "  |  ";
    if (in.acodec.empty()) s += "sem audio";
    else {
        s += "audio " + in.acodec;
        if (in.ach) s += " " + std::to_string(in.ach) + "ch";
        if (in.asr) s += " " + std::to_string(in.asr) + "Hz";
    }
    if (in.atracks > 1) s += " (+" + std::to_string(in.atracks - 1) + " faixas)";
    return s;
}

// Algo que o AvPlayer do PS4 nao decodifica. "" = parece compativel.
inline std::string warning(const Info &in) {
    if (in.vcodec.empty()) return "";
    const std::string &v = in.vcodec;
    if (v != "avc1" && v != "avc3" && v != "mp4v") {
        if (v == "hvc1" || v == "hev1") return "o video e HEVC/H.265, que o player do PS4 nao decodifica";
        if (v == "av01") return "o video e AV1, que o player do PS4 nao decodifica";
        if (v == "vp09") return "o video e VP9, que o player do PS4 nao decodifica";
        return "codec de video " + v + " nao suportado pelo player do PS4";
    }
    if (in.profile >= 110) return std::string("o video H.264 e ") + profileName(in.profile) + " (10 bits ou 4:2:2): o player do PS4 so toca 8 bits";
    return "";
}
inline std::string audioNote(const Info &in) {
    const std::string &a = in.acodec;
    if (a.empty() || a == "mp4a" || a == ".mp3") return "";
    return "audio " + a + " pode ficar sem som no PS4";
}

}  // namespace mp4

// ---------------------------------------------------------------------------------------------------------------------
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
    uint64_t have() { std::lock_guard<std::mutex> l(m_); uint64_t n = 0; for (auto &r : have_) n += r.b - r.a; return n + prep_.load(); }
    std::string error() { std::lock_guard<std::mutex> l(m_); return err_; }
    const std::string &path() const { return path_; }
    std::string info() { std::lock_guard<std::mutex> l(m_); return info_; }       // "video avc1 High@4.1 1920x1080  |  audio mp4a 2ch 48000Hz"
    std::string warn() { std::lock_guard<std::mutex> l(m_); return warn_; }       // "" = parece compativel
    std::string audioWarn() { std::lock_guard<std::mutex> l(m_); return audioWarn_; }
    bool relocated() const { return remap_.load(); }                              // o indice estava no fim e foi movido para o comeco

private:
    void fail(const std::string &e) { { std::lock_guard<std::mutex> l(m_); if (state_ != 2) err_ = e; } state_ = 2; }
    bool sleepMs(int ms) { for (int i = 0; i < ms / 20 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20)); return !stop_; }

    // Baixa [from, to] (inclusive, enderecos do ARQUIVO ORIGINAL) com novas tentativas.
    bool fetchRetry(uint64_t from, uint64_t to, std::string &body) {
        int st = 0;
        for (int attempt = 1;; attempt++) {
            if (stop_) return false;
            body.clear();
            st = fetch_(from, to, body);
            if (st == 206 && body.size() == (size_t)(to - from + 1)) return true;
            if (st == 200) { fail("o servidor nao aceita Range (nao da para tocar enquanto baixa)"); return false; }
            if (attempt >= 6) { fail("o servidor parou de enviar o arquivo (HTTP " + std::to_string(st) + ")"); return false; }
            if (!sleepMs(250 * attempt)) return false;
        }
    }
    // Grava no arquivo local (enderecos NOVOS) e registra como disponivel.
    bool putAt(uint64_t off, const char *d, size_t n) {
        if (!n) return true;
        if (fseeko(fp_, (off_t)off, SEEK_SET) != 0 || fwrite(d, 1, n, fp_) != n || fflush(fp_) != 0) { fail("nao consegui gravar no disco (cheio?)"); return false; }
        addHave(off, off + n);
        return true;
    }
    // Regiao do arquivo novo que contem 'off': 'delta' = (endereco novo - endereco original); 'end' = fim da regiao; skip = area do indice (ja gravada)
    void region(uint64_t off, uint64_t &delta, uint64_t &end, bool &skip) const {
        skip = false; delta = 0; end = total_;
        if (!remap_.load()) return;
        if (off < ftypEnd_) { end = ftypEnd_; return; }
        if (off < ftypEnd_ + moovSize_) { skip = true; end = ftypEnd_ + moovSize_; return; }
        if (off < moovOff_ + moovSize_) { delta = moovSize_; end = moovOff_ + moovSize_; return; }
    }
    // baixa [a, b) (enderecos NOVOS) e grava no lugar certo.
    bool getInto(uint64_t a, uint64_t b) {
        uint64_t off = a;
        while (off < b) {
            if (stop_) return false;
            uint64_t delta, rend; bool skip;
            region(off, delta, rend, skip);
            if (skip) { off = std::min(b, rend); continue; }
            uint64_t lim = std::min(b, rend);
            uint64_t to = std::min<uint64_t>(lim, off + kBlock) - 1;
            std::string body;
            if (!fetchRetry(off - delta, to - delta, body)) return false;
            if (!putAt(off, body.data(), body.size())) return false;
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

    // MP4/MOV: percorre as caixas do nivel superior (do arquivo ORIGINAL) ate achar o "moov". false = falha de rede.
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
            uint64_t size = mp4::rd32(p);
            if (size == 1 && hb.size() >= 16) size = mp4::rd64(p + 8);
            else if (size == 0) size = total_ - off;
            if (size < 8) return true;                               // lixo: para
            if (memcmp(p + 4, "moov", 4) == 0) { moovOff = off; moovSize = std::min<uint64_t>(size, total_ - off); return true; }
            off += size;
        }
        return true;
    }

    void setInfo(const std::string &moov) {
        mp4::Info in = mp4::parseInfo(moov);
        std::lock_guard<std::mutex> l(m_);
        info_ = mp4::describe(in); warn_ = mp4::warning(in); audioWarn_ = mp4::audioNote(in);
    }

    void run() {
        fp_ = fopen(path_.c_str(), "w+b");
        if (!fp_) { fail("nao consegui criar o arquivo em /data"); return; }
        if (total_ == 0) { fail("arquivo vazio"); return; }
        if (fseeko(fp_, (off_t)(total_ - 1), SEEK_SET) != 0 || fputc(0, fp_) == EOF || fflush(fp_) != 0) { fail("sem espaco no disco para o arquivo"); return; }

        // 1) primeiro bloco (fica na memoria ate decidirmos como gravar)
        const uint64_t headEnd = std::min<uint64_t>(total_, kHead);
        std::string head0;
        if (!fetchRetry(0, std::min<uint64_t>(total_, kBlock) - 1, head0)) return;
        bool mp4f = head0.size() >= 12 && memcmp(head0.data() + 4, "ftyp", 4) == 0;
        uint64_t mo = 0, ms = 0;
        if (mp4f && !findMoov(head0, mo, ms)) return;
        if (ms > kMaxIndex) { fail("indice do filme grande demais"); return; }

        uint64_t ftypEnd = mp4f ? mp4::rd32((const uint8_t *)head0.data()) : 0;
        bool relocate = mp4f && ms && mo >= headEnd && ftypEnd >= 16 && ftypEnd < head0.size() && ftypEnd < mo;
        uint64_t nextNew;                                            // proximo endereco NOVO a baixar depois do bloco inicial
        if (relocate) {
            // 2a) indice no FIM: baixa o moov, corrige os enderecos e grava logo depois do ftyp (o resto do arquivo desce `ms` bytes)
            std::string moov;
            for (uint64_t o = mo; o < mo + ms;) {
                std::string b;
                uint64_t to = std::min<uint64_t>(mo + ms, o + kBlock) - 1;
                if (!fetchRetry(o, to, b)) return;
                moov += b; o += b.size(); prep_ = moov.size();
            }
            if (!mp4::patchOffsets(moov, 0, moov.size(), ms)) { fail("filme grande demais para reorganizar o indice"); return; }
            setInfo(moov);
            ftypEnd_ = ftypEnd; moovOff_ = mo; moovSize_ = ms; remap_ = true;
            if (!putAt(0, head0.data(), (size_t)ftypEnd) || !putAt(ftypEnd, moov.data(), moov.size())) return;
            prep_ = 0;
            if (!putAt(ftypEnd + ms, head0.data() + ftypEnd, head0.size() - (size_t)ftypEnd)) return;
            nextNew = ms + head0.size();
        } else {
            if (!putAt(0, head0.data(), head0.size())) return;
            nextNew = head0.size();
        }
        const uint64_t headEndNew = std::min<uint64_t>(total_, headEnd + (relocate ? ms : 0));
        if (nextNew < headEndNew && !getInto(nextNew, headEndNew)) return;

        // 2b) indice (sem mover)
        if (!relocate) {
            if (mp4f && ms) {
                if (!hasAll(mo, mo + ms) && !getInto(mo, mo + ms)) return;
                std::string moov((size_t)ms, '\0');                  // le de volta do disco so para descrever o filme
                if (fseeko(fp_, (off_t)mo, SEEK_SET) == 0 && fread(&moov[0], 1, (size_t)ms, fp_) == (size_t)ms) setInfo(moov);
            } else if (!mp4f && total_ > headEnd) {
                uint64_t ta = std::max<uint64_t>(headEnd, total_ > kTail ? total_ - kTail : 0);
                if (ta < total_ && !getInto(ta, total_)) return;
            }
        }
        state_ = 1;                                                  // pode tocar

        // 3) resto do arquivo, em ordem
        for (uint64_t off = headEndNew; off < total_ && !stop_;) {
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
    std::string err_, info_, warn_, audioWarn_;
    std::atomic<int> state_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> remap_{false};
    std::atomic<uint64_t> prep_{0};
    uint64_t ftypEnd_ = 0, moovOff_ = 0, moovSize_ = 0;      // so valem com remap_ (definidos antes de remap_ ficar true)
};

}  // namespace progfile
