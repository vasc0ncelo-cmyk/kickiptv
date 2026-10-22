#pragma once
// Arquivo remoto (filme/serie .mp4 .mkv ...) lido por BLOCOS com HTTP Range, para entregar ao AvPlayer pelos callbacks de arquivo.
//  - O AvPlayer do PS4 abre "http://..." sozinho com a rede DELE (User-Agent proprio, sem redirecionamento, sem keep-alive):
//    os paineis IPTV recusam e o resultado e o erro "AddSource = 806A0002 / Open file failed". Aqui quem baixa e o cliente HTTP do app.
//  - Uma thread descobre o tamanho do arquivo (sondando com Range) e depois baixa blocos de 1 MB a frente de quem esta lendo.
//  - Independente de plataforma: a rede entra pelo parametro 'Fetch' (testavel no PC).
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httpfile {

// Baixa os bytes [from, to] (inclusive). Retorna o status HTTP: 206 = ok, 416 = alem do fim do arquivo,
// 200 = o servidor ignorou o Range (nao da para usar), <= 0 = erro de rede.
// Em 200/416 o corpo nao precisa ser lido ate o fim.
using Fetch = std::function<int(uint64_t from, uint64_t to, std::string &body)>;

// Descobre o tamanho do arquivo so com requisicoes Range de 1 byte (nao precisa ler cabecalhos).
// Retorna o tamanho, ou 0 com 'err' preenchido.
inline uint64_t probeSize(const Fetch &f, std::string &err, const std::function<bool()> &stop = nullptr) {
    auto exists = [&](uint64_t n) -> int {     // 1 = existe | 0 = alem do fim | -1 = erro | -2 = sem suporte a Range
        for (int a = 0; a < 3; a++) {
            std::string b;
            int st = f(n, n, b);
            if (st == 206 && b.size() >= 1) return 1;
            if (st == 416) return 0;
            if (st == 200) return -2;
            if (st >= 400 && st < 500 && st != 408 && st != 429) return -1;
            if (stop && stop()) return -1;
        }
        return -1;
    };
    int e0 = exists(0);
    if (e0 == -2) { err = "o servidor nao aceita Range (nao da para tocar este arquivo)"; return 0; }
    if (e0 < 0) { err = "o servidor nao respondeu"; return 0; }
    if (e0 == 0) { err = "arquivo vazio"; return 0; }
    uint64_t lo = 0, hi = 1u << 20;                   // lo: existe; hi: candidato a "nao existe"
    for (;;) {
        if (stop && stop()) { err = "cancelado"; return 0; }
        int e = exists(hi);
        if (e < 0) { err = "o servidor parou de responder"; return 0; }
        if (e == 0) break;
        lo = hi;
        if (hi > (1ULL << 42)) { err = "tamanho absurdo"; return 0; }
        hi *= 4;
    }
    while (hi - lo > 1) {
        if (stop && stop()) { err = "cancelado"; return 0; }
        uint64_t mid = lo + (hi - lo) / 2;
        int e = exists(mid);
        if (e < 0) { err = "o servidor parou de responder"; return 0; }
        if (e == 1) lo = mid; else hi = mid;
    }
    return lo + 1;
}

class Reader {
public:
    static const uint64_t kBlock = 1u << 20;

    Reader(Fetch f, size_t maxBlocks = 40, int ahead = 12) : fetch_(std::move(f)), maxBlocks_(maxBlocks), ahead_(ahead) {}
    ~Reader() { join(); }

    void setOnExit(std::function<void()> cb) { onExit_ = std::move(cb); }

    void start() {
        join();
        stop_ = false; state_ = 0;
        th_ = std::thread([this] { run(); if (onExit_) onExit_(); });
    }
    void abort() { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); }   // acorda quem esta lendo
    void join() { abort(); if (th_.joinable()) th_.join(); }

    int state() { std::lock_guard<std::mutex> l(m_); return state_; }      // 0 conectando | 1 pronto | 2 falhou
    uint64_t size() { std::lock_guard<std::mutex> l(m_); return size_; }
    std::string error() { std::lock_guard<std::mutex> l(m_); return err_; }
    size_t cachedBlocks() { std::lock_guard<std::mutex> l(m_); return blocks_.size(); }

    // Leitura por posicao absoluta (callback do AvPlayer). Bloqueia ate os bytes chegarem. Retorna o que leu (so menos que 'len' no fim do arquivo) ou 0 em erro/abort.
    int read(uint8_t *dst, uint64_t pos, uint32_t len, int timeoutMs = 40000) {
        std::unique_lock<std::mutex> l(m_);
        auto t0 = std::chrono::steady_clock::now();
        auto timedOut = [&] { return std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs); };
        while (state_ == 0 && !stop_) { cv_.wait_for(l, std::chrono::milliseconds(100)); if (timedOut()) return 0; }
        if (state_ != 1 || stop_) return 0;
        if (pos >= size_) return 0;
        if ((uint64_t)len > size_ - pos) len = (uint32_t)(size_ - pos);
        uint32_t done = 0;
        while (done < len) {
            uint64_t p = pos + done, idx = p / kBlock;
            if (done == 0) lastIdx_ = idx;
            auto it = blocks_.find(idx);
            if (it == blocks_.end()) {
                want_ = idx; wantSet_ = true;
                cv_.notify_all();
                cv_.wait_for(l, std::chrono::milliseconds(100));
                if (stop_ || state_ != 1 || timedOut()) return 0;
                continue;
            }
            const std::string &b = *it->second;
            size_t off = (size_t)(p - idx * kBlock);
            if (off >= b.size()) return (int)done;
            size_t n = std::min<size_t>(len - done, b.size() - off);
            memcpy(dst + done, b.data() + off, n);
            done += (uint32_t)n;
        }
        cv_.notify_all();                       // a posicao mudou: o worker pode adiantar mais blocos
        return (int)done;
    }

