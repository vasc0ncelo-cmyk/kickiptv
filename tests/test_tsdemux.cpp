// Testes do demuxer MPEG-TS (src/tsdemux.hpp). O fixture (tests/ts_fixture.inc) e um TS real feito com ffmpeg; os tempos esperados vieram do ffprobe.
#include "../src/tsdemux.hpp"
#include "ts_fixture.inc"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

using namespace tsdemux;

struct Out { std::vector<Packet> v, a; };
static Out run(const std::vector<uint8_t> &ts, size_t chunk /*0 = tudo*/, unsigned seed = 0, Demuxer *dm = nullptr) {
    Out o; Demuxer local; Demuxer &d = dm ? *dm : local;
    d.onPacket = [&](Packet &&p) { (p.video ? o.v : o.a).push_back(std::move(p)); };
    if (!chunk && !seed) d.feed(ts.data(), ts.size());
    else {
        srand(seed); size_t i = 0;
        while (i < ts.size()) { size_t k = chunk ? chunk : 1 + (size_t)rand() % 3000; if (i + k > ts.size()) k = ts.size() - i; d.feed(ts.data() + i, k); i += k; }
    }
    d.flush();
    return o;
}
static std::vector<uint8_t> fixture() { return std::vector<uint8_t>(kFixTs, kFixTs + sizeof kFixTs); }
static void checkFixture(const Out &o) {
    assert((int)o.v.size() == kFixVideoN);
    assert((int)o.a.size() == kFixAudioN);
    for (int i = 0; i < kFixVideoN; i++) {
        assert(o.v[i].pts == kFixVideoPts[i] && o.v[i].dts == kFixVideoDts[i]);
        assert((int)o.v[i].key == kFixVideoKey[i]);
        assert(o.v[i].data.size() > 4 && o.v[i].data[0] == 0 && o.v[i].data[1] == 0);          // Annex-B
        assert(!o.v[i].disc);
    }
    for (int i = 0; i < kFixAudioN; i++) {
        long long d = o.a[i].pts - kFixAudioPts[i]; if (d < 0) d = -d;
        assert(d <= 2);                                                                         // o ffprobe arredonda o 1/1024 s de outro jeito
        assert(o.a[i].data.size() > 7 && o.a[i].data[0] == 0xFF && (o.a[i].data[1] & 0xF0) == 0xF0);   // ADTS
    }
}

// ---------- gerador de TS sintetico ----------
struct Mux {
    std::vector<uint8_t> out; int cc[8192] = {0};
    void pkt(int pid, bool pusi, const uint8_t *pl, size_t n, bool disc = false) {
        uint8_t p[188]; memset(p, 0xFF, 188);
        p[0] = 0x47; p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid;
        size_t room = 184, o = 4;
        if (n < room || disc) {                                            // campo de adaptacao para completar com 0xFF
            size_t al = room - n - 1; if (disc && al < 1) al = 1;
            p[3] = (uint8_t)(0x30 | (cc[pid] & 15));
            if (room - n == 1) { p[4] = 0; o = 5; }
            else { p[4] = (uint8_t)(room - n - 1); p[5] = disc ? 0x80 : 0; for (size_t k = 6; k < (size_t)(5 + p[4]); k++) p[k] = 0xFF; o = 5 + p[4]; }
            (void)al;
        } else p[3] = (uint8_t)(0x10 | (cc[pid] & 15));
        memcpy(p + o, pl, n);
        cc[pid]++;
        out.insert(out.end(), p, p + 188);
    }
    void psi(int pid, const std::vector<uint8_t> &sec) {
        std::vector<uint8_t> pl; pl.push_back(0); pl.insert(pl.end(), sec.begin(), sec.end());
        pkt(pid, true, pl.data(), pl.size());
    }
    void pat(int pmt) {
        std::vector<uint8_t> s = {0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, (uint8_t)(0xE0 | (pmt >> 8)), (uint8_t)pmt, 0, 0, 0, 0};
        psi(0, s);
    }
    void pmt(int pid, int vpid, int vt, int apid, int at) {
        std::vector<uint8_t> s = {0x02, 0xB0, 0, 0, 1, 0xC1, 0, 0, (uint8_t)(0xE0 | (vpid >> 8)), (uint8_t)vpid, 0xF0, 0};
        s.insert(s.end(), {(uint8_t)vt, (uint8_t)(0xE0 | (vpid >> 8)), (uint8_t)vpid, 0xF0, 0});
        s.insert(s.end(), {(uint8_t)at, (uint8_t)(0xE0 | (apid >> 8)), (uint8_t)apid, 0xF0, 0});
        s.insert(s.end(), {0, 0, 0, 0});
        s[2] = (uint8_t)(s.size() - 3);
        psi(pid, s);
    }
    static void ts5(std::vector<uint8_t> &h, int marker, int64_t t) {
        h.push_back((uint8_t)((marker << 4) | (((t >> 30) & 7) << 1) | 1)); h.push_back((uint8_t)(t >> 22)); h.push_back((uint8_t)(((t >> 15) << 1) | 1));
        h.push_back((uint8_t)(t >> 7)); h.push_back((uint8_t)((t << 1) | 1));
    }
    void pes(int pid, int sid, int64_t pts, const std::vector<uint8_t> &payload, bool disc = false) {
        std::vector<uint8_t> h = {0, 0, 1, (uint8_t)sid, 0, 0, 0x80, 0x80, 5};
        ts5(h, 2, pts);
        h.insert(h.end(), payload.begin(), payload.end());
        size_t i = 0; bool first = true;
        while (i < h.size()) { size_t k = h.size() - i < 184 ? h.size() - i : 184; pkt(pid, first, h.data() + i, k, first && disc); i += k; first = false; }
    }
};
static std::vector<uint8_t> nal(int type, size_t size, uint8_t fill = 0x55) {
    std::vector<uint8_t> v = {0, 0, 0, 1, (uint8_t)(0x60 | type)}; v.insert(v.end(), size, fill); return v;
}
// SPS real (96x64 do fixture): tira do primeiro quadro-chave
static void grab(const Packet &idr, std::vector<uint8_t> &sps, std::vector<uint8_t> &pps, std::vector<uint8_t> &slice) {
    const auto &b = idr.data; sps.clear(); pps.clear(); slice.clear();
    for (size_t i = 0; i + 4 < b.size(); i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            size_t e = i + 3; while (e + 2 < b.size() && !(b[e] == 0 && b[e + 1] == 0 && b[e + 2] == 1)) e++;
            if (e + 2 >= b.size()) e = b.size();
            int t = b[i + 3] & 0x1F;
            std::vector<uint8_t> n(b.begin() + (long)i + 3, b.begin() + (long)e); while (!n.empty() && n.back() == 0) n.pop_back();
            std::vector<uint8_t> w = {0, 0, 0, 1}; w.insert(w.end(), n.begin(), n.end());
            if (t == 7) sps = w; else if (t == 8) pps = w; else if (t == 5) slice = w;
            i = e - 1;
        }
    }
}

