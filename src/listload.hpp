#pragma once
// Carrega a lista M3U numa thread propria, para o app abrir e responder na hora.
// REGRA: essa thread NUNCA desenha na tela, le o controle ou mexe na interface (era isso que fazia o menu piscar e
// perder botoes). Ela so le/baixa e monta a lista; a thread principal pergunta o andamento (items/bytes) e pega o
// resultado com take(), trocando a lista de uma vez so.
// Ordem: copia salva no console -> se nao houver (ou estiver vazia), baixa da rede.
// Independente de plataforma (a leitura e o download entram como funcoes): testavel no PC.
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

#include "m3u.hpp"

class ListLoader {
public:
    using Sink = std::function<bool(const char *, size_t)>;                       // false = pare
    using Source = std::function<bool(const std::string &url, const Sink &sink, const std::atomic<bool> &stop)>;   // true = leu tudo
    enum State { Idle = 0, Running, Done, Partial, Failed, Cancelled };

    ListLoader() = default;
    ListLoader(const ListLoader &) = delete;
    ListLoader &operator=(const ListLoader &) = delete;
    ~ListLoader() { cancel(0.5); }

    void start(const std::string &url, Source cache, Source net) {
        cancel(0.5);
        sh_ = std::make_shared<Shared>();
        taken_ = false;
        auto sh = sh_;
        th_ = std::thread([sh, url, cache, net] { run(sh, url, cache, net); });
    }
    // Pede para parar e espera ate 'waitSec'. true = a thread terminou. Se ficou presa na rede, e solta (termina sozinha).
    bool cancel(double waitSec = 3.0) {
        if (!sh_) return true;
        sh_->stop = true;
        auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(waitSec);
        while (!sh_->finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (th_.joinable()) { if (sh_->finished) th_.join(); else th_.detach(); }
        return sh_->finished.load();
    }
    bool running() const { return sh_ && !sh_->finished && !taken_; }
    bool stillAlive() const { return sh_ && !sh_->finished; }          // true = ainda ha uma thread usando a rede
    size_t items() const { return sh_ ? sh_->items.load() : 0; }
    size_t bytes() const { return sh_ ? sh_->bytes.load() : 0; }
    bool fromNet() const { return sh_ && sh_->fromNet.load(); }
    bool outOfMemory() const { return sh_ && sh_->oom.load(); }
    // Thread principal: devolve true UMA vez, quando terminou. 'out' pode vir vazio (falhou/cancelou).
    bool take(std::shared_ptr<Playlist> &out, State &st) {
        if (!sh_ || taken_ || !sh_->finished) return false;
        taken_ = true;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> l(sh_->m);
        out = sh_->result; st = sh_->state;
        return true;
    }

private:
    struct Shared {
        std::atomic<bool> stop{false}, finished{false}, fromNet{false}, oom{false};
        std::atomic<size_t> bytes{0}, items{0};
        std::mutex m;
        std::shared_ptr<Playlist> result;
        State state = Running;
    };

    static void run(std::shared_ptr<Shared> sh, std::string url, Source cache, Source net) {
        std::shared_ptr<Playlist> res;
        State fin = Failed;
        try {
            auto attempt = [&](const Source &src, bool isNet) -> bool {      // true = ha uma lista aproveitavel (res preenchido)
                if (!src) return false;
                sh->fromNet = isNet; sh->bytes = 0; sh->items = 0;
                PlaylistBuilder b;
                bool ok = false;
                try {
                    ok = src(url, [&](const char *d, size_t n) {
                        if (sh->stop) return false;
                        try { b.feed(d, n); } catch (const std::bad_alloc &) { sh->oom = true; return false; }
                        sh->bytes += n; sh->items = b.count();
                        if (b.full()) return false;                 // orcamento cheio: para parse E download (antes ficava "travado" no mesmo item)
                        return true;
                    }, sh->stop);
                } catch (const std::bad_alloc &) { sh->oom = true; }
                catch (...) {}
                try { b.finish(); } catch (...) {}
                if (sh->stop || b.count() == 0) return false;
                res = std::make_shared<Playlist>(b.take());
                fin = ((ok && !sh->oom) || b.full()) ? Done : Partial;           // Partial: a conexao caiu ou acabou a memoria (b.full = so truncou de proposito)
                return true;
            };
            if (!attempt(cache, false) && !sh->stop) { sh->oom = false; attempt(net, true); }
        } catch (...) {}
        if (sh->stop && !res) fin = Cancelled;
        if (!res && fin != Cancelled) fin = Failed;
        { std::lock_guard<std::mutex> l(sh->m); sh->result = res; sh->state = fin; }
        sh->finished = true;                                                     // sempre por ultimo
    }

    std::shared_ptr<Shared> sh_;
    std::thread th_;
    bool taken_ = false;
};