private:
    void setFail(const std::string &e) { { std::lock_guard<std::mutex> l(m_); state_ = 2; err_ = e; } cv_.notify_all(); }

    void run() {
        std::string err;
        uint64_t sz = probeSize(fetch_, err, [this] { std::lock_guard<std::mutex> l(m_); return stop_; });
        if (sz == 0) { setFail(err.empty() ? "falha ao medir o arquivo" : err); return; }
        { std::lock_guard<std::mutex> l(m_); size_ = sz; state_ = 1; }
        cv_.notify_all();
        uint64_t nBlocks = (sz + kBlock - 1) / kBlock;
        int fails = 0;
        for (;;) {
            uint64_t idx = 0; bool have = false;
            {
                std::unique_lock<std::mutex> l(m_);
                for (;;) {
                    if (stop_) return;
                    if (wantSet_ && blocks_.count(want_)) wantSet_ = false;
                    if (wantSet_ && want_ < nBlocks) { idx = want_; have = true; break; }
                    for (int k = 0; k <= ahead_; k++) {              // adianta os blocos seguintes ao que o player esta lendo
                        uint64_t c = lastIdx_ + k;
                        if (c < nBlocks && !blocks_.count(c)) { idx = c; have = true; break; }
                    }
                    if (have) break;
                    cv_.wait_for(l, std::chrono::milliseconds(200));
                }
            }
            uint64_t from = idx * kBlock, to = std::min<uint64_t>(sz, from + kBlock) - 1;
            std::string body;
            int st = fetch_(from, to, body);
            bool ok = st == 206 && body.size() == (size_t)(to - from + 1);
            if (!ok) {
                if (++fails > 6) { setFail("o servidor parou de enviar o arquivo (HTTP " + std::to_string(st) + ")"); return; }
                for (int i = 0; i < 5 * fails; i++) { { std::lock_guard<std::mutex> l(m_); if (stop_) return; } std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
                continue;
            }
            fails = 0;
            {
                std::lock_guard<std::mutex> l(m_);
                blocks_[idx] = std::make_shared<std::string>(std::move(body));
                while (blocks_.size() > maxBlocks_) {                // tira o bloco mais distante de quem esta lendo
                    uint64_t far = blocks_.begin()->first, fd = 0;
                    for (auto &kv : blocks_) {
                        uint64_t d = kv.first > lastIdx_ ? kv.first - lastIdx_ : (lastIdx_ - kv.first) * 2;   // atras do leitor vale menos
                        if (d >= fd) { fd = d; far = kv.first; }
                    }
                    if (far == idx && blocks_.size() > 1) break;
                    blocks_.erase(far);
                }
            }
            cv_.notify_all();
        }
    }

    Fetch fetch_;
    std::function<void()> onExit_;
    size_t maxBlocks_; int ahead_;
    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    std::map<uint64_t, std::shared_ptr<std::string>> blocks_;
    uint64_t size_ = 0, lastIdx_ = 0, want_ = 0;
    bool wantSet_ = false, stop_ = true;
    int state_ = 0;
    std::string err_;
};

}  // namespace httpfile
