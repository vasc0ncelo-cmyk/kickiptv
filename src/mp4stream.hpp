#pragma once
// =============================================================================
// Demux MP4 em STREAMING (acesso aleatorio) — nao carrega o arquivo na RAM.
// =============================================================================
// Diferenca para mp4annexb.hpp::parse (que continua la, mas nao serve para filme real):
//   - le so o 'moov' (poucos MB) via ReadAt: funciona com moov no comeco OU no fim do arquivo
//   - escolhe a trilha avc1/avc3 (o parse antigo misturava stsz/stco da trilha de AUDIO)
//   - entende stsc (varios samples por chunk) — o parse antigo falhava com "MP4 complexo"
//   - ctts (B-frames) -> pts correto; dts vem do stts
//   - le UM sample por vez (readAnnexB) e ja entrega em Annex-B para o decoder
// Independente de plataforma: a leitura entra por ReadAt (httpfile::Reader::read no PS4, fread no PC).
// =============================================================================
#include "mp4annexb.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp4s {

// Gancho opcional de log (o app aponta para o trace()). nullptr = sem log.
inline void (*g_trace)(const char *) = nullptr;
inline void T(const char *s) { if (g_trace) g_trace(s); }

// Le 'len' bytes em 'pos'. Retorna quantos leu. Menos que 'len' (ou 0) = nao chegou a tempo / erro.
using ReadAt = std::function<int(uint8_t *dst, uint64_t pos, uint32_t len, int timeoutMs)>;

struct Sample {
    uint64_t off = 0;
    uint32_t size = 0;
    int64_t dtsMs = 0;
    int64_t ptsMs = 0;
    bool sync = false;
};

class Demuxer {
public:
    bool open(const ReadAt &rd, uint64_t total, std::string &err, int timeoutMs = 60000) {
        reset();
        rd_ = rd; total_ = total;
        std::vector<uint8_t> moov;
        T("dm-open");
        if (!findMoov(moov, err, timeoutMs)) { T("dm-moov-falhou"); return false; }
        T("dm-moov-lido");
        if (!parseMoov(moov, err)) { T("dm-parse-falhou"); return false; }
        T("dm-parse-ok");
        return true;
    }

