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


// ---------- MP4 "de verdade" (com stco/co64 e descricao de codec) ----------
static std::string B(const char *t, const std::string &payload) { std::string s; put32(s, (uint32_t)(8 + payload.size())); s += t; s += payload; return s; }
static void put16(std::string &s, int v) { s += (char)(v >> 8); s += (char)v; }
static std::string zeros(size_t n) { return std::string(n, '\0'); }
static std::string hdlr(const char *t) { std::string p; put32(p, 0); put32(p, 0); p += t; p += zeros(12); p += std::string("x\0", 2); return B("hdlr", p); }
static std::string stsdVideo(const char *fcc, int w, int h, int profile, int level) {
    std::string e = zeros(6); put16(e, 1); e += zeros(16); put16(e, w); put16(e, h); e += zeros(50);
    std::string c; c += (char)1; c += (char)profile; c += (char)0; c += (char)level; c += zeros(4);
    e += B("avcC", c);
    std::string p; put32(p, 0); put32(p, 1); p += B(fcc, e); return B("stsd", p);
}
static std::string stsdAudio(const char *fcc, int ch, int rate) {
    std::string e = zeros(6); put16(e, 1); e += zeros(8); put16(e, ch); put16(e, 16); put16(e, 0); put16(e, 0); put32(e, (uint32_t)rate << 16);
    std::string p; put32(p, 0); put32(p, 1); p += B(fcc, e); return B("stsd", p);
}
static std::string stco(const std::vector<uint32_t> &v) { std::string p; put32(p, 0); put32(p, (uint32_t)v.size()); for (auto x : v) put32(p, x); return B("stco", p); }
static std::string co64(const std::vector<uint64_t> &v) { std::string p; put32(p, 0); put32(p, (uint32_t)v.size()); for (auto x : v) put64(p, x); return B("co64", p); }
static std::string trak(const char *handler, const std::string &stsd, const std::string &chunkOff) {
    return B("trak", B("mdia", hdlr(handler) + B("minf", B("stbl", stsd + chunkOff))));
}
static uint32_t rd32(const std::string &s, size_t o) { return ((uint8_t)s[o] << 24) | ((uint8_t)s[o + 1] << 16) | ((uint8_t)s[o + 2] << 8) | (uint8_t)s[o + 3]; }
static uint64_t rd64(const std::string &s, size_t o) { return ((uint64_t)rd32(s, o) << 32) | rd32(s, o + 4); }

