#pragma once
// Navegacao e desenho das telas. Independente de plataforma: o PS4 so implementa Renderer + leitura do pad.
// Telas: Home (banner + 4 atalhos + lista M3U), Kick TV (digitar @streamer) e listas (Filmes / Series / Canais).
#include "app.hpp"
#include "coverurl.hpp"
#include "detailmeta.hpp"
#include "donate.hpp"
#include "qrcode.hpp"
#include "sans.hpp"
#include <algorithm>
#include <map>
#include <chrono>
#include <functional>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

enum class Input { Up, Down, Left, Right, PrevTab, NextTab, Confirm, Back, PageUp, PageDown, Extra };   // Extra = triangulo: buscar (lupa)
enum class Screen { Home, Kick, List, Help, Detail, Donate };

struct Renderer {
    virtual ~Renderer() {}
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void fillRect(int x, int y, int w, int h, unsigned argb) = 0;
    virtual void drawText(int x, int y, const std::string &text, unsigned argb, int size) = 0;
    // extras visuais (o PS4 implementa de verdade; nos testes caem no retangulo simples)
    virtual void roundRect(int x, int y, int w, int h, int /*raio*/, unsigned argb) { fillRect(x, y, w, h, argb); }
    virtual void gradient(int x, int y, int w, int h, unsigned top, unsigned /*bottom*/) { fillRect(x, y, w, h, top); }
    virtual void pixel(int x, int y, unsigned argb) { fillRect(x, y, 1, 1, argb); }   // com transparencia
    virtual int textWidth(const std::string &t, int size) const {
        int n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) n++;
        return (int)(n * (int)(8 * size / 16.0f + 0.5f));
    }
    // Ganchos de desempenho (o PS4 implementa; quem nao implementar cai no desenho normal):
    virtual bool blitBackground() { return false; }        // true = o fundo ja foi copiado pronto
    virtual bool glow(int, int, int, int, int, unsigned, int, int, bool) { return false; }   // true = brilho desenhado
    virtual void blitImage(int /*x*/, int /*y*/, int /*dw*/, int /*dh*/,
                           const uint32_t * /*px*/, int /*sw*/, int /*sh*/) {}
    // Imagem com cantos de cima (rTop) e de baixo (rBot) arredondados. Padrao: sem arredondar.
    virtual void blitImageRound(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh, int /*rTop*/, int /*rBot*/) {
        blitImage(x, y, dw, dh, px, sw, sh);
    }
    // Mascara de cobertura 0..255 (w x h) pintada com argb: usada pelo texto proporcional (sans.hpp). Padrao: pixel a pixel.
    virtual void drawMask(int x, int y, int w, int h, const uint8_t *a, unsigned argb) {
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                unsigned v = a[(size_t)j * w + i];
                if (!v) continue;
                pixel(x + i, y + j, (((argb >> 24) * v / 255) << 24) | (argb & 0xFFFFFF));
            }
    }
};

// ---------- Tema (cores ARGB) ----------
namespace theme {
constexpr unsigned BG = 0xFF000000;
constexpr unsigned GREEN = 0xFF53FC18, GREEN_DK = 0xFF1F8A0C, GREEN_GLOW = 0x3353FC18, BLACK = 0xFF000000;
constexpr unsigned CARD = 0xFF121212, CARD2 = 0xFF1A1A1A, CARD_LINE = 0xFF2A2A2A, SEL = 0xFF0A1408, KEY = 0xFF2E2E2E, CHIP = 0xFF222222;
constexpr unsigned TEXT = 0xFFFFFFFF, SOFT = 0xFFD6D6D6, MUTED = 0xFF9A9A9A, DIM = 0xFF5E5E5E;
constexpr unsigned RED = 0xFFFF4D5A, AMBER = 0xFFFFB020;
}

// ---------- Matematica simples (sem libm) ----------
inline float uiSqrt(float v) {
    if (v <= 0.f) return 0.f;
#if defined(__SSE__) || defined(__x86_64__)
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v)));
#else
    float g = v > 1.f ? v * 0.5f + 0.5f : 1.f;
    for (int i = 0; i < 8; i++) g = 0.5f * (g + v / g);
    return g;
#endif
}
inline float uiAbs(float v) { return v < 0 ? -v : v; }
inline unsigned uiAlpha(unsigned argb, int a) {                 // multiplica o alfa da cor por a/255
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    unsigned A = ((argb >> 24) * (unsigned)a) / 255;
    return (A << 24) | (argb & 0xFFFFFF);
}

// ---------- Helpers de texto ----------
inline void uiText(Renderer &r, int x, int y, const std::string &t, unsigned c, int size, bool bold = false) {
    r.drawText(x, y, t, c, size);
    if (bold) r.drawText(x + 1, y, t, c, size);
}
// texto "pesado" (titulos): varias passadas deslocadas
inline void uiHeavy(Renderer &r, int x, int y, const std::string &t, unsigned c, int size) {
    r.drawText(x, y, t, c, size);
    r.drawText(x + 1, y, t, c, size);
    if (size >= 30) { r.drawText(x + 2, y, t, c, size); r.drawText(x + 1, y + 1, t, c, size); }
}
// centraliza em (cx, cy)
inline void uiCenter(Renderer &r, int cx, int cy, const std::string &t, unsigned c, int size, bool bold = false) {
    uiText(r, cx - r.textWidth(t, size) / 2, cy - size / 2, t, c, size, bold);
}
inline void uiCenterHeavy(Renderer &r, int cx, int cy, const std::string &t, unsigned c, int size) {
    uiHeavy(r, cx - r.textWidth(t, size) / 2, cy - size / 2, t, c, size);
}
// corta o texto com "..." para caber em maxW
inline std::string uiFit(const Renderer &r, std::string s, int size, int maxW) {
    if (r.textWidth(s, size) <= maxW) return s;
    int adv = r.textWidth("M", size); if (adv < 1) adv = 1;          // fonte monoespacada
    int keep = maxW / adv - 3; if (keep < 0) keep = 0;
    size_t i = 0; int cnt = 0;                                        // corta em 'keep' letras (UTF-8 inteiro)
    while (i < s.size() && cnt < keep) { i++; while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++; cnt++; }
    s.resize(i);
    while (!s.empty() && r.textWidth(s + "...", size) > maxW) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0xC0) s.pop_back();
    }
    return s + "...";
}
// Dicas de botoes (ex.: [X] Selecionar) alinhadas a direita em rightX. Retorna o x onde comecam.
inline void uiGlyphTriangle(Renderer &r, int cx, int cy, int s, unsigned c);      // botao triangulo (a fonte nao tem o caractere)
inline int uiHints(Renderer &r, int rightX, int cy, const std::vector<std::pair<std::string, std::string>> &h) {
    int x = rightX;
    for (int i = (int)h.size() - 1; i >= 0; i--) {
        int lw = r.textWidth(h[i].second, 20), kw = r.textWidth(h[i].first, 18) + 24;
        x -= lw; uiText(r, x, cy - 10, h[i].second, theme::MUTED, 20);
        x -= 10 + kw; r.roundRect(x, cy - 17, kw, 34, 11, theme::KEY);
        if (h[i].first == "△") uiGlyphTriangle(r, x + kw / 2, cy, 18, theme::TEXT);
        else uiCenter(r, x + kw / 2, cy, h[i].first, theme::TEXT, 18, true);
        x -= 30;
    }
    return x + 30;
}

// ---------- Formas ----------
// Retangulo arredondado como area de recorte
struct UiClip {
    int x, y, w, h, rad;
    bool span(int row, float &xl, float &xr) const {
        if (row < y || row >= y + h) return false;
        float cy = row + 0.5f, dy = 0.f;
        if (cy < y + rad) dy = (y + rad) - cy; else if (cy > y + h - rad) dy = cy - (y + h - rad);
        float inset = 0.f;
        if (dy > 0.f) { float t = (float)rad * rad - dy * dy; inset = rad - uiSqrt(t < 0 ? 0 : t); }
        xl = x + inset; xr = x + w - inset; return true;
    }
};
struct UiPt { float x, y; };

inline void uiSpan(Renderer &r, int row, float a, float b, unsigned c) {
    if (b <= a) return;
    if (a < 0) a = 0;
    if (b < 0) return;
    int xa = (int)a, xb = (int)b;
    float fa = a - xa, fb = b - xb;
    if (xa == xb) { r.pixel(xa, row, uiAlpha(c, (int)((b - a) * 255))); return; }
    if (fa > 0.01f) { r.pixel(xa, row, uiAlpha(c, (int)((1.f - fa) * 255))); xa++; }
    if (xb > xa) r.fillRect(xa, row, xb - xa, 1, c);
    if (fb > 0.01f) r.pixel(xb, row, uiAlpha(c, (int)(fb * 255)));
}
// poligono (convexo ou simples), com recorte opcional em retangulo arredondado
inline void uiPoly(Renderer &r, const UiPt *p, int n, unsigned c, const UiClip *clip = nullptr) {
    float miny = p[0].y, maxy = p[0].y;
    for (int i = 1; i < n; i++) { if (p[i].y < miny) miny = p[i].y; if (p[i].y > maxy) maxy = p[i].y; }
    int y0 = (int)miny; if (y0 < 0) y0 = 0;
    int y1 = (int)maxy + 1; if (y1 > r.height()) y1 = r.height();
    for (int row = y0; row < y1; row++) {
        float yc = row + 0.5f, xs[16]; int k = 0;
        for (int i = 0; i < n && k < 16; i++) {
            UiPt a = p[i], b = p[(i + 1) % n];
            if (a.y == b.y) continue;
            float lo = a.y < b.y ? a.y : b.y, hi = a.y < b.y ? b.y : a.y;
            if (yc < lo || yc >= hi) continue;
            xs[k++] = a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y);
        }
        for (int i = 1; i < k; i++) for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) std::swap(xs[j], xs[j - 1]);
        float cl = -1e9f, cr = 1e9f;
        if (clip && !clip->span(row, cl, cr)) continue;
        for (int i = 0; i + 1 < k; i += 2) {
            float a = xs[i] < cl ? cl : xs[i], b = xs[i + 1] > cr ? cr : xs[i + 1];
            uiSpan(r, row, a, b, c);
        }
    }
}
// faixa diagonal a 45 graus (de baixo-esquerda para cima-direita) entre x+y = k0 e x+y = k1
inline void uiBand(Renderer &r, float k0, float k1, float top, float bottom, unsigned c, const UiClip *clip) {
    UiPt p[4] = {{k0 - top, top}, {k1 - top, top}, {k1 - bottom, bottom}, {k0 - bottom, bottom}};
    uiPoly(r, p, 4, c, clip);
}
// pinta pixel a pixel por cobertura 0..1 (formas curvas)
template <class F> inline void uiPaint(Renderer &r, int x0, int y0, int x1, int y1, unsigned col, F cov) {
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float c = cov(x + 0.5f, y + 0.5f);
            if (c <= 0.02f) continue;
            if (c > 1.f) c = 1.f;
            r.pixel(x, y, uiAlpha(col, (int)(c * 255)));
        }
}
inline float uiSdBox(float px, float py, float hx, float hy, float rad) {   // distancia ate um retangulo arredondado
    float qx = uiAbs(px) - hx + rad, qy = uiAbs(py) - hy + rad;
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy; if (in > 0) in = 0;
    return uiSqrt(mx * mx + my * my) + in - rad;
}
inline float uiClamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float uiSin(float a) {                    // seno por polinomio (o app nao usa libm)
    const float PI = 3.14159265f, TP = 6.2831853f;
    while (a > PI) a -= TP;
    while (a < -PI) a += TP;
    float a2 = a * a;
    return a * (1.f - a2 / 6.f * (1.f - a2 / 20.f * (1.f - a2 / 42.f * (1.f - a2 / 72.f * (1.f - a2 / 110.f)))));
}

// brilho suave em volta de um retangulo arredondado
// inside=false: o chamador desenha uma moldura/cor opaca por cima do retangulo logo em seguida, entao o miolo nao precisa do brilho.
inline void uiGlow(Renderer &r, int x, int y, int w, int h, int rad, unsigned color, int spread, int peakAlpha, bool inside = true) {
    if (r.glow(x, y, w, h, rad, color, spread, peakAlpha, inside)) return;
    for (int i = spread; i >= 1; i--) {
        int a = peakAlpha * (spread - i + 1) / (spread * 3);
        r.roundRect(x - i, y - i, w + 2 * i, h + 2 * i, rad + i, uiAlpha(color, a));
    }
}
// contorno: preenche com a cor da borda e depois o miolo
inline void uiFrame(Renderer &r, int x, int y, int w, int h, int rad, int bw, unsigned border, unsigned fill) {
    r.roundRect(x, y, w, h, rad, border);
    r.roundRect(x + bw, y + bw, w - 2 * bw, h - 2 * bw, rad > bw ? rad - bw : 0, fill);
}

