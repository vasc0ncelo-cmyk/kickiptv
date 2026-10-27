#pragma once
// Navegacao e desenho das telas. Independente de plataforma: o PS4 so implementa Renderer + leitura do pad.
// Telas: Home (banner + 4 atalhos + lista M3U), Kick TV (digitar @streamer) e listas (Filmes / Series / Canais).
#include "app.hpp"
#include <algorithm>
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
enum class Screen { Home, Kick, List, Help };

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
inline int uiHints(Renderer &r, int rightX, int cy, const std::vector<std::pair<std::string, std::string>> &h) {
    int x = rightX;
    for (int i = (int)h.size() - 1; i >= 0; i--) {
        int lw = r.textWidth(h[i].second, 20), kw = r.textWidth(h[i].first, 18) + 24;
        x -= lw; uiText(r, x, cy - 10, h[i].second, theme::MUTED, 20);
        x -= 10 + kw; r.roundRect(x, cy - 17, kw, 34, 11, theme::KEY);
        uiCenter(r, x + kw / 2, cy, h[i].first, theme::TEXT, 18, true);
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
        uiHeavy(r, rx - r.textWidth(hhmm, 46), 44, hhmm, theme::TEXT, 46);
        uiText(r, rx - r.textWidth(date, 22), 94, date, theme::SOFT, 22);
    }
}
inline void uiHeader(Renderer &r, const std::string &hhmm, const std::string &date) {
    uiBackground(r);
    uiLogo(r, 73, 46, 69);
    uiTopRight(r, hhmm, date);
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
    int homeRow = 1;          // 0 = banner, 1 = atalhos, 2 = lista M3U
    int homeCol = 0;          // 0 Kick TV, 1 Filmes, 2 Series, 3 Canais
    int cursor[4] = {0, 0, 0, 0};
    int scroll[4] = {0, 0, 0, 0};
    int level = 0;            // so Series: 0 series, 1 temporadas, 2 episodios
    int catCur[4] = {0, 0, 0, 0}, catScroll[4] = {0, 0, 0, 0};   // categoria escolhida (barra da esquerda) em cada aba
    bool inItems = false;     // false = foco na barra de categorias, true = foco nos itens (Filmes/Series/Canais)
    std::string curSeries;
    int curSeason = 0;
    std::string kickLast;     // ultimo @ pesquisado
    std::string m3uUrl;       // lista M3U atual
    std::string timeText, dateText;   // relogio (a plataforma atualiza)
    int textKind = 0;         // 0 nada, 1 = @streamer, 2 = endereco M3U, 3 = busca
    std::string searchQ;
    std::vector<uint32_t> searchHits;
    static const int kRows = 10;
    static const int ImgW = 148, ImgH = 222;       // tamanho do poster (igual ao da miniatura em imgcache.hpp)
    static const int kCols = 8, kGridRows = 3;     // grade de posters (Filmes, Series, Canais)

    explicit UI(App &a) : app(a) {}

    static Tab tileTab(int col) { static const Tab m[4] = {Tab::Kick, Tab::Movies, Tab::Series, Tab::Channels}; return m[col & 3]; }
    static int tabTile(Tab t) { for (int i = 0; i < 4; i++) if (tileTab(i) == t) return i; return 0; }

    // Dados guardados da ultima vez
    void loadSaved() {
        kickLast = kickSlug(app.pf.loadSetting("kick"));
        m3uUrl = app.pf.loadSetting("m3u");
    }
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
            auto it = p.seriesTree.find(c->series[i]);
            if (it == p.seriesTree.end() || it->second.empty()) return "";
            const auto &eps = it->second.begin()->second;
            return eps.empty() ? "" : eps[0].logo;
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
        if (it == app.list.seriesTree.end() || it->second.empty()) return "";
        const auto &eps = it->second.begin()->second;
        return eps.empty() ? "" : eps[0].logo;
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
    void wantHomeCovers() {
        if (!coverBatch) return;
        const Playlist &p = app.list;
        std::string key = "H|" + std::to_string(p.channels.size()) + "|" + std::to_string(p.movies.size()) + "|" + std::to_string(p.seriesTree.size());
        if (key == lastBatchKey) return;
        lastBatchKey = key;
        std::vector<std::string> back;
        const int per = kCols * kGridRows;
        if (!p.movieCats.empty()) for (size_t i = 0; i < p.movieCats[0].items.size() && (int)i < per; i++) back.push_back(p.movies[p.movieCats[0].items[i]].logo);
        if (!p.seriesCats.empty()) for (size_t i = 0; i < p.seriesCats[0].series.size() && (int)i < per; i++) back.push_back(seriesLogo(p.seriesCats[0].series[i]));
        if (!p.channelCats.empty()) for (size_t i = 0; i < p.channelCats[0].items.size() && (int)i < per; i++) back.push_back(p.channels[p.channelCats[0].items[i]].logo);
        coverBatch({}, back);
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
        } else if (tab == Tab::Channels) {
            for (uint32_t i = 0; i < (uint32_t)p.channels.size(); i++)
                if (nameMatch(p.channels[i].name, searchQ)) searchHits.push_back(i);
        } else if (tab == Tab::Series) {
            uint32_t idx = 0;
            for (const auto &kv : p.seriesTree) {
                if (nameMatch(kv.first, searchQ)) searchHits.push_back(idx);
                idx++;
            }
        }
        level = 0; inItems = true;
        int t = (int)tab; cursor[t] = scroll[t] = 0;
    }
    void startSearch() {
        if (!browsable(tab)) { app.say("Abra Filmes, Series ou Canais para buscar"); return; }
        textKind = 3;
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
            rebuildSearch();
            app.say(std::to_string(searchHits.size()) + " resultado(s) para \"" + searchQ + "\"");
            return;
        }
        kickLast = kickSlug(raw);
        if (kickLast.empty() && !raw.empty()) { app.say("@ invalido"); return; }
        if (!kickLast.empty()) { app.pf.saveSetting("kick", kickLast); app.watchKick(kickLast); }
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
            m3uUrl.clear(); app.list = Playlist(); app.pf.saveSetting("m3u", ""); app.pf.cacheClear();
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

    void handle(Input in) {
        if (in == Input::Extra) { startSearch(); return; }
        if (screen == Screen::Home) { handleHome(in); return; }
        int t = (int)tab;
        switch (in) {
        case Input::PrevTab: openTab(tileTab((tabTile(tab) + 3) % 4)); return;
        case Input::NextTab: openTab(tileTab((tabTile(tab) + 1) % 4)); return;
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
        case Input::Right: case Input::Confirm: if (itemCount() > 0) inItems = true; return;
        case Input::Back: screen = Screen::Home; return;
        default: return;
        }
        if (catCur[t] != before) { cursor[t] = scroll[t] = 0; level = 0; }   // outra categoria: itens desde o comeco
        if (catCur[t] < catScroll[t]) catScroll[t] = catCur[t];
        if (catCur[t] >= catScroll[t] + kRows) catScroll[t] = catCur[t] - kRows + 1;
    }

    // ---------- Desenho ----------
    std::function<void(const char *)> dbg;         // diagnostico: o PS4 grava cada etapa do 1o desenho (para achar onde cai)
    void render(Renderer &r) {
        if (dbg) dbg("ui-render-ini");
        backgroundCovers();
        if (dbg) dbg("ui-capas-ok");
        uiHeader(r, timeText, dateText);
        if (dbg) dbg("ui-header-ok");
        if (screen == Screen::Home) renderHome(r); else renderList(r);
        if (dbg) dbg("ui-corpo-ok");
        renderFooter(r);
        if (dbg) dbg("ui-rodape-ok");
    }

