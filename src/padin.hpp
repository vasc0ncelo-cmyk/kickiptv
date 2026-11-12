#pragma once
// Leitura do controle PS4 — versao simples e robusta.
// D-pad + analogico esquerdo, edge detect, repeat por tempo.
#include <stdint.h>

// Mascaras alinhadas ao OpenOrbis (orbis/Pad.h)
static const uint32_t B_L3 = 0x0002, B_R3 = 0x0004,
                      B_OPTIONS = 0x0008, B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
                      B_L2 = 0x0100, B_R2 = 0x0200, B_L1 = 0x0400, B_R1 = 0x0800,
                      B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000;

// Touchpad click (alguns firmwares)
static const uint32_t B_TOUCH = 0x100000;

struct PadSample { uint32_t buttons = 0; uint8_t lx = 128, ly = 128; };

struct PadTuning {
    static constexpr double kDelay = 0.28, kSlow = 0.055, kFast = 0.030, kFastAfter = 0.8;
    static constexpr double kAccel1 = 1.0, kAccel2 = 2.2;
    static constexpr int kStickOn = 45, kStickOff = 28;
};

template <class Src>
struct PadInT {
    int h = -1;
    uint32_t held = 0, pending = 0;
    uint32_t realHeld = 0;
    uint32_t stickDir = 0;
    double stickMag = 0;
    double nextRep[4] = {0, 0, 0, 0}, repStart[4] = {0, 0, 0, 0};
    uint32_t lastRaw = 0;   // ultimo buttons cru do hardware

    static const uint32_t *dirs() {
        static const uint32_t d[4] = {B_UP, B_DOWN, B_LEFT, B_RIGHT};
        return d;
    }

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

    uint32_t merged(const PadSample &s) {
        stickDir = stickToDir(s);
        // botoes digitais + direcao do analogico
        return s.buttons | stickDir;
    }

    void poll() {
        if (h < 0) return;

        PadSample s{};
        bool ok = Src::state(h, s);

        // Tambem drena a fila (eventos curtos)
        PadSample buf[8];
        int n = Src::read(h, buf, 8);
        uint32_t seen = 0;
        for (int i = 0; i < n; i++) seen |= merged(buf[i]);

        if (ok) {
            lastRaw = s.buttons;
            realHeld = s.buttons;
            uint32_t cur = merged(s);
            seen |= cur;
            uint32_t fresh = seen & ~held;
            // edge: botoes que ligaram agora
            pending |= fresh;
            // se o estado atual tem botao que pending perdeu por race, nao — held atualiza depois
            held = cur;
        } else if (n > 0) {
            // so fila
            uint32_t cur = seen;
            pending |= (seen & ~held);
            held = cur;
        }

        double t = Src::now();
        for (int i = 0; i < 4; i++) {
            if (pending & dirs()[i]) { // acabou de apertar
                if (!(held & dirs()[i])) continue;
            }
            if ((held & dirs()[i]) && nextRep[i] == 0) {
                // comecou a segurar
                if (pending & dirs()[i]) {
                    repStart[i] = t;
                    nextRep[i] = t + PadTuning::kDelay;
                }
            }
            if (held & dirs()[i]) {
                if (nextRep[i] == 0) { repStart[i] = t; nextRep[i] = t + PadTuning::kDelay; }
            } else {
                nextRep[i] = 0;
            }
        }
    }

    uint32_t take() {
        uint32_t e = pending;
        pending = 0;
        return e;
    }

    void steps(int out[4]) {
        double t = Src::now();
        for (int i = 0; i < 4; i++) {
            out[i] = 0;
            if (!(nextRep[i] > 0 && (held & dirs()[i]) && t >= nextRep[i])) continue;
            double iv = (t - repStart[i]) > PadTuning::kFastAfter ? PadTuning::kFast : PadTuning::kSlow;
            if (dirs()[i] == stickDir && !(realHeld & dirs()[i]))
                iv *= 1.0 - 0.30 * stickMag;
            int n = 1 + (int)((t - nextRep[i]) / iv);
            if (n > 3) n = 3;
            out[i] = n;
            nextRep[i] += n * iv;
            if (nextRep[i] <= t) nextRep[i] = t + iv;
        }
    }

    int accel(int dirIdx) const {
        double hold = Src::now() - repStart[dirIdx];
        return hold > PadTuning::kAccel2 ? 4 : hold > PadTuning::kAccel1 ? 2 : 1;
    }

    uint32_t repeats() {
        int st[4]; steps(st);
        uint32_t m = 0;
        for (int i = 0; i < 4; i++) if (st[i]) m |= dirs()[i];
        return m;
    }
};
