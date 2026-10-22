#pragma once
// Renderizador de software (framebuffer ARGB). Sem libm/libc extra: usado no PS4 e no preview do PC.
#include <stdint.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>
#include "ui.hpp"
#include "font8x16.hpp"
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

// Matematica sem libm (o app original nao importava nenhuma funcao de matematica do libc)
static inline int ifloor(float f) { int i = (int)f; return ((float)i > f) ? i - 1 : i; }
static inline float fsqrt(float v) {
    if (v <= 0.f) return 0.f;
#if defined(__SSE__) || defined(__x86_64__)
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v)));      // 1 instrucao (antes: 8 iteracoes com divisao)
#else
    float g = v > 1.f ? v * 0.5f + 0.5f : 1.f;
    for (int i = 0; i < 8; i++) g = 0.5f * (g + v / g);
    return g;
#endif
}

inline bool g_fbTrunc = false;                 // true so ao montar o cache do fundo: mantem o visual original (arredondamento por truncamento)

// Mistura src sobre dst, a = 0..255. Sem divisoes: R e B juntos numa palavra, G separado; resultado = round(v / 255).
static inline uint32_t blendPx(uint32_t d, uint32_t s, int a) {
    if (a <= 0) return d;
    if (a >= 255) return 0xFF000000u | (s & 0xFFFFFFu);
    uint32_t ia = 255 - (uint32_t)a, ua = (uint32_t)a;
    if (g_fbTrunc) {
        uint32_t rr = (((s >> 16) & 255) * ua + ((d >> 16) & 255) * ia) / 255;
        uint32_t gg = (((s >> 8) & 255) * ua + ((d >> 8) & 255) * ia) / 255;
        uint32_t bb = ((s & 255) * ua + (d & 255) * ia) / 255;
        return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
    }
    uint32_t rb = (d & 0x00FF00FFu) * ia + (s & 0x00FF00FFu) * ua + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g = ((d >> 8) & 0xFFu) * ia + ((s >> 8) & 0xFFu) * ua + 128u;
    g = ((g + (g >> 8)) >> 8) & 0xFFu;
    return 0xFF000000u | rb | (g << 8);
}

// Cache de letras: a fonte 8x16 ampliada com interpolacao bilinear e calculada UMA vez por (letra, tamanho).
// Antes isso era refeito com contas de ponto flutuante a cada pixel de cada letra, a cada quadro.
struct FbGlyph { int w = 0, h = 0; std::vector<uint8_t> a; };
inline std::map<uint32_t, FbGlyph> g_fbGlyphCache;            // global (sem "static" local: nao precisa de guard do compilador)
inline const FbGlyph &fbGlyph(int gi, int size) {
    auto &cache = g_fbGlyphCache;
    uint32_t key = (uint32_t)gi * 1024u + (uint32_t)size;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    FbGlyph &out = cache[key];
    float s = size / 16.0f;
    out.w = (int)(8 * s) + 2; out.h = (int)(16 * s) + 2;
    out.a.assign((size_t)out.w * out.h, 0);
    const unsigned char *g = kFont8x16[gi];
    auto bit = [&](int gx, int gy) -> float {
        if (gx < 0 || gx > 7 || gy < 0 || gy > 15) return 0.f;
        return (float)((g[gy] >> (7 - gx)) & 1);
    };
    for (int dy = 0; dy < out.h; dy++) {
        float v = (dy + 0.5f) / s - 0.5f; int v0 = ifloor(v); float fv = v - v0;
        for (int dx = 0; dx < out.w; dx++) {
            float u = (dx + 0.5f) / s - 0.5f; int u0 = ifloor(u); float fu = u - u0;
            float cv = (bit(u0, v0) * (1 - fu) + bit(u0 + 1, v0) * fu) * (1 - fv) +
                       (bit(u0, v0 + 1) * (1 - fu) + bit(u0 + 1, v0 + 1) * fu) * fv;
            if (cv <= 0.03f) continue;
            cv = (cv - 0.12f) * 1.45f;                       // afina um pouco as bordas
            if (cv <= 0.f) continue;
            if (cv > 1.f) cv = 1.f;
            out.a[(size_t)dy * out.w + dx] = (uint8_t)(cv * 255.f + 0.5f);
        }
    }
    return out;
}

