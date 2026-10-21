#pragma once
// Buffer deslizante para alimentar o AvPlayer (callback "readOffset"): uma thread grava
// (append), o player le por posicao absoluta. Independente de plataforma.
#include <stdint.h>
#include <string.h>
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
    size_t softCap = 16u << 20, hardCap = 48u << 20;

    void reset() { std::lock_guard<std::mutex> l(m); buf.clear(); base = readPos = 0; aborted = false; }
    void abort() { std::lock_guard<std::mutex> l(m); aborted = true; cv.notify_all(); }
    uint64_t total() { std::lock_guard<std::mutex> l(m); return base + buf.size(); }

    void append(const uint8_t *p, size_t n) {
        std::lock_guard<std::mutex> l(m);
        buf.insert(buf.end(), p, p + n);
        if (buf.size() > softCap) {
            uint64_t keep = readPos > (1u << 20) ? readPos - (1u << 20) : 0;     // guarda 1MB atras do leitor
            uint64_t drop = keep > base ? keep - base : 0;
            if (buf.size() - drop > hardCap) drop = buf.size() - hardCap;       // leitor parado: descarta mesmo assim
            if (drop > buf.size()) drop = buf.size();
            if (drop) { buf.erase(buf.begin(), buf.begin() + (size_t)drop); base += drop; }
        }
        cv.notify_all();
    }

    // Le ate len bytes a partir de pos. Bloqueia (ate timeoutMs) se pos == fim atual.
    // Retorna 0 se: abortado, pos fora da janela, pos muito alem do fim (sondagem de tamanho) ou timeout.
    int read(uint8_t *dst, uint64_t pos, uint32_t len, int timeoutMs = 30000) {
        std::unique_lock<std::mutex> l(m);
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (aborted) return 0;
            uint64_t end = base + buf.size();
            if (pos < base) return 0;
            if (pos < end) {
                size_t n = (size_t)(end - pos < len ? end - pos : len);
                memcpy(dst, buf.data() + (size_t)(pos - base), n);
                readPos = pos + n;
                return (int)n;
            }
            if (pos - end > (1u << 20)) return 0;
            cv.wait_for(l, std::chrono::milliseconds(200));
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs)) return 0;
        }
    }
};
