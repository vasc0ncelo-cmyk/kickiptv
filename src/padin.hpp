#pragma once
// Leitura do controle: toques curtos nao se perdem, o analogico esquerdo vira "setas" e as setas/analogico
// repetem sozinhos enquanto seguradas (contando em TEMPO REAL, nao em voltas do loop).
// Independente do hardware (Src fornece a leitura e o relogio): da para testar no PC.
#include <stdint.h>

static const uint32_t B_L3 = 0x0002, B_R3 = 0x0004,
                      B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_LEFT = 0x0080, B_RIGHT = 0x0020, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

struct PadSample { uint32_t buttons = 0; uint8_t lx = 128, ly = 128; };

// Velocidades (segundos). Segurando uma direcao: 1 passo ao apertar, espera 'kDelay', depois 1 passo a cada
// 'kSlow'; apos 'kFastAfter' segurando, 1 passo a cada 'kFast'. No analogico, quanto mais inclinado, mais rapido.
struct PadTuning {
    static constexpr double kDelay = 0.28, kSlow = 0.055, kFast = 0.030, kFastAfter = 0.8;
    static constexpr double kAccel1 = 1.0, kAccel2 = 2.2;     // listas: apos 1,0 s anda 2 itens por passo; apos 2,2 s, 4
    static constexpr int kStickOn = 50, kStickOff = 30;       // zona morta com histerese (0..127)
};

// Src: static int read(int h, PadSample *buf, int n); static bool state(int h, PadSample &s); static double now();
template <class Src>
struct PadInT {
    int h = -1;
    uint32_t held = 0, pending = 0;
    uint32_t realHeld = 0;                          // so os botoes reais (sem as setas virtuais do analogico)
    uint32_t stickDir = 0;                          // direcao do analogico (B_UP/DOWN/LEFT/RIGHT) ou 0
    double stickMag = 0;                            // 0..1: o quanto esta inclinado
    double nextRep[4] = {0, 0, 0, 0}, repStart[4] = {0, 0, 0, 0};

    static const uint32_t *dirs() { static const uint32_t d[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT}; return d; }

    uint32_t stickToDir(const PadSample &s) {
        int dx = (int)s.lx - 128, dy = (int)s.ly - 128;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        int th = stickDir ? PadTuning::kStickOff : PadTuning::kStickOn;
        if (ax < th && ay < th) { stickMag = 0; return 0; }
        int m = ax >= ay ? ax : ay;
        stickMag = (double)(m - PadTuning::kStickOn) / (127.0 - PadTuning::kStickOn);
        if (stickMag < 0) stickMag = 0;
        if (stickMag > 1) stickMag = 1;
        if (ax >= ay) return dx < 0 ? B_LEFT : B_RIGHT;
        return dy < 0 ? B_UP : B_DOWN;
    }
    uint32_t merged(const PadSample &s) { stickDir = stickToDir(s); return s.buttons | stickDir; }

    void poll() {
        if (h < 0) return;
        PadSample buf[16]; uint32_t seen = 0;
        int n = Src::read(h, buf, 16);
        for (int i = 0; i < n; i++) seen |= merged(buf[i]);      // cada estado guardado (um toque curto nao se perde)
        PadSample s; uint32_t cur = held;
        if (Src::state(h, s)) { cur = merged(s); realHeld = s.buttons; }
        seen |= cur;
        uint32_t fresh = seen & ~held;
        pending |= fresh;
        held = cur;
        double t = Src::now();
        for (int i = 0; i < 4; i++) {
            if (fresh & dirs()[i]) { repStart[i] = t; nextRep[i] = t + PadTuning::kDelay; }
            if (!(held & dirs()[i])) nextRep[i] = 0;
        }
    }
    uint32_t take() { uint32_t e = pending; pending = 0; return e; }

    // Quantos passos cada direcao (UP, DOWN, LEFT, RIGHT) deve dar AGORA por estar segurada (0..3).
    // Se o desenho demorou, "recupera" os passos perdidos: a velocidade nao depende dos quadros por segundo.
    void steps(int out[4]) {
        double t = Src::now();
        for (int i = 0; i < 4; i++) {
            out[i] = 0;
            if (!(nextRep[i] > 0 && (held & dirs()[i]) && t >= nextRep[i])) continue;
            double iv = (t - repStart[i]) > PadTuning::kFastAfter ? PadTuning::kFast : PadTuning::kSlow;
            if (dirs()[i] == stickDir && !(realHeld & dirs()[i])) iv *= 1.0 - 0.30 * stickMag;   // analogico: inclinar mais = mais rapido
            int n = 1 + (int)((t - nextRep[i]) / iv);
            if (n > 3) n = 3;
            out[i] = n;
            nextRep[i] += n * iv;
            if (nextRep[i] <= t) nextRep[i] = t + iv;
        }
    }
    // Multiplicador para listas longas: segurando por mais tempo, anda 2 e depois 4 itens por passo.
    int accel(int dirIdx) const {
        double hold = Src::now() - repStart[dirIdx];
        return hold > PadTuning::kAccel2 ? 4 : hold > PadTuning::kAccel1 ? 2 : 1;
    }
    // mascara simples (1 passo por direcao), usada pelo teclado
    uint32_t repeats() { int st[4]; steps(st); uint32_t m = 0; for (int i = 0; i < 4; i++) if (st[i]) m |= dirs()[i]; return m; }
};
