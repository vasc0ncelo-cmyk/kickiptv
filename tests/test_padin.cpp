// Testa a leitura do controle (padin.hpp) no PC com um controle "de mentira" que segue um roteiro no tempo.
#include <assert.h>
#include <stdio.h>
#include "../src/padin.hpp"

static double g_t = 0, g_lastRead = 0;
static int g_sc = 0;
static uint32_t scBtn(double t) {
    if (g_sc == 1 || g_sc == 4) return (t >= 0.10 && t < 3.10) ? B_DOWN : 0;     // seta segurada 3 s
    if (g_sc == 3) return (t >= 0.100 && t < 0.112) ? B_CROSS : 0;                // toque de 12 ms
    return 0;
}
static void scStick(double t, uint8_t &x, uint8_t &y) {
    x = 128; y = 128;
    if (g_sc == 2 && t >= 0.10 && t < 3.10) x = 255;                              // analogico todo para a direita
    if (g_sc == 5 && t >= 0.10 && t < 3.10) x = 128 + 70;                         // analogico inclinado so um pouco
    if (g_sc == 6) { x = 128 + 8; y = 128 - 9; }                                  // folga (drift) do analogico parado
}
struct Fake {
    static int read(int, PadSample *buf, int n) {            // 1 estado a cada 4 ms desde a ultima leitura (~250 Hz)
        int k = 0;
        for (double x = g_lastRead + 0.004; x <= g_t + 1e-9 && k < n; x += 0.004) {
            buf[k].buttons = scBtn(x); scStick(x, buf[k].lx, buf[k].ly); k++; g_lastRead = x;
        }
        return k;
    }
    static bool state(int, PadSample &s) { s.buttons = scBtn(g_t); scStick(g_t, s.lx, s.ly); return true; }
    static double now() { return g_t; }
};

struct Res { int ev, first, e1, e2, e3, cross; double firstT; int items; };
static Res run(int sc, double loopDt) {
    g_sc = sc; g_lastRead = 0;
    PadInT<Fake> pin; pin.h = 0;
    Res r = {0, 0, 0, 0, 0, 0, -1, 0};
    for (g_t = 0; g_t < 3.5; g_t += loopDt) {
        pin.poll();
        uint32_t pressed = pin.take();
        int st[4]; pin.steps(st);
        int n = 0;
        for (int i = 0; i < 4; i++) n += ((pressed & PadInT<Fake>::dirs()[i]) ? 1 : 0) + st[i];
        if (pressed & B_CROSS) r.cross++;
        if (n > 0) {
            if (r.firstT < 0) r.firstT = g_t;
            r.ev += n;
            if (g_t < 1.0) r.e1 += n; else if (g_t < 2.0) r.e2 += n; else r.e3 += n;
        }
        int dd = (st[1] ? pin.accel(1) : 1); (void)dd;
    }
    return r;
}

int main() {
    // 1) seta segurada: 1 passo ao apertar, depois repete rapido e acelera
    Res a = run(1, 0.004);
    assert(a.firstT >= 0.09 && a.firstT < 0.12);       // reage na hora
    assert(a.e1 >= 12 && a.e1 <= 20);                  // 0,1-1,0 s: ~ 1 + (0,62 s / 55 ms)
    assert(a.e2 > a.e1 && a.e2 >= 25);                 // depois de 0,8 s: mais rapido (30 ms)
    assert(a.ev >= 70);                                // em 3 s: dezenas de passos (antes eram ~49)
    // 2) a velocidade NAO depende dos quadros por segundo: laco lento (desenho de 120 ms) anda o mesmo
    Res slow = run(4, 0.120);
    assert(slow.ev >= a.ev * 7 / 10);                  // recupera os passos perdidos (no maximo 3 por volta)
    // 3) analogico totalmente inclinado = seta; pouco inclinado = mais devagar
    Res full = run(2, 0.004);
    Res half = run(5, 0.004);
    assert(full.ev >= 70);
    assert(half.ev > 20 && half.ev < full.ev);         // inclinacao menor: mais lento, mas anda
    // 4) toque muito curto (12 ms, mais curto que 3 voltas do laco) nao se perde
    Res tap = run(3, 0.016);
    assert(tap.cross == 1);
    // 5) folga do analogico parado nao vira movimento
    Res drift = run(6, 0.004);
    assert(drift.ev == 0);
    printf("OK  (seta: %d passos em 3 s; laco lento: %d; analogico cheio: %d; meio: %d)\n", a.ev, slow.ev, full.ev, half.ev);
    return 0;
}
