// Download progressivo de filme (servidor falso): libera o play cedo e grava tudo no lugar certo.
#include "../src/progfile.hpp"
#include <cassert>
#include <iostream>
#include <unistd.h>

static void put32(std::string &s, uint32_t v) { for (int i = 3; i >= 0; i--) s += (char)((v >> (i * 8)) & 255); }
static void put64(std::string &s, uint64_t v) { for (int i = 7; i >= 0; i--) s += (char)((v >> (i * 8)) & 255); }
static std::string box(const char *t, size_t payload, bool big = false) {
    std::string s;
    if (big) { put32(s, 1); s += t; put64(s, 16 + payload); } else { put32(s, (uint32_t)(8 + payload)); s += t; }
    uint32_t x = 12345; for (size_t i = 0; i < payload; i++) { x = x * 1103515245u + 12345u; s += (char)(x >> 16); }
    return s;
}
struct Fake {
    std::string data; std::atomic<int> calls{0}; bool range = true; int failEvery = 0; int delayUs = 0; std::atomic<int> failFrom{-1};
    progfile::Fetch fn() {
        return [this](uint64_t from, uint64_t to, std::string &body) -> int {
            int c = ++calls;
            if (delayUs) usleep(delayUs);
            if (!range) return 200;
            if (failFrom >= 0 && c > failFrom) return 503;
            if (failEvery && c % failEvery == 0) return 0;
            if (from >= data.size()) return 416;
            if (to >= data.size()) to = data.size() - 1;
            body = data.substr(from, to - from + 1);
            return 206;
        };
    }
};
static std::string readAll(const std::string &p) { FILE *f = fopen(p.c_str(), "rb"); std::string s; char b[65536]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static void waitState(progfile::Downloader &d, int want, int ms = 20000) { for (int i = 0; i < ms / 5 && d.state() != want && d.state() != 2; i++) usleep(5000); }

int main() {
    const std::string P = "/tmp/pf_test.bin";
    // 1) MP4 "faststart": ftyp, moov, mdat
    {
        Fake f; f.data = box("ftyp", 16) + box("moov", 100000) + box("mdat", 20u << 20);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3); assert(d.state() == 3);
        d.join();
        assert(readAll(P) == f.data);
        assert(d.frontier() == f.data.size());
    }
    // 2) MP4 com moov NO FINAL, mdat de 64 bits: libera o play so com comeco + indice, e o indice ja esta no lugar
    {
        Fake f; f.delayUs = 20000;                              // rede "lenta": da para ver o estado intermediario
        std::string ftyp = box("ftyp", 16), mdat = box("mdat", 30u << 20, true), moov = box("moov", 300000);
        f.data = ftyp + mdat + moov;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 1);
        assert(d.state() == 1);
        assert(f.calls < 15);                                   // comeco (5 blocos) + 2 sondas + indice: poucas requisicoes
        assert(d.frontier() < f.data.size() / 2);               // ainda baixando: o play foi liberado cedo
        std::string cur = readAll(P);                           // arquivo ja tem o tamanho final
        assert(cur.size() == f.data.size());
        size_t mo = ftyp.size() + mdat.size();
        assert(cur.compare(mo, moov.size(), moov) == 0);        // indice gravado no lugar certo
        assert(cur.compare(0, 5u << 20, f.data, 0, 5u << 20) == 0);
        waitState(d, 3, 30000); assert(d.state() == 3); d.join();
        assert(readAll(P) == f.data);
    }
    // 3) MKV / outros: baixa o final tambem
    {
        Fake f; f.data = std::string("\x1A\x45\xDF\xA3", 4) + box("xxxx", 25u << 20) + box("cues", 1000);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3); assert(d.state() == 3); d.join();
        assert(readAll(P) == f.data);
    }
    // 4) arquivo menor que o comeco
    {
        Fake f; f.data = box("ftyp", 16) + box("moov", 500) + box("mdat", 1000);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3); assert(d.state() == 3); d.join(); assert(readAll(P) == f.data);
    }
    // 5) quedas de rede de vez em quando: recupera
    {
        Fake f; f.failEvery = 3; f.data = box("ftyp", 16) + box("mdat", 12u << 20) + box("moov", 50000);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 60000); assert(d.state() == 3); d.join(); assert(readAll(P) == f.data);
    }
    // 6) servidor sem Range
    {
        Fake f; f.range = false; f.data = box("ftyp", 16) + box("mdat", 1u << 20);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3); assert(d.state() == 2 && d.error().find("Range") != std::string::npos); d.join();
    }
    // 7) servidor some no meio: falha com mensagem (depois de liberar o play)
    {
        Fake f; f.data = box("ftyp", 16) + box("moov", 1000) + box("mdat", 30u << 20); f.failFrom = 12;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        for (int i = 0; i < 20000 && d.state() != 2; i++) usleep(5000);
        assert(d.state() == 2 && !d.error().empty()); d.join();
    }
    // 8) cancelar no meio nao trava
    {
        Fake f; f.delayUs = 30000; f.data = box("ftyp", 16) + box("moov", 1000) + box("mdat", 60u << 20);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        usleep(100000); d.join();                               // join() aborta
        assert(d.state() != 3);
    }
    remove(P.c_str());
    std::cout << "test_progfile OK\n";
}
