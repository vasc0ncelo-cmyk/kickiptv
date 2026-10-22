// Leitura de arquivo remoto por blocos (HTTP Range) com servidor falso.
#include "../src/httpfile.hpp"
#include <cassert>
#include <iostream>

static uint8_t byteAt(uint64_t p) { return (uint8_t)((p * 31 + 7) >> 3); }

struct Fake {
    uint64_t size; bool range = true; int failEvery = 0; std::atomic<int> calls{0};
    httpfile::Fetch fn() {
        return [this](uint64_t from, uint64_t to, std::string &body) -> int {
            int c = ++calls;
            if (!range) return 200;
            if (failEvery && c % failEvery == 0) return 0;                 // queda de rede de vez em quando
            if (from >= size) return 416;
            if (to >= size) to = size - 1;
            body.resize(to - from + 1);
            for (uint64_t i = from; i <= to; i++) body[i - from] = (char)byteAt(i);
            return 206;
        };
    }
};

int main() {
    // tamanho exato, de varios formatos
    for (uint64_t sz : {1ull, 2ull, 1048575ull, 1048576ull, 1048577ull, 5500000ull, 123456789ull, 3ull << 30}) {
        Fake f; f.size = sz; std::string err;
        uint64_t got = httpfile::probeSize(f.fn(), err);
        assert(got == sz);
        assert(f.calls < 80);
    }
    // sem suporte a Range e erro de rede
    { Fake f; f.size = 100; f.range = false; std::string err; assert(httpfile::probeSize(f.fn(), err) == 0 && err.find("Range") != std::string::npos); }

    // leitor: leituras aleatorias (inclui o fim do arquivo, como o moov de um MP4)
    {
        Fake f; f.size = 7u * 1024 * 1024 + 12345;
        httpfile::Reader r(f.fn(), 6, 3);
        r.start();
        std::vector<uint8_t> buf(300000);
        uint64_t pos[] = {0, 1000, 1048000, 3000000, f.size - 200000, 5, f.size - 100, 2097152 - 10, 4000000, 0};
        for (uint64_t p : pos) {
            int n = r.read(buf.data(), p, (uint32_t)buf.size());
            uint64_t want = std::min<uint64_t>(buf.size(), f.size - p);
            assert(n == (int)want);
            for (int i = 0; i < n; i += 997) assert(buf[i] == byteAt(p + i));
            assert(buf[n - 1] == byteAt(p + n - 1));
        }
        assert(r.size() == f.size && r.read(buf.data(), f.size, 10) == 0);
        assert(r.cachedBlocks() <= 6);
        // leitura sequencial grande atravessando blocos
        std::vector<uint8_t> big(3 * 1024 * 1024 + 17);
        assert(r.read(big.data(), 100, (uint32_t)big.size()) == (int)big.size());
        assert(big[0] == byteAt(100) && big.back() == byteAt(100 + big.size() - 1) && big[1048576] == byteAt(100 + 1048576));
    }
    // quedas de rede: o leitor tenta de novo e entrega tudo certo
    {
        Fake f; f.size = 3u << 20; f.failEvery = 4;
        httpfile::Reader r(f.fn());
        r.start();
        std::vector<uint8_t> buf(1 << 20);
        for (uint64_t p = 0; p + buf.size() <= f.size; p += buf.size()) {
            assert(r.read(buf.data(), p, (uint32_t)buf.size()) == (int)buf.size());
            assert(buf[12345] == byteAt(p + 12345));
        }
    }
    // abort acorda leitura bloqueada (servidor que nunca responde) e a thread termina
    {
        std::atomic<bool> hang{true};
        httpfile::Reader r([&](uint64_t, uint64_t, std::string &) { while (hang) std::this_thread::sleep_for(std::chrono::milliseconds(5)); return 0; });
        r.start();
        std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); r.abort(); hang = false; });
        uint8_t b[10];
        assert(r.read(b, 0, 10, 20000) == 0);
        t.join(); r.join();
    }
    std::cout << "OK\n";
}
