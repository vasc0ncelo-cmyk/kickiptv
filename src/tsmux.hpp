#pragma once
// Converte MPEG-TS (H.264 + AAC, como o HLS da Kick/IVS) em um MP4 simples com o "moov" no inicio.
// Motivo: o AvPlayer do PS4 toca MP4 local de forma confiavel, mas recusa o .ts/HLS cru.
// Independente de plataforma (testavel no PC). Sem re-codificar: so reorganiza os bytes (rapido).
#include <stdint.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace tsmux {

struct Info {
    int width = 0, height = 0;
    int vSamples = 0, aSamples = 0;
    int sampleRate = 0, channels = 0;
    double seconds = 0;
};

namespace detail {

// ---------- leitor de bits (SPS do H.264) ----------
struct Bits {
    const uint8_t *p; size_t n; size_t pos = 0;
    Bits(const uint8_t *d, size_t len) : p(d), n(len) {}
    uint32_t u(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) {
            uint32_t b = 0;
            if (pos < n * 8) b = (p[pos >> 3] >> (7 - (pos & 7))) & 1;
            pos++; v = (v << 1) | b;
        }
        return v;
    }
    uint32_t ue() {
        int z = 0;
        while (pos < n * 8 && !u(1) && z < 32) z++;
        return z ? (((1u << z) - 1) + u(z)) : 0;
    }
    int32_t se() { uint32_t k = ue(); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
};

inline bool parseSps(const std::vector<uint8_t> &sps, int &w, int &h) {
    if (sps.size() < 5) return false;
    std::vector<uint8_t> r;                           // remove os bytes de "emulation prevention" (00 00 03)
    for (size_t i = 1; i < sps.size(); i++) {          // pula o byte do cabecalho do NAL
        if (i >= 3 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue;
        r.push_back(sps[i]);
    }
    Bits b(r.data(), r.size());
    int profile = (int)b.u(8); b.u(8); b.u(8);
    b.ue();                                            // seq_parameter_set_id
    int chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        chroma = (int)b.ue();
        if (chroma == 3) b.u(1);
        b.ue(); b.ue(); b.u(1);
        if (b.u(1)) {                                  // matrizes de escala
            int cnt = chroma != 3 ? 8 : 12;
            for (int i = 0; i < cnt; i++) if (b.u(1)) {
                int size = i < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < size; j++) {
                    if (next != 0) { int d = b.se(); next = (last + d + 256) % 256; }
                    last = next == 0 ? last : next;
                }
            }
        }
    }
    b.ue();                                            // log2_max_frame_num_minus4
    uint32_t poc = b.ue();
    if (poc == 0) b.ue();
    else if (poc == 1) { b.u(1); b.se(); b.se(); uint32_t c = b.ue(); for (uint32_t i = 0; i < c && i < 256; i++) b.se(); }
    b.ue(); b.u(1);                                    // max_num_ref_frames, gaps
    int wmb = (int)b.ue() + 1, hmu = (int)b.ue() + 1;
    int fmo = (int)b.u(1);
    if (!fmo) b.u(1);
    b.u(1);                                            // direct_8x8
    int cl = 0, cr = 0, ct = 0, cb = 0;
    if (b.u(1)) { cl = (int)b.ue(); cr = (int)b.ue(); ct = (int)b.ue(); cb = (int)b.ue(); }
    int cux = chroma == 0 || chroma == 3 ? 1 : 2;
    int cuy = (chroma == 1 ? 2 : 1) * (2 - fmo);
    if (chroma == 0) cuy = 2 - fmo;
    w = wmb * 16 - (cl + cr) * cux;
    h = (2 - fmo) * hmu * 16 - (ct + cb) * cuy;
    return w > 0 && h > 0 && w <= 8192 && h <= 8192;
}

// ---------- escrita de caixas MP4 ----------
inline void p8(std::string &s, uint32_t v) { s += (char)(v & 255); }
inline void p16(std::string &s, uint32_t v) { s += (char)((v >> 8) & 255); s += (char)(v & 255); }
inline void p32(std::string &s, uint32_t v) { p16(s, v >> 16); p16(s, v & 0xFFFF); }
inline void pz(std::string &s, size_t n) { s.append(n, '\0'); }
inline std::string box(const char *type, const std::string &payload) {
    std::string s; p32(s, (uint32_t)(8 + payload.size())); s.append(type, 4); s += payload; return s;
}
inline std::string fullbox(const char *type, uint32_t versionFlags, const std::string &payload) {
    std::string s; p32(s, versionFlags); s += payload; return box(type, s);
}