// ---------- Animacao: curvas de suavizacao e camada com deslocamento/transparencia ----------
inline float uiEaseOut(float t) { t = uiClamp01(t); float u = 1.f - t; return 1.f - u * u * u; }                    // desacelera no fim
inline float uiEaseInOut(float t) { t = uiClamp01(t); return t < 0.5f ? 4.f * t * t * t : 1.f - (1.f - t) * (1.f - t) * (1.f - t) * 4.f; }
inline float uiEaseBack(float t) {                                                                                   // passa um pouco do alvo e volta (efeito "pop")
    t = uiClamp01(t); const float c1 = 1.70158f, c3 = c1 + 1.f, u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}
inline float uiLerp(float a, float b, float t) { return a + (b - a) * t; }
inline float uiFrac(float v) { return v - (float)(int)v; }

// Desenha o que vier por cima de outro Renderer deslocado (dx, dy) e com opacidade 'a' (0..1). Usado nas transicoes de tela e na entrada
// dos blocos do Inicio. Imagens nao tem alfa no Renderer: escurecem na direcao do preto (o fundo do app).
struct UiFx : Renderer {
    Renderer &r; int dx, dy; float a;
    UiFx(Renderer &base, int x, int y, float alpha) : r(base), dx(x), dy(y), a(alpha < 0.f ? 0.f : alpha > 1.f ? 1.f : alpha) {}
    unsigned mul(unsigned c) const { return a >= 0.998f ? c : uiAlpha(c, (int)(a * 255.f + 0.5f)); }
    void dim(int x, int y, int w, int h) { if (a < 0.998f) r.fillRect(x + dx, y + dy, w, h, ((unsigned)((1.f - a) * 255.f) << 24) | 0x000000u); }
    int width() const override { return r.width(); }
    int height() const override { return r.height(); }
    void fillRect(int x, int y, int w, int h, unsigned c) override { r.fillRect(x + dx, y + dy, w, h, mul(c)); }
    void drawText(int x, int y, const std::string &t, unsigned c, int size) override { r.drawText(x + dx, y + dy, t, mul(c), size); }
    void roundRect(int x, int y, int w, int h, int rad, unsigned c) override { r.roundRect(x + dx, y + dy, w, h, rad, mul(c)); }
    void gradient(int x, int y, int w, int h, unsigned top, unsigned bot) override { r.gradient(x + dx, y + dy, w, h, mul(top), mul(bot)); }
    void pixel(int x, int y, unsigned c) override { r.pixel(x + dx, y + dy, mul(c)); }
    int textWidth(const std::string &t, int size) const override { return r.textWidth(t, size); }
    bool blitBackground() override { return false; }
    bool glow(int x, int y, int w, int h, int rad, unsigned color, int spread, int peak, bool inside) override {
        return r.glow(x + dx, y + dy, w, h, rad, color, spread, (int)(peak * a), inside);
    }
    void blitImage(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh) override { r.blitImage(x + dx, y + dy, dw, dh, px, sw, sh); dim(x, y, dw, dh); }
    void blitImageRound(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh, int rt, int rb) override {
        r.blitImageRound(x + dx, y + dy, dw, dh, px, sw, sh, rt, rb); dim(x, y, dw, dh);
    }
    void drawMask(int x, int y, int w, int h, const uint8_t *m, unsigned c) override { r.drawMask(x + dx, y + dy, w, h, m, mul(c)); }
};

// Texto proporcional (Inter) posicionado pelo CENTRO vertical das maiusculas (cy) e nao pela linha de base.
inline int uiSansL(Renderer &r, int x, int cy, const std::string &t, unsigned c, float size, int weight) {
    return sansDraw(r, x, cy + (int)(size * 0.3635f + 0.5f), t, c, size, weight);
}
inline void uiSansC(Renderer &r, int cx, int cy, const std::string &t, unsigned c, float size, int weight) {
    sansDraw(r, cx - sansWidth(t, size, weight) / 2, cy + (int)(size * 0.3635f + 0.5f), t, c, size, weight);
}
inline void uiSansR(Renderer &r, int rx, int cy, const std::string &t, unsigned c, float size, int weight) {
    sansDraw(r, rx - sansWidth(t, size, weight), cy + (int)(size * 0.3635f + 0.5f), t, c, size, weight);
}

// ---------- Marca Kick (estilo pixel) ----------
// "K" em blocos, caixa w x h
inline void uiKMark(Renderer &r, int x, int y, int w, int h, unsigned c) {
    struct R { float x0, x1, y0, y1; };
    static const R rs[8] = {{0, .370f, 0, 1}, {.370f, .491f, .245f, .764f}, {.491f, .620f, .136f, .882f}, {.620f, 1, 0, .318f},
                            {.620f, .861f, .318f, .445f}, {.620f, .741f, .445f, .564f}, {.620f, .861f, .564f, .691f}, {.620f, 1, .691f, 1}};
    for (const R &q : rs) {
        int rx0 = x + (int)(q.x0 * w + 0.5f), rx1 = x + (int)(q.x1 * w + 0.5f);
        int ry0 = y + (int)(q.y0 * h + 0.5f), ry1 = y + (int)(q.y1 * h + 0.5f);
        r.fillRect(rx0, ry0, rx1 - rx0, ry1 - ry0, c);
    }
}
// letras pesadas do wordmark (K I C P T V). Retorna a largura usada.
inline int uiLetterW(char ch, int h) {
    switch (ch) { case 'K': case 'C': case 'T': return h * 4 / 5; case 'I': return h * 17 / 50; case 'P': return h * 18 / 25; case 'V': return h * 9 / 10; default: return h * 7 / 20; }
}
inline int uiLetter(Renderer &r, char ch, int x, int y, int w, int h, unsigned c) {
    int t = h * 27 / 100;
    switch (ch) {
    case 'K': uiKMark(r, x, y, w, h, c); break;
    case 'I': r.fillRect(x, y, w, h, c); break;
    case 'C':
        r.fillRect(x, y, t, h, c); r.fillRect(x, y, w, t, c); r.fillRect(x, y + h - t, w, t, c);
        r.fillRect(x + w - t * 4 / 5, y + t, t * 4 / 5, t / 2, c); r.fillRect(x + w - t * 4 / 5, y + h - t - t / 2, t * 4 / 5, t / 2, c);
        break;
    case 'P': {
        int bh = h * 64 / 100;
        r.fillRect(x, y, t, h, c); r.fillRect(x, y, w, t, c); r.fillRect(x, y + bh - t, w, t, c); r.fillRect(x + w - t, y, t, bh, c);
        break; }
    case 'T': r.fillRect(x, y, w, t, c); r.fillRect(x + (w - t) / 2, y, t, h, c); break;
    case 'V': {
        float tw = t * 1.15f, fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
        UiPt a[4] = {{fx, fy}, {fx + tw, fy}, {fx + fw / 2 + tw / 2, fy + fh}, {fx + fw / 2 - tw / 2, fy + fh}};
        UiPt b[4] = {{fx + fw - tw, fy}, {fx + fw, fy}, {fx + fw / 2 + tw / 2, fy + fh}, {fx + fw / 2 - tw / 2, fy + fh}};
        uiPoly(r, a, 4, c); uiPoly(r, b, 4, c);
        break; }
    }
    return w;
}
// palavra com letras pesadas; wScale em % da largura normal (100 = normal). Retorna a largura total.
inline int uiWord(Renderer &r, const char *s, int x, int y, int h, unsigned c, int wScale = 100) {
    int cx = x, gap = h / 9 + 1;
    for (; *s; s++) {
        if (*s == ' ') { cx += h * 3 / 10; continue; }
        int w = uiLetterW(*s, h) * wScale / 100;
        uiLetter(r, *s, cx, y, w, h, c);
        cx += w + gap;
    }
    return cx - gap - x;
}

inline int uiWordWidth(const char *s, int h, int wScale = 100) {
    int cx = 0, gap = h / 9 + 1;
    for (; *s; s++) { if (*s == ' ') { cx += h * 3 / 10; continue; } cx += uiLetterW(*s, h) * wScale / 100 + gap; }
    return cx - gap;
}

// ---------- Icones ----------
inline void uiIconWifi(Renderer &r, int cx, int cy, int s, unsigned c) {   // (cx, cy) = ponto de baixo
    float t = s * 0.13f;
    for (int k = 1; k <= 3; k++) {
        float R = s * (0.30f * k + 0.05f);
        uiPaint(r, cx - (int)(R + 2), cy - (int)(R + 2), cx + (int)(R + 2), cy + 2, c, [&](float px, float py) {
            float dx = px - cx, dy = py - cy;
            if (dy > -s * 0.04f || uiAbs(dx) > -dy * 1.0f) return 0.f;
            float d = uiSqrt(dx * dx + dy * dy);
            return uiClamp01(0.5f - (uiAbs(d - R) - t * 0.5f));
        });
    }
    r.roundRect(cx - (int)(s * 0.08f), cy - (int)(s * 0.08f), (int)(s * 0.16f), (int)(s * 0.16f), (int)(s * 0.08f), c);
}
inline void uiGlyphTriangle(Renderer &r, int cx, int cy, int s, unsigned c) {      // contorno de triangulo (botao △ do controle)
    const float ax = cx, ay = cy - s * 0.52f, bx = cx - s * 0.52f, by = cy + s * 0.40f, dx2 = cx + s * 0.52f, t = s * 0.14f;
    auto seg = [](float px, float py, float x1, float y1, float x2, float y2) {
        float vx = x2 - x1, vy = y2 - y1, wx = px - x1, wy = py - y1;
        float h = (wx * vx + wy * vy) / (vx * vx + vy * vy); h = h < 0.f ? 0.f : (h > 1.f ? 1.f : h);
        float qx = wx - vx * h, qy = wy - vy * h; return uiSqrt(qx * qx + qy * qy);
    };
    uiPaint(r, cx - s, cy - s, cx + s, cy + s, c, [&](float px, float py) {
        float d = seg(px, py, ax, ay, bx, by);
        float d2 = seg(px, py, bx, by, dx2, by); if (d2 < d) d = d2;
        float d3 = seg(px, py, dx2, by, ax, ay); if (d3 < d) d = d3;
        return uiClamp01(0.5f - (d - t * 0.5f));
    });
}
inline void uiIconSearch(Renderer &r, int cx, int cy, int s, unsigned c) {   // lupa
    const float t = s * 0.11f, R = s * 0.30f, ox = cx - s * 0.10f, oy = cy - s * 0.10f;
    const float ax = ox + R * 0.7071f, ay = oy + R * 0.7071f, bx = cx + s * 0.40f, by = cy + s * 0.40f;
    uiPaint(r, cx - s / 2 - 2, cy - s / 2 - 2, cx + s / 2 + 2, cy + s / 2 + 2, c, [&](float px, float py) {
        float dx = px - ox, dy = py - oy;
        float d = uiSqrt(dx * dx + dy * dy);
        float cv = uiClamp01(0.5f - (uiAbs(d - R) - t * 0.5f));
        float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
        float h = (wx * vx + wy * vy) / (vx * vx + vy * vy);
        h = h < 0.f ? 0.f : (h > 1.f ? 1.f : h);
        float qx = wx - vx * h, qy = wy - vy * h;
        float c2 = uiClamp01(0.5f - (uiSqrt(qx * qx + qy * qy) - t * 0.6f));
        return c2 > cv ? c2 : cv;
    });
}
inline void uiIconLink(Renderer &r, int cx, int cy, int s, unsigned c) {
    const float k = 0.7071f, t = s * 0.085f, hx = s * 0.30f, hy = s * 0.17f;
    uiPaint(r, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, c, [&](float px, float py) {
        float dx = px - cx, dy = py - cy;
        float u = (dx - dy) * k, v = (dx + dy) * k;
        float best = 0.f;
        for (int i = -1; i <= 1; i += 2) {
            float sd = uiSdBox(u - i * s * 0.19f, v, hx, hy, hy);
            float cv = uiClamp01(0.5f - (uiAbs(sd + t * 0.5f) - t * 0.5f));
            if (cv > best) best = cv;
        }
        return best;
    });
}
inline void uiIconClapper(Renderer &r, int x, int y, int s, unsigned c, unsigned bg) {
    float S = (float)s, fx = (float)x, fy = (float)y;
    r.roundRect(x, y + (int)(S * 0.50f), s, (int)(S * 0.50f), (int)(S * 0.07f), c);
    UiPt arm[4] = {{fx, fy + S * 0.30f}, {fx + S * 0.88f, fy + S * 0.06f}, {fx + S * 0.93f, fy + S * 0.24f}, {fx + S * 0.03f, fy + S * 0.47f}};
    uiPoly(r, arm, 4, c);
    for (int i = 0; i < 3; i++) {   // listras escuras do claquete
        float o = S * (0.14f + 0.22f * i);
        UiPt st[4] = {{fx + o, fy + S * 0.28f - o * 0.27f}, {fx + o + S * 0.09f, fy + S * 0.255f - o * 0.27f},
                      {fx + o + S * 0.02f, fy + S * 0.47f - o * 0.27f}, {fx + o - S * 0.07f, fy + S * 0.49f - o * 0.27f}};
        uiPoly(r, st, 4, bg);
    }
    r.fillRect(x, y + (int)(S * 0.47f), s, (int)(S * 0.045f) + 1, bg);
    r.roundRect(x + (int)(S * 0.22f), y + (int)(S * 0.62f), (int)(S * 0.16f), (int)(S * 0.06f), (int)(S * 0.03f), bg);
    r.roundRect(x + (int)(S * 0.62f), y + (int)(S * 0.62f), (int)(S * 0.16f), (int)(S * 0.06f), (int)(S * 0.03f), bg);
}
inline void uiAntennas(Renderer &r, float cx, float baseY, float S, float t, unsigned c) {
    UiPt a[4] = {{cx - S * 0.30f, baseY - S * 0.22f}, {cx - S * 0.30f + t, baseY - S * 0.22f}, {cx + t * 0.5f, baseY}, {cx - t * 0.5f, baseY}};
    UiPt b[4] = {{cx + S * 0.30f - t, baseY - S * 0.22f}, {cx + S * 0.30f, baseY - S * 0.22f}, {cx + t * 0.5f, baseY}, {cx - t * 0.5f, baseY}};
    uiPoly(r, a, 4, c); uiPoly(r, b, 4, c);
}
inline void uiIconTvPlay(Renderer &r, int x, int y, int s, unsigned c, unsigned bg) {
    float S = (float)s;
    uiAntennas(r, x + S * 0.5f, y + S * 0.22f, S, S * 0.07f, c);
    r.roundRect(x, y + (int)(S * 0.20f), s, (int)(S * 0.68f), (int)(S * 0.09f), c);
    r.roundRect(x + (int)(S * 0.065f), y + (int)(S * 0.265f), (int)(S * 0.87f), (int)(S * 0.55f), (int)(S * 0.04f), bg);
    UiPt tri[3] = {{x + S * 0.42f, y + S * 0.38f}, {x + S * 0.42f, y + S * 0.70f}, {x + S * 0.67f, y + S * 0.54f}};
    uiPoly(r, tri, 3, c);
    r.roundRect(x + (int)(S * 0.28f), y + (int)(S * 0.93f), (int)(S * 0.44f), (int)(S * 0.065f), (int)(S * 0.03f), c);
}
inline void uiIconTvRetro(Renderer &r, int x, int y, int s, unsigned c, unsigned bg) {
    float S = (float)s;
    uiAntennas(r, x + S * 0.5f, y + S * 0.26f, S, S * 0.07f, c);
    r.roundRect(x, y + (int)(S * 0.24f), s, (int)(S * 0.70f), (int)(S * 0.17f), c);
    r.roundRect(x + (int)(S * 0.095f), y + (int)(S * 0.335f), (int)(S * 0.64f), (int)(S * 0.52f), (int)(S * 0.11f), bg);
    r.roundRect(x + (int)(S * 0.83f), y + (int)(S * 0.575f), (int)(S * 0.06f), (int)(S * 0.06f), (int)(S * 0.03f), bg);
    r.roundRect(x + (int)(S * 0.17f), y + (int)(S * 0.925f), (int)(S * 0.16f), (int)(S * 0.06f), (int)(S * 0.03f), c);
    r.roundRect(x + (int)(S * 0.67f), y + (int)(S * 0.925f), (int)(S * 0.16f), (int)(S * 0.06f), (int)(S * 0.03f), c);
}

