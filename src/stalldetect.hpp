#pragma once
// Detector de travamento da thread principal. Independente de plataforma (testavel no PC).
// Chame step() UMA vez por segundo (de uma thread propria) com o contador de "batimentos" da thread principal e o relogio.
// Retorna ha quantos segundos seguidos o contador nao anda (0 = andando). Se o console suspendeu (menu do PS, repouso), o intervalo
// entre duas chamadas fica grande e a contagem recomeca: suspender NAO e travamento.
#include <stdint.h>

class StallDetector {
public:
    explicit StallDetector(double maxGapSec = 5.0) : maxGap_(maxGapSec) {}
    int step(uint64_t tick, double now) {
        if (first_) { first_ = false; last_ = tick; prev_ = now; stale_ = 0; return 0; }
        double dt = now - prev_; prev_ = now;
        if (dt > maxGap_ || dt < 0) { last_ = tick; stale_ = 0; return 0; }
        if (tick != last_) { last_ = tick; stale_ = 0; return 0; }
        return ++stale_;
    }
private:
    double maxGap_;
    bool first_ = true;
    uint64_t last_ = 0;
    double prev_ = 0;
    int stale_ = 0;
};
