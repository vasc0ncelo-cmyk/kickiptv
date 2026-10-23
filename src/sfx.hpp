#pragma once
// Sons de UI sintéticos (sem arquivos): clique, confirmar, voltar, erro.
// Usa sceAudioOut em rajadas curtas numa thread dedicada.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <orbis/AudioOut.h>
#include <orbis/libkernel.h>

namespace sfx {

inline float fastSin(float x) {
    // x in radians, normalize to -pi..pi then poly approx
    const float pi = 3.14159265f, twopi = 6.2831853f;
    x = x - (int)(x / twopi) * twopi;
    if (x > pi) x -= twopi;
    if (x < -pi) x += twopi;
    float x2 = x * x;
    return x * (1.f - x2 * (1.f/6.f - x2 * (1.f/120.f)));
}

enum class Kind { None, Move, Confirm, Back, Open, Error, Success };

class Engine {
public:
    void init() {
        if (ready_) return;
        sceAudioOutInit();
        stop_ = false;
        th_ = std::thread([this] { loop(); });
        ready_ = true;
    }
    void shutdown() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        if (port_ > 0) { sceAudioOutClose(port_); port_ = -1; }
        ready_ = false;
    }
    void setMuted(bool m) { muted_ = m; }
    void play(Kind k) {
        if (!ready_ || muted_ || k == Kind::None) return;
        std::lock_guard<std::mutex> l(m_);
        queue_.push_back(k);
        if (queue_.size() > 8) queue_.erase(queue_.begin());
    }
    ~Engine() { shutdown(); }

private:
    std::mutex m_;
    std::vector<Kind> queue_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    bool ready_ = false;
    std::atomic<bool> muted_{false};
    int port_ = -1;
    static const int GRAIN = 256;
    static const int RATE = 48000;

    void tone(float freq, float ms, float vol, float freqEnd = 0.f) {
        if (port_ <= 0) {
            port_ = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, RATE,
                                    ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            if (port_ <= 0) return;
        }
        int samples = (int)(RATE * ms / 1000.f);
        if (samples < GRAIN) samples = GRAIN;
        std::vector<int16_t> buf((size_t)samples * 2);
        for (int i = 0; i < samples; i++) {
            float t = (float)i / RATE;
            float f = freq;
            if (freqEnd > 0.f) f = freq + (freqEnd - freq) * ((float)i / samples);
            // envelope attack/release
            float env = 1.f;
            float at = 0.008f, rel = 0.025f;
            float dur = ms / 1000.f;
            if (t < at) env = t / at;
            else if (t > dur - rel && rel > 0) env = (dur - t) / rel;
            if (env < 0) env = 0;
            float s = fastSin(2.f * 3.14159265f * f * t) * vol * env;
            // leve harmônico
            s += 0.25f * fastSin(2.f * 3.14159265f * f * 2.f * t) * vol * env;
            int16_t v = (int16_t)(s * 20000.f);
            buf[(size_t)i * 2] = v;
            buf[(size_t)i * 2 + 1] = v;
        }
        // pad até múltiplo de GRAIN
        while (buf.size() / 2 % GRAIN) { buf.push_back(0); buf.push_back(0); }
        for (size_t off = 0; off + GRAIN * 2 <= buf.size(); off += GRAIN * 2)
            sceAudioOutOutput(port_, buf.data() + off);
    }

    void playKind(Kind k) {
        switch (k) {
        case Kind::Move:    tone(880.f,  28.f, 0.22f); break;
        case Kind::Confirm: tone(660.f,  40.f, 0.28f, 990.f); break;
        case Kind::Back:    tone(520.f,  35.f, 0.22f, 360.f); break;
        case Kind::Open:    tone(440.f,  50.f, 0.25f, 880.f); break;
        case Kind::Error:   tone(200.f,  90.f, 0.30f, 140.f); break;
        case Kind::Success: tone(523.f,  40.f, 0.26f); tone(659.f, 45.f, 0.26f); break;
        default: break;
        }
    }

    void loop() {
        while (!stop_) {
            Kind k = Kind::None;
            {
                std::lock_guard<std::mutex> l(m_);
                if (!queue_.empty()) { k = queue_.front(); queue_.erase(queue_.begin()); }
            }
            if (k != Kind::None) playKind(k);
            else sceKernelUsleep(5000);
        }
    }
};

inline Engine &get() {
    static Engine e;
    return e;
}

inline void init() { get().init(); }
inline void shutdown() { get().shutdown(); }
inline void play(Kind k) { get().play(k); }
inline void setMuted(bool m) { get().setMuted(m); }

} // namespace sfx
