#pragma once
// Texto proporcional (Inter) para a pagina de detalhes. O resto do app usa a fonte bitmap 8x16 (monoespacada);
// aqui os glifos vem de contornos vetoriais (sansdata.hpp, gerado por scripts/gen_sans.py) e sao rasterizados com
// anti-aliasing exato (area de cobertura) na primeira vez que aparecem; depois vem do cache. Sem libm: igual ao resto do app.
#include <stdint.h>
#include <map>
#include <string>
#include <vector>
#include "sansdata.hpp"

enum SansWeight { SansRegular = 0, SansMedium = 1, SansSemi = 2, SansBold = 3 };

// ---------- Acesso aos dados ----------
inline const int16_t *sansData(int w) {
    switch (w) { case SansMedium: return kSansMedium_data; case SansSemi: return kSansSemi_data; case SansBold: return kSansBold_data; default: return kSansRegular_data; }
}
inline const SansGlyphRec *sansRecs(int w) {
    switch (w) { case SansMedium: return kSansMedium_glyphs; case SansSemi: return kSansSemi_glyphs; case SansBold: return kSansBold_glyphs; default: return kSansRegular_glyphs; }
}
// codepoint -> indice do glifo (ou -1)
inline int sansIndex(unsigned cp) {
    if (cp >= 32 && cp <= 126) return (int)cp - 32;
    if (cp >= 160 && cp <= 255) return 95 + (int)cp - 160;
    switch (cp) {
    case 0x2013: return 191; case 0x2014: return 192; case 0x2018: return 193; case 0x2019: return 194;
    case 0x201C: return 195; case 0x201D: return 196; case 0x2022: return 197; case 0x2026: return 198;
    }
    return -1;
}
// kerning (em 1/2048 em) entre dois indices de glifo
inline int sansKern(int a, int b) {
    unsigned key = (unsigned)a * 256u + (unsigned)b;
    int lo = 0, hi = kSansKernN - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        unsigned k = kSansKern[mid].key;
        if (k == key) return kSansKern[mid].v;
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
// proximo codepoint de uma string UTF-8 (avanca i); invalido vira '?'
inline unsigned sansNext(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i += 1; return c; }
    if ((c >> 5) == 6 && i + 1 < s.size()) { unsigned cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu); i += 2; return cp; }
    if ((c >> 4) == 14 && i + 2 < s.size()) { unsigned cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu); i += 3; return cp; }
    if ((c >> 3) == 30 && i + 3 < s.size()) { i += 4; return '?'; }
    i += 1; return '?';
}

// ---------- Rasterizador ----------
struct SansBmp { int w = 0, h = 0, ox = 0, oy = 0; std::vector<uint8_t> a; };   // ox/oy: canto superior esquerdo relativo a origem (na linha de base)
inline std::map<uint32_t, SansBmp> g_sansCache;

namespace sansdetail {
inline float fl(float v) { int i = (int)v; return (float)((float)i > v ? i - 1 : i); }
inline float ce(float v) { int i = (int)v; return (float)((float)i < v ? i + 1 : i); }
inline float ab(float v) { return v < 0 ? -v : v; }

// Acumula area/cobertura de um segmento (algoritmo de area com sinal: soma corrida por linha da a cobertura exata de cada pixel).
inline void line(float *a, int w, int h, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1.f;
    if (y0 > y1) { dir = -1.f; float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    if (y0 < 0) x -= y0 * dxdy;
    int ys = (int)(y0 < 0 ? 0 : y0);
    int ye = (int)ce(y1); if (ye > h) ye = h;
    for (int y = ys; y < ye; y++) {
        float *row = a + (size_t)y * w;
        float yt = (float)y > y0 ? (float)y : y0, yb = (float)(y + 1) < y1 ? (float)(y + 1) : y1;
        float dy = yb - yt;
        float xn = x + dxdy * dy, d = dy * dir;
        float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
        float xaf = fl(xa); int xai = (int)xaf;
        float xbc = ce(xb); int xbi = (int)xbc;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (x + xn) - xaf;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float s = 1.f / (xb - xa), xaf2 = xa - xaf;
            float a0 = 0.5f * s * (1.f - xaf2) * (1.f - xaf2);
            float xbf = xb - xbc + 1.f;
            float am = 0.5f * s * xbf * xbf;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1.f - a0 - am);
            } else {
                float a1 = s * (1.5f - xaf2);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(xbi - xai - 3) * s;
                row[xbi - 1] += d * (1.f - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xn;
    }
}
inline void cubic(float *a, int w, int h, float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    float mnx = x0, mxx = x0, mny = y0, mxy = y0;
    const float xs[3] = {x1, x2, x3}, ys[3] = {y1, y2, y3};
    for (int i = 0; i < 3; i++) { if (xs[i] < mnx) mnx = xs[i]; if (xs[i] > mxx) mxx = xs[i]; if (ys[i] < mny) mny = ys[i]; if (ys[i] > mxy) mxy = ys[i]; }
    float dim = (mxx - mnx) + (mxy - mny);
    int n = 3 + (int)(dim * 0.35f); if (n > 20) n = 20;
    float px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.f - t;
        float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        float cx = b0 * x0 + b1 * x1 + b2 * x2 + b3 * x3, cy = b0 * y0 + b1 * y1 + b2 * y2 + b3 * y3;
        line(a, w, h, px, py, cx, cy);
        px = cx; py = cy;
    }
}
}  // namespace sansdetail