struct VSample { std::vector<uint8_t> data; int64_t dts, pts; bool key; };
struct ASample { std::vector<uint8_t> data; int64_t pts; };
struct Chunk { int track; size_t first, count; double start; uint32_t offset; };

struct Demux {
    // estado
    int pmtPid = -1, vPid = -1, aPid = -1;
    std::vector<uint8_t> sps, pps;
    std::vector<VSample> vs; std::vector<ASample> as;
    bool waitKey = true;
    int aProfile = 2, aFreq = 4, aChan = 2;
    bool aInfo = false;
    std::string err;
    static const int kFreq[13];

    static bool pesHeader(const std::vector<uint8_t> &b, int64_t &pts, int64_t &dts, size_t &off) {
        if (b.size() < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1) return false;
        int flags = b[7]; size_t hl = b[8];
        auto rd = [&](size_t o) -> int64_t {
            return ((int64_t)(b[o] & 0x0E) << 29) | ((int64_t)b[o + 1] << 22) | ((int64_t)(b[o + 2] & 0xFE) << 14) |
                   ((int64_t)b[o + 3] << 7) | ((int64_t)b[o + 4] >> 1);
        };
        pts = dts = -1;
        if ((flags & 0x80) && b.size() >= 14) pts = rd(9);
        dts = ((flags & 0x40) && b.size() >= 19) ? rd(14) : pts;
        off = 9 + hl;
        return off <= b.size();
    }