    size_t count() const { return samples_.size(); }
    const Sample &at(size_t i) const { return samples_[i]; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint64_t durationMs() const { return samples_.empty() ? 0 : (uint64_t)std::max<int64_t>(0, lastEndMs_); }

    // Indice do sync sample mais proximo, antes (ou em) 'ms'. Base para seek.
    size_t syncBefore(int64_t ms) const {
        size_t best = 0;
        for (size_t i = 0; i < samples_.size(); i++) {
            if (samples_[i].dtsMs > ms) break;
            if (samples_[i].sync) best = i;
        }
        return best;
    }

    // Le o sample 'i' e o converte para Annex-B (com SPS/PPS na frente dos sync samples).
    // Retorno: 1 ok | 0 ainda nao chegou (tente de novo) | -1 sample invalido/corrompido
    int readAnnexB(size_t i, std::vector<uint8_t> &out, int timeoutMs) {
        out.clear();
        if (i >= samples_.size()) return -1;
        const Sample &s = samples_[i];
        if (s.size == 0 || s.size > (32u << 20) || s.off + s.size > total_) return -1;
        raw_.resize(s.size);
        int n = rd_(raw_.data(), s.off, s.size, timeoutMs);
        if (n < (int)s.size) return 0;
        out.reserve(s.size + 128);
        if (s.sync) out.insert(out.end(), paramSets_.begin(), paramSets_.end());
        size_t p = 0;
        bool any = false;
        while (p + nalLen_ <= raw_.size()) {
            uint32_t len = 0;
            for (int k = 0; k < nalLen_; k++) len = (len << 8) | raw_[p + k];
            p += nalLen_;
            if (len == 0 || p + len > raw_.size()) break;
            mp4ab::appendStartCode(out);
            out.insert(out.end(), raw_.begin() + p, raw_.begin() + p + len);
            p += len;
            any = true;
        }
        return any ? 1 : -1;
    }

private:
    void reset() {
        samples_.clear(); raw_.clear(); paramSets_.clear();
        width_ = height_ = 0; nalLen_ = 4; lastEndMs_ = 0; total_ = 0;
    }

    bool readFull(uint8_t *dst, uint64_t pos, uint64_t len, int timeoutMs) {
        uint64_t done = 0;
        while (done < len) {
            uint32_t chunk = (uint32_t)std::min<uint64_t>(len - done, 4u << 20);
            int n = rd_(dst + done, pos + done, chunk, timeoutMs);
            if (n < (int)chunk) return false;
            done += chunk;
        }
        return true;
    }

    // Anda pelas caixas de nivel superior lendo so cabecalhos (16 bytes) e pulando o mdat.
    bool findMoov(std::vector<uint8_t> &moov, std::string &err, int timeoutMs) {
        uint64_t pos = 0;
        int boxes = 0;
        while (pos + 8 <= total_ && boxes++ < 64) {
            uint8_t h[16] = {0};
            uint32_t n = (uint32_t)std::min<uint64_t>(16, total_ - pos);
            T("dm-caixa");
            if (!readFull(h, pos, n, timeoutMs)) { err = "falha ao ler cabecalho do MP4"; return false; }
            uint64_t size = mp4ab::be32(h);
            uint32_t type = mp4ab::be32(h + 4);
            uint64_t hdr = 8;
            if (size == 1) {
                if (n < 16) { err = "MP4 truncado"; return false; }
                size = mp4ab::be64(h + 8); hdr = 16;
            } else if (size == 0) size = total_ - pos;
            if (size < hdr) { err = "caixa MP4 invalida"; return false; }
            if (type == mp4ab::fourcc("moov")) {
                uint64_t body = size - hdr;
                if (body < 8 || body > (96ull << 20)) { err = "moov com tamanho absurdo"; return false; }
                if (pos + size > total_) { err = "moov passa do fim do arquivo"; return false; }
                T("dm-moov-alloc");
                moov.resize((size_t)body);
                if (!readFull(moov.data(), pos + hdr, body, timeoutMs)) { err = "falha ao ler o indice (moov)"; return false; }
                return true;
            }
            pos += size;
        }
        err = "MP4 sem moov (arquivo fragmentado/MKV nao e suportado aqui)";
        return false;
    }

    struct Box { uint32_t type; size_t data, end; };
    static bool children(const std::vector<uint8_t> &b, size_t from, size_t to, std::vector<Box> &out) {
        out.clear();
        size_t p = from;
        while (p + 8 <= to) {
            uint64_t sz = mp4ab::be32(b.data() + p);
            uint32_t ty = mp4ab::be32(b.data() + p + 4);
            size_t hdr = 8;
            if (sz == 1) { if (p + 16 > to) return false; sz = mp4ab::be64(b.data() + p + 8); hdr = 16; }
            else if (sz == 0) sz = to - p;
            if (sz < hdr || p + sz > to) return false;
            out.push_back({ty, p + hdr, (size_t)(p + sz)});
            p += (size_t)sz;
        }
        return true;
    }

    struct Trak {
        bool avc = false;
        std::vector<uint8_t> avcC;
        uint32_t w = 0, h = 0, timescale = 0;
        std::vector<uint32_t> sizes;
        std::vector<uint64_t> chunkOff;
        struct Stsc { uint32_t first, per; };
        std::vector<Stsc> stsc;
        std::vector<std::pair<uint32_t, uint32_t>> stts, ctts;   // contagem, delta/offset
        std::vector<uint32_t> stss;
        bool hasStss = false;
    };

    bool parseTrak(const std::vector<uint8_t> &b, size_t from, size_t to, Trak &t) {
        const uint8_t *d = b.data();
        std::vector<Box> mdia, l;
        if (!children(b, from, to, l)) return false;
        for (auto &x : l) if (x.type == mp4ab::fourcc("mdia")) { if (!children(b, x.data, x.end, mdia)) return false; }
        std::vector<Box> minf;
        for (auto &x : mdia) {
            if (x.type == mp4ab::fourcc("mdhd") && x.end - x.data >= 20) {
                if (d[x.data] == 1 && x.end - x.data >= 32) t.timescale = mp4ab::be32(d + x.data + 20);
                else t.timescale = mp4ab::be32(d + x.data + 12);
            }
            if (x.type == mp4ab::fourcc("minf")) if (!children(b, x.data, x.end, minf)) return false;
        }
        std::vector<Box> stbl;
        for (auto &x : minf) if (x.type == mp4ab::fourcc("stbl")) { if (!children(b, x.data, x.end, stbl)) return false; }
        for (auto &x : stbl) {
            size_t a = x.data, e = x.end;
            if (x.type == mp4ab::fourcc("stsd") && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                size_t p = a + 8;
                for (uint32_t k = 0; k < cnt && p + 8 <= e; k++) {
                    uint32_t esz = mp4ab::be32(d + p), ety = mp4ab::be32(d + p + 4);
                    if (esz < 8 || p + esz > e) break;
                    if ((ety == mp4ab::fourcc("avc1") || ety == mp4ab::fourcc("avc3")) && esz >= 86) {
                        t.w = (d[p + 32] << 8) | d[p + 33];
                        t.h = (d[p + 34] << 8) | d[p + 35];
                        std::vector<Box> ext;
                        if (children(b, p + 86, p + esz, ext))        // extensoes do VisualSampleEntry comecam em +86
                            for (auto &c : ext) if (c.type == mp4ab::fourcc("avcC")) { t.avcC.assign(d + c.data, d + c.end); t.avc = true; }
                    }
                    p += esz;
                }
            } else if (x.type == mp4ab::fourcc("stsz") && e - a >= 12) {
                uint32_t fixed = mp4ab::be32(d + a + 4), cnt = mp4ab::be32(d + a + 8);
                if (cnt > 20000000) return false;
                if (fixed) t.sizes.assign(cnt, fixed);
                else {
                    if ((uint64_t)a + 12 + (uint64_t)cnt * 4 > e) return false;
                    t.sizes.resize(cnt);
                    for (uint32_t i = 0; i < cnt; i++) t.sizes[i] = mp4ab::be32(d + a + 12 + (size_t)i * 4);
                }
            } else if (x.type == mp4ab::fourcc("stco") && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                if ((uint64_t)a + 8 + (uint64_t)cnt * 4 > e) return false;
                t.chunkOff.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) t.chunkOff[i] = mp4ab::be32(d + a + 8 + (size_t)i * 4);
            } else if (x.type == mp4ab::fourcc("co64") && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                if ((uint64_t)a + 8 + (uint64_t)cnt * 8 > e) return false;
                t.chunkOff.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) t.chunkOff[i] = mp4ab::be64(d + a + 8 + (size_t)i * 8);
            } else if (x.type == mp4ab::fourcc("stsc") && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                if ((uint64_t)a + 8 + (uint64_t)cnt * 12 > e) return false;
                t.stsc.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) {
                    t.stsc[i].first = mp4ab::be32(d + a + 8 + (size_t)i * 12);
                    t.stsc[i].per = mp4ab::be32(d + a + 12 + (size_t)i * 12);
                }
            } else if ((x.type == mp4ab::fourcc("stts") || x.type == mp4ab::fourcc("ctts")) && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                if ((uint64_t)a + 8 + (uint64_t)cnt * 8 > e) return false;
                auto &v = x.type == mp4ab::fourcc("stts") ? t.stts : t.ctts;
                v.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) {
                    v[i].first = mp4ab::be32(d + a + 8 + (size_t)i * 8);
                    v[i].second = mp4ab::be32(d + a + 12 + (size_t)i * 8);
                }
            } else if (x.type == mp4ab::fourcc("stss") && e - a >= 8) {
                uint32_t cnt = mp4ab::be32(d + a + 4);
                if ((uint64_t)a + 8 + (uint64_t)cnt * 4 > e) return false;
                t.hasStss = true;
                t.stss.resize(cnt);
                for (uint32_t i = 0; i < cnt; i++) t.stss[i] = mp4ab::be32(d + a + 8 + (size_t)i * 4);
            }
        }
        return true;
    }

    bool parseMoov(const std::vector<uint8_t> &b, std::string &err) {
        std::vector<Box> top;
        if (!children(b, 0, b.size(), top)) { err = "moov corrompido"; return false; }
        Trak t;
        bool found = false;
        for (auto &x : top) {
            if (x.type == mp4ab::fourcc("cmov")) { err = "moov comprimido (cmov) nao suportado"; return false; }
            if (x.type != mp4ab::fourcc("trak")) continue;
            Trak cur;
            if (!parseTrak(b, x.data, x.end, cur)) continue;
            if (cur.avc && !cur.sizes.empty() && !cur.chunkOff.empty()) { t = std::move(cur); found = true; break; }
        }
        if (!found) { err = "MP4 sem trilha de video H.264 (avc1)"; return false; }
        if (t.timescale == 0) t.timescale = 1000;
        if (t.stsc.empty()) t.stsc.push_back({1, 1});
        width_ = t.w; height_ = t.h;

        std::vector<uint8_t> ps;
        if (!mp4ab::avcCToAnnexB(t.avcC, ps, nalLen_)) { err = "avcC invalido"; return false; }
        paramSets_ = std::move(ps);

        const size_t n = t.sizes.size();
        samples_.resize(n);
        // 1) offsets: stsc distribui os samples pelos chunks
        size_t s = 0, si = 0;
        for (size_t c = 0; c < t.chunkOff.size() && s < n; c++) {
            while (si + 1 < t.stsc.size() && t.stsc[si + 1].first <= c + 1) si++;
            uint64_t off = t.chunkOff[c];
            for (uint32_t k = 0; k < t.stsc[si].per && s < n; k++, s++) {
                samples_[s].off = off;
                samples_[s].size = t.sizes[s];
                off += t.sizes[s];
            }
        }
        if (s < n) { err = "stsc/stco nao cobrem todos os samples"; samples_.clear(); return false; }
        // 2) tempos: dts (stts) e pts = dts + ctts
        const double toMs = 1000.0 / (double)t.timescale;
        int64_t dts = 0;
        size_t idx = 0;
        for (auto &e : t.stts)
            for (uint32_t k = 0; k < e.first && idx < n; k++, idx++) {
                samples_[idx].dtsMs = (int64_t)(dts * toMs);
                samples_[idx].ptsMs = samples_[idx].dtsMs;
                dts += e.second;
            }
        for (; idx < n; idx++) { samples_[idx].dtsMs = (int64_t)(dts * toMs); samples_[idx].ptsMs = samples_[idx].dtsMs; dts += 3000; }
        idx = 0;
        // ctts: o offset e 32 bits com sinal (versao 1) ou sem sinal (versao 0) — int32 cobre os dois nos arquivos reais
        std::vector<int64_t> rawDts(n);
        {   // dts em ticks para somar o ctts em ticks (evita erro de arredondamento)
            int64_t acc = 0; size_t k2 = 0;
            for (auto &e : t.stts)
                for (uint32_t k = 0; k < e.first && k2 < n; k++, k2++) { rawDts[k2] = acc; acc += e.second; }
            for (; k2 < n; k2++) { rawDts[k2] = acc; acc += 3000; }
        }
        for (auto &e : t.ctts)
            for (uint32_t k = 0; k < e.first && idx < n; k++, idx++)
                samples_[idx].ptsMs = (int64_t)((rawDts[idx] + (int32_t)e.second) * toMs);
        // 3) sync samples
        if (!t.hasStss) for (auto &x : samples_) x.sync = true;
        else for (uint32_t v : t.stss) if (v >= 1 && v <= n) samples_[v - 1].sync = true;
        if (!samples_.empty()) samples_[0].sync = true;
        lastEndMs_ = samples_.back().dtsMs + (n > 1 ? samples_.back().dtsMs - samples_[n - 2].dtsMs : 33);
        return true;
    }

    ReadAt rd_;
    uint64_t total_ = 0;
    std::vector<Sample> samples_;
    std::vector<uint8_t> raw_, paramSets_;
    uint32_t width_ = 0, height_ = 0;
    int nalLen_ = 4;
    int64_t lastEndMs_ = 0;
};

} // namespace mp4s