// size = tamanho do "em" em pixels
inline const SansBmp &sansGlyph(int weight, int gi, float size) {
    int sq = (int)(size * 4.f + 0.5f); if (sq < 4) sq = 4; if (sq > 16000) sq = 16000;
    uint32_t key = ((uint32_t)sq << 10) | ((uint32_t)gi << 2) | (uint32_t)weight;
    auto it = g_sansCache.find(key);
    if (it != g_sansCache.end()) return it->second;
    if (g_sansCache.size() > 1500) g_sansCache.clear();          // limite de seguranca (so uns 100 glifos por tela)
    SansBmp &out = g_sansCache[key];
    const SansGlyphRec &rec = sansRecs(weight)[gi];
    const int16_t *d = sansData(weight) + rec.off;
    const float sc = (float)sq / 4.f / 2048.f;
    // caixa (usa tambem os pontos de controle: conservador)
    float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
    for (int i = 0; i < rec.len;) {
        int op = d[i]; int np = op == 2 ? 3 : (op == 3 ? 0 : 1); i++;
        for (int k = 0; k < np; k++, i += 2) {
            float x = d[i] * sc, y = -d[i + 1] * sc;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
        }
    }
    if (mnx > mxx) return out;                                    // glifo vazio (espaco)
    using namespace sansdetail;
    int x0 = (int)fl(mnx) - 1, y0 = (int)fl(mny) - 1;
    int w = (int)ce(mxx) + 2 - x0, h = (int)ce(mxy) + 2 - y0;
    std::vector<float> acc((size_t)(w + 2) * h, 0.f);
    const int stride = w + 2;
    float cx = 0, cy = 0, sx = 0, sy = 0;
    for (int i = 0; i < rec.len;) {
        int op = d[i++];
        if (op == 0) {
            if (cx != sx || cy != sy) line(acc.data(), stride, h, cx, cy, sx, sy);
            cx = sx = d[i] * sc - x0; cy = sy = -d[i + 1] * sc - y0; i += 2;
        } else if (op == 1) {
            float x = d[i] * sc - x0, y = -d[i + 1] * sc - y0; i += 2;
            line(acc.data(), stride, h, cx, cy, x, y); cx = x; cy = y;
        } else if (op == 2) {
            float x1 = d[i] * sc - x0, y1 = -d[i + 1] * sc - y0, x2 = d[i + 2] * sc - x0, y2 = -d[i + 3] * sc - y0, x3 = d[i + 4] * sc - x0, y3 = -d[i + 5] * sc - y0;
            i += 6;
            cubic(acc.data(), stride, h, cx, cy, x1, y1, x2, y2, x3, y3); cx = x3; cy = y3;
        } else {
            if (cx != sx || cy != sy) line(acc.data(), stride, h, cx, cy, sx, sy);
            cx = sx; cy = sy;
        }
    }
    if (cx != sx || cy != sy) line(acc.data(), stride, h, cx, cy, sx, sy);
    out.w = w; out.h = h; out.ox = x0; out.oy = y0;
    out.a.assign((size_t)w * h, 0);
    for (int y = 0; y < h; y++) {
        float s = 0.f;
        const float *row = acc.data() + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            s += row[x];
            float c = ab(s); if (c > 1.f) c = 1.f;
            out.a[(size_t)y * w + x] = (uint8_t)(c * 255.f + 0.5f);
        }
    }
    return out;
}