    void videoAu(const std::vector<uint8_t> &pes) {
        int64_t pts, dts; size_t off;
        if (!pesHeader(pes, pts, dts, off) || pts < 0) return;
        const uint8_t *d = pes.data(); size_t n = pes.size();
        // separa os NALs (Annex B)
        std::vector<std::pair<size_t, size_t>> nals;
        size_t i = off, start = (size_t)-1;
        while (i + 2 < n) {
            if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
                if (start != (size_t)-1) { size_t e = i; while (e > start && d[e - 1] == 0) e--; nals.push_back({start, e - start}); }
                start = i + 3; i += 3;
            } else i++;
        }
        if (start != (size_t)-1 && start < n) { size_t e = n; while (e > start && d[e - 1] == 0) e--; nals.push_back({start, e - start}); }
        VSample s; s.dts = dts; s.pts = pts; s.key = false;
        bool slice = false;
        for (auto &nl : nals) {
            if (nl.second == 0) continue;
            int type = d[nl.first] & 0x1F;
            if (type == 7 && sps.empty()) sps.assign(d + nl.first, d + nl.first + nl.second);
            else if (type == 8 && pps.empty()) pps.assign(d + nl.first, d + nl.first + nl.second);
            else if (type == 1 || type == 5) {
                slice = true; if (type == 5) s.key = true;
                uint32_t L = (uint32_t)nl.second;
                s.data.push_back(L >> 24); s.data.push_back(L >> 16); s.data.push_back(L >> 8); s.data.push_back(L);
                s.data.insert(s.data.end(), d + nl.first, d + nl.first + nl.second);
            }
        }
        if (!slice) return;
        if (waitKey) { if (!s.key || sps.empty() || pps.empty()) return; waitKey = false; }   // comeca no primeiro quadro-chave
        vs.push_back(std::move(s));
    }

    void audioAu(const std::vector<uint8_t> &pes) {
        int64_t pts, dts; size_t off;
        if (!pesHeader(pes, pts, dts, off) || pts < 0) return;
        const uint8_t *d = pes.data(); size_t n = pes.size(), i = off; int k = 0;
        while (i + 7 <= n) {
            if (!(d[i] == 0xFF && (d[i + 1] & 0xF0) == 0xF0)) { i++; continue; }
            int prot = d[i + 1] & 1, prof = (d[i + 2] >> 6) & 3, fr = (d[i + 2] >> 2) & 15;
            int ch = ((d[i + 2] & 1) << 2) | (d[i + 3] >> 6);
            size_t fl = ((size_t)(d[i + 3] & 3) << 11) | ((size_t)d[i + 4] << 3) | (d[i + 5] >> 5);
            size_t hdr = prot ? 7 : 9;
            if (fl < hdr || i + fl > n || fr > 12) break;
            if (!aInfo) { aProfile = prof + 1; aFreq = fr; aChan = ch ? ch : 2; aInfo = true; }
            ASample a; a.pts = pts + (int64_t)((double)k * 1024.0 * 90000.0 / kFreq[aFreq]);
            a.data.assign(d + i + hdr, d + i + fl);
            as.push_back(std::move(a));
            k++; i += fl;
        }
    }

    bool run(const std::string &ts) {
        const uint8_t *t = (const uint8_t *)ts.data(); size_t n = ts.size();
        std::map<int, std::vector<uint8_t>> pes;
        size_t i = 0;
        while (i + 188 <= n) {
            if (t[i] != 0x47) { i++; continue; }
            const uint8_t *p = t + i; i += 188;
            if (p[1] & 0x80) continue;
            bool pusi = p[1] & 0x40; int pid = ((p[1] & 0x1F) << 8) | p[2];
            int afc = (p[3] >> 4) & 3; size_t o = 4;
            if (afc & 2) o += 1 + p[4];
            if (!(afc & 1) || o >= 188) continue;
            const uint8_t *pl = p + o; size_t pn = 188 - o;
            if (pid == 0 && pusi) {                               // PAT
                size_t ptr = pl[0]; if (1 + ptr + 12 > pn) continue;
                const uint8_t *s = pl + 1 + ptr; size_t sl = ((s[1] & 0x0F) << 8) | s[2];
                for (size_t k = 8; k + 4 <= 3 + sl - 4 && 1 + ptr + k + 4 <= pn; k += 4)
                    if (((s[k] << 8) | s[k + 1]) != 0) { pmtPid = ((s[k + 2] & 0x1F) << 8) | s[k + 3]; break; }
            } else if (pid == pmtPid && pusi) {                   // PMT
                size_t ptr = pl[0]; if (1 + ptr + 12 > pn) continue;
                const uint8_t *s = pl + 1 + ptr; size_t sl = ((s[1] & 0x0F) << 8) | s[2];
                size_t pil = ((s[10] & 0x0F) << 8) | s[11], k = 12 + pil, end = 3 + sl - 4;
                while (k + 5 <= end && 1 + ptr + k + 5 <= pn) {
                    int st = s[k], ep = ((s[k + 1] & 0x1F) << 8) | s[k + 2]; size_t il = ((s[k + 3] & 0x0F) << 8) | s[k + 4];
                    if (st == 0x1B && vPid < 0) vPid = ep;
                    else if (st == 0x0F && aPid < 0) aPid = ep;
                    else if ((st == 0x24 || st == 0x10 || st == 0x02) && vPid < 0 && err.empty()) err = "video nao e H.264 (tipo " + std::to_string(st) + ")";
                    k += 5 + il;
                }
            } else if (pid == vPid || pid == aPid) {
                auto &b = pes[pid];
                if (pusi) { if (!b.empty()) { if (pid == vPid) videoAu(b); else audioAu(b); } b.clear(); }
                if (pusi || !b.empty()) b.insert(b.end(), pl, pl + pn);
            }
        }
        for (auto &kv : pes) if (!kv.second.empty()) { if (kv.first == vPid) videoAu(kv.second); else if (kv.first == aPid) audioAu(kv.second); }
        return true;
    }
};
inline const int Demux::kFreq[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

}  // namespace detail

