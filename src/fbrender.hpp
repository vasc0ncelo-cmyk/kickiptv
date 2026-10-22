#pragma once
// Renderizador de software (framebuffer ARGB). Sem libm/libc extra: usado no PS4 e no preview do PC.
#include <stdint.h>
#include <string>
#include "ui.hpp"
#include "font8x16.hpp"

// Matematica sem libm (o app original nao importava nenhuma funcao de matematica do libc)
static inline int ifloor(float f) { int i = (int)f; return ((float)i > f) ? i - 1 : i; }
static inline float fsqrt(float v) {
    if (v <= 0.f) return 0.f;
    float g = v > 1.f ? v * 0.5f + 0.5f : 1.f;
    for (int i = 0; i < 8; i++) g = 0.5f * (g + v / g);
    return g;
}

static inline uint32_t blendPx(uint32_t d, uint32_t s, int a) {   // a: 0..255
    if (a <= 0) return d;
    if (a >= 255) return 0xFF000000u | (s & 0xFFFFFFu);
    uint32_t ia = 255 - a;
    uint32_t rr = (((s >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
    uint32_t gg = (((s >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
    uint32_t bb = ((s & 255) * a + (d & 255) * ia) / 255;
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

struct FbRenderer : Renderer {
    uint32_t *fb; int W, H;
    FbRenderer(void *p, int w, int h) : fb((uint32_t *)p), W(w), H(h) {}
    int width() const override { return W; }
    int height() const override { return H; }
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
    // texto com suavizacao (a fonte 8x16 e ampliada com interpolacao bilinear)
    void drawText(int x, int y, const std::string &text, unsigned argb, int size) override {
        float s = size / 16.0f;
        int adv = (int)(8 * s + 0.5f);
        int gw = (int)(8 * s) + 2, gh = (int)(16 * s) + 2;
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
            if (cp != 32) {
                const unsigned char *g = kFont8x16[gi];
                auto bit = [&](int gx, int gy) -> float {
                    if (gx < 0 || gx > 7 || gy < 0 || gy > 15) return 0.f;
                    return (float)((g[gy] >> (7 - gx)) & 1);
                };
                for (int dy = 0; dy < gh; dy++) {
                    int py = y + dy;
                    if (py < 0 || py >= H) continue;
                    float v = (dy + 0.5f) / s - 0.5f; int v0 = ifloor(v); float fv = v - v0;
                    uint32_t *row = fb + (size_t)py * W;
                    for (int dx = 0; dx < gw; dx++) {
                        int px = cx + dx;
                        if (px < 0 || px >= W) continue;
                        float u = (dx + 0.5f) / s - 0.5f; int u0 = ifloor(u); float fu = u - u0;
                        float cv = (bit(u0, v0) * (1 - fu) + bit(u0 + 1, v0) * fu) * (1 - fv) +
                                   (bit(u0, v0 + 1) * (1 - fu) + bit(u0 + 1, v0 + 1) * fu) * fv;
                        if (cv <= 0.03f) continue;
                        cv = (cv - 0.12f) * 1.45f;                       // afina um pouco as bordas
                        if (cv <= 0.f) continue;
                        if (cv > 1.f) cv = 1.f;
                        row[px] = blendPx(row[px], argb, (int)(A * cv));
                    }
                }
            }
            cx += adv;
        }
    }
};