// ---------- Fundo e cabecalho ----------
inline void uiBackground(Renderer &r) {
    if (r.blitBackground()) return;
    const int W = r.width(), H = r.height();
    r.fillRect(0, 0, W, H, theme::BG);
    // brilho verde sutil no canto inferior direito
    for (int i = 18; i >= 1; i--) {
        int rad = 70 + i * 28;
        r.roundRect(W - rad + 60, H - rad + 60, rad * 2, rad * 2, rad, 0x0653FC18);
    }
    // faixa diagonal discreta no topo
    UiPt p[4] = {{1330, 0}, {1470, 0}, {1230, 190}, {1090, 190}};
    uiPoly(r, p, 4, 0x12FFFFFF);
    // riscos de neon nos cantos de cima (como na referencia): halo largo + nucleo fino
    static const struct { float k0, k1; unsigned c; } L[] = {
        {95, 255, 0x0A53FC18}, {150, 200, 0x2253FC18}, {160, 178, 0xB053FC18}, {222, 232, 0x6053FC18}, {262, 266, 0x3053FC18}};
    for (const auto &q : L) uiBand(r, q.k0, q.k1, 0, 300, q.c, nullptr);
    for (const auto &q : L) uiBand(r, (float)W + 40 + (q.k0 - 95), (float)W + 40 + (q.k1 - 95), 0, 300, q.c, nullptr);
}
inline int uiLogoWidth(int h) { return uiWordWidth("KICK", h) + h * 3 / 8 + uiWordWidth("IPTV", h); }
inline void uiLogo(Renderer &r, int x, int y, int h) {      // "KICK IPTV"
    int w = uiWord(r, "KICK", x, y, h, theme::GREEN);
    uiWord(r, "IPTV", x + w + h * 3 / 8, y, h, theme::TEXT);
}
inline void uiTopRight(Renderer &r, const std::string &hhmm, const std::string &date) {
    const int W = r.width();
    uiIconWifi(r, W - 301, 100, 80, theme::GREEN);
    r.fillRect(W - 230, 46, 2, 69, 0xFF6A6A6A);
    if (!hhmm.empty()) {
        const int rx = W - 72;
        uiSansR(r, rx, 68, hhmm, theme::TEXT, 49.f, SansBold);
        uiSansR(r, rx, 105, date, theme::SOFT, 22.f, SansMedium);
    }
}
inline void uiHeader(Renderer &r, const std::string &hhmm, const std::string &date) {
    uiBackground(r);
    uiLogo(r, 73, 46, 69);
    uiTopRight(r, hhmm, date);
}

// coracao (apoiar): dois circulos no alto + um triangulo embaixo (o entalhe do meio fica bem marcado), em linhas horizontais
inline void uiHeart(Renderer &r, int cx, int cy, int size, unsigned c) {
    const float half = size / 2.0f;
    for (int j = 0; j < size; j++) {
        float v = (half - j - 0.5f) / half;                          // topo = +1, base = -1
        int run = -1;                                                  // inicio do trecho preenchido (o entalhe do meio fica de fora)
        for (int i = 0; i <= size; i++) {
            bool in = false;
            if (i < size) {
                float u = (i + 0.5f - half) / half;
                float au = u < 0 ? -u : u;
                float dx = au - 0.5f, dy = v - 0.3f;                  // circulos de raio 0.5 centrados em (+-0.5, 0.3)
                in = dx * dx + dy * dy <= 0.25f;
                if (!in && v <= 0.3f && v >= -0.98f && au <= (v + 0.98f) * 0.78f) in = true;   // ponta de baixo
            }
            if (in && run < 0) run = i;
            else if (!in && run >= 0) { r.fillRect(cx - size / 2 + run, cy - size / 2 + j, i - run, 1, c); run = -1; }
        }
    }
}

struct UI {
    App &app;
    std::function<void(const std::string &)> coverRequest;
    std::function<bool(const std::string &, const uint32_t *&, int &, int &)> coverGet;
    // pede as capas: 1o argumento = o que esta na tela (em ordem), 2o = proximas paginas (pre-carregamento)
    std::function<void(const std::vector<std::string> &, const std::vector<std::string> &)> coverBatch;
    std::function<void(std::vector<std::string>)> coverBackground;   // lista inteira de capas, para baixar em segundo plano
    std::function<void(std::vector<std::string>)> coverBulk;         // TRIANGULO: baixa as capas de TODAS as categorias (ficam salvas no PS4)
    std::function<void()> coverBulkStop;
    std::function<bool()> coverBulkActive;
    std::string lastBatchKey, lastBgKey;
    Screen screen = Screen::Home;
    Tab tab = Tab::Kick;
    int homeRow = 1;          // 0 = banner, 1 = atalhos, 2 = lista M3U, 3 = comandos / apoiar
    int homeCol3 = 0;         // linha 3: 0 = comandos e ajuda, 1 = apoiar o app
    std::string donateUrl = kDonateUrl, donatePix = kDonatePixKey;   // (testes podem trocar)
    bool hasDonate() const { return !donateUrl.empty(); }
    // carrossel do Inicio: filmes e series do catalogo (a Kick aparece so no atalho KICK TV, nunca aqui)
    //  logo = poster | backdrop = imagem larga do painel (vazio = nao tem) | plot = sinopse | key = "m:<id>" (filme) para pedir o backdrop ao painel
    struct HeroItem { int kind = 1; std::string title, sub, logo, cat, key, backdrop, plot, tlogo; uint32_t idx = 0; bool infoDone = false, logoDone = false; };   // kind: 1 filme, 2 serie
    std::vector<HeroItem> hero;
    std::string heroSig, lastHeroKey_;
    std::vector<HeroItem> heroCandM_;   // candidatos a filme (ate 10): os que tem backdrop sobem para o carrossel
    bool heroSettled_ = false;
    int heroInfoVer_ = 0;     // sobe quando chega um backdrop novo (pede a imagem)
    std::vector<uint32_t> peekBuf_[2]; std::string peekKey_[2];   // cartas laterais inclinadas ja montadas
    Screen prevScreen_ = Screen::Home;
    uint32_t heroSeed_ = 0;            // semente do sorteio dos filmes do banner (muda a cada abertura do app)
    bool detailBackHome = false;       // detalhe aberto pelo banner do Inicio: Voltar volta para o Inicio (nao para a aba)
    int heroCur = 0, heroFrom = 0;
    double heroT0 = -10, heroAuto = 0;
    int homeCol = 1;          // atalhos: 0 = apoiar (ou KICK TV se nao houver link), 1 = filmes, 2 = series, 3 = canais
    int helpScroll = 0;
    // ---- animacao ----
    // tick(t) roda uma vez por quadro; as telas leem animT/animDt e os valores suavizados (sm). Nenhum som: a interface e silenciosa.
    double animT = 0, fadeAt = -10, lastTickT = 0;     // relogio, quando a tela mudou
    float animDt = 0.033f;                              // duracao do ultimo quadro (s)
    int fadeSig = -1, trDir = 1;                        // trDir: de que lado a tela nova entra (+1 direita, -1 esquerda)
    int prevScreenDepth = 0, prevTabSig = 0;
    std::map<int, float> smv_;                          // valores que "deslizam" ate o alvo (foco, brilho, escala)
    static int screenDepth(Screen s) { return s == Screen::Home ? 0 : s == Screen::Detail ? 2 : 1; }
    bool tick(double t) {
        animDt = lastTickT > 0 ? (float)(t - lastTickT) : 0.033f;
        if (animDt < 0.001f) animDt = 0.001f;
        if (animDt > 0.1f) animDt = 0.1f;
        lastTickT = t; animT = t;
        int sig = (int)screen * 64 + (int)tab * 8 + (detailSeries ? 2 : 0);
        if (sig != fadeSig) {
            fadeSig = sig; fadeAt = t;
            int d = screenDepth(screen), tb = (int)tab;
            if (d != prevScreenDepth) trDir = d > prevScreenDepth ? 1 : -1;
            else trDir = tb >= prevTabSig ? 1 : -1;
            prevScreenDepth = d; prevTabSig = tb;
            smv_.clear();                               // cada tela comeca com o foco ja no lugar (sem "voar" de outra tela)
        }
        if (screen == Screen::Home && prevScreen_ != Screen::Home && prevScreen_ != Screen::Donate && prevScreen_ != Screen::Help) {   // voltou ao Inicio: novo sorteio dos filmes do banner
            heroSeed_ = 0; heroSig.clear(); heroCur = 0; heroFrom = 0; heroAuto = t;
        }
        prevScreen_ = screen;
        heroTick(t);
        return wantsFrames();
    }
    bool wantsFrames() const { return screen != Screen::Help && screen != Screen::Donate; }     // as telas de texto/QR ficam paradas
    // valor que desliza ate 'target' (velocidade em 1/s). Na 1a vez ja nasce no alvo.
    float sm(int id, float target, float speed = 14.f) {
        auto it = smv_.find(id);
        if (it == smv_.end()) { smv_[id] = target; return target; }
        float x = speed * animDt, k = x / (1.f + x + 0.5f * x * x);                             // ~ 1 - e^-x
        it->second += (target - it->second) * k;
        if (uiAbs(target - it->second) < 0.003f) it->second = target;
        return it->second;
    }
    float pulse() const { return 0.80f + 0.20f * uiSin((float)animT * 3.2f); }                   // brilho do foco "respirando" (suave)
    int pk(int base) const { return (int)(base * pulse()); }
    static bool looksLikeError(const std::string &m) {
        return m.find("Falha") != std::string::npos || m.find("offline") != std::string::npos || m.find("inválido") != std::string::npos ||
               m.find("não parece") != std::string::npos || m.find("Erro") != std::string::npos || m.find("erro") != std::string::npos;
    }
    int cursor[4] = {0, 0, 0, 0};
    int scroll[4] = {0, 0, 0, 0};
    int level = 0;            // so Series: 0 series, 1 temporadas, 2 episodios
    int catCur[4] = {0, 0, 0, 0}, catScroll[4] = {0, 0, 0, 0};   // categoria escolhida (barra da esquerda) em cada aba
    bool inItems = false;     // false = foco na barra de categorias, true = foco nos itens (Filmes/Series/Canais)
    std::string curSeries;
    int curSeason = 0;
    std::string kickLast;     // ultimo @ pesquisado
    std::string m3uUrl;       // lista M3U atual
    std::string defaultUrl;   // lista padrao do app (defaultlist.hpp). Preenchida = a lista e fixa: nao da para adicionar outra, so atualizar
    bool reloadRequested = false;   // o botao ATUALIZAR foi apertado: a plataforma baixa a lista de novo (veja o laco principal)
    std::string timeText, dateText;   // relogio (a plataforma atualiza)
    int textKind = 0;         // 0 nada, 1 = @streamer, 2 = endereco M3U, 3 = busca
    std::string searchQ;
    std::vector<uint32_t> searchHits;
    // Pagina de detalhes (filme / serie)
    bool detailSeries = false;
    std::string detailTitle, detailLogo, detailUrl, detailGroup;   // titulo ja limpo (sem ano/qualidade); detailGroup = categoria
    int detailSeasonIdx = 0;
    int detailFocus = 0;       // 0 Assistir, 1 temporadas, 2 episodios
    int detailEpCur = 0;
    int detailEpScroll = 0, detailSeaScroll = 0;   // primeiro cartao / aba visivel
    std::vector<int> detailSeasons;
    DetailMeta detailMeta;
    // Gancho opcional: preenche sinopse/nota/generos/duracao (ex.: TMDB). Chamado ao abrir a pagina, ja com o que da para tirar do nome.
    std::function<bool(const std::string &title, bool series, DetailMeta &)> metaLookup;
    static const int kDetEps = 5, kDetTabs = 5;    // cartoes e abas visiveis por vez
    static const int kRows = 10;
    static const int ImgW = 148, ImgH = 222;       // tamanho do poster (igual ao da miniatura em imgcache.hpp)
    static const int kCols = 8, kGridRows = 3;     // grade de posters (Filmes, Series, Canais)