// Fundo (preto + brilho verde + faixa) e sempre igual: e desenhado UMA vez (18 camadas de transparencia em tela cheia,
// o que custava centenas de ms) e depois so copiado.
inline std::vector<uint32_t> g_fbBgCache;

struct FbRenderer : Renderer {
    uint32_t *fb; int W, H;
    bool buildingBg = false;                    // true so enquanto monta o cache do fundo (evita recursao)
    FbRenderer(void *p, int w, int h) : fb((uint32_t *)p), W(w), H(h) {}
    int width() const override { return W; }
    int height() const override { return H; }

    // Fundo pronto: copia do cache (so escreve no destino, em ordem).
    bool blitBackground() override {
        if (buildingBg) return false;
        std::vector<uint32_t> &c = g_fbBgCache;
        if (c.size() != (size_t)W * H) {
            c.assign((size_t)W * H, 0xFF000000u);
            FbRenderer t(c.data(), W, H);
            t.buildingBg = true;
            g_fbTrunc = true;
            uiBackground(t);
            g_fbTrunc = false;
        }
        memcpy(fb, c.data(), (size_t)W * H * 4);
        return true;
    }

    // Brilho em volta de um retangulo arredondado, em UMA passada.
    // O original desenhava ate 22 retangulos grandes sobrepostos; aqui calculamos direto o alfa acumulado de cada pixel
    // (tabela E[m] = efeito das camadas m..spread) a partir da distancia dele ate o retangulo.
    bool glow(int x, int y, int w, int h, int rad, unsigned color, int spread, int peak, bool inside) override {
        if (spread < 1 || w <= 0 || h <= 0) return true;
        if (spread > 64) return false;
        if (rad * 2 > h) rad = h / 2;
        if (rad * 2 > w) rad = w / 2;
        float E[66];
        int ca = color >> 24;
        E[spread + 1] = 0.f;
        for (int m = spread; m >= 1; m--) {
            int a = peak * (spread - m + 1) / (spread * 3);
            if (a > 255) a = 255;
            float f = (float)((ca * a) / 255) / 255.f;
            E[m] = E[m + 1] + f - E[m + 1] * f;
        }
        const float cx = x + w * 0.5f, cy = y + h * 0.5f, hx = w * 0.5f, hy = h * 0.5f;
        const float ix = hx - rad, iy = hy - rad;
        int j0 = y - spread - 1, j1 = y + h + spread + 1, i0 = x - spread - 1, i1 = x + w + spread + 1;
        if (j0 < 0) j0 = 0;
        if (i0 < 0) i0 = 0;
        if (j1 > H) j1 = H;
        if (i1 > W) i1 = W;
        for (int j = j0; j < j1; j++) {
            float dy = (j + 0.5f) - cy; if (dy < 0) dy = -dy;
            float qy = dy - iy;
            int skipL = 1 << 30, skipR = -1;
            if (!inside && qy <= 0.f && hx > 3.f) { skipL = (int)(cx - hx) + 3; skipR = (int)(cx + hx) - 2; }   // miolo: o desenho seguinte cobre
            uint32_t *row = fb + (size_t)j * W;
            for (int i = i0; i < i1; i++) {
                if (i >= skipL && i < skipR) { i = skipR - 1; continue; }
                float dx = (i + 0.5f) - cx; if (dx < 0) dx = -dx;
                float qx = dx - ix;
                float sd;
                if (qx > 0.f && qy > 0.f) sd = fsqrt(qx * qx + qy * qy) - rad;      // so nos cantos
                else sd = (qx > qy ? qx : qy) - rad;
                float m = sd + 0.5f;
                float A;
                if (m < 1.f) A = E[1];
                else if (m >= (float)(spread + 1)) continue;
                else { int lo = (int)m; float fr = m - lo; A = E[lo] + (E[lo + 1] - E[lo]) * fr; }
                int a8 = (int)(A * 255.f + 0.5f);
                if (a8 > 0) row[i] = blendPx(row[i], color, a8);
            }
        }
        return true;
    }
    void fillRect(int x, int y, int w, int h, unsigned argb) override {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        int a = argb >> 24;
        for (int j = y0; j < y1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            if (a >= 255) for (int i = x0; i < x1; i++) row[i] = argb;
            else for (int i = x0; i < x1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    void pixel(int x, int y, unsigned argb) override {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        uint32_t *p = fb + (size_t)y * W + x;
        int a = argb >> 24;
        *p = a >= 255 ? argb : blendPx(*p, argb, a);
    }
    // retangulo com cantos arredondados (bordas suavizadas)
    void roundRect(int x, int y, int w, int h, int rad, unsigned argb) override {
        if (rad * 2 > h) rad = h / 2;
        if (rad * 2 > w) rad = w / 2;
        if (rad <= 0) { fillRect(x, y, w, h, argb); return; }
        int a = argb >> 24;
        int j0 = y < 0 ? 0 : y, j1 = y + h > H ? H : y + h;
        for (int j = j0; j < j1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            bool top = j < y + rad, bot = j >= y + h - rad;
            int mx0 = x, mx1 = x + w;
            if (top || bot) {
                float cy = top ? (float)(y + rad) : (float)(y + h - rad);
                float dy = (j + 0.5f) - cy;
                mx0 = x + rad; mx1 = x + w - rad;
                for (int side = 0; side < 2; side++) {
                    int px0 = side ? x + w - rad : x, px1 = side ? x + w : x + rad;
                    float cx = side ? (float)(x + w - rad) : (float)(x + rad);
                    for (int i = px0; i < px1; i++) {
                        if (i < 0 || i >= W) continue;
                        float dx = (i + 0.5f) - cx;
                        float cov = (float)rad - fsqrt(dx * dx + dy * dy) + 0.5f;
                        if (cov <= 0.f) continue;
                        if (cov > 1.f) cov = 1.f;
                        row[i] = blendPx(row[i], argb, (int)(a * cov));
                    }
                }
            }
            int i0 = mx0 < 0 ? 0 : mx0, i1 = mx1 > W ? W : mx1;
            if (a >= 255) for (int i = i0; i < i1; i++) row[i] = argb;
            else for (int i = i0; i < i1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    void gradient(int x, int y, int w, int h, unsigned top, unsigned bot) override {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        for (int j = y0; j < y1; j++) {
            int t = h > 1 ? (j - y) * 255 / (h - 1) : 0;
            uint32_t c = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int a = (top >> sh) & 255, b = (bot >> sh) & 255;
                c |= (uint32_t)((a * (255 - t) + b * t) / 255) << sh;
            }
            uint32_t *row = fb + (size_t)j * W;
            for (int i = x0; i < x1; i++) row[i] = c;
        }
    }
    int textWidth(const std::string &t, int size) const override {
        int n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) n++;
        return n * (int)(8 * (size / 16.0f) + 0.5f);
    }
    // texto com suavizacao (letras ja ampliadas ficam no cache, ver fbGlyph)
    void drawText(int x, int y, const std::string &text, unsigned argb, int size) override {
        float s = size / 16.0f;
        int adv = (int)(8 * s + 0.5f);
        int A = argb >> 24;
        int cx = x;
        for (size_t i = 0; i < text.size();) {
            unsigned char c = text[i];
            unsigned cp;
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c >> 5) == 6 && i + 1 < text.size()) { cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F); i += 2; }
            else if ((c >> 4) == 14 && i + 2 < text.size()) { cp = '?'; i += 3; }
            else if ((c >> 3) == 30 && i + 3 < text.size()) { cp = '?'; i += 4; }
            else { cp = '?'; i += 1; }
            int gi;
            if (cp >= 32 && cp <= 126) gi = cp - 32;
            else if (cp >= 160 && cp <= 255) gi = 95 + (cp - 160);
            else gi = '?' - 32;
            if (cp != 32 && cx < W && cx + adv + 4 > 0) {
                const FbGlyph &g = fbGlyph(gi, size);
                for (int dy = 0; dy < g.h; dy++) {
                    int py = y + dy;
                    if (py < 0 || py >= H) continue;
                    uint32_t *row = fb + (size_t)py * W;
                    const uint8_t *src = g.a.data() + (size_t)dy * g.w;
                    for (int dx = 0; dx < g.w; dx++) {
                        int cv = src[dx];
                        if (!cv) continue;
                        int px = cx + dx;
                        if (px < 0 || px >= W) continue;
                        int a = A >= 255 ? cv : (A * cv + 127) / 255;
                        row[px] = blendPx(row[px], argb, a);
                    }
                }
            }
            cx += adv;
        }
    }
};
