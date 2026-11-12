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

// Download RETOMAVEL. 'attempt' faz UMA conexao a partir do byte 'pos' (entrega a onData e soma em pos o que entregou).
// Retorna: 1 = terminou | 0 = caiu no meio (retoma de pos) | -1 = recusado | -2 = onData mandou parar.
// Se a conexao cair, espera um pouco (o servidor precisa liberar a conexao antiga) e retoma de onde parou, ate 'maxFails' quedas SEM progresso.
using Attempt = std::function<int(uint64_t &pos, const std::function<bool(const char *, size_t)> &onData, int &status)>;
inline bool resumable(const Attempt &attempt, const Push &push, int &status, const std::function<void(int)> &onRetry = nullptr,
                      int maxFails = 12, int unitMs = 100) {
    uint64_t pos = 0; int fails = 0;
    auto onData = [&](const char *d, size_t n) { if (onRetry) onRetry(0); return push(d, n); };
    for (;;) {
        uint64_t before = pos;
        int r = attempt(pos, onData, status);
        if (r == 1) return true;
        if (r < 0) return false;
        if (pos > before) fails = 0;                              // andou antes de cair: o contador recomeca
        if (++fails > maxFails) return false;
        if (onRetry) onRetry(fails);
        for (int i = 0; i < 10 + 5 * fails && i < 40; i++) {      // ~1-4 s de espera, em fatias (a cada uma, pergunta se foi cancelado)
            if (!push("", 0)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(unitMs));
        }
    }
}

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
            if (n == 0) return !sh->cancel;                  // "ainda estou aqui": so pergunta se o usuario cancelou (usado ao esperar para retomar)
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
