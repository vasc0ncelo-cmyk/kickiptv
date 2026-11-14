#pragma once
// Redesenho so do que mudou (PS4 e testes no PC).
//
// Problema: o desenho e por software (CPU) e o menu era redesenhado INTEIRO (1920x1080) varias vezes por segundo, mesmo com a tela parada.
// Solucao em 3 passos, sem mexer nas telas (elas continuam desenhando "tudo", do mesmo jeito):
//   1) Passada de ANOTACAO: a UI desenha num DamageRecorder, que nao pinta nada: so calcula, para cada bloco da tela (64x54 px),
//      um resumo (hash) de TODAS as operacoes de desenho que tocam o bloco, na ordem. Custa so a montagem das telas.
//   2) Compara com o resumo do quadro anterior: blocos com resumo diferente = blocos que mudaram.
//   3) Passada de PINTURA: a UI desenha de novo, agora num FbRenderer com recorte, so nos retangulos que mudaram.
//      Dentro do recorte o resultado e IDENTICO ao desenho da tela inteira (o teste tests/test_damage.cpp confere pixel a pixel).
// Tela parada = nenhum bloco mudou = nada e pintado nem copiado para o video.
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "ui.hpp"
#include "fbrender.hpp"

struct DRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    long long area() const { return empty() ? 0LL : (long long)(x1 - x0) * (y1 - y0); }
};
inline DRect dUnion(const DRect &a, const DRect &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    DRect r; r.x0 = a.x0 < b.x0 ? a.x0 : b.x0; r.y0 = a.y0 < b.y0 ? a.y0 : b.y0; r.x1 = a.x1 > b.x1 ? a.x1 : b.x1; r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}
inline bool dTouch(const DRect &a, const DRect &b) { return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1; }   // encosta ou sobrepoe

// Conjunto pequeno de retangulos (no maximo N). Passou disso: junta os dois que menos "engordam" a area.
template <int N> struct DRectSet {
    DRect r[N + 1]; int n = 0;
    void clear() { n = 0; }
    void add(DRect a) {
        if (a.empty()) return;
        for (int i = 0; i < n;) {                                  // junta com quem encosta (repete: a uniao pode encostar em outro)
            if (dTouch(r[i], a)) { a = dUnion(r[i], a); r[i] = r[--n]; i = 0; } else i++;
        }
        r[n++] = a;
        while (n > N) {
            int bi = 0, bj = 1; long long best = -1;
            for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) {
                long long grow = dUnion(r[i], r[j]).area() - r[i].area() - r[j].area();
                if (best < 0 || grow < best) { best = grow; bi = i; bj = j; }
            }
            r[bi] = dUnion(r[bi], r[bj]); r[bj] = r[--n];
        }
    }
};

// ------------------------------------------------------------------------------------------------
// Passada de anotacao: implementa o Renderer sem pintar nada. Para cada operacao, soma "o que ela e" (parametros) ao resumo
// de cada bloco que ela cobre. Resumo = hash de 64 bits que depende da ORDEM das operacoes.
class DamageRecorder : public Renderer {
public:
    static const int TW = 64, TH = 54;                              // bloco: 1920x1080 = 30 x 20 blocos
    DamageRecorder(std::vector<uint64_t> &tiles, int w, int h) : t_(tiles), W_(w), H_(h), tx_((w + TW - 1) / TW), ty_((h + TH - 1) / TH) {
        t_.assign((size_t)tx_ * ty_, 1469598103934665603ull);
    }
    int width() const override { return W_; }
    int height() const override { return H_; }
    int tilesX() const { return tx_; }
    int tilesY() const { return ty_; }

