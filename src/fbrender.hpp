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
    int cx0, cy0, cx1, cy1;                     // recorte: so pixels dentro deste retangulo sao escritos (padrao = tela inteira)
    bool buildingBg = false;                    // true so enquanto monta o cache do fundo (evita recursao)
    FbRenderer(void *p, int w, int h) : fb((uint32_t *)p), W(w), H(h), cx0(0), cy0(0), cx1(w), cy1(h) {}
    int width() const override { return W; }
    int height() const override { return H; }
    // Limita o desenho a um retangulo [x0,x1) x [y0,y1). O resultado dentro dele e IDENTICO ao desenho da tela inteira
    // (cada pixel so depende das coordenadas absolutas): serve para redesenhar so o trecho que mudou.
    void setClip(int x0, int y0, int x1, int y1) {
        cx0 = x0 < 0 ? 0 : x0; cy0 = y0 < 0 ? 0 : y0; cx1 = x1 > W ? W : x1; cy1 = y1 > H ? H : y1;
        if (cx1 < cx0) cx1 = cx0;
        if (cy1 < cy0) cy1 = cy0;
    }
    void clearClip() { cx0 = 0; cy0 = 0; cx1 = W; cy1 = H; }

    // Formas por pixel (poligonos, icones): limita a caixa ao recorte; true = nada a desenhar.
    bool paintBox(int, int &x0, int &y0, int &x1, int &y1, unsigned) override {
        if (x0 < cx0) x0 = cx0;
        if (y0 < cy0) y0 = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
        return x1 <= x0 || y1 <= y0;
    }

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
        if (cx0 == 0 && cx1 == W) { memcpy(fb + (size_t)cy0 * W, c.data() + (size_t)cy0 * W, (size_t)(cy1 - cy0) * W * 4); return true; }
        for (int j = cy0; j < cy1; j++) memcpy(fb + (size_t)j * W + cx0, c.data() + (size_t)j * W + cx0, (size_t)(cx1 - cx0) * 4);
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
        if (j0 < cy0) j0 = cy0;
        if (i0 < cx0) i0 = cx0;
        if (j1 > cy1) j1 = cy1;
        if (i1 > cx1) i1 = cx1;
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

    // Imagem escalada (vizinho mais proximo). A coluna de origem de cada pixel de destino vem de uma tabela montada uma vez
    // por chamada (antes era uma divisao inteira POR PIXEL, que e lenta no processador do PS4); o resultado e o mesmo.
    void blitImage(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh) override {
        if (!px || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        const int jlo = y < cy0 ? cy0 - y : 0, jhi = y + dh > cy1 ? cy1 - y : dh;
        const int ilo = x < cx0 ? cx0 - x : 0, ihi = x + dw > cx1 ? cx1 - x : dw;
        if (jlo >= jhi || ilo >= ihi) return;
        int xm[1024];
        for (int base = ilo; base < ihi; base += 1024) {
            const int bend = base + 1024 < ihi ? base + 1024 : ihi;
            for (int i = base; i < bend; i++) xm[i - base] = i * sw / dw;
            for (int j = jlo; j < jhi; j++) {
                const uint32_t *src = px + (size_t)(j * sh / dh) * sw;
                uint32_t *dst = fb + (size_t)(y + j) * W + x;
                for (int i = base; i < bend; i++) {
                    uint32_t c = src[xm[i - base]];
                    unsigned a = c >> 24;
                    if (a == 0) continue;
                    if (a >= 250) { dst[i] = c | 0xFF000000u; continue; }
                    uint32_t d = dst[i];
                    unsigned inv = 255 - a;
                    unsigned r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * inv) / 255;
                    unsigned g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * inv) / 255;
                    unsigned b = ((c & 255) * a + (d & 255) * inv) / 255;
                    dst[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
    }

    // Mascara de cobertura (texto proporcional): mistura 'argb' sobre o destino conforme a cobertura de cada pixel.
    void drawMask(int x, int y, int w, int h, const uint8_t *m, unsigned argb) override {
        const int A = argb >> 24;
        const int jlo = y < cy0 ? cy0 - y : 0, jhi = y + h > cy1 ? cy1 - y : h;
        const int ilo = x < cx0 ? cx0 - x : 0, ihi = x + w > cx1 ? cx1 - x : w;
        for (int j = jlo; j < jhi; j++) {
            uint32_t *row = fb + (size_t)(y + j) * W + x;
            const uint8_t *src = m + (size_t)j * w;
            for (int i = ilo; i < ihi; i++) {
                int cv = src[i];
                if (!cv) continue;
                int a = A >= 255 ? cv : (A * cv + 127) / 255;
                row[i] = a >= 255 ? (argb | 0xFF000000u) : blendPx(row[i], argb, a);
            }
        }
    }
    // Imagem com cantos arredondados em cima (rTop) e/ou embaixo (rBot), bordas suavizadas.
    // Fora dos cantos (quase todos os pixels) a cobertura e 1: caminho inteiro, sem ponto flutuante.
    void blitImageRound(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh, int rTop, int rBot) override {
        if (!px || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
        if (rTop * 2 > dw) rTop = dw / 2;
        if (rBot * 2 > dw) rBot = dw / 2;
        const int jlo = y < cy0 ? cy0 - y : 0, jhi = y + dh > cy1 ? cy1 - y : dh;
        const int ilo = x < cx0 ? cx0 - x : 0, ihi = x + dw > cx1 ? cx1 - x : dw;
        if (jlo >= jhi || ilo >= ihi) return;
        int xm[1024];
        for (int base = ilo; base < ihi; base += 1024) {
            const int bend = base + 1024 < ihi ? base + 1024 : ihi;
            for (int i = base; i < bend; i++) xm[i - base] = sw == dw ? i : i * sw / dw;
            for (int j = jlo; j < jhi; j++) {
                // cobertura do canto nesta linha: so as colunas dentro do raio precisam de conta
                int rad = 0; float cyc = 0;
                if (j < rTop) { rad = rTop; cyc = (float)rTop; }
                else if (j >= dh - rBot) { rad = rBot; cyc = (float)(dh - rBot); }
                const float ddy = rad ? (j + 0.5f) - cyc : 0.f;
                uint32_t *dst = fb + (size_t)(y + j) * W + x;
                const uint32_t *src = px + (size_t)(sh == dh ? j : j * sh / dh) * sw;
                const int midLo = rad, midHi = dw - rad;             // [midLo, midHi) = sem canto
                for (int i = base; i < bend; i++) {
                    uint32_t c = src[xm[i - base]];
                    unsigned a;
                    if (rad && (i < midLo || i >= midHi)) {
                        float ddx = i < rad ? (i + 0.5f) - (float)rad : (i + 0.5f) - (float)(dw - rad);
                        float cov = (float)rad - fsqrt(ddx * ddx + ddy * ddy) + 0.5f;
                        if (cov <= 0.f) continue;
                        if (cov > 1.f) cov = 1.f;
                        a = (unsigned)((c >> 24) * cov + 0.5f);
                    } else a = c >> 24;
                    if (a >= 255) dst[i] = c | 0xFF000000u;
                    else if (a > 0) dst[i] = blendPx(dst[i], c, (int)a);
                }
            }
        }
    }

    void fillRect(int x, int y, int w, int h, unsigned argb) override {
        int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
        int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
        int a = argb >> 24;
        for (int j = y0; j < y1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            if (a >= 255) for (int i = x0; i < x1; i++) row[i] = argb;
            else for (int i = x0; i < x1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    void pixel(int x, int y, unsigned argb) override {
        if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
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
        int j0 = y < cy0 ? cy0 : y, j1 = y + h > cy1 ? cy1 : y + h;
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
                    if (px0 < cx0) px0 = cx0;
                    if (px1 > cx1) px1 = cx1;
                    for (int i = px0; i < px1; i++) {
                        float dx = (i + 0.5f) - cx;
                        float cov = (float)rad - fsqrt(dx * dx + dy * dy) + 0.5f;
                        if (cov <= 0.f) continue;
                        if (cov > 1.f) cov = 1.f;
                        row[i] = blendPx(row[i], argb, (int)(a * cov));
                    }
                }
            }
            int i0 = mx0 < cx0 ? cx0 : mx0, i1 = mx1 > cx1 ? cx1 : mx1;
            if (a >= 255) for (int i = i0; i < i1; i++) row[i] = argb;
            else for (int i = i0; i < i1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    void gradient(int x, int y, int w, int h, unsigned top, unsigned bot) override {
        int x0 = x < cx0 ? cx0 : x, y0 = y < cy0 ? cy0 : y;
        int x1 = x + w > cx1 ? cx1 : x + w, y1 = y + h > cy1 ? cy1 : y + h;
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
        if (y >= cy1 || y + (int)(16 * s) + 2 <= cy0) return;           // acima/abaixo do recorte
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
            if (cx >= cx1) break;                                       // o resto do texto esta a direita do recorte
            int gi;
            if (cp >= 32 && cp <= 126) gi = cp - 32;
            else if (cp >= 160 && cp <= 255) gi = 95 + (cp - 160);
            else gi = '?' - 32;
            if (cp != 32 && cx + adv + 4 > cx0) {
                const FbGlyph &g = fbGlyph(gi, size);
                for (int dy = 0; dy < g.h; dy++) {
                    int py = y + dy;
                    if (py < cy0 || py >= cy1) continue;
                    uint32_t *row = fb + (size_t)py * W;
                    const uint8_t *src = g.a.data() + (size_t)dy * g.w;
                    for (int dx = 0; dx < g.w; dx++) {
                        int cv = src[dx];
                        if (!cv) continue;
                        int px = cx + dx;
                        if (px < cx0 || px >= cx1) continue;
                        int a = A >= 255 ? cv : (A * cv + 127) / 255;
                        row[px] = blendPx(row[px], argb, a);
                    }
                }
            }
            cx += adv;
        }
    }
};
