// Player nativo: indice MP4 (moov) e Session, com um decoder falso. Cobre o erro "MP4 sem trilha avc1/avcC" em MP4 real.
//   g++ -std=c++17 -o tn tests/test_native.cpp && ./tn [arquivo.mp4 ...]   (arquivos extras = MP4 reais, opcional)
#include "../src/nativeplayer.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>

// ---- decoder falso (no PS4 e o sceVideodec) ----
static std::vector<std::vector<uint8_t>> g_fed;
static uint8_t g_dummyNv12[64];
namespace hwplayer {
bool Player::tryLoadApi() { return true; }
bool Player::hwAvailable() { return true; }
bool Player::open(int, int) { open_ = true; return true; }
bool Player::openFile(const std::string &, int, int) { return false; }
bool Player::feedAu(const uint8_t *d, size_t n, Frame &out) {
    g_fed.emplace_back(d, d + n);
    out.valid = true; out.nv12 = g_dummyNv12; out.width = 320; out.height = 180; out.pitch = 320; out.yRows = 180;
    return true;
}
bool Player::pump(Frame &) { return false; }
void Player::close() { open_ = false; }
bool Player::initDecoder(int, int) { return true; }
bool Player::nextAccessUnit(std::vector<uint8_t> &) { return false; }
size_t Player::findStartCode(const uint8_t *, size_t, size_t, size_t *) { return 0; }
}  // namespace hwplayer

// ---- construtor de MP4 (estrutura igual a de um arquivo real) ----
typedef std::vector<uint8_t> Bytes;
static void u16(Bytes &b, unsigned v) { b.push_back(v >> 8); b.push_back(v & 255); }
static void u32(Bytes &b, uint32_t v) { for (int i = 3; i >= 0; i--) b.push_back((v >> (8 * i)) & 255); }
static Bytes box(const char *type, const Bytes &payload) {
    Bytes b; u32(b, (uint32_t)payload.size() + 8); b.insert(b.end(), type, type + 4); b.insert(b.end(), payload.begin(), payload.end()); return b;
}
static Bytes cat(std::initializer_list<Bytes> l) { Bytes r; for (auto &x : l) r.insert(r.end(), x.begin(), x.end()); return r; }
static Bytes fullbox(const char *type, const Bytes &payload) { Bytes p; u32(p, 0); p.insert(p.end(), payload.begin(), payload.end()); return box(type, p); }
static Bytes mdhd(uint32_t ts) { Bytes p; u32(p, 0); u32(p, 0); u32(p, ts); u32(p, ts * 4); u16(p, 0x55C4); u16(p, 0); return fullbox("mdhd", p); }