    void fillRect(int x, int y, int w, int h, unsigned argb) override { fold(x, y, x + w, y + h, h64(1, x, y, w, h, argb)); }
    void roundRect(int x, int y, int w, int h, int rad, unsigned argb) override { fold(x, y, x + w, y + h, h64(2, x, y, w, h, argb, (unsigned)rad)); }
    void gradient(int x, int y, int w, int h, unsigned top, unsigned bot) override { fold(x, y, x + w, y + h, h64(3, x, y, w, h, top, bot)); }
    void pixel(int x, int y, unsigned argb) override { fold(x, y, x + 1, y + 1, h64(4, x, y, argb)); }
    int textWidth(const std::string &t, int size) const override {      // MESMA conta do FbRenderer (a montagem das telas depende disto)
        int n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) n++;
        return n * (int)(8 * (size / 16.0f) + 0.5f);
    }
    void drawText(int x, int y, const std::string &text, unsigned argb, int size) override {
        const float s = size / 16.0f;
        const int adv = (int)(8 * s + 0.5f);
        int n = 0; uint64_t h = h64(5, x, y, argb, (unsigned)size);
        for (unsigned char c : text) { if ((c & 0xC0) != 0x80) n++; h = (h ^ c) * 0x100000001b3ull; }
        fold(x, y, x + n * adv + (int)(8 * s) + 4, y + (int)(16 * s) + 3, h);
    }
    bool blitBackground() override { fold(0, 0, W_, H_, 0xB61D5EEDull); return true; }
    bool glow(int x, int y, int w, int h, int rad, unsigned color, int spread, int peak, bool inside) override {
        if (spread < 1 || w <= 0 || h <= 0) return true;
        if (spread > 64) return false;                                 // o FbRenderer tambem recusa: a UI desenha por retangulos
        fold(x - spread - 1, y - spread - 1, x + w + spread + 1, y + h + spread + 1, h64(6, x, y, w, h, color, (unsigned)(rad * 4096 + spread), (unsigned)(peak * 2 + (inside ? 1 : 0))));
        return true;
    }
    void blitImage(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh) override {
        fold(x, y, x + dw, y + dh, h64(7, x, y, dw, dh, (unsigned)sw, (unsigned)sh) ^ imgSig(px, sw, sh));
    }
    void blitImageRound(int x, int y, int dw, int dh, const uint32_t *px, int sw, int sh, int rTop, int rBot) override {
        fold(x, y, x + dw, y + dh, h64(8, x, y, dw, dh, (unsigned)(sw * 65536 + sh), (unsigned)(rTop * 256 + rBot)) ^ imgSig(px, sw, sh));
    }
    void drawMask(int x, int y, int w, int h, const uint8_t *m, unsigned argb) override {
        uint64_t sig = 0xcbf29ce484222325ull;
        const size_t n = (size_t)w * h;
        if (m && n) { const size_t step = n > 24 ? n / 24 : 1; for (size_t i = 0; i < n; i += step) sig = (sig ^ m[i]) * 0x100000001b3ull; sig = (sig ^ m[n - 1]) * 0x100000001b3ull; }
        fold(x, y, x + w, y + h, h64(9, x, y, w, h, argb) ^ sig);
    }
    bool paintBox(int tag, int &x0, int &y0, int &x1, int &y1, unsigned key) override {
        fold(x0, y0, x1, y1, h64(10, x0, y0, x1 - x0, y1 - y0, key, (unsigned)tag));
        return true;                                                   // nao desenha: so anota
    }

private:
    std::vector<uint64_t> &t_;
    int W_, H_, tx_, ty_;

    static uint64_t m64(uint64_t h, uint64_t v) { h = (h ^ v) * 0x9E3779B97F4A7C15ull; return h ^ (h >> 31); }
    static uint64_t h64(unsigned op, int a, int b, int c, int d, unsigned e = 0, unsigned f = 0, unsigned g = 0) {
        uint64_t h = op;
        h = m64(h, (uint64_t)(uint32_t)a | ((uint64_t)(uint32_t)b << 32));
        h = m64(h, (uint64_t)(uint32_t)c | ((uint64_t)(uint32_t)d << 32));
        h = m64(h, (uint64_t)e | ((uint64_t)f << 32));
        return m64(h, g);
    }
    static uint64_t h64(unsigned op, int a, int b, unsigned c, unsigned d = 0) { return h64(op, a, b, (int)c, (int)d); }
    // Assinatura do conteudo de uma imagem: ponteiro + ~48 pixels espalhados (capa trocada no mesmo lugar muda a assinatura).
    static uint64_t imgSig(const uint32_t *px, int sw, int sh) {
        uint64_t s = (uint64_t)(uintptr_t)px * 0x9E3779B97F4A7C15ull;
        if (!px || sw <= 0 || sh <= 0) return s;
        const size_t n = (size_t)sw * sh, step = n > 48 ? n / 48 : 1;
        for (size_t i = 0; i < n; i += step) s = (s ^ px[i]) * 0x100000001b3ull;
        return (s ^ px[n - 1]) * 0x100000001b3ull;
    }
    void fold(int x0, int y0, int x1, int y1, uint64_t oh) {
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W_) x1 = W_;
        if (y1 > H_) y1 = H_;
        if (x1 <= x0 || y1 <= y0) return;
        const int a = x0 / TW, b = (x1 - 1) / TW, c = y0 / TH, d = (y1 - 1) / TH;
        for (int j = c; j <= d; j++) {
            uint64_t *row = &t_[(size_t)j * tx_];
            for (int i = a; i <= b; i++) row[i] = (row[i] ^ oh) * 0x100000001b3ull ^ (row[i] >> 29);   // depende da ordem
        }
    }
};

