#pragma once
// Sons suaves (senoides com envelope) — sem "bip" agressivo.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

struct SoftSfx {
    int port = -1;
    bool ready = false;
    static constexpr int kRate = 48000;
    static constexpr int kCh = 2;

    bool init(int audioPort) {
        port = audioPort;
        ready = port >= 0;
        return ready;
    }

    // Gera tom suave: freq Hz, dur ms, volume 0..1, harmonia leve
    void tone(float freq, float ms, float vol = 0.18f) {
        if (!ready || port < 0) return;
        int n = (int)(kRate * (ms / 1000.f));
        if (n < 64) n = 64;
        if (n > kRate * 2) n = kRate * 2;
        std::vector<int16_t> buf((size_t)n * kCh);
        const float twoPi = 6.28318530718f;
        const float at = 0.012f, rel = 0.08f;
        float dur = ms / 1000.f;
        for (int i = 0; i < n; i++) {
            float t = (float)i / (float)kRate;
            float env = 1.f;
            if (t < at) env = t / at;
            else if (t > dur - rel && dur > rel) env = (dur - t) / rel;
            if (env < 0) env = 0;
            // seno + harmônico suave (1/4)
            float s = sinf(twoPi * freq * t) * 0.85f + sinf(twoPi * freq * 2.f * t) * 0.15f;
            int16_t v = (int16_t)(s * env * vol * 28000.f);
            buf[(size_t)i * 2] = v;
            buf[(size_t)i * 2 + 1] = v;
        }
        // escreve em blocos
        extern "C" int sceAudioOutOutput(int handle, const void *ptr);
        const int frame = 256;
        for (int off = 0; off < n; off += frame) {
            int m = n - off; if (m > frame) m = frame;
            // pad se preciso
            if (m < frame) {
                std::vector<int16_t> tmp(frame * kCh, 0);
                memcpy(tmp.data(), buf.data() + off * kCh, (size_t)m * kCh * sizeof(int16_t));
                sceAudioOutOutput(port, tmp.data());
            } else {
                sceAudioOutOutput(port, buf.data() + off * kCh);
            }
        }
    }

    void nav()   { tone(523.25f, 45, 0.12f); }          // C5 suave
    void ok()    { tone(659.25f, 70, 0.14f); tone(783.99f, 90, 0.12f); } // E5 G5
    void back()  { tone(392.00f, 55, 0.11f); }          // G4
    void err()   { tone(220.00f, 100, 0.10f); }         // A3 baixo
    void select(){ tone(587.33f, 40, 0.13f); }          // D5
};
