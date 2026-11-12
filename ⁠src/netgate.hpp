#pragma once
// Limita quantas transferencias de capas rodam AO MESMO TEMPO.
// Os pools de rede do PS4 (Net/Ssl/Http) sao pequenos: com o lote de capas (TRIANGULO) rodando e o menu pedindo mais capas,
// chegava-se a 6 conexoes HTTPS juntas e o app fechava. Aqui: no maximo 'total' ao mesmo tempo, e o lote de fundo
// (bg) nunca fica com todas as vagas (sempre sobra uma para as capas que a tela esta pedindo).
// Independente de plataforma (testavel no PC).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

class NetGate {
public:
    explicit NetGate(int total = 3, int bgMax = 2) : total_(total), bgMax_(bgMax) {}
    // true = tem vaga (chame release depois). false = o app esta fechando (quit): nao baixe.
    bool acquire(bool bg, const std::atomic<bool> *quit = nullptr) {
        std::unique_lock<std::mutex> l(m_);
        for (;;) {
            if (quit && quit->load()) return false;
            if (used_ < total_ && (!bg || bgUsed_ < bgMax_)) { used_++; if (bg) bgUsed_++; return true; }
            cv_.wait_for(l, std::chrono::milliseconds(50));
        }
    }
    void release(bool bg) {
        std::lock_guard<std::mutex> l(m_);
        if (used_ > 0) used_--;
        if (bg && bgUsed_ > 0) bgUsed_--;
        cv_.notify_all();
    }
    int inUse() { std::lock_guard<std::mutex> l(m_); return used_; }
    // vaga que se devolve sozinha
    struct Permit {
        NetGate &g; bool bg; bool ok;
        Permit(NetGate &gate, bool isBg, const std::atomic<bool> *quit) : g(gate), bg(isBg), ok(gate.acquire(isBg, quit)) {}
        ~Permit() { if (ok) g.release(bg); }
        Permit(const Permit &) = delete; Permit &operator=(const Permit &) = delete;
    };
private:
    std::mutex m_;
    std::condition_variable cv_;
    int total_, bgMax_, used_ = 0, bgUsed_ = 0;
};