// ------------------------------------------------------------------------------------------------
// Guarda o resumo do quadro anterior e diz quais retangulos mudaram.
class DamageTracker {
public:
    static const int kMaxRects = 4;
    // Compara 'cur' (resumo recem-anotado) com o quadro anterior. force = tela inteira (ex.: algo mais pintou por cima).
    // Devolve os retangulos (em pixels) que precisam ser repintados e GUARDA 'cur' como "quadro anterior".
    DRectSet<kMaxRects> diff(std::vector<uint64_t> &cur, int w, int h, bool force) {
        const int tx = (w + DamageRecorder::TW - 1) / DamageRecorder::TW, ty = (h + DamageRecorder::TH - 1) / DamageRecorder::TH;
        DRectSet<kMaxRects> out;
        const bool all = force || prev_.size() != cur.size();
        std::vector<char> dirty((size_t)tx * ty, 0);
        for (size_t i = 0; i < dirty.size(); i++) dirty[i] = all || cur[i] != prev_[i];
        // corridas horizontais de blocos sujos; DRectSet junta as vizinhas
        for (int j = 0; j < ty; j++) {
            for (int i = 0; i < tx;) {
                if (!dirty[(size_t)j * tx + i]) { i++; continue; }
                int e = i; while (e < tx && dirty[(size_t)j * tx + e]) e++;
                DRect r; r.x0 = i * DamageRecorder::TW; r.x1 = e * DamageRecorder::TW < w ? e * DamageRecorder::TW : w;
                r.y0 = j * DamageRecorder::TH; r.y1 = (j + 1) * DamageRecorder::TH < h ? (j + 1) * DamageRecorder::TH : h;
                out.add(r);
                i = e;
            }
        }
        prev_ = cur;
        return out;
    }
    void forget() { prev_.clear(); }
private:
    std::vector<uint64_t> prev_;
};

// ------------------------------------------------------------------------------------------------
// Memoria de video em anel (3 telas): cada uma esta "atrasada" em relacao ao rascunho (shadow) nos trechos que mudaram
// depois da ultima vez que ela recebeu uma copia. Antes de mostrar uma tela, copia-se para ela so esse trecho.
class FlipStale {
public:
    static const int kMax = 6;
    void invalidateAll(int w, int h) { DRect f; f.x1 = w; f.y1 = h; for (int b = 0; b < 3; b++) { s_[b].clear(); s_[b].add(f); } }
    void mark(const DRect &r) { for (int b = 0; b < 3; b++) s_[b].add(r); }
    // retorna e zera o que a tela 'b' precisa receber
    DRectSet<kMax> take(int b) { DRectSet<kMax> o = s_[b]; s_[b].clear(); return o; }
private:
    DRectSet<kMax> s_[3];
};

inline void dCopyRects(uint32_t *dst, const uint32_t *src, int W, const DRect *r, int n) {
    for (int k = 0; k < n; k++) {
        if (r[k].empty()) continue;
        const size_t bytes = (size_t)(r[k].x1 - r[k].x0) * 4;
        for (int y = r[k].y0; y < r[k].y1; y++) memcpy(dst + (size_t)y * W + r[k].x0, src + (size_t)y * W + r[k].x0, bytes);
    }
}

// ------------------------------------------------------------------------------------------------
// Une as duas passadas. 'shadow' = rascunho em RAM (W*H pixels) que PERSISTE entre quadros.
// Uso a cada quadro:  ui.tick(agora);  auto d = painter.paint(ui, shadow, W, H);  se (d.n) { copia d.r[...] para a tela e flip }
struct MenuPainter {
    DamageTracker tracker;
    std::vector<uint64_t> tiles;
    bool valid = false;                    // false = o rascunho/telas nao tem mais o ultimo quadro do menu: repinta tudo
    void invalidate() { valid = false; }

    template <class UIT> DRectSet<DamageTracker::kMaxRects> paint(UIT &ui, uint32_t *shadow, int W, int H) {
        // 1) anotacao (avanca as animacoes uma vez)
        { DamageRecorder rec(tiles, W, H); ui.render(rec); }
        DRectSet<DamageTracker::kMaxRects> d = tracker.diff(tiles, W, H, !valid);
        valid = true;
        // 2) pintura so dos retangulos que mudaram (animacoes congeladas: a 2a montagem da tela nao avanca nada)
        if (d.n) {
            ui.freezeAnim(true);
            for (int i = 0; i < d.n; i++) {
                FbRenderer r(shadow, W, H);
                r.setClip(d.r[i].x0, d.r[i].y0, d.r[i].x1, d.r[i].y1);
                ui.render(r);
            }
            ui.freezeAnim(false);
        }
        return d;
    }
};