private:
    void confirm(int n);
    void renderFooter(Renderer &r);
    void renderHome(Renderer &r);
    void renderHelp(Renderer &r);
    void renderList(Renderer &r);
    void handleHome(Input in);
    void handleItems(Input in);

    void openTab(Tab t) { tab = t; screen = t == Tab::Kick ? Screen::Kick : Screen::List; level = 0; inItems = false; searchQ.clear(); searchHits.clear(); }
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

    void handleHome(Input in) {
        switch (in) {
        case Input::Up: homeRow = std::max(0, homeRow - 1); break;
        case Input::Down: homeRow = std::min(3, homeRow + 1); break;
        case Input::Left: if (homeRow == 1) homeCol = std::max(0, homeCol - 1); break;
        case Input::Right: if (homeRow == 1) homeCol = std::min(3, homeCol + 1); break;
        case Input::PrevTab: if (homeRow == 1) homeCol = std::max(0, homeCol - 1); break;
        case Input::NextTab: if (homeRow == 1) homeCol = std::min(3, homeCol + 1); break;
        case Input::Confirm:
            if (homeRow == 0) openTab(Tab::Kick);
            else if (homeRow == 1) openTab(tileTab(homeCol));
            else if (homeRow == 2) requestM3U();
            else { screen = Screen::Help; helpScroll = 0; }
            break;
        default: break;
        }
    }
    int helpScroll = 0;

    std::string listSummary() const {
        const Playlist &p = app.list;
        if (p.channels.empty() && p.movies.empty() && p.seriesTree.empty()) return m3uUrl.empty() ? "Adicione sua lista de IPTV" : "Lista sem itens - X para trocar";
        return std::to_string(p.channels.size()) + " canais  |  " + std::to_string(p.movies.size()) + " filmes  |  " + std::to_string(p.seriesTree.size()) + " séries";
    }


    void renderHelp(Renderer &r) {
        using namespace theme;
        const int W = r.width(), H = r.height();
        uiBackground(r);
        uiHeavy(r, 48, 40, "COMANDOS E FUNCOES", TEXT, 36);
        uiText(r, 48, 90, "Tudo que o Kick IPTV faz — use O ou X para voltar", MUTED, 20);
        struct Line { const char *a, *b; };
        static const Line lines[] = {
            {"MENU", ""},
            {"D-pad / analogico", "Navegar"},
            {"X", "Selecionar / abrir"},
            {"O", "Voltar"},
            {"△ (triangulo)", "Buscar filmes, series e canais"},
            {"OPTIONS", "No menu: busca | Na live: troca qualidade"},
            {"", ""},
            {"PLAYER — FILME / SERIE", ""},
            {"X (foco pause)", "Pausar / continuar"},
            {"L2 / R2", "-10 s  /  +10 s"},
            {"Setas no seek (↑)", "Entrar na barra; L2/R2 fino, L1/R1 1 min, X confirma"},
            {"L1 / R1", "Episodio anterior / proximo"},
            {"□ (quadrado)", "Mudo"},
            {"△", "Menu de opcoes (proporcao, info...)"},
            {"", ""},
            {"PLAYER — LIVE KICK / CANAL", ""},
            {"OPTIONS", "Ciclar qualidade (Fluido / 720 / 1080) e reconectar"},
            {"O", "Sair do player"},
            {"", ""},
            {"TECLADO", ""},
            {"X no botao #+ =", "Abre caracteres especiais"},
            {"△", "Espaco"},
            {"L1", "Maiusculas"},
            {"□", "Apagar"},
            {"R1", "Confirmar"},
        };
        const int n = (int)(sizeof(lines) / sizeof(lines[0]));
        int y = 140 - helpScroll * 36;
        for (int i = 0; i < n; i++) {
            int yy = y + i * 36;
            if (yy < 120 || yy > H - 80) continue;
            if (!lines[i].b[0] && lines[i].a[0]) {
                uiHeavy(r, 56, yy, lines[i].a, GREEN, 22);
            } else if (lines[i].a[0]) {
                uiText(r, 56, yy, lines[i].a, SOFT, 20);
                uiText(r, 520, yy, lines[i].b, TEXT, 20);
            }
        }
        uiText(r, 48, H - 48, "O / X voltar ao menu", DIM, 18);
    }

    void renderHome(Renderer &r) {
        wantHomeCovers();
        using namespace theme;
        const int W = r.width();
        const int mx = 37, fw = W - 2 * mx;
        // ---- Banner ----
        {
            const int x = mx, y = 169, w = fw, h = 337, rad = 28;
            bool on = homeRow == 0;
            uiGlow(r, x, y, w, h, rad, GREEN, on ? 22 : 12, on ? 120 : 70, false);
            uiFrame(r, x, y, w, h, rad, 3, GREEN, 0xFF020302);
            UiClip clip{x + 3, y + 3, w - 6, h - 6, rad - 3};
            // KICK desfocado ao fundo
            {
                const char *s = "KICK"; int bh = 232, bx = 1096, by = 205;
                for (int i = 0; i < 4; i++) {
                    static const int ox[4] = {-5, 5, -5, 5}, oy[4] = {-5, -5, 5, 5};
                    uiWord(r, s, bx + ox[i], by + oy[i], bh, 0x1853FC18, 64);
                }
                uiWord(r, s, bx, by, bh, 0x3A53FC18, 64);
            }
            // faixas diagonais do canto
            const float K0 = (float)(x + w + y + h);
            const float T = (float)y, B = (float)(y + h);
            uiBand(r, K0 - 600, K0 - 470, T, B, 0x2253FC18, &clip);
            uiBand(r, K0 - 440, K0 - 408, T, B, 0xE053FC18, &clip);
            uiBand(r, K0 - 372, K0 - 215, T, B, 0xE653FC18, &clip);
            uiBand(r, K0 - 205, K0 - 170, T, B, 0x2253FC18, &clip);
            uiBand(r, K0 - 160, K0 - 120, T, B, 0xF053FC18, &clip);
            uiBand(r, K0 - 112, K0 + 20, T, B, 0x6653FC18, &clip);
            // conteudo
            uiKMark(r, 180, 234, 207, 211, GREEN);
            r.fillRect(476, 234, 3, 211, GREEN_DK);
            int gw = uiWord(r, "KICK", 535, 234, 74, GREEN);
            uiWord(r, "TV", 535 + gw + 28, 234, 74, TEXT);
            uiText(r, 535, 334, "Assista suas lives favoritas", TEXT, 31);
            r.drawText(535, 372, "da Kick em ", TEXT, 31);
            int aw = r.textWidth("da Kick em ", 31);
            uiText(r, 535 + aw, 372, "tempo real", GREEN, 31, true);
            r.drawText(535 + aw + r.textWidth("tempo real", 31), 372, "!", TEXT, 31);
            // botao
            const int bx = 535, by = 423, bw = 305, bh = 61;
            if (on) { r.roundRect(bx, by, bw, bh, 14, GREEN); }
            else uiFrame(r, bx, by, bw, bh, 14, 3, GREEN, 0xFF020302);
            unsigned bc = on ? BLACK : GREEN;
            UiPt tri[3] = {{(float)bx + 24, (float)by + 17}, {(float)bx + 24, (float)by + 44}, {(float)bx + 50, (float)by + 30.5f}};
            uiPoly(r, tri, 3, bc);
            uiText(r, bx + 71, by + 15, "Acessar Agora", bc, 31, true);
        }
        // ---- Atalhos ----
        {
            static const char *names[4] = {"KICK TV", "FILMES", "SÉRIES", "CANAIS"};
            const int gap = 19, tw = (fw - 3 * gap) / 4, ty = 560, th = 251, rad = 24;
            for (int i = 0; i < 4; i++) {
                const int x = mx + i * (tw + gap);
                bool sel = homeRow == 1 && homeCol == i;
                unsigned fill = sel ? SEL : CARD;
                if (sel) uiGlow(r, x, ty, tw, th, rad, GREEN, 18, 130, false);
                uiFrame(r, x, ty, tw, th, rad, sel ? 4 : 2, sel ? GREEN : CARD_LINE, fill);
                UiClip clip{x + 2, ty + 2, tw - 4, th - 4, rad - 2};
                const float K0 = (float)(x + tw + ty + th), T = (float)ty, B = (float)(ty + th);
                uiBand(r, K0 - 215, K0 - 185, T, B, sel ? 0x4053FC18 : 0x2653FC18, &clip);
                uiBand(r, K0 - 160, K0 - 88, T, B, sel ? 0xDD53FC18 : 0xB053FC18, &clip);
                uiBand(r, K0 - 74, K0 + 10, T, B, sel ? 0x6653FC18 : 0x4053FC18, &clip);
                const int cx = x + tw / 2;
                unsigned ic = TEXT;
                if (i == 0) uiKMark(r, cx - 47, ty + 36, 94, 96, GREEN);
                else if (i == 1) uiIconClapper(r, cx - 54, ty + 33, 108, ic, fill);
                else if (i == 2) uiIconTvPlay(r, cx - 54, ty + 33, 108, ic, fill);
                else uiIconTvRetro(r, cx - 54, ty + 33, 108, ic, fill);
                int fs = i == 0 ? 52 : 50;
                uiCenterHeavy(r, cx, ty + 187, names[i], TEXT, fs);
                int lw = sel ? r.textWidth(names[i], fs) : 120;
                if (sel) r.fillRect(cx - lw / 2, ty + 222, lw, 4, GREEN);
                else { r.roundRect(cx - 60, ty + 222, 120, 4, 2, 0xFF1E7A0C); r.roundRect(cx - 30, ty + 222, 60, 4, 2, GREEN_DK); }
            }
        }
        // ---- Lista M3U ----
        {
            const int x = mx, y = 820, w = fw, h = 120, rad = 24;
            bool sel = homeRow == 2;
            if (sel) uiGlow(r, x, y, w, h, rad, GREEN, 18, 130, false);
            uiFrame(r, x, y, w, h, rad, sel ? 4 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            uiIconLink(r, x + 84, y + 70, 78, TEXT);
            r.fillRect(x + 170, y + 40, 3, 62, GREEN_DK);
            uiHeavy(r, x + 222, y + 28, "LISTA M3U", TEXT, 40);
            uiText(r, x + 222, y + 80, uiFit(r, listSummary(), 26, w - 222 - 140), SOFT, 26);
            const int pcx = x + w - 90, pcy = y + h / 2;
            r.roundRect(pcx - 36, pcy - 36, 72, 72, 36, GREEN);
            r.fillRect(pcx - 15, pcy - 3, 30, 6, BLACK); r.fillRect(pcx - 3, pcy - 15, 6, 30, BLACK);
        }
    }

    // Filmes / Series / Canais: barra de categorias (esquerda) + itens da categoria (direita)
        // ---- Comandos ----
        {
            const int x = mx, y = 968, w = fw, h = 80, rad = 20;
            bool sel = homeRow == 3;
            if (sel) uiGlow(r, x, y, w, h, rad, GREEN, 14, 110, false);
            uiFrame(r, x, y, w, h, rad, sel ? 3 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            uiHeavy(r, x + 40, y + 24, "COMANDOS E AJUDA", TEXT, 26);
            uiText(r, x + 400, y + 28, "Botoes, player, teclado, Kick TV", MUTED, 20);
        }

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
            if (sel) uiGlow(r, cx, y, cw, rh, 18, GREEN, 10, 110, false);
            uiFrame(r, cx, y, cw, rh, 18, sel ? 3 : 2, sel ? GREEN : CARD_LINE, sel ? SEL : CARD);
            if (cur && !sel) r.roundRect(cx + 10, y + 14, 5, rh - 28, 2, GREEN);        // categoria aberta (foco nos itens)
            std::string cnt = std::to_string(catSize(idx));
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
                    if (sel) uiGlow(r, px - 4, py - 4, pw + 8, ph + 8, 14, GREEN, 12, 120, false);
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
                bool noList = app.list.channels.empty() && app.list.movies.empty() && app.list.seriesTree.empty();
                uiCenterHeavy(r, W / 2 + 200, y0 + 150, noList ? "Nenhuma lista M3U adicionada" : "Nada por aqui ainda", MUTED, 32);
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
            if (sel) uiGlow(r, x, y, rw, rh, 18, GREEN, 10, 110, false);
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
            uiCenterHeavy(r, W / 2, y0 + 150, noList ? "Nenhuma lista M3U adicionada" : "Nada por aqui ainda", MUTED, 32);
            if (noList) uiCenter(r, W / 2, y0 + 200, "Volte ao início e use LISTA M3U para adicionar a sua", DIM, 24);
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
            if (sel) uiGlow(r, x, y, rw, rh, 18, GREEN, 10, 110, false);
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
            bool noList = app.list.channels.empty() && app.list.movies.empty() && app.list.seriesTree.empty();
            uiCenterHeavy(r, W / 2, y0 + 150, noList ? "Nenhuma lista M3U adicionada" : "Nada por aqui ainda", MUTED, 32);
            if (noList) uiCenter(r, W / 2, y0 + 200, "Volte ao início e use LISTA M3U para adicionar a sua", DIM, 24);
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
        if (screen == Screen::Home) hints = {{"D-pad", "Navegar"}, {"X", "Selecionar"}, {"△", "Buscar"}};
        if (screen == Screen::Help) hints = {{"O", "Voltar"}, {"↑↓", "Rolar"}};
        else if (browsable(tab) && !inItems) hints = {{"L1/R1", "Abas"}, {"△", "Buscar"}, {"X", "Abrir"}, {"O", "Início"}};
        else if (browsable(tab)) hints = {{"△", searching() ? "Nova busca" : "Buscar"}, {"X", "Selecionar"}, {"O", searching() ? "Limpar busca" : "Categorias"}};
        else hints = {{"L1/R1", "Abas"}, {"X", "Selecionar"}, {"O", "Voltar"}};
        int hx = uiHints(r, W - 73, cy, hints);
        const std::string &st = app.lastStatus;
        unsigned dot = AMBER;
        if (st.find("Falha") != std::string::npos || st.find("offline") != std::string::npos || st.find("invalido") != std::string::npos || st.find("Aviso") != std::string::npos) dot = RED;
        else if (st.compare(0, 10, "Assistindo") == 0 || st == "Pronto" || st.compare(0, 6, "Canais") == 0) dot = GREEN;
        r.roundRect(73, cy - 6, 12, 12, 6, dot);
        uiText(r, 98, cy - 11, uiFit(r, st, 22, hx - 98 - 30), SOFT, 22);
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
            if (tab == Tab::Channels && hi < p.channels.size()) { const Entry &e = p.channels[hi]; app.pf.play(e.url, e.name); }
            else if (tab == Tab::Movies && hi < p.movies.size()) { const Entry &e = p.movies[hi]; app.pf.play(e.url, e.name); }
            else if (tab == Tab::Series) {
                curSeries = seriesNameAt(hi);
                if (!curSeries.empty()) { level = 1; clearSearch(); cursor[t] = scroll[t] = 0; inItems = true; }
            }
            return;
        }
        const Category *cat = curCat();
        if (!cat || c >= n) return;
        if (tab == Tab::Channels) { const Entry &e = p.channels[cat->items[c]]; app.pf.play(e.url, e.name); }
        else if (tab == Tab::Movies) { const Entry &e = p.movies[cat->items[c]]; app.pf.play(e.url, e.name); }
        else if (level == 0) { curSeries = cat->series[c]; level = 1; cursor[t] = scroll[t] = 0; }
        else if (level == 1) { auto &m = p.seriesTree.at(curSeries); auto it = m.begin(); std::advance(it, c); curSeason = it->first; level = 2; cursor[t] = scroll[t] = 0; }
        else {
            auto eps = sortedEps(p.seriesTree.at(curSeries).at(curSeason));
            if (c < 0 || c >= (int)eps.size()) return;
            const Entry &e = eps[c];
            std::vector<std::string> urls, titles;
            urls.reserve(eps.size()); titles.reserve(eps.size());
            for (const auto &ep : eps) {
                urls.push_back(ep.url);
                titles.push_back(ep.episode ? ("Ep " + std::to_string(ep.episode) + " - " + ep.name) : ep.name);
            }
            app.pf.play(e.url, titles[c]);
            app.pf.setEpisodeList(urls, titles, c);
        }
    }
};