static void testRealIndex() {
    const std::string P = "/tmp/pf_test2.bin";
    std::string ftyp = box("ftyp", 16), free_ = box("free", 0);
    std::string mdatPayload; { uint32_t x = 99; for (size_t i = 0; i < (22u << 20); i++) { x = x * 1664525u + 1013904223u; mdatPayload += (char)(x >> 24); } }
    std::string mdat; put32(mdat, (uint32_t)(8 + mdatPayload.size())); mdat += "mdat"; mdat += mdatPayload;
    size_t mdatOff = ftyp.size() + free_.size();
    // amostras em varios pontos do mdat (inclusive depois do 5 MB inicial)
    std::vector<uint32_t> vOff, aOff; std::vector<uint64_t> aOff64;
    for (uint32_t k : {0u, 1000u, 2u << 20, 9u << 20, 21u << 20}) vOff.push_back((uint32_t)(mdatOff + 8 + k));
    for (uint32_t k : {500u, 3u << 20, 15u << 20}) aOff64.push_back(mdatOff + 8 + k);
    auto makeMoov = [&](const char *vfcc, int prof, const char *afcc, std::vector<uint32_t> v) {
        return B("moov", B("mvhd", zeros(100)) + trak("vide", stsdVideo(vfcc, 1280, 720, prof, 41), stco(v)) + trak("soun", stsdAudio(afcc, 2, 48000), co64(aOff64)));
    };
    std::string moov = makeMoov("avc1", 100, "mp4a", vOff);
    std::string orig = ftyp + free_ + mdat + moov;

    // (a) indice no fim: reorganiza e corrige todos os enderecos
    {
        Fake f; f.data = orig;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 30000); assert(d.state() == 3 && d.relocated()); d.join();
        std::string cur = readAll(P);
        assert(cur.size() == orig.size());
        assert(cur.compare(0, ftyp.size(), ftyp) == 0);
        assert(cur.compare(ftyp.size() + 4, 4, "moov") == 0);                      // moov logo depois do ftyp
        size_t M = moov.size();
        assert(cur.compare(ftyp.size() + M, orig.size() - ftyp.size() - M, orig, ftyp.size(), orig.size() - ftyp.size() - M) == 0);
        // acha o stco/co64 no arquivo novo e confere que cada endereco aponta para os MESMOS bytes de antes
        size_t st = cur.find("stco"), c6 = cur.find("co64");
        assert(st != std::string::npos && c6 != std::string::npos);
        for (size_t i = 0; i < vOff.size(); i++) {
            uint32_t nv = rd32(cur, st + 12 + i * 4);
            assert(nv == vOff[i] + M);
            assert(cur.compare(nv, 64, orig, vOff[i], 64) == 0);
        }
        for (size_t i = 0; i < aOff64.size(); i++) {
            uint64_t nv = rd64(cur, c6 + 12 + i * 8);
            assert(nv == aOff64[i] + M);
            assert(cur.compare((size_t)nv, 64, orig, (size_t)aOff64[i], 64) == 0);
        }
        assert(d.info() == "video avc1 High@4.1 1280x720  |  audio mp4a 2ch 48000Hz");
        assert(d.warn().empty() && d.audioWarn().empty());
    }
    // (b) o mesmo arquivo com o indice NA FRENTE: nada muda, byte a byte
    {
        std::string front = ftyp + moov + free_ + mdat;
        // enderecos do indice passam a valer com o moov na frente
        std::vector<uint32_t> v2; for (auto x : vOff) v2.push_back(x + (uint32_t)moov.size());
        std::vector<uint64_t> save = aOff64; for (auto &x : aOff64) x += moov.size();
        std::string moov2 = makeMoov("avc1", 100, "mp4a", v2); aOff64 = save;
        front = ftyp + moov2 + free_ + mdat;
        Fake f; f.data = front;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 30000); assert(d.state() == 3 && !d.relocated()); d.join();
        assert(readAll(P) == front);
        assert(d.info() == "video avc1 High@4.1 1280x720  |  audio mp4a 2ch 48000Hz");
    }
    // (c) HEVC + AC-3: avisa que o PS4 nao toca
    {
        Fake f; f.data = ftyp + free_ + mdat + makeMoov("hvc1", 0, "ac-3", vOff);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 30000); assert(d.state() == 3); d.join();
        assert(d.warn().find("HEVC") != std::string::npos);
        assert(d.audioWarn().find("ac-3") != std::string::npos);
        assert(d.info().find("hvc1") != std::string::npos);
    }
    // (d) H.264 de 10 bits
    {
        Fake f; f.data = ftyp + free_ + mdat + makeMoov("avc1", 110, "mp4a", vOff);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 30000); assert(d.state() == 3); d.join();
        assert(d.warn().find("10 bits") != std::string::npos);
    }
    // (e) endereco perto de 4 GB nao cabe em 32 bits depois de mover o indice: falha com mensagem (em vez de gravar lixo)
    {
        std::vector<uint32_t> big = {0xFFFFFFF0u};
        Fake f; f.data = ftyp + free_ + mdat + makeMoov("avc1", 100, "mp4a", big);
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 30000); assert(d.state() == 2 && d.error().find("grande demais") != std::string::npos); d.join();
    }
    // (f) quedas de rede durante a reorganizacao: recupera e o resultado e o mesmo
    {
        Fake f; f.failEvery = 3; f.data = orig;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 60000); assert(d.state() == 3 && d.relocated()); d.join();
        std::string cur = readAll(P);
        assert(cur.size() == orig.size());
        assert(cur.compare(ftyp.size() + moov.size(), orig.size() - ftyp.size() - moov.size(), orig, ftyp.size(), orig.size() - ftyp.size() - moov.size()) == 0);
    }
    remove(P.c_str());
}

int main() {
    testRealIndex();
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
    // 2) MP4 com moov NO FINAL, mdat de 64 bits: libera o play so com comeco + indice, e o indice vai para o COMECO (faststart)
    {
        Fake f; f.delayUs = 20000;                              // rede "lenta": da para ver o estado intermediario
        std::string ftyp = box("ftyp", 16), mdat = box("mdat", 30u << 20, true), moov = box("moov", 300000);
        f.data = ftyp + mdat + moov;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 1);
        assert(d.state() == 1 && d.relocated());
        assert(f.calls < 15);                                   // comeco (1 bloco) + sondas + indice + resto do comeco: poucas requisicoes
        assert(d.frontier() < f.data.size() / 2);               // ainda baixando: o play foi liberado cedo
        std::string cur = readAll(P);                           // arquivo ja tem o tamanho final
        assert(cur.size() == f.data.size());
        assert(cur.compare(0, ftyp.size(), ftyp) == 0);
        assert(cur.compare(ftyp.size(), moov.size(), moov) == 0);                                // indice logo depois do ftyp
        assert(cur.compare(ftyp.size() + moov.size(), 4u << 20, mdat, 0, 4u << 20) == 0);       // dados deslocados
        waitState(d, 3, 30000); assert(d.state() == 3); d.join();
        assert(readAll(P) == ftyp + moov + mdat);
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
        std::string a = box("ftyp", 16), b = box("mdat", 12u << 20), c = box("moov", 50000);
        Fake f; f.failEvery = 3; f.data = a + b + c;
        progfile::Downloader d(f.fn(), P, f.data.size()); d.start();
        waitState(d, 3, 60000); assert(d.state() == 3); d.join(); assert(readAll(P) == a + c + b);   // indice movido para o comeco
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