    explicit UI(App &a) : app(a) {}

    static Tab tileTab(int col) { static const Tab m[4] = {Tab::Kick, Tab::Movies, Tab::Series, Tab::Channels}; return m[col & 3]; }
    // L1/R1 entre as listas: Filmes -> Series -> Canais (a Kick TV abre so pelo banner)
    static Tab cycleTab(Tab t, int dir) {
        static const Tab m[3] = {Tab::Movies, Tab::Series, Tab::Channels};
        int cur = -1; for (int i = 0; i < 3; i++) if (m[i] == t) cur = i;
        if (cur < 0) return dir > 0 ? Tab::Movies : Tab::Channels;
        return m[(cur + (dir > 0 ? 1 : 2)) % 3];
    }
    static int tabTile(Tab t) { for (int i = 0; i < 4; i++) if (tileTab(i) == t) return i; return 0; }

    // Dados guardados da ultima vez
    void loadSaved() {
        kickLast = kickSlug(app.pf.loadSetting("kick"));
        m3uUrl = app.pf.loadSetting("m3u");
        if (!defaultUrl.empty()) m3uUrl = defaultUrl;       // lista fixa: ignora qualquer outra que tenha ficado guardada
    }
    bool listLocked() const { return !defaultUrl.empty(); }
    bool hasSavedList() const { return !m3uUrl.empty(); }
    void loadSavedList() { if (!m3uUrl.empty()) app.loadM3U(m3uUrl, true); }   // true = usa a copia salva (nao baixa de novo)

    // Itens da tela Kick TV
    enum { KA_TYPE = 0, KA_LAST = 1 };
    std::vector<int> kickActions() const {
        std::vector<int> a;
        a.push_back(KA_TYPE);
        if (!kickLast.empty()) a.push_back(KA_LAST);
        return a;
    }
    std::string kickLabel(int a) const { return a == KA_LAST ? "Assistir @" + kickLast : "Digitar @streamer"; }
    // Capa tvg-logo (filmes, canais, series)
    std::string itemLogo(int i) const {
        const Playlist &p = app.list;
        if (i < 0 || i >= itemCount()) return "";
        if (searching()) {
            uint32_t hi = searchHits[i];
            if (tab == Tab::Movies && hi < p.movies.size()) return p.movies[hi].logo;
            if (tab == Tab::Channels && hi < p.channels.size()) return p.channels[hi].logo;
            if (tab == Tab::Series) return seriesLogo(seriesNameAt(hi));
            return "";
        }
        const Category *c = curCat();
        if (tab == Tab::Movies && c && i < (int)c->items.size())
            return p.movies[c->items[i]].logo;
        if (tab == Tab::Channels && c && i < (int)c->items.size())
            return p.channels[c->items[i]].logo;
        if (tab == Tab::Series && level == 0 && c && i < (int)c->series.size()) {
            return seriesLogo(c->series[i]);
        }
        if (tab == Tab::Series && level == 2) {
            auto it = p.seriesTree.find(curSeries);
            if (it == p.seriesTree.end()) return "";
            auto st = it->second.find(curSeason);
            if (st == it->second.end() || i >= (int)st->second.size()) return "";
            return st->second[i].logo;
        }
        return "";
    }

    std::string kickSub(int a) const {
        return a == KA_LAST ? "Abrir de novo o último canal pesquisado" : "Abre o teclado na tela e procura o canal ao vivo";
    }
    static std::string kickIcon(int a) { return a == KA_LAST ? ">" : "@"; }

    // Quantidade de itens e nome de UM item, sem copiar a lista inteira.
    // (Antes, cada aperto de botao e cada quadro copiavam o nome de TODOS os canais: com listas de milhares de canais isso travava.)
    static bool browsable(Tab t) { return t != Tab::Kick; }