int main() {
    // 1) fixture real: tudo de uma vez, em pedacos fixos e aleatorios -> sempre o mesmo resultado
    {
        auto ts = fixture();
        Demuxer d; Out o = run(ts, 0, 0, &d); checkFixture(o);
        const Info &in = d.info();
        assert(in.vType == kH264 && in.aType == kAacAdts && in.videoSupported() && in.audioSupported());
        assert(in.width == 96 && in.height == 64 && in.sampleRate == 44100 && in.channels == 2 && in.aacObjectType == 2);
        assert(d.unsupportedReason().empty());
        assert(d.stats().ccErrors == 0 && d.stats().syncLost == 0 && d.stats().droppedBeforeKey == 0);
        for (size_t c : {1u, 7u, 187u, 188u, 189u, 377u, 1000u}) checkFixture(run(ts, c));
        for (unsigned s = 1; s <= 25; s++) checkFixture(run(ts, 0, s));
    }
    // 2) comeca no meio de um GOP (alinhamento perdido + sem quadro-chave): so entrega a partir do primeiro quadro-chave
    {
        auto ts = fixture();
        std::vector<uint8_t> cut(ts.begin() + 188 * 3 + 29, ts.end());
        Out o = run(cut, 500);
        assert(o.v.empty() || o.v[0].key);                                   // sem PAT/PMT no inicio: nada ou so apos reencontrar
        // com PAT+PMT mantidos, mas sem o 1o GOP inteiro: cortar depois do primeiro quadro-chave
    }
    // 3) a live reinicia (mesmo trecho de novo: tempos voltam atras): a emenda e marcada e o video recomeca num quadro-chave
    {
        auto ts = fixture(); std::vector<uint8_t> two = ts; two.insert(two.end(), ts.begin(), ts.end());
        Demuxer d3; d3.discontinuitySecs = 0.3;                          // o fixture dura 1 s: o salto para tras e de ~1 s
        Out o = run(two, 777, 0, &d3);
        assert((int)o.v.size() == 2 * kFixVideoN && (int)o.a.size() == 2 * kFixAudioN);
        int dv = 0, da = 0; for (auto &p : o.v) dv += p.disc; for (auto &p : o.a) da += p.disc;
        assert(dv == 1 && da == 1);
        assert(o.v[kFixVideoN].disc && o.v[kFixVideoN].key);
    }
    // 4) lixo e arquivo corrompido nunca derrubam o demuxer
    {
        std::vector<uint8_t> junk(200000); srand(9); for (auto &b : junk) b = (uint8_t)rand();
        Out o = run(junk, 0, 3); assert(o.v.empty() && o.a.empty());
        auto ts = fixture();
        for (unsigned seed = 1; seed <= 40; seed++) {
            srand(seed); auto bad = ts; for (int k = 0; k < 60; k++) bad[(size_t)rand() % bad.size()] = (uint8_t)rand();
            run(bad, 0, seed);
        }
        std::vector<uint8_t> html(5000, 'a'); run(html, 100);
    }
    // 5) codec que o player proprio nao decodifica e relatado (HEVC + AC-3), e o video nao e entregue
    {
        Mux m; m.pat(0x100); m.pmt(0x100, 0x101, 0x24, 0x102, 0x81);
        m.pes(0x101, 0xE0, 90000, nal(19, 100)); m.pes(0x102, 0xBD, 90000, std::vector<uint8_t>(100, 1));
        Demuxer d; Out o = run(m.out, 0, 0, &d);
        assert(d.info().vType == kHevc && d.info().aType == kAc3 && !d.info().videoSupported() && !d.info().audioSupported());
        assert(d.unsupportedReason() == "video H.265/HEVC");
        assert(o.v.empty() && o.a.empty());
        Mux m2; m2.pat(0x100); m2.pmt(0x100, 0x101, 0x1B, 0x102, 0x81);
        Demuxer d2; run(m2.out, 0, 0, &d2); assert(d2.unsupportedReason() == "audio AC-3 (Dolby Digital)");
    }
    // 6) SPS/PPS: quadro-chave sem parametros ganha os do ultimo SPS/PPS; P antes do 1o IDR e descartado; valores de PTS/DTS sao preservados
    {
        auto ts = fixture(); Out ref = run(ts, 0);
        std::vector<uint8_t> sps, pps, slice; grab(ref.v[0], sps, pps, slice);
        assert(!sps.empty() && !pps.empty() && !slice.empty());
        Mux m; m.pat(0x100); m.pmt(0x100, 0x101, 0x1B, 0x102, 0x0F);
        std::vector<uint8_t> pOnly = nal(1, 50);                           // quadro P sozinho, antes de qualquer IDR
        m.pes(0x101, 0xE0, 9000, pOnly);
        std::vector<uint8_t> first = sps; first.insert(first.end(), pps.begin(), pps.end()); first.insert(first.end(), slice.begin(), slice.end());
        m.pes(0x101, 0xE0, 18000, first);
        m.pes(0x101, 0xE0, 27000, pOnly);
        m.pes(0x101, 0xE0, 36000, slice);                                  // IDR sem SPS/PPS
        Demuxer d; Out o = run(m.out, 321, 0, &d);
        assert(o.v.size() == 3 && d.stats().droppedBeforeKey == 1);
        assert(o.v[0].key && o.v[0].pts == 18000 && !o.v[1].key && o.v[2].key && o.v[2].pts == 36000);
        std::vector<uint8_t> want = sps; want.insert(want.end(), pps.begin(), pps.end()); want.insert(want.end(), slice.begin(), slice.end());
        assert(o.v[2].data == want);
        assert(d.info().width == 96 && d.info().height == 64);
    }
    // 7) AAC: quadro ADTS que atravessa dois PES e varios quadros por PES (PTS dos seguintes = PTS + n * 1024 / taxa)
    {
        auto ts = fixture(); Out ref = run(ts, 0);
        std::vector<uint8_t> all; for (int i = 0; i < 6; i++) all.insert(all.end(), ref.a[i].data.begin(), ref.a[i].data.end());
        Mux m; m.pat(0x100); m.pmt(0x100, 0x101, 0x1B, 0x102, 0x0F);
        size_t cut = ref.a[0].data.size() + ref.a[1].data.size() + 10;     // o 3o quadro comeca no PES 1 e termina no PES 2 (depois de cut)
        m.pes(0x102, 0xC0, 90000, std::vector<uint8_t>(all.begin(), all.begin() + (long)cut));
        m.pes(0x102, 0xC0, 90000 + 3 * 2090, std::vector<uint8_t>(all.begin() + (long)cut, all.end()));
        Demuxer d; Out o = run(m.out, 0, 0, &d);
        assert(o.a.size() == 6);
        for (int i = 0; i < 6; i++) assert(o.a[i].data == ref.a[i].data);
        assert(o.a[0].pts == 90000 && o.a[1].pts == 90000 + 2089 && o.a[2].pts == 90000 + 4179 && o.a[3].pts == 90000 + 3 * 2090 && o.a[4].pts == 90000 + 3 * 2090 + 2089);
    }
    // 8) reset(): comeca limpo mas mantem o callback
    {
        auto ts = fixture(); Demuxer d; Out o = run(ts, 0, 0, &d); assert(!o.v.empty());
        d.reset(); assert(d.info().vPid < 0 && d.stats().packets == 0);
        int n = 0; d.onPacket = [&](Packet &&) { n++; };
        d.feed(ts.data(), ts.size()); d.flush(); assert(n == kFixVideoN + kFixAudioN);
    }
    puts("OK");
    return 0;
}
