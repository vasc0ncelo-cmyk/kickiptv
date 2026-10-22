#pragma once
// Download em segundo plano, entregue em pedacos para a thread principal.
//  - A tela continua desenhando e o botao O consegue cancelar, mesmo quando o servidor demora
//    dezenas de segundos para comecar a responder (paineis IPTV gerando uma lista de 15 mil itens).
//  - Nada e guardado alem do que ainda nao foi processado (limite de ~4 MB na fila).
//  - Independente de plataforma: a rede (sceHttp no PS4) entra pelo parametro 'fetch'.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace netstream {

using Push = std::function<bool(const char *, size_t)>;        // thread de rede -> fila. false = pare.
using Fetch = std::function<bool(const Push &push, int &status)>;   // true = leu tudo com sucesso

struct Shared {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> q;
    size_t queued = 0;
    bool done = false, ok = false, cancel = false;
    int status = 0;
};

// Roda 'fetch' numa thread. 'sink' recebe os pedacos na thread de quem chamou; retornar false cancela.
// 'tick' roda a cada ~tickMs (desenhar progresso / ler o controle); retornar true cancela.
// Retorna true se o download terminou inteiro. 'cancelled' indica cancelamento pelo usuario.
// Se cancelar, NAO espera a thread de rede (ela termina sozinha quando a chamada de rede voltar ou der timeout).
inline bool run(Fetch fetch, const std::function<bool(const char *, size_t)> &sink, const std::function<bool()> &tick,
                int &status, bool &cancelled, int tickMs = 150, size_t maxQueued = 4u * 1024 * 1024) {
    auto sh = std::make_shared<Shared>();
    cancelled = false; status = 0;
    std::thread([sh, fetch, maxQueued]() {
        Push push = [sh, maxQueued](const char *p, size_t n) -> bool {
            std::unique_lock<std::mutex> lk(sh->m);
            sh->cv.wait(lk, [&] { return sh->cancel || sh->queued < maxQueued; });
            if (sh->cancel) return false;
            sh->q.emplace_back(p, n);
            sh->queued += n;
            sh->cv.notify_all();
            return true;
        };
        int st = 0; bool ok = false;
        try { ok = fetch(push, st); } catch (...) { ok = false; }
        std::lock_guard<std::mutex> lk(sh->m);
        sh->ok = ok; sh->status = st; sh->done = true;
        sh->cv.notify_all();
    }).detach();

    using clk = std::chrono::steady_clock;
    auto lastTick = clk::now() - std::chrono::milliseconds(tickMs);
    for (;;) {
        std::deque<std::string> batch;
        bool done = false, ok = false;
        {
            std::unique_lock<std::mutex> lk(sh->m);
            sh->cv.wait_for(lk, std::chrono::milliseconds(tickMs / 3 > 10 ? tickMs / 3 : 10), [&] { return !sh->q.empty() || sh->done; });
            batch.swap(sh->q);
            sh->queued = 0;
            done = sh->done && batch.empty(); ok = sh->ok; status = sh->status;
            sh->cv.notify_all();                         // a fila esvaziou: libera a thread de rede
        }
        for (auto &c : batch) {
            if (!sink(c.data(), c.size())) { cancelled = true; break; }
        }
        if (!cancelled && clk::now() - lastTick >= std::chrono::milliseconds(tickMs)) {
            lastTick = clk::now();
            if (tick && tick()) cancelled = true;
        }
        if (cancelled) {
            std::lock_guard<std::mutex> lk(sh->m);
            sh->cancel = true; sh->cv.notify_all();
            return false;
        }
        if (done) return ok;
    }
}

}  // namespace netstream
