#pragma once
// Buffer deslizante para alimentar o AvPlayer (callback "readOffset"): uma thread grava
// (append), o player le por posicao absoluta. Independente de plataforma.
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

struct StreamBuf {
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> buf;
    uint64_t base = 0;          // posicao absoluta de buf[0]
    uint64_t readPos = 0;       // fim da ultima leitura
    bool aborted = false;
    // Janela maior: Kick em 5+ min enchia 16MB e o player lia pos < base → trava
    size_t softCap = 24u << 20, hardCap = 40u << 20;
    std::atomic<int> underruns{0}; // pos < base (dessincronizou)

    void reset() {
        std::lock_guard<std::mutex> l(m);
        buf.clear(); base = readPos = 0; aborted = false; underruns = 0;
        buf.shrink_to_fit();
    }
    void abort() { std::lock_guard<std::mutex> l(m); aborted = true; cv.notify_all(); }
    uint64_t total() { std::lock_guard<std::mutex> l(m); return base + buf.size(); }

    void append(const uint8_t *p, size_t n) {
        std::lock_guard<std::mutex> l(m);
        buf.insert(buf.end(), p, p + n);
        if (buf.size() > softCap) {
            // mantem ~2MB atras do leitor (antes 1MB)
            uint64_t keep = readPos > (2u << 20) ? readPos - (2u << 20) : 0;
            uint64_t drop = keep > base ? keep - base : 0;
            if (buf.size() - drop > hardCap) drop = buf.size() > hardCap ? buf.size() - hardCap : 0;
            if (drop > buf.size()) drop = buf.size();
            if (drop) { buf.erase(buf.begin(), buf.begin() + (size_t)drop); base += drop; }
        }
        cv.notify_all();
    }

    int read(uint8_t *dst, uint64_t pos, uint32_t len, int timeoutMs = 8000) {
        std::unique_lock<std::mutex> l(m);
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (aborted) return 0;
            uint64_t end = base + buf.size();
            if (pos < base) {
                // player atrasado em relacao ao descarte: sinaliza dessincronia
                underruns.fetch_add(1, std::memory_order_relaxed);
                return 0;
            }
            if (pos < end) {
                size_t n = (size_t)(end - pos < len ? end - pos : len);
                memcpy(dst, buf.data() + (size_t)(pos - base), n);
                readPos = pos + n;
                return (int)n;
            }
            // sondagem muito a frente
            if (pos - end > (1u << 20)) return 0;
            cv.wait_for(l, std::chrono::milliseconds(150));
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) return 0;
        }
    }
};