// Converte. Retorna false (com 'err') se nao deu. 'info' e opcional.
inline bool convert(const std::string &ts, std::string &mp4, std::string &err, Info *info = nullptr) {
    using namespace detail;
    Demux dm; dm.run(ts);
    if (dm.vPid < 0) { err = dm.err.empty() ? "nenhum video H.264 encontrado no segmento" : dm.err; return false; }
    if (dm.vs.empty()) { err = "sem quadro-chave (IDR) nos segmentos"; return false; }
    int W = 0, H = 0;
    if (!parseSps(dm.sps, W, H)) { err = "SPS invalido"; return false; }

    const int64_t TS = 90000;
    int64_t dts0 = dm.vs[0].dts, pts0 = dm.vs[0].pts;
    size_t nv = dm.vs.size();
    // audio: descarta quadros anteriores ao primeiro quadro de video exibido
    std::vector<ASample> au;
    for (auto &a : dm.as) if (a.pts >= pts0) au.push_back(a);
    bool hasAudio = dm.aPid >= 0 && dm.aInfo && !au.empty();
    if (dm.aPid >= 0 && dm.aInfo && dm.aProfile > 4) hasAudio = false;      // perfis AAC exoticos (HE etc.) ficam sem audio
    int sr = dm.kFreq[dm.aFreq];
    int64_t aStart = hasAudio ? au[0].pts - pts0 : 0;

    // duracoes do video
    std::vector<uint32_t> vdur(nv), vcts(nv);
    for (size_t i = 0; i < nv; i++) {
        int64_t d = i + 1 < nv ? dm.vs[i + 1].dts - dm.vs[i].dts : (i ? vdur[i - 1] : 3000);
        if (d <= 0) d = i ? vdur[i - 1] : 3000;
        vdur[i] = (uint32_t)d;
        int64_t c = dm.vs[i].pts - dm.vs[i].dts; vcts[i] = c < 0 ? 0 : (uint32_t)c;
    }
    int64_t vTotal = 0; for (auto d : vdur) vTotal += d;
    int64_t vShow = vTotal - (pts0 - dts0);                                  // duracao exibida do video
    int64_t aTotalSamples = hasAudio ? (int64_t)au.size() * 1024 : 0;
    int64_t aShow = hasAudio ? aTotalSamples * TS / sr : 0;                  // em unidades de 90 kHz
    int64_t movieDur = std::max(vShow, aStart + aShow);

    // ---- blocos (chunks) intercalados por tempo ----
    std::vector<Chunk> chunks;
    { size_t i = 0; while (i < nv) { size_t j = i; int64_t acc = 0; while (j < nv && acc < TS / 2) { acc += vdur[j]; j++; }
        chunks.push_back({0, i, j - i, (double)(dm.vs[i].dts - dts0) / TS, 0}); i = j; } }
    if (hasAudio) { size_t i = 0; while (i < au.size()) { size_t c = std::min<size_t>(21, au.size() - i);
        chunks.push_back({1, i, c, (double)(aStart + (int64_t)i * 1024 * TS / sr) / TS, 0}); i += c; } }
    std::stable_sort(chunks.begin(), chunks.end(), [](const Chunk &a, const Chunk &b) { return a.start < b.start; });

    // ---- tabelas ----
    auto stszOf = [&](int track) { std::string s; p32(s, 0);
        size_t n = track == 0 ? nv : au.size(); p32(s, (uint32_t)n);
        for (size_t i = 0; i < n; i++) p32(s, (uint32_t)(track == 0 ? dm.vs[i].data.size() : au[i].data.size()));
        return fullbox("stsz", 0, s); };
    auto stscStcoOf = [&](int track, std::string &stsc, std::string &stco, uint32_t base, bool real) {
        std::string a, c; uint32_t nch = 0, run = 0, lastCount = 0, entries = 0; std::string ents;
        for (auto &ch : chunks) if (ch.track == track) {
            nch++;
            if (ch.count != lastCount) { p32(ents, nch); p32(ents, (uint32_t)ch.count); p32(ents, 1); entries++; lastCount = (uint32_t)ch.count; }
            p32(c, real ? base + ch.offset : 0);
        }
        (void)run; p32(a, entries); a += ents; stsc = fullbox("stsc", 0, a);
        std::string co; p32(co, nch); co += c; stco = fullbox("stco", 0, co);
    };
    auto sttsOf = [&](int track) { std::string s; std::string e; uint32_t cnt = 0, entries = 0, last = 0;
        if (track == 1) { p32(s, 1); p32(s, (uint32_t)au.size()); p32(s, 1024); return fullbox("stts", 0, s); }
        for (size_t i = 0; i < nv; i++) { if (cnt && vdur[i] != last) { p32(e, cnt); p32(e, last); entries++; cnt = 0; } last = vdur[i]; cnt++; }
        if (cnt) { p32(e, cnt); p32(e, last); entries++; }
        p32(s, entries); s += e; return fullbox("stts", 0, s); };
    auto cttsOf = [&]() { std::string s, e; uint32_t cnt = 0, entries = 0, last = 0; bool any = false;
        for (size_t i = 0; i < nv; i++) { if (vcts[i]) any = true; if (cnt && vcts[i] != last) { p32(e, cnt); p32(e, last); entries++; cnt = 0; } last = vcts[i]; cnt++; }
        if (cnt) { p32(e, cnt); p32(e, last); entries++; }
        if (!any) return std::string();
        p32(s, entries); s += e; return fullbox("ctts", 0, s); };
    auto stssOf = [&]() { std::string s, e; uint32_t entries = 0;
        for (size_t i = 0; i < nv; i++) if (dm.vs[i].key) { p32(e, (uint32_t)i + 1); entries++; }
        p32(s, entries); s += e; return fullbox("stss", 0, s); };

    auto dinf = [&]() { std::string u = fullbox("url ", 1, ""); std::string d; p32(d, 1); d += u; return box("dinf", fullbox("dref", 0, d)); };
    auto hdlr = [&](const char *type, const char *name) { std::string s; p32(s, 0); s.append(type, 4); pz(s, 12); s += name; s += '\0'; return fullbox("hdlr", 0, s); };
    auto mdhd = [&](uint32_t ts, uint32_t dur) { std::string s; p32(s, 0); p32(s, 0); p32(s, ts); p32(s, dur); p16(s, 0x55C4); p16(s, 0); return fullbox("mdhd", 0, s); };
    auto tkhd = [&](uint32_t id, uint32_t dur, bool audio) { std::string s; p32(s, 0); p32(s, 0); p32(s, id); p32(s, 0); p32(s, dur); pz(s, 8); p16(s, 0); p16(s, 0); p16(s, audio ? 0x0100 : 0); p16(s, 0);
        uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000}; for (auto v : m) p32(s, v);
        p32(s, audio ? 0 : (uint32_t)W << 16); p32(s, audio ? 0 : (uint32_t)H << 16); return fullbox("tkhd", 3, s); };
    auto elst = [&](const std::vector<std::pair<uint32_t, int32_t>> &e) { std::string s; p32(s, (uint32_t)e.size());
        for (auto &x : e) { p32(s, x.first); p32(s, (uint32_t)x.second); p32(s, 0x00010000); } return box("edts", fullbox("elst", 0, s)); };

    // stsd de video (avc1 + avcC)
    std::string avcC; { std::string s; p8(s, 1); p8(s, dm.sps[1]); p8(s, dm.sps[2]); p8(s, dm.sps[3]); p8(s, 0xFF); p8(s, 0xE1);
        p16(s, (uint32_t)dm.sps.size()); s.append((const char *)dm.sps.data(), dm.sps.size()); p8(s, 1);
        p16(s, (uint32_t)dm.pps.size()); s.append((const char *)dm.pps.data(), dm.pps.size()); avcC = box("avcC", s); }
    std::string stsdV; { std::string s; pz(s, 6); p16(s, 1); p16(s, 0); p16(s, 0); pz(s, 12); p16(s, (uint32_t)W); p16(s, (uint32_t)H);
        p32(s, 0x00480000); p32(s, 0x00480000); p32(s, 0); p16(s, 1); pz(s, 32); p16(s, 0x0018); p16(s, 0xFFFF); s += avcC;
        std::string e; p32(e, 1); e += box("avc1", s); stsdV = fullbox("stsd", 0, e); }
    // stsd de audio (mp4a + esds)
    std::string stsdA; if (hasAudio) {
        uint32_t asc = ((uint32_t)dm.aProfile << 11) | ((uint32_t)dm.aFreq << 7) | ((uint32_t)dm.aChan << 3);
        std::string es; p8(es, 3); p8(es, 0x19); p16(es, 0); p8(es, 0);
        p8(es, 4); p8(es, 0x11); p8(es, 0x40); p8(es, 0x15); pz(es, 3); p32(es, 0); p32(es, 0);
        p8(es, 5); p8(es, 2); p16(es, asc); p8(es, 6); p8(es, 1); p8(es, 2);
        std::string s; pz(s, 6); p16(s, 1); pz(s, 8); p16(s, (uint32_t)dm.aChan); p16(s, 16); p16(s, 0); p16(s, 0); p32(s, (uint32_t)sr << 16);
        s += fullbox("esds", 0, es);
        std::string e; p32(e, 1); e += box("mp4a", s); stsdA = fullbox("stsd", 0, e); }

    // chunks -> posicoes no mdat
    uint32_t pos = 0;
    for (auto &ch : chunks) { ch.offset = pos;
        for (size_t k = 0; k < ch.count; k++) pos += (uint32_t)(ch.track == 0 ? dm.vs[ch.first + k].data.size() : au[ch.first + k].data.size()); }
    uint32_t mdatSize = pos;

    std::string ftyp; { std::string s = "isom"; p32(s, 512); s += "isomiso2avc1mp41"; ftyp = box("ftyp", s); }

    auto buildMoov = [&](uint32_t base, bool real) {
        std::string vStsc, vStco, aStsc, aStco;
        stscStcoOf(0, vStsc, vStco, base, real);
        std::string vstbl = box("stbl", stsdV + sttsOf(0) + cttsOf() + stssOf() + vStsc + stszOf(0) + vStco);
        std::string vmhd = fullbox("vmhd", 1, std::string(8, '\0'));
        std::string vtrak = box("trak", tkhd(1, (uint32_t)vShow, false) +
            elst({{(uint32_t)vShow, (int32_t)(pts0 - dts0)}}) +
            box("mdia", mdhd((uint32_t)TS, (uint32_t)vTotal) + hdlr("vide", "VideoHandler") + box("minf", vmhd + dinf() + vstbl)));
        std::string atrak;
        if (hasAudio) {
            stscStcoOf(1, aStsc, aStco, base, real);
            std::string astbl = box("stbl", stsdA + sttsOf(1) + aStsc + stszOf(1) + aStco);
            std::string smhd = fullbox("smhd", 0, std::string(4, '\0'));
            std::vector<std::pair<uint32_t, int32_t>> e;
            if (aStart > 0) e.push_back({(uint32_t)aStart, -1});
            e.push_back({(uint32_t)aShow, 0});
            atrak = box("trak", tkhd(2, (uint32_t)(aStart + aShow), true) + elst(e) +
                box("mdia", mdhd((uint32_t)sr, (uint32_t)aTotalSamples) + hdlr("soun", "SoundHandler") + box("minf", smhd + dinf() + astbl)));
        }
        std::string mv; p32(mv, 0); p32(mv, 0); p32(mv, (uint32_t)TS); p32(mv, (uint32_t)movieDur); p32(mv, 0x00010000); p16(mv, 0x0100); pz(mv, 10);
        uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000}; for (auto v : m) p32(mv, v);
        pz(mv, 24); p32(mv, 3);
        return box("moov", fullbox("mvhd", 0, mv) + vtrak + atrak);
    };
    std::string moov0 = buildMoov(0, false);
    uint32_t base = (uint32_t)(ftyp.size() + moov0.size() + 8);
    std::string moov = buildMoov(base, true);

    mp4.clear(); mp4.reserve(ftyp.size() + moov.size() + 8 + mdatSize);
    mp4 += ftyp; mp4 += moov; p32(mp4, mdatSize + 8); mp4.append("mdat", 4);
    for (auto &ch : chunks) for (size_t k = 0; k < ch.count; k++) {
        const std::vector<uint8_t> &d = ch.track == 0 ? dm.vs[ch.first + k].data : au[ch.first + k].data;
        mp4.append((const char *)d.data(), d.size());
    }
    if (info) { info->width = W; info->height = H; info->vSamples = (int)nv; info->aSamples = hasAudio ? (int)au.size() : 0;
        info->sampleRate = hasAudio ? sr : 0; info->channels = hasAudio ? dm.aChan : 0; info->seconds = (double)movieDur / TS; }
    return true;
}

}  // namespace tsmux
