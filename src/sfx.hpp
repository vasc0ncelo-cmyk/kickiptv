#pragma once
// Sons da interface (bips "tecnologicos"), sintetizados na hora: nao precisa de arquivo de audio.
// PCM s16 estereo 48 kHz, em blocos de 256 quadros (o tamanho do bloco da porta de audio do PS4).
// Sem libm: seno por polinomio. Independente de plataforma (testado no PC); quem toca e o SfxPlayer do ps4_platform.cpp.
#include <stdint.h>
#include <vector>

namespace sfx {
enum Kind { Nav = 0, Ok = 1, Back = 2, Err = 3, Tab = 4, Boot = 5, Count = 6 };

inline float sn(float a) {                       // seno (a em radianos)
    const float PI = 3.14159265f, TP = 6.2831853f;
    while (a > PI) a -= TP;
    while (a < -PI) a += TP;
    float a2 = a * a;
    return a * (1.f - a2 / 6.f * (1.f - a2 / 20.f * (1.f - a2 / 42.f * (1.f - a2 / 72.f * (1.f - a2 / 110.f)))));
}

// nota com glissando f0 -> f1 (Hz), 'ms' de duracao, comecando em startMs. wave: 0 = seno suave, 1 = timbre digital (seno + quadrada leve)
inline void voice(std::vector<float> &buf, float startMs, float ms, float f0, float f1, float amp, int wave = 0) {
    const float sr = 48000.f, TP = 6.2831853f;
    size_t s0 = (size_t)(startMs * sr / 1000.f), n = (size_t)(ms * sr / 1000.f);
    if (s0 + n > buf.size()) buf.resize(s0 + n, 0.f);
    float ph = 0.f;
    for (size_t i = 0; i < n; i++) {
        float t = (float)i / (float)n;                                 // 0..1
        float f = f0 + (f1 - f0) * t;
        ph += TP * f / sr; if (ph > TP) ph -= TP;
        float w = sn(ph);
        if (wave == 1) w = 0.72f * w + 0.28f * (w >= 0.f ? 1.f : -1.f) * 0.6f;
        float att = (float)i / (sr * 0.002f); if (att > 1.f) att = 1.f;  // ataque de 2 ms (sem estalo)
        float dec = (1.f - t); dec *= dec;                               // decaimento
        buf[s0 + i] += w * amp * att * dec;
    }
}

// volume: 0..1 (0.22 e discreto)
inline std::vector<int16_t> render(int kind, float volume = 0.22f) {
    std::vector<float> b;
    switch (kind) {
    case Nav:  voice(b, 0, 26, 1900.f, 1550.f, 0.80f, 1); break;
    case Ok:   voice(b, 0, 46, 784.f, 784.f, 0.75f, 1); voice(b, 38, 90, 1175.f, 1175.f, 0.80f, 1); voice(b, 38, 90, 2350.f, 2350.f, 0.18f, 0); break;
    case Back: voice(b, 0, 80, 900.f, 520.f, 0.80f, 1); break;
    case Err:  voice(b, 0, 70, 170.f, 150.f, 0.85f, 1); voice(b, 100, 90, 150.f, 120.f, 0.85f, 1); break;
    case Tab:  voice(b, 0, 120, 420.f, 1500.f, 0.70f, 1); voice(b, 0, 120, 840.f, 3000.f, 0.14f, 0); break;
    case Boot: voice(b, 0, 110, 523.f, 523.f, 0.55f, 1); voice(b, 85, 110, 659.f, 659.f, 0.55f, 1); voice(b, 170, 110, 784.f, 784.f, 0.55f, 1);
               voice(b, 255, 260, 1047.f, 1047.f, 0.65f, 1); voice(b, 255, 260, 2093.f, 2093.f, 0.14f, 0); break;
    default: break;
    }
    size_t frames = b.size();
    frames = (frames + 255) / 256 * 256 + 256;          // multiplo de 256 + um bloco de silencio no fim (sem "clique" ao parar)
    std::vector<int16_t> out(frames * 2, 0);
    for (size_t i = 0; i < b.size(); i++) {
        float v = b[i] * volume; if (v > 0.95f) v = 0.95f; if (v < -0.95f) v = -0.95f;
        int16_t s = (int16_t)(v * 32767.f);
        out[i * 2] = s; out[i * 2 + 1] = s;
    }
    return out;
}
}  // namespace sfx