static const uint8_t kSps[] = {0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40};
static const uint8_t kPps[] = {0x68, 0xeb, 0xe3, 0xcb};
static Bytes avcC() {
    Bytes p{1, 0x64, 0, 0x1f, 0xFF, 0xE1}; u16(p, sizeof kSps); p.insert(p.end(), kSps, kSps + sizeof kSps);
    p.push_back(1); u16(p, sizeof kPps); p.insert(p.end(), kPps, kPps + sizeof kPps);
    return box("avcC", p);
}
static Bytes visualEntry(const char *fourcc, bool withAvcC) {   // VisualSampleEntry: 78 bytes fixos + caixas filhas (avcC comeca em +86)
    Bytes p(6, 0); u16(p, 1);
    for (int i = 0; i < 16; i++) p.push_back(0);
    u16(p, 320); u16(p, 180); u32(p, 0x00480000); u32(p, 0x00480000); u32(p, 0); u16(p, 1);
    for (int i = 0; i < 32; i++) p.push_back(0);
    u16(p, 0x18); u16(p, 0xFFFF);
    assert(p.size() == 78);
    if (withAvcC) { Bytes a = avcC(); p.insert(p.end(), a.begin(), a.end()); }
    return box(fourcc, p);
}
static const int kN = 7, kPer = 3;                       // 7 samples, 3 por chunk: chunks de 3, 3 e 1
static size_t sampleSize(int i) { return 20 + i; }
static Bytes videoTrak(const char *fourcc, bool avc, const uint64_t off[3]) {
    Bytes stsd; { Bytes p; u32(p, 1); Bytes e = visualEntry(fourcc, avc); p.insert(p.end(), e.begin(), e.end()); stsd = fullbox("stsd", p); }
    Bytes stts; { Bytes p; u32(p, 1); u32(p, kN); u32(p, 40); stts = fullbox("stts", p); }               // timescale 1000: 40 ms por quadro
    Bytes ctts; { Bytes p; u32(p, 2); u32(p, 1); u32(p, 80); u32(p, kN - 1); u32(p, 40); ctts = fullbox("ctts", p); }
    Bytes stss; { Bytes p; u32(p, 2); u32(p, 1); u32(p, 4); stss = fullbox("stss", p); }
    Bytes stsc; { Bytes p; u32(p, 2); u32(p, 1); u32(p, kPer); u32(p, 1); u32(p, 3); u32(p, 1); u32(p, 1); stsc = fullbox("stsc", p); }
    Bytes stsz; { Bytes p; u32(p, 0); u32(p, kN); for (int i = 0; i < kN; i++) u32(p, (uint32_t)sampleSize(i)); stsz = fullbox("stsz", p); }
    Bytes stco; { Bytes p; u32(p, 3); for (int i = 0; i < 3; i++) u32(p, (uint32_t)off[i]); stco = fullbox("stco", p); }
    Bytes stbl = box("stbl", cat({stsd, stts, ctts, stss, stsc, stsz, stco}));
    return box("trak", box("mdia", cat({mdhd(1000), box("minf", stbl)})));
}
static Bytes audioTrak() {                               // trilha de AUDIO ANTES do video, com tabelas de outro tamanho (o parser antigo se perdia aqui)
    Bytes ent; { Bytes p(6, 0); u16(p, 1); for (int i = 0; i < 8; i++) p.push_back(0); u16(p, 2); u16(p, 16); u32(p, 0); u32(p, 44100u << 16); ent = box("mp4a", p); }
    Bytes stsd; { Bytes p; u32(p, 1); p.insert(p.end(), ent.begin(), ent.end()); stsd = fullbox("stsd", p); }
    Bytes stsz; { Bytes p; u32(p, 0); u32(p, 5); for (int i = 0; i < 5; i++) u32(p, 300); stsz = fullbox("stsz", p); }
    Bytes stco; { Bytes p; u32(p, 5); for (int i = 0; i < 5; i++) u32(p, 9999 + i); stco = fullbox("stco", p); }
    Bytes stts; { Bytes p; u32(p, 1); u32(p, 5); u32(p, 1024); stts = fullbox("stts", p); }
    return box("trak", box("mdia", cat({mdhd(44100), box("minf", box("stbl", cat({stsd, stts, stsz, stco})))})));
}
static Bytes mdatPayload() {                             // cada sample: [tamanho 4 bytes][NAL]; o 1o e o 4o sao IDR (0x65)
    Bytes m;
    for (int i = 0; i < kN; i++) {
        u32(m, (uint32_t)sampleSize(i) - 4);
        m.push_back((i == 0 || i == 3) ? 0x65 : 0x41);
        for (size_t k = 1; k < sampleSize(i) - 4; k++) m.push_back((uint8_t)(i * 16 + k));
    }
    return m;
}
struct Built { Bytes file; uint64_t firstSample; };
static Built build(bool moovFirst, const char *fourcc = "avc1", bool avc = true, bool withVideo = true) {
    Bytes ftyp = box("ftyp", Bytes{'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's', 'o', 'm', 'a', 'v', 'c', '1'});
    Bytes md = mdatPayload();
    uint64_t sz[3] = {0, 0, 0}; for (int i = 0; i < kN; i++) sz[i / kPer] += sampleSize(i);
    auto moovFor = [&](uint64_t base) {                  // base = onde comeca o conteudo do mdat
        uint64_t off[3] = {base, base + sz[0], base + sz[0] + sz[1]};
        Bytes mvhd = fullbox("mvhd", Bytes(96, 0));
        return box("moov", withVideo ? cat({mvhd, audioTrak(), videoTrak(fourcc, avc, off)}) : cat({mvhd, audioTrak()}));
    };
    Bytes mdat = box("mdat", md);
    Built b;
    if (moovFirst) { uint64_t base = ftyp.size() + moovFor(0).size() + 8; b.file = cat({ftyp, moovFor(base), mdat}); b.firstSample = base; }
    else { uint64_t base = ftyp.size() + 8; b.file = cat({ftyp, mdat, moovFor(base)}); b.firstSample = base; }
    return b;
}
static mp4s::ReadAt readerOf(const Bytes &f) {
    return [&f](uint8_t *dst, uint64_t pos, uint32_t len, int) -> int {
        if (pos >= f.size()) return 0;
        size_t n = std::min<size_t>(len, f.size() - pos); memcpy(dst, f.data() + pos, n); return (int)n;
    };
}
static nativeplayer::FetchRange fetcherOf(const Bytes &f) {
    return [&f](uint64_t from, uint64_t to, std::string &body) -> bool {
        if (from >= f.size()) return false;
        size_t n = (size_t)std::min<uint64_t>(to - from + 1, f.size() - from);
        body.assign((const char *)f.data() + from, n); return true;
    };
}

int main(int argc, char **argv) {
    for (int moovFirst = 0; moovFirst < 2; moovFirst++) {
        Built b = build(moovFirst);
        mp4s::Demuxer dm; std::string err;
        assert(dm.open(readerOf(b.file), b.file.size(), err));                   // indice no inicio OU no fim
        assert(dm.count() == (size_t)kN && dm.width() == 320 && dm.height() == 180);
        // stsc: chunks de 3, 3 e 1 sample: o 4o sample comeca no 2o chunk, o 7o no 3o
        uint64_t o = b.firstSample;
        for (int i = 0; i < kN; i++) { assert(dm.at(i).off == o && dm.at(i).size == sampleSize(i)); o += sampleSize(i); }
        assert(dm.at(0).sync && !dm.at(1).sync && dm.at(3).sync && !dm.at(6).sync);
        assert(dm.at(1).dtsMs == 40 && dm.at(1).ptsMs == 40 + 40 && dm.at(0).ptsMs == 80);   // ctts: B-frames
        std::vector<uint8_t> au;
        assert(dm.readAnnexB(0, au, 1000) == 1);                                // sync: SPS + PPS + IDR, todos com start code
        const uint8_t want[] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40, 0, 0, 0, 1, 0x68, 0xeb, 0xe3, 0xcb, 0, 0, 0, 1, 0x65};
        assert(au.size() > sizeof want && memcmp(au.data(), want, sizeof want) == 0);
        assert(dm.readAnnexB(1, au, 1000) == 1 && au[3] == 1 && au[4] == 0x41);   // nao sync: so o quadro
        assert(dm.readAnnexB(99, au, 1000) == -1);
    }
    // ---- erros com mensagem clara ----
    { Built b = build(true, "hvc1", false); mp4s::Demuxer dm; std::string err;
      assert(!dm.open(readerOf(b.file), b.file.size(), err) && err.find("HEVC") != std::string::npos); }
    { Built b = build(true, "avc1", true, false); mp4s::Demuxer dm; std::string err;
      assert(!dm.open(readerOf(b.file), b.file.size(), err) && err.find("sem trilha de video") != std::string::npos); }
    { Bytes junk = box("ftyp", Bytes(16, 0)); junk.resize(4096, 0); junk[8 + 4] = 0;
      mp4s::Demuxer dm; std::string err; assert(!dm.open(readerOf(junk), junk.size(), err)); }
    // ---- Session inteira (o que o ps4_platform usa) ----
    for (int moovFirst = 0; moovFirst < 2; moovFirst++) {
        g_fed.clear();
        Built b = build(moovFirst);
        b.file.resize(std::max<size_t>(b.file.size(), 1024));          // o app exige >= 1 KB (a sondagem do servidor)
        nativeplayer::Session s;
        assert(s.openUrl("http://x/a.mp4", b.file.size(), fetcherOf(b.file)));
        assert(s.isOpen() && s.sampleCount() == (size_t)kN && s.width() == 320 && s.height() == 180);
        int frames = 0;
        for (int i = 0; i < 20; i++) { hwplayer::Frame fr; if (s.pump(fr) && fr.valid) frames++; }
        assert(frames == kN && g_fed.size() == (size_t)kN);              // todos os samples chegam ao decoder, uma vez cada
        assert(g_fed[0].size() > 24 && g_fed[0][4] == 0x67);            // o 1o carrega SPS/PPS (o decoder precisa deles antes do IDR)
        assert(g_fed[3][4] == 0x67 && g_fed[1][4] == 0x41);
        s.close(); assert(!s.isOpen());
    }
    { Bytes ts(4096, 0); ts[0] = 0x47; nativeplayer::Session s; assert(!s.openUrl("u", ts.size(), fetcherOf(ts)) && s.lastError().find("MPEG-TS") != std::string::npos); }
    { Bytes mkv(4096, 0); mkv[0] = 0x1A; mkv[1] = 0x45; mkv[2] = 0xDF; mkv[3] = 0xA3; nativeplayer::Session s; assert(!s.openUrl("u", mkv.size(), fetcherOf(mkv)) && s.lastError().find("MKV") != std::string::npos); }
    { Built b = build(true, "hvc1", false); b.file.resize(4096); nativeplayer::Session s; assert(!s.openUrl("u", b.file.size(), fetcherOf(b.file)) && s.lastError().find("HEVC") != std::string::npos); }
    // ---- MP4 reais passados na linha de comando (ex.: gerados com ffmpeg) ----
    for (int a = 1; a < argc; a++) {
        g_fed.clear();
        nativeplayer::Session s;
        bool ok = s.openFile(argv[a]);
        std::cout << argv[a] << ": " << (ok ? "OK" : "ERRO: " + s.lastError());
        if (ok) { int n = 0; for (int i = 0; i < 100000; i++) { hwplayer::Frame fr; if (!s.isOpen() || s.sampleIndex() >= s.sampleCount()) break; if (s.pump(fr) && fr.valid) n++; }
                  std::cout << "  amostras=" << s.sampleCount() << " " << s.width() << "x" << s.height() << " entregues=" << n; }
        std::cout << "\n";
    }
    std::cout << "OK\n";
}