    // Episodios sempre em ordem SxxE01, E02... (M3U costuma vir baguncado)
    static std::vector<Entry> sortedEps(const std::vector<Entry> &in) {
        std::vector<Entry> v = in;
        std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            if (a.episode != b.episode) return a.episode < b.episode;
            return a.name < b.name;
        });
        return v;
    }

    bool gridMode() const { return browsable(tab) && !(tab == Tab::Series && level > 0); }   // temporadas/episodios continuam em lista
    std::string seriesLogo(const std::string &name) const {
        auto it = app.list.seriesTree.find(name);
        if (it != app.list.seriesTree.end() && !it->second.empty()) {
            const auto &eps = it->second.begin()->second;
            if (!eps.empty() && !eps[0].logo.empty()) return eps[0].logo;
        }
        auto ri = app.list.seriesInfo.find(name);      // painel Xtream: a capa vem na lista de series
        return ri == app.list.seriesInfo.end() ? "" : ri->second.cover;
    }
    // pede as capas da tela atual (so quando algo mudou) e das proximas paginas, para ja estarem prontas
    void wantCovers(int firstVisible, int visibleCount) {
        if (!coverBatch) return;
        const Playlist &p = app.list;
        int n = itemCount();
        std::string key = std::to_string((int)tab) + "|" + std::to_string(catCur[(int)tab]) + "|" + std::to_string(level) + "|" + curSeries + "|" +
                          std::to_string(curSeason) + "|" + std::to_string(firstVisible) + "|" + std::to_string(p.channels.size() + p.movies.size() * 7 + p.seriesTree.size() * 13);
        if (key == lastBatchKey) return;
        lastBatchKey = key;
        std::vector<std::string> front, back;
        for (int i = firstVisible; i < firstVisible + visibleCount && i < n; i++) front.push_back(itemLogo(i));
        int per = visibleCount;
        for (int i = firstVisible + visibleCount; i < firstVisible + visibleCount * 3 && i < n; i++) back.push_back(itemLogo(i));   // 2 paginas adiante
        for (int i = std::max(0, firstVisible - per); i < firstVisible; i++) back.push_back(itemLogo(i));                           // e a anterior
        coverBatch(front, back);
    }
    // Todas as capas da lista, em ordem de importancia (as 3 abas intercaladas: primeira categoria de cada uma primeiro).
    // O baixador de fundo guarda isso no disco; depois disso abrir qualquer aba mostra as capas na hora.
    void backgroundCovers() {
        if (!coverBackground) return;
        const Playlist &p = app.list;
        std::string key = std::to_string(p.channels.size()) + "|" + std::to_string(p.movies.size()) + "|" + std::to_string(p.seriesTree.size());
        if (key == lastBgKey) return;
        lastBgKey = key;
        // So as primeiras capas de cada aba (as outras sao baixadas quando aparecem na tela). Antes copiava o endereco de TODAS as
        // capas (centenas de milhares de textos, varias copias) e isso estourava a memoria assim que a lista terminava de carregar.
        // A primeira tela de cada uma das primeiras categorias (abrir uma categoria mostra as capas na hora); o resto baixa ao aparecer.
        const size_t kPerCat = (size_t)(kCols * kGridRows), kMaxCats = 14;
        std::vector<std::string> mv, sr, ch;
        { size_t k = 0; for (const auto &c : p.movieCats) { if (k++ >= kMaxCats) break; for (size_t j = 0; j < c.items.size() && j < kPerCat; j++) mv.push_back(p.movies[c.items[j]].logo); } }
        { size_t k = 0; for (const auto &c : p.seriesCats) { if (k++ >= kMaxCats) break; for (size_t j = 0; j < c.series.size() && j < kPerCat; j++) sr.push_back(seriesLogo(c.series[j])); } }
        { size_t k = 0; for (const auto &c : p.channelCats) { if (k++ >= kMaxCats) break; for (size_t j = 0; j < c.items.size() && j < kPerCat; j++) ch.push_back(p.channels[c.items[j]].logo); } }
        std::vector<std::string> all;
        all.reserve(mv.size() + sr.size() + ch.size());
        for (size_t i = 0; i < std::max(mv.size(), std::max(sr.size(), ch.size())); i++) {   // intercala as 3 listas
            if (i < mv.size()) all.push_back(std::move(mv[i]));
            if (i < sr.size()) all.push_back(std::move(sr[i]));
            if (i < ch.size()) all.push_back(std::move(ch[i]));
        }
        coverBackground(std::move(all));
    }
    // Inicio: ja baixa as primeiras capas de cada aba (quando a lista carrega), para abrir tudo com as capas prontas
    // Capas das primeiras linhas de cada aba (carregam antes de abrir a aba). 'key' muda quando a lista muda.
    std::string homeCoverKey() const {
        const Playlist &p = app.list;
        return "H|" + std::to_string(p.channels.size()) + "|" + std::to_string(p.movies.size()) + "|" + std::to_string(p.seriesTree.size());
    }
    std::vector<std::string> homeCoverList() const {
        const Playlist &p = app.list;
        std::vector<std::string> back;
        const int per = kCols * kGridRows;
        if (!p.movieCats.empty()) for (size_t i = 0; i < p.movieCats[0].items.size() && (int)i < per; i++) back.push_back(p.movies[p.movieCats[0].items[i]].logo);
        if (!p.seriesCats.empty()) for (size_t i = 0; i < p.seriesCats[0].series.size() && (int)i < per; i++) back.push_back(seriesLogo(p.seriesCats[0].series[i]));
        if (!p.channelCats.empty()) for (size_t i = 0; i < p.channelCats[0].items.size() && (int)i < per; i++) back.push_back(p.channels[p.channelCats[0].items[i]].logo);
        return back;
    }
    const std::vector<Category> &cats(Tab t) const {
        const Playlist &p = app.list;
        return t == Tab::Channels ? p.channelCats : t == Tab::Movies ? p.movieCats : p.seriesCats;
    }
    int catCount() const { return browsable(tab) ? (int)cats(tab).size() : 0; }
    const Category *curCat() const {
        if (!browsable(tab)) return nullptr;
        const auto &v = cats(tab); int c = catCur[(int)tab];
        return c >= 0 && c < (int)v.size() ? &v[c] : nullptr;
    }
    std::string catLabel(int i) const { const auto &v = cats(tab); return i >= 0 && i < (int)v.size() ? v[i].name : ""; }
    size_t catSize(int i) const { const auto &v = cats(tab); return i >= 0 && i < (int)v.size() ? v[i].size() : 0; }


    static bool nameMatch(const std::string &name, const std::string &q) {
        if (q.empty()) return true;
        auto low = [](unsigned char c) -> char {
            return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : (char)c;
        };
        if (q.size() > name.size()) return false;
        for (size_t i = 0; i + q.size() <= name.size(); i++) {
            bool ok = true;
            for (size_t j = 0; j < q.size(); j++)
                if (low((unsigned char)name[i + j]) != low((unsigned char)q[j])) { ok = false; break; }
            if (ok) return true;
        }
        return false;
    }
    bool searching() const { return !searchQ.empty() && browsable(tab); }
    std::string seriesNameAt(uint32_t flatIdx) const {
        uint32_t idx = 0;
        if (app.list.xtream) {                              // painel: busca nas series das categorias ja abertas
            for (const auto &kv : app.list.seriesInfo) { if (idx == flatIdx) return kv.first; idx++; }
            return "";
        }
        for (const auto &kv : app.list.seriesTree) {
            if (idx == flatIdx) return kv.first;
            idx++;
        }
        return "";
    }
    void clearSearch() {
        searchQ.clear(); searchHits.clear();
        int t = (int)tab; cursor[t] = scroll[t] = 0;
    }
    void rebuildSearch() {
        searchHits.clear();
        if (searchQ.empty()) return;
        const Playlist &p = app.list;
        if (tab == Tab::Movies) {
            for (uint32_t i = 0; i < (uint32_t)p.movies.size(); i++)
                if (nameMatch(p.movies[i].name, searchQ)) searchHits.push_back(i);
            // prioriza dublado / portugues e URL com extensao de video
            std::sort(searchHits.begin(), searchHits.end(), [&](uint32_t a, uint32_t b) {
                auto score = [&](uint32_t i) {
                    const Entry &e = p.movies[i];
                    int s = 0;
                    std::string n = e.name; for (char &c : n) c = (char)tolower((unsigned char)c);
                    if (n.find("dublado") != std::string::npos || n.find("portug") != std::string::npos ||
                        n.find("pt-br") != std::string::npos || n.find("ptbr") != std::string::npos) s += 5;
                    if (n.find("legendado") != std::string::npos) s += 1;
                    if (n.find("english") != std::string::npos || n.find("[en]") != std::string::npos) s -= 3;
                    std::string u = e.url; for (char &c : u) c = (char)tolower((unsigned char)c);
                    if (u.find(".mp4") != std::string::npos || u.find(".mkv") != std::string::npos) s += 2;
                    if (e.url.empty()) s -= 10;
                    return s;
                };
                int sa = score(a), sb = score(b);
                if (sa != sb) return sa > sb;
                return p.movies[a].name < p.movies[b].name;
            });
        } else if (tab == Tab::Channels) {
            for (uint32_t i = 0; i < (uint32_t)p.channels.size(); i++)
                if (nameMatch(p.channels[i].name, searchQ)) searchHits.push_back(i);
        } else if (tab == Tab::Series) {
            uint32_t idx = 0;
            if (p.xtream) { for (const auto &kv : p.seriesInfo) { if (nameMatch(kv.first, searchQ)) searchHits.push_back(idx); idx++; } }
            else for (const auto &kv : p.seriesTree) {
                if (nameMatch(kv.first, searchQ)) searchHits.push_back(idx);
                idx++;
            }
        }
        level = 0; inItems = true;
        int t = (int)tab; cursor[t] = scroll[t] = 0;
    }
    void startSearch() {
        textKind = 3;                                  // funciona de qualquer tela: no Inicio/Kick procura em Filmes, Series e Canais
        app.pf.requestText(searchQ.empty() ? "Buscar..." : searchQ);
    }

    // Itens da direita (da categoria escolhida), sem copiar a lista inteira.
    int itemCount() const {
        const Playlist &p = app.list;
        if (tab == Tab::Kick) return (int)kickActions().size();
        if (searching()) return (int)searchHits.size();
        const Category *c = curCat();
        if (!c) return 0;
        if (tab != Tab::Series) return (int)c->items.size();
        if (level == 0) return (int)c->series.size();
        auto it = p.seriesTree.find(curSeries);
        if (it == p.seriesTree.end()) return 0;
        if (level == 1) return (int)it->second.size();
        auto st = it->second.find(curSeason);
        return st == it->second.end() ? 0 : (int)st->second.size();
    }
    std::string itemLabel(int i) const {
        const Playlist &p = app.list;
        if (i < 0 || i >= itemCount()) return "";
        if (tab == Tab::Kick) return kickLabel(kickActions()[i]);
        if (searching()) {
            uint32_t hi = searchHits[i];
            if (tab == Tab::Movies && hi < p.movies.size()) return p.movies[hi].name;
            if (tab == Tab::Channels && hi < p.channels.size()) return p.channels[hi].name;
            if (tab == Tab::Series) return seriesNameAt(hi);
            return "";
        }
        const Category *c = curCat();
        if (tab == Tab::Channels) return p.channels[c->items[i]].name;
        if (tab == Tab::Movies) return p.movies[c->items[i]].name;
        if (level == 0) return c->series[i];
        auto &seasons = p.seriesTree.find(curSeries)->second;
        if (level == 1) { auto it = seasons.begin(); std::advance(it, i); int sn = it->first; return "Temporada " + std::to_string(sn < 1 ? 1 : sn); }
        auto eps = sortedEps(seasons.find(curSeason)->second);
        if (i < 0 || i >= (int)eps.size()) return "";
        const Entry &e = eps[i];
        return e.episode ? "Ep " + std::to_string(e.episode) + " - " + e.name : e.name;
    }
    std::vector<std::string> labels() const {         // so para testes/compatibilidade (copia tudo)
        std::vector<std::string> v;
        for (int i = 0, n = itemCount(); i < n; i++) v.push_back(itemLabel(i));
        return v;
    }

    // texto confirmado no teclado
    void onText(const std::string &raw) {
        int kind = textKind; textKind = 0;
        if (kind == 2) { setM3U(raw); return; }
        if (kind == 3) {
            size_t a = raw.find_first_not_of(" \t\r\n");
            size_t b = raw.find_last_not_of(" \t\r\n");
            searchQ = a == std::string::npos ? "" : raw.substr(a, b - a + 1);
            if (searchQ.empty()) { clearSearch(); app.say("Busca limpa"); return; }
            searchEverywhere();
            return;
        }
        kickLast = kickSlug(raw);
        if (kickLast.empty() && !raw.empty()) { app.say("@ invalido"); return; }
        if (!kickLast.empty()) { app.pf.saveSetting("kick", kickLast); app.watchKick(kickLast); }
    }
    // Conta os resultados em Filmes, Series e Canais; fica na aba atual se ela tem resultado (ou se ja estava numa lista), senao vai para a que tem mais.
    void searchEverywhere() {
        static const Tab ts[3] = {Tab::Movies, Tab::Series, Tab::Channels};
        static const char *nm[3] = {"Filmes", "Séries", "Canais"};
        size_t cnt[3] = {0, 0, 0};
        Tab keep = tab;
        for (int i = 0; i < 3; i++) { tab = ts[i]; rebuildSearch(); cnt[i] = searchHits.size(); }
        int best = -1, cur = -1;
        for (int i = 0; i < 3; i++) { if (ts[i] == keep) cur = i; if (cnt[i] > 0 && (best < 0 || cnt[i] > cnt[best])) best = i; }
        int pick = (screen == Screen::List && cur >= 0) ? cur : (best >= 0 ? best : (cur >= 0 ? cur : 0));
        tab = ts[pick]; screen = Screen::List; rebuildSearch();
        std::string m = "Busca \"" + searchQ + "\": ";
        for (int i = 0; i < 3; i++) m += std::string(i ? ", " : "") + nm[i] + " " + std::to_string(cnt[i]);
        if (cnt[0] + cnt[1] + cnt[2] > 0) m += "  (L1/R1 troca de aba)";
        else if (app.xt.active) m += "  (so nas categorias ja abertas)";
        app.say(m);
    }
    // L1/R1 durante uma busca: muda de aba (Filmes/Series/Canais) mantendo o texto buscado
    void switchTabKeepSearch(int dir) {
        static const Tab ts[3] = {Tab::Movies, Tab::Series, Tab::Channels};
        int cur = 0; for (int i = 0; i < 3; i++) if (ts[i] == tab) cur = i;
        tab = ts[(cur + (dir > 0 ? 1 : 2)) % 3];
        rebuildSearch();
        app.say(std::to_string(searchHits.size()) + " resultado(s) para \"" + searchQ + "\"");
    }
    std::string textInitial() const {
        if (textKind == 2) return m3uUrl;
        if (textKind == 3) return searchQ;
        return kickLast;
    }

    void setM3U(std::string url) {
        size_t a = url.find_first_not_of(" \t\r\n"), b = url.find_last_not_of(" \t\r\n");
        url = a == std::string::npos ? "" : url.substr(a, b - a + 1);
        if (url.empty()) {                       // campo vazio = remover a lista
            m3uUrl.clear(); app.list = Playlist(); app.clearXtream(); app.pf.saveSetting("m3u", ""); app.pf.cacheClear();
            resetBrowse();
            app.say("Lista M3U removida");
            return;
        }
        if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) url = "http://" + url;
        if (app.loadM3U(url)) {
            m3uUrl = url; app.pf.saveSetting("m3u", url);
            resetBrowse();
        }
    }

    // Todas as capas de filmes, series e canais (categoria por categoria). Chamar de novo enquanto baixa = cancela.
    void bulkCovers() {
        if (coverBulkActive && coverBulkActive()) { if (coverBulkStop) coverBulkStop(); app.say("Download de capas cancelado (as ja baixadas ficam salvas)"); return; }
        if (!coverBulk) return;
        const Playlist &p = app.list;
        std::vector<std::string> all;
        for (const auto &c : p.movieCats) for (uint32_t i : c.items) all.push_back(p.movies[i].logo);
        for (const auto &c : p.seriesCats) for (const auto &n : c.series) all.push_back(seriesLogo(n));
        for (const auto &c : p.channelCats) for (uint32_t i : c.items) all.push_back(p.channels[i].logo);
        if (all.empty()) { app.say("Carregue uma lista M3U primeiro"); return; }
        size_t n = all.size();
        coverBulk(std::move(all));
        app.say("Baixando capas de todas as categorias (" + std::to_string(n) + " itens)...  TRIANGULO cancela");
    }

    // categoria de um filme / serie (quando aberto pela busca)
    std::string movieCategoryOf(uint32_t idx) const {
        for (const auto &c : app.list.movieCats) for (uint32_t i : c.items) if (i == idx) return c.name;
        return "";
    }
    std::string seriesCategoryOf(const std::string &name) const {
        for (const auto &c : app.list.seriesCats) for (const auto &n : c.series) if (n == name) return c.name;
        return "";
    }

    void handle(Input in) { handleImpl(in); }
    void handleImpl(Input in) {
        if (screen == Screen::Detail) { handleDetail(in); return; }
        if (screen == Screen::Donate) { if (in == Input::Back || in == Input::Confirm) screen = Screen::Home; return; }
        if (in == Input::Extra) { startSearch(); return; }
        if (screen == Screen::Help) {
            if (in == Input::Back || in == Input::Confirm) { screen = Screen::Home; return; }
            if (in == Input::Up) helpScroll = std::max(0, helpScroll - 1);
            if (in == Input::Down) helpScroll = std::min(12, helpScroll + 1);
            return;
        }
        if (screen == Screen::Home) { handleHome(in); return; }
        int t = (int)tab;
        switch (in) {
        case Input::PrevTab: if (searching()) { switchTabKeepSearch(-1); return; } openTab(cycleTab(tab, -1)); return;
        case Input::NextTab: if (searching()) { switchTabKeepSearch(1); return; } openTab(cycleTab(tab, 1)); return;
        default: break;
        }
        if (!browsable(tab)) { handleItems(in); return; }
        if (searching()) { inItems = true; handleItems(in); return; }
        if (inItems) {
            if (in == Input::Left && !(gridMode() && cursor[t] % kCols != 0)) { goBack(); return; }   // 1a coluna: volta para as categorias (ou um nivel acima em Series)
            handleItems(in); return;
        }
        // ---- foco na barra de categorias ----
        int n = catCount(), before = catCur[t];
        switch (in) {
        case Input::Up: catCur[t] = std::max(0, catCur[t] - 1); break;
        case Input::Down: catCur[t] = std::min(std::max(0, n - 1), catCur[t] + 1); break;
        case Input::PageUp: catCur[t] = std::max(0, catCur[t] - kRows); break;
        case Input::PageDown: catCur[t] = std::min(std::max(0, n - 1), catCur[t] + kRows); break;
        case Input::Right: case Input::Confirm:
            if (curCat() && curCat()->unloaded()) {                       // painel Xtream: baixa so os itens desta categoria
                if (!app.ensureCategory(tab, catCur[t])) return;
                cursor[t] = scroll[t] = 0;
            }
            if (itemCount() > 0) inItems = true;
            return;
        case Input::Back: screen = Screen::Home; return;
        default: return;
        }
        if (catCur[t] != before) { cursor[t] = scroll[t] = 0; level = 0; wantAutoOpen(); }   // outra categoria: itens desde o comeco
        if (catCur[t] < catScroll[t]) catScroll[t] = catCur[t];
        if (catCur[t] >= catScroll[t] + kRows) catScroll[t] = catCur[t] - kRows + 1;
    }

    // Painel Xtream: a categoria sob o cursor abre sozinha depois que o cursor para nela (~0,3 s), sem apertar X.
    // Chamar uma vez por quadro (laco principal). true = carregou algo: redesenhar.
    bool autoOpenTick() {
        if (!autoWant) return false;
        if (screen != Screen::List || inItems || searching() || !browsable(tab) || !app.xt.active) { autoWant = false; return false; }
        if (++autoFrames < 18) return false;
        autoWant = false;
        const Category *c = curCat();
        if (!c || !c->unloaded()) return false;
        int t = (int)tab;
        app.ensureCategory(tab, catCur[t]);
        cursor[t] = scroll[t] = 0;
        return true;
    }

    // ---------- Desenho ----------
    std::function<void(const char *)> dbg;         // diagnostico: o PS4 grava cada etapa do 1o desenho (para achar onde cai)

    void render(Renderer &r) {
        if (dbg) dbg("ui-render-ini");
        backgroundCovers();
        if (dbg) dbg("ui-capas-ok");
        uiHeader(r, timeText, dateText);
        drawAmbient(r);                                     // particulas e brilho dos riscos de neon (atras do conteudo)
        if (dbg) dbg("ui-header-ok");
        if (screen == Screen::Home) renderHome(r);          // o Inicio anima a propria entrada, bloco por bloco
        else {
            const float age = (float)(animT - fadeAt), e = uiEaseOut(age / 0.34f);
            auto body = [&](Renderer &q) {
                if (screen == Screen::Help) renderHelp(q); else if (screen == Screen::Donate) renderDonate(q);
                else if (screen == Screen::Detail) renderDetail(q); else renderList(q);
            };
            if (e < 1.f) { UiFx fx(r, (int)((1.f - e) * 56.f * (float)trDir), 0, e); body(fx); }     // desliza e aparece
            else body(r);
        }
        if (dbg) dbg("ui-corpo-ok");
        if (screen != Screen::Detail) renderFooter(r);      // a pagina de detalhes desenha o proprio rodape
        if (dbg) dbg("ui-rodape-ok");
    }

