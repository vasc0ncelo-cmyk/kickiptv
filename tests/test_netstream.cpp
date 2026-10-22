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
    // 6) download retomavel: a conexao cai varias vezes, o resultado sai completo e na ordem
    {
        std::string data(3000000, 'a'); for (size_t i = 0; i < data.size(); i++) data[i] = (char)('a' + (i * 7 + i / 1000) % 26);
        int calls = 0, retries = 0;
        Attempt att = [&](uint64_t &pos, const std::function<bool(const char *, size_t)> &onData, int &st) -> int {
            calls++; st = 206;
            size_t dropAt = calls % 3 == 0 ? pos : std::min<size_t>(data.size(), pos + 400000 + calls * 1000);   // 1 a cada 3 conexoes cai sem entregar nada
            while (pos < dropAt) { size_t n = std::min<size_t>(65536, dropAt - pos); if (!onData(data.data() + pos, n)) return -2; pos += n; }
            return pos >= data.size() ? 1 : 0;
        };
        std::string got; int st = 0;
        bool ok = resumable(att, [&](const char *d, size_t n) { got.append(d, n); return true; }, st, [&](int n) { if (n) retries++; }, 12, 1);
        assert(ok && got == data && retries >= 3);
        // so falha: desiste depois de maxFails sem progresso
        calls = 0; int tries = 0;
        Attempt bad = [&](uint64_t &, const std::function<bool(const char *, size_t)> &, int &s2) -> int { tries++; s2 = 0; return 0; };
        assert(!resumable(bad, [](const char *, size_t) { return true; }, st, nullptr, 3, 1) && tries == 4);
        // recusado (4xx logo de cara): nao insiste
        tries = 0;
        Attempt refused = [&](uint64_t &, const std::function<bool(const char *, size_t)> &, int &s2) -> int { tries++; s2 = 403; return -1; };
        assert(!resumable(refused, [](const char *, size_t) { return true; }, st, nullptr, 3, 1) && tries == 1 && st == 403);
        // cancelar durante a espera para retomar
        tries = 0;
        assert(!resumable(bad, [](const char *, size_t n) { return n != 0; }, st, nullptr, 3, 1) && tries == 1);
    }
    std::cout << "OK\n";
}