// ---------- Medidas e desenho ----------
// largura do texto em pixels (inclui kerning)
inline float sansWidthF(const std::string &t, float size, int weight = SansRegular) {
    const SansGlyphRec *rc = sansRecs(weight);
    float k = size / 2048.f, w = 0; int prev = -1;
    for (size_t i = 0; i < t.size();) {
        int gi = sansIndex(sansNext(t, i)); if (gi < 0) gi = sansIndex('?');
        if (prev >= 0) w += sansKern(prev, gi) * k;
        w += rc[gi].adv * k; prev = gi;
    }
    return w;
}
inline int sansWidth(const std::string &t, float size, int weight = SansRegular) { return (int)(sansWidthF(t, size, weight) + 0.5f); }

// Desenha com a linha de base em 'baseline' e o inicio do texto em x. Retorna a largura.
template <class R> inline int sansDraw(R &r, int x, int baseline, const std::string &t, unsigned argb, float size, int weight = SansRegular) {
    const SansGlyphRec *rc = sansRecs(weight);
    float k = size / 2048.f, pen = (float)x; int prev = -1;
    for (size_t i = 0; i < t.size();) {
        int gi = sansIndex(sansNext(t, i)); if (gi < 0) gi = sansIndex('?');
        if (prev >= 0) pen += sansKern(prev, gi) * k;
        if (gi != 0) {                                            // 0 = espaco
            const SansBmp &g = sansGlyph(weight, gi, size);
            if (g.w > 0) r.drawMask((int)(pen + (pen < 0 ? -0.5f : 0.5f)) + g.ox, baseline + g.oy, g.w, g.h, g.a.data(), argb);
        }
        pen += rc[gi].adv * k; prev = gi;
    }
    return (int)(pen - (float)x + 0.5f);
}
// corta com reticencias para caber em maxW
inline std::string sansFit(const std::string &s, float size, int weight, int maxW) {
    if (sansWidth(s, size, weight) <= maxW) return s;
    std::string r = s;
    while (!r.empty()) {
        r.pop_back();
        while (!r.empty() && ((unsigned char)r.back() & 0xC0) == 0x80) r.pop_back();
        if (!r.empty() && ((unsigned char)r.back() & 0xC0) == 0xC0) r.pop_back();
        while (!r.empty() && r.back() == ' ') r.pop_back();
        if (sansWidth(r + "\xE2\x80\xA6", size, weight) <= maxW) return r + "\xE2\x80\xA6";
    }
    return "\xE2\x80\xA6";
}
// quebra em linhas (por palavras) com no maximo maxLines; a ultima ganha reticencias se sobrou texto
inline std::vector<std::string> sansWrap(const std::string &text, float size, int weight, int maxW, int maxLines) {
    std::vector<std::string> lines;
    std::vector<std::string> words;
    { std::string w; for (char c : text) { if (c == ' ' || c == '\n' || c == '\t') { if (!w.empty()) { words.push_back(w); w.clear(); } } else w += c; } if (!w.empty()) words.push_back(w); }
    std::string cur; size_t wi = 0;
    for (; wi < words.size(); wi++) {
        std::string cand = cur.empty() ? words[wi] : cur + " " + words[wi];
        if (cur.empty() || sansWidth(cand, size, weight) <= maxW) { cur = cand; continue; }
        if ((int)lines.size() + 1 >= maxLines) break;
        lines.push_back(cur); cur = words[wi];
    }
    if (!cur.empty()) {
        if (wi < words.size()) { for (size_t j = wi; j < words.size(); j++) cur += " " + words[j]; cur = sansFit(cur, size, weight, maxW - 1); if (cur.size() < 3 || cur.compare(cur.size() - 3, 3, "\xE2\x80\xA6") != 0) cur += "\xE2\x80\xA6"; }
        lines.push_back(cur);
    }
    return lines;
}