private:

    bool autoWant = false; int autoFrames = 0;
    void wantAutoOpen() { autoFrames = 0; autoWant = app.xt.active && browsable(tab) && curCat() && curCat()->unloaded(); }
    void openTab(Tab t) { tab = t; screen = t == Tab::Kick ? Screen::Kick : Screen::List; level = 0; inItems = false; searchQ.clear(); searchHits.clear(); wantAutoOpen(); }
    void resetBrowse() {
        for (int i = 0; i < 4; i++) cursor[i] = scroll[i] = catCur[i] = catScroll[i] = 0;
        level = 0; inItems = false;
    }
    // O (ou seta esquerda): Series sobe um nivel; depois os itens voltam para as categorias; nas categorias volta ao inicio.
    void goBack() {
        int t = (int)tab;
        if (searching()) { clearSearch(); app.say("Busca limpa"); return; }
        if (screen == Screen::Kick || !browsable(tab)) { screen = Screen::Home; return; }
        if (!inItems) { screen = Screen::Home; return; }
        if (tab == Tab::Series && level > 0) { level--; cursor[t] = 0; scroll[t] = 0; }
        else inItems = false;
    }
    void handleItems(Input in) {
        int t = (int)tab, n = itemCount();
        if (gridMode()) {                                  // grade de posters: setas andam de 1 em 1 (esq/dir) e de linha em linha (cima/baixo)
            int cur = cursor[t];
            switch (in) {
            case Input::Up: if (cur >= kCols) cur -= kCols; break;
            case Input::Down: if (cur + kCols < n) cur += kCols; else if (cur / kCols < (n - 1) / kCols) cur = n - 1; break;
            case Input::Left: if (cur % kCols > 0) cur--; break;
            case Input::Right: if (cur + 1 < n && cur % kCols < kCols - 1) cur++; break;
            case Input::PageUp: cur = std::max(0, cur - kCols * kGridRows); break;
            case Input::PageDown: cur = std::min(std::max(0, n - 1), cur + kCols * kGridRows); break;
            case Input::Back: goBack(); return;
            case Input::Confirm: confirm(n); return;
            default: return;
            }
            cursor[t] = cur;
            int row = cur / kCols;
            if (row < scroll[t]) scroll[t] = row;
            if (row >= scroll[t] + kGridRows) scroll[t] = row - kGridRows + 1;
            return;
        }
        switch (in) {
        case Input::Up: cursor[t] = std::max(0, cursor[t] - 1); break;
        case Input::Down: cursor[t] = std::min(std::max(0, n - 1), cursor[t] + 1); break;
        case Input::PageUp: cursor[t] = std::max(0, cursor[t] - kRows); break;
        case Input::PageDown: cursor[t] = std::min(std::max(0, n - 1), cursor[t] + kRows); break;
        case Input::Left: case Input::Right: return;
        case Input::Back: goBack(); return;
        case Input::Confirm: confirm(n); return;
        default: return;
        }
        if (cursor[t] < scroll[t]) scroll[t] = cursor[t];
        if (cursor[t] >= scroll[t] + kRows) scroll[t] = cursor[t] - kRows + 1;
    }
    void requestM3U() { textKind = 2; app.pf.requestText("Endereço da lista M3U"); }
    // Botao ATUALIZAR: baixa a lista de novo (painel Xtream: reconecta e atualiza as categorias).
    void requestReload() {
        if (m3uUrl.empty()) { app.say("Nenhuma lista configurada neste app"); return; }
        reloadRequested = true;
        app.say("Atualizando a lista...");
    }

    void handleHome(Input in) {
        switch (in) {
        case Input::Up: homeRow = std::max(0, homeRow - 1); break;
        case Input::Down: homeRow = std::min(3, homeRow + 1); break;
        case Input::Left:
            if (homeRow == 0) heroMove(-1);
            else if (homeRow == 1) homeCol = std::max(0, homeCol - 1);
            else if (homeRow == 3) homeCol3 = 0;
            break;
        case Input::Right:
            if (homeRow == 0) heroMove(1);
            else if (homeRow == 1) homeCol = std::min(3, homeCol + 1);
            else if (homeRow == 3 && hasDonate()) homeCol3 = 1;
            break;
        case Input::PrevTab: if (homeRow == 0) heroMove(-1); else if (homeRow == 1) homeCol = std::max(0, homeCol - 1); break;
        case Input::NextTab: if (homeRow == 0) heroMove(1); else if (homeRow == 1) homeCol = std::min(3, homeCol + 1); break;
        case Input::Confirm:
            if (homeRow == 0) heroOpen();
            else if (homeRow == 1) openTab(tileTab(homeCol));
            else if (homeRow == 2) { if (listLocked()) requestReload(); else requestM3U(); }
            else if (homeCol3 == 1 && hasDonate()) screen = Screen::Donate;
            else { screen = Screen::Help; helpScroll = 0; }
            break;
        default: break;
        }
        if (homeRow != 3 || !hasDonate()) homeCol3 = 0;
    }

    std::string noListMsg() const { return listLocked() ? "A lista ainda não carregou" : "Nenhuma lista M3U adicionada"; }
    std::string noListHint() const { return listLocked() ? "Volte ao início e aperte ATUALIZAR na lista" : "Volte ao início e use LISTA M3U para adicionar a sua"; }
    std::string emptyMsg() const {
        const Category *c = curCat();
        return c && c->unloaded() ? "Aperte X para abrir a categoria" : "Nada por aqui ainda";
    }
    std::string listSummary() const {
        const Playlist &p = app.list;
        if (p.xtream) return "Painel IPTV  |  " + std::to_string(p.channelCats.size()) + " categorias de canais  |  " + std::to_string(p.movieCats.size()) + " de filmes  |  " + std::to_string(p.seriesCats.size()) + " de séries";
        if (p.channels.empty() && p.movies.empty() && p.seriesTree.empty()) {
            if (listLocked()) return "Lista ainda não carregada - aperte X para atualizar";
            return m3uUrl.empty() ? "Adicione sua lista de IPTV" : "Lista sem itens - X para trocar";
        }
        return std::to_string(p.channels.size()) + " canais  |  " + std::to_string(p.movies.size()) + " filmes  |  " + std::to_string(p.seriesTree.size()) + " séries";
    }


    // ---------- Tela "Comandos e ajuda": cartoes por assunto, cada botao do controle em um "chip" ----------
    struct HelpItem { std::vector<std::string> keys; const char *desc; };
    // chip de botao (retangulo arredondado com o nome); devolve a largura. △ e □ sao desenhados (a fonte nao tem esses caracteres)
    static int helpChip(Renderer &r, int x, int cy, const std::string &k) {
        using namespace theme;
        int w = (k == "△" || k == "□") ? 56 : r.textWidth(k, 20) + 30;
        if (w < 56) w = 56;
        r.roundRect(x, cy - 20, w, 40, 12, KEY);
        if (k == "△") uiGlyphTriangle(r, x + w / 2, cy, 20, TEXT);
        else if (k == "□") { r.roundRect(x + w / 2 - 10, cy - 10, 20, 20, 4, TEXT); r.roundRect(x + w / 2 - 7, cy - 7, 14, 14, 2, KEY); }
        else uiCenter(r, x + w / 2, cy, k, TEXT, 20, true);
        return w;
    }
    static std::vector<std::string> helpWrap(const Renderer &r, const std::string &t, int size, int maxW) {
        std::vector<std::string> out; std::string cur;
        size_t i = 0;
        while (i < t.size()) {
            size_t j = t.find(' ', i); if (j == std::string::npos) j = t.size();
            std::string w = t.substr(i, j - i);
            std::string tryS = cur.empty() ? w : cur + " " + w;
            if (!cur.empty() && r.textWidth(tryS, size) > maxW) { out.push_back(cur); cur = w; } else cur = tryS;
            i = j + 1;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    // uma linha: chips dos botoes a esquerda, explicacao (ate 2 linhas) a direita
    static void helpRow(Renderer &r, int x, int cy, int keyCol, int descW, const HelpItem &it) {
        using namespace theme;
        int kx = x;
        for (size_t k = 0; k < it.keys.size(); k++) {
            if (k) { uiText(r, kx + 4, cy - 12, "/", DIM, 22); kx += 22; }
            kx += helpChip(r, kx, cy, it.keys[k]);
        }
        auto ls = helpWrap(r, it.desc, 21, descW);
        if (ls.size() > 2) ls.resize(2);
        int ty = cy - (int)ls.size() * 13;
        for (size_t i = 0; i < ls.size(); i++) uiText(r, x + keyCol, ty + (int)i * 26, ls[i], SOFT, 21);
    }
    static void helpCard(Renderer &r, int x, int y, int w, int h, const char *title, const std::vector<HelpItem> &items, const char *note) {
        using namespace theme;
        uiFrame(r, x, y, w, h, 24, 2, CARD_LINE, CARD);
        r.roundRect(x + 30, y + 30, 6, 34, 3, GREEN);
        uiHeavy(r, x + 52, y + 30, title, TEXT, 28);
        const int keyCol = 196, descW = w - 30 - keyCol - 28;
        int cy = y + 118;
        for (const auto &it : items) { helpRow(r, x + 30, cy, keyCol, descW, it); cy += 76; }
        if (note && note[0]) {
            auto ls = helpWrap(r, note, 19, w - 60);
            if (ls.size() > 3) ls.resize(3);
            int ny = y + h - 26 - (int)ls.size() * 25;
            r.fillRect(x + 30, ny - 14, w - 60, 2, 0xFF242424);
            for (size_t i = 0; i < ls.size(); i++) uiText(r, x + 30, ny + (int)i * 25, ls[i], MUTED, 19);
        }
    }

    void renderHelp(Renderer &r) {
        using namespace theme;
        const int W = r.width(), H = r.height();
        uiBackground(r);
        uiKMark(r, 73, 52, 40, 41, GREEN);
        uiHeavy(r, 130, 58, "COMANDOS E AJUDA", TEXT, 36);
        uiText(r, 130, 104, "Todos os botões do app. Aperte O ou X para voltar.", MUTED, 20);
        r.fillRect(73, 142, W - 146, 2, 0xFF1C1C1C);

        const int gx = 73, gw = W - 146, gap = 24, cw = (gw - 2 * gap) / 3, cy0 = 168, ch = 560;
        helpCard(r, gx, cy0, cw, ch, "NO MENU", {
            {{"D-pad"}, "Navegar entre os itens"},
            {{"X"}, "Selecionar ou abrir"},
            {{"O"}, "Voltar"},
            {{"△"}, "Buscar filmes, séries e canais"},
            {{"L1", "R1"}, "Trocar de aba (ou de temporada numa série)"},
            {{"L2", "R2"}, "Subir / descer uma página da lista"},
        }, nullptr);
        helpCard(r, gx + cw + gap, cy0, cw, ch, "FILME E SÉRIE", {
            {{"OPTIONS"}, "Mostrar ou esconder a barra de tempo"},
            {{"□"}, "Ligar ou desligar o som"},
            {{"△"}, "Opções: proporção da tela, áudio e informações"},
            {{"O"}, "Sair e voltar ao menu"},
        }, "A barra de tempo aparece ao abrir, mostra o minuto atual e a duração, e some sozinha. Qualquer botão traz ela de volta.");
        helpCard(r, gx + 2 * (cw + gap), cy0, cw, ch, "AO VIVO: KICK E CANAIS", {
            {{"L1", "R1"}, "Canal anterior / próximo (nos canais)"},
            {{"OPTIONS"}, "Trocar a qualidade: Fluido, 720p ou 1080p"},
            {{"□"}, "Ligar ou desligar o som"},
            {{"△"}, "Opções: qualidade, proporção e áudio"},
            {{"O"}, "Sair da transmissão"},
        }, nullptr);

        // ---- teclado na tela ----
        {
            const int ky = cy0 + ch + 24, kh = 244;
            uiFrame(r, gx, ky, gw, kh, 24, 2, CARD_LINE, CARD);
            r.roundRect(gx + 30, ky + 30, 6, 34, 3, GREEN);
            uiHeavy(r, gx + 52, ky + 30, "TECLADO NA TELA", TEXT, 28);
            uiText(r, gx + 52 + r.textWidth("TECLADO NA TELA", 28) + 28, ky + 36, "usado para digitar @streamer, links e buscas", MUTED, 20);
            static const HelpItem ks[6] = {
                {{"X"}, "Digitar a tecla escolhida"},
                {{"□"}, "Apagar (segure para apagar rápido)"},
                {{"△"}, "Espaço"},
                {{"L1"}, "Letras maiúsculas"},
                {{"R1"}, "Confirmar o texto"},
                {{"O"}, "Cancelar e voltar"},
            };
            const int colW = (gw - 60) / 3;
            for (int i = 0; i < 6; i++) {
                int col = i % 3, row = i / 3;
                helpRow(r, gx + 30 + col * colW, ky + 118 + row * 72, 84, colW - 84 - 20, ks[i]);
            }
        }
    }

    // Inicio (homeview.hpp): carrossel, atalhos, lista e rodape de comandos/apoiar
    void renderHome(Renderer &r);
    void heroTick(double t);
    void heroMove(int d);
    void heroOpen();
    void heroRebuild();
    void drawAmbient(Renderer &r);
    void drawHeroCard(Renderer &r, int slide, int x, int y, int w, int h, float center, bool focus);
    void drawPeek(Renderer &r, int slide, int x, int y, int w, int h, float dimA, int dir);
    bool heroArt(Renderer &r, const std::string &imgUrl, int vw, int vh, int x, int y, int w, int h, int rt, int rb);
    void heroApplyInfo();
    void requestHomeCovers();
    static void heroChevron(Renderer &r, int cx, int cy, int dir, unsigned c);

    // Tela "Apoiar": QR Code do LivePix (ou da pagina de doacao) + explicacao
    void renderDonate(Renderer &r) {
        using namespace theme;
        const int W = r.width();
        uiKMark(r, 73, 146, 40, 41, GREEN);
        uiHeavy(r, 130, 152, "APOIE O KICK IPTV", TEXT, 36);
        r.fillRect(73, 196, W - 146, 2, 0xFF1C1C1C);
        QrCode q;
        const bool ok = qrEncode(donateUrl, q);
        const int quiet = 4, box = 640, bx = 150, by = 260;
        uiGlow(r, bx, by, box, box, 28, GREEN, 18, pk(110), false);
        r.roundRect(bx, by, box, box, 28, 0xFFFFFFFF);
        if (ok) {
            int scale = box / (q.size + 2 * quiet);                // 4 modulos de margem branca (leitores pedem isso)
            int qs = scale * q.size, ox = bx + (box - qs) / 2, oy = by + (box - qs) / 2;
            for (int y = 0; y < q.size; y++)
                for (int x = 0; x < q.size;) {                     // junta modulos escuros vizinhos: menos retangulos
                    if (!q.dark(x, y)) { x++; continue; }
                    int e = x; while (e < q.size && q.dark(e, y)) e++;
                    r.fillRect(ox + x * scale, oy + y * scale, (e - x) * scale, scale, 0xFF000000);
                    x = e;
                }
        } else uiCenterHeavy(r, bx + box / 2, by + box / 2 - 16, "QR indisponivel", 0xFF000000, 32);
        const int tx = 880;
        uiHeavy(r, tx, 268, "Gostou do app?", GREEN, 44);
        uiText(r, tx, 330, "Ajude a manter e melhorar o Kick IPTV.", TEXT, 28);
        uiText(r, tx, 372, "A contribuicao e opcional, mas ajuda muito. Obrigado!", SOFT, 24);
        static const char *steps[3] = {"1.  Abra a camera do celular", "2.  Aponte para o QR Code ao lado", "3.  Escolha o valor e pague com Pix"};
        for (int i = 0; i < 3; i++) {
            r.roundRect(tx, 450 + i * 74, 56, 56, 28, GREEN);
            uiCenterHeavy(r, tx + 28, 450 + i * 74 + 28, std::to_string(i + 1), BLACK, 28);
            uiText(r, tx + 82, 450 + i * 74 + 14, steps[i] + 4, TEXT, 26, true);
        }
        uiText(r, tx, 692, "Ou acesse:", MUTED, 22);
        uiText(r, tx, 726, uiFit(r, donateUrl, 28, W - tx - 100), GREEN, 28, true);
        if (!donatePix.empty()) {
            uiText(r, tx, 790, "Chave Pix:", MUTED, 22);
            uiText(r, tx, 824, uiFit(r, donatePix, 28, W - tx - 100), TEXT, 28, true);
        }
    }

    // Filmes / Series / Canais: barra de categorias (esquerda) + itens da categoria (direita)
    void renderBrowse(Renderer &r) {
        using namespace theme;
        const int W = r.width();
        int t = (int)tab, n = itemCount(), nc = catCount();
        const Category *cat = curCat();
        // titulo: FILMES > Comedia > ...
        std::vector<std::string> parts;
        parts.push_back(tab == Tab::Channels ? "CANAIS" : tab == Tab::Movies ? "FILMES" : "SÉRIES");
        if (searching()) parts.push_back("Busca: " + searchQ);
        else if (cat) parts.push_back(cat->name);
        if (tab == Tab::Series && inItems && level >= 1) parts.push_back(curSeries);
        if (tab == Tab::Series && inItems && level >= 2) parts.push_back("Temporada " + std::to_string(curSeason));
        uiKMark(r, 73, 146, 40, 41, GREEN);
        {
            int tx = 130, maxX = W - 360;
            for (size_t i = 0; i < parts.size(); i++) {
                bool last = i + 1 == parts.size();
                std::string seg = uiFit(r, parts[i], 36, std::max(80, (maxX - tx) / (int)(parts.size() - i)));
                uiHeavy(r, tx, 152, seg, last && parts.size() > 1 ? GREEN : TEXT, 36);
                tx += r.textWidth(seg, 36);
                if (!last) { uiText(r, tx + 16, 152, ">", MUTED, 36, true); tx += 16 + r.textWidth(">", 36) + 16; }
            }
        }
        r.fillRect(73, 196, W - 146, 2, 0xFF1C1C1C);
        { std::string c = std::to_string(n) + (n == 1 ? " item" : " itens");
          uiText(r, W - 73 - r.textWidth(c, 22), 158, c, MUTED, 22); }

        const int y0 = 218, rh = 64, pitch = 74;
        // ---- categorias ----
        const int cx = 73, cw = 430;
        for (int row = 0; row < kRows; row++) {
            int idx = catScroll[t] + row;
            if (idx >= nc) break;
            int y = y0 + row * pitch;
            bool cur = idx == catCur[t], sel = cur && !inItems;
            if (sel) uiGlow(r, cx, y, cw, rh, 18, GREEN, 10, pk(110), false);
            uiFrame(r, cx, y, cw, rh, 18, sel ? 3 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            if (cur && !sel) r.roundRect(cx + 10, y + 14, 5, rh - 28, 2, GREEN);        // categoria aberta (foco nos itens)
            std::string cnt = cats(tab)[idx].unloaded() ? std::string() : std::to_string(catSize(idx));
            int cwid = r.textWidth(cnt, 20);
            uiText(r, cx + cw - 22 - cwid, y + (rh - 20) / 2, cnt, sel ? SOFT : MUTED, 20);
            unsigned tc = cur ? (sel ? TEXT : GREEN) : SOFT;
            uiText(r, cx + 30, y + (rh - 26) / 2, uiFit(r, catLabel(idx), 26, cw - 30 - 40 - cwid), tc, 26, cur);
        }
        if (nc > kRows) {
            int th = kRows * pitch - 10;
            r.roundRect(cx + cw + 8, y0, 6, th, 3, CARD2);
            int kh = std::max(40, th * kRows / nc);
            r.roundRect(cx + cw + 8, y0 + (th - kh) * catScroll[t] / std::max(1, nc - kRows), 6, kh, 3, GREEN_DK);
        }
        // ---- itens em GRADE de posters (Filmes, Series, Canais) ----
        if (gridMode()) {
            const int gx0 = cx + cw + 36, gw = W - 73 - 28 - gx0;
            const int pw = ImgW, ph = ImgH, gap = (gw - kCols * pw) / (kCols - 1), pitchY = ph + 40;
            wantCovers(scroll[t] * kCols, kCols * kGridRows);
            for (int row = 0; row < kGridRows; row++)
                for (int col = 0; col < kCols; col++) {
                    int idx = (scroll[t] + row) * kCols + col;
                    if (idx >= n) break;
                    int px = gx0 + col * (pw + gap), py = y0 + row * pitchY;
                    bool sel = inItems && idx == cursor[t];
                    const std::string label = itemLabel(idx);
                    if (sel) uiGlow(r, px - 4, py - 4, pw + 8, ph + 8, 14, GREEN, 12, pk(120), false);
                    if (sel) r.roundRect(px - 4, py - 4, pw + 8, ph + 8, 14, GREEN);      // moldura verde (fica atras do poster)
                    r.roundRect(px, py, pw, ph, 10, CHIP);
                    bool drew = false;
                    std::string logoUrl = itemLogo(idx);
                    if (!logoUrl.empty() && coverGet) {
                        const uint32_t *pxc = nullptr; int cw2 = 0, ch2 = 0;
                        if (coverGet(logoUrl, pxc, cw2, ch2) && pxc && cw2 > 0 && ch2 > 0) { r.blitImage(px, py, pw, ph, pxc, cw2, ch2); drew = true; }
                    }
                    if (!drew) {
                        std::string ic = "#";
                        for (unsigned char ch : label) if (std::isalnum(ch)) { ic = std::string(1, (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch)); break; }
                        uiCenterHeavy(r, px + pw / 2, py + ph / 2, ic, sel ? GREEN : MUTED, 56);
                    }
                    if (sel) {
                        int bx = px + pw - 34, by = py + ph - 34;                       // botao play
                        r.roundRect(bx - 22, by - 22, 44, 44, 22, GREEN);
                        for (int j = 0; j < 22; j++) { int d = 2 * j + 1 - 22; if (d < 0) d = -d; int ww = 18 * (22 - d) / 22; if (ww > 0) r.fillRect(bx - 6, by - 11 + j, ww, 1, BLACK); }
                    }
                    uiText(r, px, py + ph + 10, uiFit(r, label, 20, pw + gap - 6), sel ? TEXT : SOFT, 20, sel);
                }
            if (n > kCols * kGridRows) {
                int rows = (n + kCols - 1) / kCols, th = kGridRows * pitchY - 14;
                r.roundRect(W - 52, y0, 8, th, 4, CARD2);
                int kh = std::max(48, th * kGridRows / rows);
                r.roundRect(W - 52, y0 + (th - kh) * scroll[t] / std::max(1, rows - kGridRows), 8, kh, 4, GREEN_DK);
            }
            if (n == 0) {
                bool noList = app.list.empty();
                uiCenterHeavy(r, W / 2 + 200, y0 + 150, noList ? noListMsg() : emptyMsg(), MUTED, 32);
            }
            return;
        }
        wantCovers(scroll[t], kRows);
        // ---- itens ----
        const int x = cx + cw + 36, rw = W - 73 - 28 - x;
        for (int row = 0; row < kRows; row++) {
            int idx = scroll[t] + row;
            if (idx >= n) break;
            int y = y0 + row * pitch;
            bool sel = inItems && idx == cursor[t];
            if (sel) uiGlow(r, x, y, rw, rh, 18, GREEN, 10, pk(110), false);
            uiFrame(r, x, y, rw, rh, 18, sel ? 3 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            const std::string label = itemLabel(idx);
            std::string ic = "#";
            for (unsigned char ch : label) if (std::isalnum(ch)) { ic = std::string(1, (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch)); break; }
            // capa pequena (48x72 escalada para o quadrado) — leve e rapida
            int isW = 40, isH = rh - 16;
            int px = x + 12, py = y + 8;
            r.roundRect(px, py, isW, isH, 8, sel ? GREEN : CHIP);
            bool drew = false;
            std::string logoUrl = itemLogo(idx);
            if (!logoUrl.empty() && coverGet) {
                const uint32_t *pxc = nullptr; int cw = 0, ch = 0;
                if (coverGet(logoUrl, pxc, cw, ch) && pxc && cw > 0 && ch > 0) {
                    r.blitImage(px + 1, py + 1, isW - 2, isH - 2, pxc, cw, ch);
                    drew = true;
                }
            }
            if (!drew)
                uiCenterHeavy(r, px + isW / 2, py + isH / 2, ic, sel ? BLACK : MUTED, 22);
            int tx = px + isW + 16, textMax = rw - (tx - x) - 110;
            uiText(r, tx, y + (rh - 28) / 2, uiFit(r, label, 28, textMax), sel ? TEXT : SOFT, 28, sel);
            if (sel) { r.roundRect(x + rw - 84, y + rh / 2 - 19, 56, 38, 12, KEY); uiCenter(r, x + rw - 56, y + rh / 2, "X", TEXT, 22, true); }
        }
        if (n > kRows) {
            int th = kRows * pitch - 10;
            r.roundRect(W - 52, y0, 8, th, 4, CARD2);
            int kh = std::max(48, th * kRows / n);
            r.roundRect(W - 52, y0 + (th - kh) * scroll[t] / std::max(1, n - kRows), 8, kh, 4, GREEN_DK);
        }
        if (nc == 0) {
            bool noList = app.list.empty();
            uiCenterHeavy(r, W / 2, y0 + 150, noList ? noListMsg() : emptyMsg(), MUTED, 32);
            if (noList) uiCenter(r, W / 2, y0 + 200, noListHint(), DIM, 24);
        }
    }

    void renderList(Renderer &r) {
        using namespace theme;
        if (browsable(tab)) { renderBrowse(r); return; }
        const int W = r.width();
        int t = (int)tab, n = itemCount();
        bool kick = tab == Tab::Kick;
        // titulo da secao
        std::string title;
        switch (tab) {
        case Tab::Kick: title = "KICK TV"; break;
        case Tab::Channels: title = "CANAIS"; break;
        case Tab::Movies: title = "FILMES"; break;
        default: title = level == 0 ? "SÉRIES" : level == 1 ? curSeries : curSeries + "  /  Temporada " + std::to_string(curSeason);
        }
        uiKMark(r, 73, 146, 40, 41, GREEN);
        uiHeavy(r, 130, 152, uiFit(r, title, 36, W - 560), TEXT, 36);
        r.fillRect(73, 196, W - 146, 2, 0xFF1C1C1C);
        if (!kick) {
            std::string c = std::to_string(n) + (n == 1 ? " item" : " itens");
            uiText(r, W - 73 - r.textWidth(c, 22), 158, c, MUTED, 22);
        }

        const int x = 73, rw = W - 146 - 28, rh = kick ? 92 : 64, pitch = kick ? 108 : 74, y0 = 218;
        auto acts = kickActions();
        for (int row = 0; row < kRows; row++) {
            int idx = scroll[t] + row;
            if (idx >= n) break;
            int y = y0 + row * pitch;
            bool sel = idx == cursor[t];
            if (sel) uiGlow(r, x, y, rw, rh, 18, GREEN, 10, pk(110), false);
            uiFrame(r, x, y, rw, rh, 18, sel ? 3 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            const std::string label = itemLabel(idx);
            std::string ic = "#";
            if (kick) ic = kickIcon(acts[idx]);
            else for (unsigned char ch : label) if (std::isalnum(ch)) { ic = std::string(1, (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch)); break; }
            int is = rh - 20;
            r.roundRect(x + 14, y + 10, is, is, 14, sel ? GREEN : CHIP);
            uiCenterHeavy(r, x + 14 + is / 2, y + rh / 2, ic, sel ? BLACK : MUTED, kick ? 34 : 28);
            unsigned tc = sel ? TEXT : SOFT;
            int tx = x + 14 + is + 22, textMax = rw - (tx - x) - 110;
            if (kick) {
                uiHeavy(r, tx, y + 14, uiFit(r, label, 30, textMax), tc, 30);
                uiText(r, tx, y + 54, uiFit(r, kickSub(acts[idx]), 21, textMax), MUTED, 21);
            } else {
                uiText(r, tx, y + (rh - 28) / 2, uiFit(r, label, 28, textMax), tc, 28, sel);
            }
            if (sel) { r.roundRect(x + rw - 84, y + rh / 2 - 19, 56, 38, 12, KEY); uiCenter(r, x + rw - 56, y + rh / 2, "X", TEXT, 22, true); }
        }
        if (n == 0) {
            bool noList = app.list.empty();
            uiCenterHeavy(r, W / 2, y0 + 150, noList ? noListMsg() : emptyMsg(), MUTED, 32);
            if (noList) uiCenter(r, W / 2, y0 + 200, noListHint(), DIM, 24);
        }
        // barra de rolagem
        if (n > kRows) {
            int th = kRows * pitch - 10;
            r.roundRect(W - 52, y0, 8, th, 4, CARD2);
            int kh = std::max(48, th * kRows / n);
            int ky = y0 + (th - kh) * scroll[t] / std::max(1, n - kRows);
            r.roundRect(W - 52, ky, 8, kh, 4, GREEN_DK);
        }
        // ajuda (tela Kick)
        if (kick) {
            int hy = y0 + n * pitch + 10;
            uiFrame(r, x, hy, rw, 190, 18, 2, CARD_LINE, 0xFF0B0B0B);
            uiText(r, x + 30, hy + 20, "Como usar", GREEN, 24, true);
            uiText(r, x + 30, hy + 62, "1. Escolha \"Digitar @streamer\" e informe o nome do canal (ex.: @dekin021).", SOFT, 22);
            uiText(r, x + 30, hy + 98, "2. O canal precisa estar ao vivo. Aperte O para voltar ao menu a qualquer momento.", SOFT, 22);
            uiText(r, x + 30, hy + 134, "3. As lives da Kick abrem direto aqui no PS4, sem navegador.", SOFT, 22);
        }
    }

    void renderFooter(Renderer &r) {
        using namespace theme;
        const int W = r.width(), cy = 1043;
        std::vector<std::pair<std::string, std::string>> hints;
        if (screen == Screen::Donate) hints = {{"O", "Voltar"}};
        else if (screen == Screen::Home) hints = {{"D-pad", "Navegar"}, {"X", "Selecionar"}};
        else if (browsable(tab) && !inItems) hints = {{"L1/R1", "Abas"}, {"X", "Abrir"}, {"O", "Início"}};
        else if (browsable(tab)) hints = {{"X", "Selecionar"}, {"O", searching() ? "Limpar busca" : "Categorias"}};
        else hints = {{"L1/R1", "Abas"}, {"X", "Selecionar"}, {"O", "Voltar"}};
        int hintsRight = W - 73;
        if (screen != Screen::Donate && screen != Screen::Help) {         // botao da lupa (canto inferior direito): mesmo que apertar △ (ou clicar o touchpad)
            const bool on = searching();
            const int pw = on ? 250 : 214, ph = 48, px = W - 73 - pw, py = cy - ph / 2;
            r.roundRect(px, py, pw, ph, 24, on ? GREEN : KEY);
            uiIconSearch(r, px + 32, cy, 28, on ? BLACK : TEXT);
            uiText(r, px + 58, cy - 11, on ? "Nova busca" : "Buscar", on ? BLACK : TEXT, 21, true);
            r.roundRect(px + pw - 44, cy - 15, 34, 30, 10, on ? 0x40000000 : 0xFF3C424C);
            uiGlyphTriangle(r, px + pw - 27, cy, 17, on ? BLACK : TEXT);
            hintsRight = px - 26;
        }
        int hx = uiHints(r, hintsRight, cy, hints);
        const std::string &st = app.lastStatus;
        unsigned dot = AMBER;
        if (st.find("Falha") != std::string::npos || st.find("offline") != std::string::npos || st.find("invalido") != std::string::npos || st.find("Aviso") != std::string::npos) dot = RED;
        else if (st.compare(0, 10, "Assistindo") == 0 || st == "Pronto" || st.compare(0, 6, "Canais") == 0) dot = GREEN;
        r.roundRect(73, cy - 6, 12, 12, 6, dot);
        uiText(r, 98, cy - 11, uiFit(r, st, 22, hx - 98 - 30), SOFT, 22);
    }

public:
    // Pagina de detalhes (detailpage.hpp)
    void openMovieDetail(const Entry &e, const std::string &category);
    void openSeriesDetail(const std::string &name, const std::string &category);
    void playSeriesEpisode(int epIndex);
    void handleDetail(Input in);
    void renderDetail(Renderer &r);
    std::vector<Entry> detailEpisodes() const;
    void wantDetailCovers(const std::string &poster, const std::vector<std::string> &front, const std::vector<std::string> &back);
private:

    // Toca o canal e entrega a lista da categoria (ou da busca) ao player: L1/R1 durante o video troca para o canal anterior/proximo.
    void playChannelFrom(const std::vector<uint32_t> &idx, int pos) {
        const Playlist &p = app.list;
        if (pos < 0 || pos >= (int)idx.size() || idx[pos] >= p.channels.size()) return;
        const Entry &e = p.channels[idx[pos]];
        if (e.url.empty()) { app.say("Canal sem link"); return; }
        app.pf.play(e.url, e.name);
        const int a = std::max(0, pos - 1000), b = std::min((int)idx.size(), pos + 1001);     // janela: categorias enormes nao viram uma lista gigante
        std::vector<std::string> urls, titles; int cur = 0;
        for (int i = a; i < b; i++) {
            if (idx[i] >= p.channels.size() || p.channels[idx[i]].url.empty()) continue;
            if (i == pos) cur = (int)urls.size();
            urls.push_back(p.channels[idx[i]].url); titles.push_back(p.channels[idx[i]].name);
        }
        app.pf.setEpisodeList(urls, titles, cur);
    }

    void confirm(int n) {
        int t = (int)tab, c = cursor[t];
        const Playlist &p = app.list;
        if (tab == Tab::Kick) {
            auto acts = kickActions();
            if (c >= (int)acts.size()) return;
            if (acts[c] == KA_LAST) app.watchKick(kickLast);
            else { textKind = 1; app.pf.requestText("@ do streamer"); }
            return;
        }
        if (searching()) {
            if (c < 0 || c >= (int)searchHits.size()) return;
            uint32_t hi = searchHits[c];
            if (tab == Tab::Channels && hi < p.channels.size()) {
                playChannelFrom(searchHits, c);
            } else if (tab == Tab::Movies && hi < p.movies.size()) {
                openMovieDetail(p.movies[hi], movieCategoryOf(hi));
            } else if (tab == Tab::Series) {
                curSeries = seriesNameAt(hi);
                if (!curSeries.empty()) { std::string nm = curSeries; if (!app.ensureSeries(nm)) return; openSeriesDetail(nm, seriesCategoryOf(nm)); }      // a busca continua: O volta para os resultados
            }
            return;
        }
        const Category *cat = curCat();
        if (!cat || c >= n) return;
        if (tab == Tab::Channels) {
            playChannelFrom(cat->items, c);
        } else if (tab == Tab::Movies) {
            openMovieDetail(p.movies[cat->items[c]], cat->name);
        } else if (level == 0) {
            std::string nm = cat->series[c];
            if (!app.ensureSeries(nm)) return;                 // painel: baixa os episodios agora (sem episodios: fica na lista e avisa)
            openSeriesDetail(nm, cat->name);
        } else if (level == 1) {
            auto &m = p.seriesTree.at(curSeries);
            auto it = m.begin(); std::advance(it, c);
            curSeason = it->first; level = 2; cursor[t] = scroll[t] = 0;
        } else {
            playSeriesEpisode(c);
        }
    }
};

#include "detailpage.hpp"
#include "homeview.hpp"
