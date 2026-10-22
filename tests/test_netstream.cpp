#include "../src/netstream.hpp"
#include <atomic>
#include <cassert>
#include <iostream>

using namespace netstream;
int main() {
    // 1) download grande, com rede lenta e consumidor lento: chega tudo, em ordem, sem estourar a fila
    {
        const size_t total = 40u * 1024 * 1024; size_t got = 0; unsigned sum = 0, want = 0;
        int status = 0; bool canc = false; int ticks = 0;
        bool ok = run([&](const Push &push, int &st) {
            st = 200; std::string blk(64 * 1024, 'a');
            for (size_t sent = 0; sent < total; sent += blk.size()) { blk[0] = (char)(sent / 65536); want += (unsigned char)blk[0]; if (!push(blk.data(), blk.size())) return false; }
            return true;
        }, [&](const char *d, size_t n) { for (size_t i = 0; i < n; i += 65536) sum += (unsigned char)d[i]; got += n; return true; },
           [&]() { ticks++; return false; }, status, canc, 20, 1u * 1024 * 1024);
        assert(ok && !canc && status == 200 && got == total && sum == want && ticks > 0);
    }
    // 2) servidor demora para responder (nada chega): o tick continua rodando (tela nao congela)
    {
        int ticks = 0, status = 0; bool canc = false; size_t got = 0;
        bool ok = run([&](const Push &push, int &st) {
            std::this_thread::sleep_for(std::chrono::milliseconds(600)); st = 200; push("abc", 3); return true;
        }, [&](const char *, size_t n) { got += n; return true; }, [&]() { ticks++; return false; }, status, canc, 50);
        assert(ok && got == 3 && ticks >= 5);
    }
    // 3) erro no meio: devolve false, mas o que chegou foi entregue
    {
        int status = 0; bool canc = false; size_t got = 0;
        bool ok = run([&](const Push &push, int &st) { st = 200; push("12345", 5); return false; },
                      [&](const char *, size_t n) { got += n; return true; }, nullptr, status, canc);
        assert(!ok && !canc && got == 5);
    }
    // 4) usuario cancela (O) enquanto a rede esta travada: volta na hora, sem esperar a thread de rede
    {
        int status = 0; bool canc = false; std::atomic<bool> finished{false};
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run([&](const Push &, int &) { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); finished = true; return true; },
                      [&](const char *, size_t) { return true; }, []() { return true; }, status, canc, 30);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        assert(!ok && canc && ms < 500 && !finished);
        std::this_thread::sleep_for(std::chrono::milliseconds(1700));   // a thread de rede termina sozinha, sem travar nada
        assert(finished);
    }
    // 5) cancelar com dados fluindo: a thread de rede recebe false no push e para
    {
        int status = 0; bool canc = false; std::atomic<int> pushed{0}; std::atomic<bool> stopped{false};
        bool ok = run([&](const Push &push, int &) { std::string b(1000, 'x'); for (int i = 0; i < 1000000; i++) { if (!push(b.data(), b.size())) { stopped = true; return false; } pushed++; } return true; },
                      [&](const char *, size_t) { return true; }, []() { return true; }, status, canc, 10);
        assert(!ok && canc);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(stopped);
    }
    std::cout << "OK\n";
}
