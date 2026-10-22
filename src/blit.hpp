#pragma once
// NV12 -> BGRA com escala (vizinho mais proximo), rapido e em varias threads. Independente de plataforma (testavel no PC).
// Ideia: cada linha de ORIGEM e convertida uma unica vez para RGB (dois pixels por par de croma) e depois
// "espalhada" para a largura de destino por uma tabela (xmap). Isso evita converter 2M de pixels por quadro.
// A memoria de video e "write-combined": aqui so se ESCREVE nela, sempre em ordem (nada de ler de volta).
#include <stdint.h>
#include <string.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace blit {

static int g_yTab[256], g_rv[256], g_gu[256], g_gv[256], g_bu[256];
static uint8_t g_clamp[1024];                 // g_clamp[i + 256] = min(max(i, 0), 255), para i em [-256, 767]
static bool g_init = false;

inline void initTabs() {
    if (g_init) return;
    for (int i = 0; i < 256; i++) {
        int y = ((i - 16) * 298) >> 8; g_yTab[i] = y < 0 ? 0 : y;
        int c = i - 128;
        g_rv[i] = (409 * c) >> 8;             // BT.709 aproximado
        g_gu[i] = (100 * c) >> 8;
        g_gv[i] = (208 * c) >> 8;
        g_bu[i] = (516 * c) >> 8;
    }
    for (int i = 0; i < 1024; i++) { int v = i - 256; g_clamp[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
    g_init = true;
}

// Converte uma linha de origem (sw pixels) para RGB.
inline void convRow(const uint8_t *yr, const uint8_t *uvr, int sw, uint32_t *out) {
    const uint8_t *cl = g_clamp + 256;
    int x = 0;
    for (; x + 1 < sw; x += 2) {
        int u = uvr[x], v = uvr[x + 1];
        int rv = g_rv[v], gg = g_gu[u] + g_gv[v], bu = g_bu[u];
        int y0 = g_yTab[yr[x]], y1 = g_yTab[yr[x + 1]];
        out[x]     = 0xFF000000u | ((unsigned)cl[y0 + rv] << 16) | ((unsigned)cl[y0 - gg] << 8) | cl[y0 + bu];
        out[x + 1] = 0xFF000000u | ((unsigned)cl[y1 + rv] << 16) | ((unsigned)cl[y1 - gg] << 8) | cl[y1 + bu];
    }
    if (x < sw) { int y0 = g_yTab[yr[x]]; out[x] = 0xFF000000u | ((unsigned)cl[y0] << 16) | ((unsigned)cl[y0] << 8) | cl[y0]; }
}

struct Job {
    const uint8_t *Y = nullptr, *UV = nullptr;
    int sw = 0, sh = 0, pitch = 0;
    uint32_t *dst = nullptr; int dw = 0, dh = 0;
    const int *xmap = nullptr;
};

inline void doRows(const Job &j, int y0, int y1) {
    std::vector<uint32_t> row((size_t)j.sw + 2);
    int lastSy = -1;
    for (int y = y0; y < y1; y++) {
        int sy = (int)((long long)y * j.sh / j.dh);
        if (sy != lastSy) {
            convRow(j.Y + (size_t)sy * j.pitch, j.UV + (size_t)(sy >> 1) * j.pitch, j.sw, row.data());
            lastSy = sy;
        }
        uint32_t *o = j.dst + (size_t)y * j.dw;
        const uint32_t *r = row.data();
        if (j.dw == j.sw) memcpy(o, r, (size_t)j.dw * 4);
        else for (int x = 0; x < j.dw; x++) o[x] = r[j.xmap[x]];
    }
}

// Pool simples: (nthreads-1) trabalhadores fixos + a thread que chamou.
struct Pool {
    int n = 1;
    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable cv, cvDone;
    Job job; unsigned gen = 0; int pending = 0; bool quit = false;
    std::vector<int> xmap; int xmapW = 0, xmapS = 0;

    void start(int threads) {
        if (!th.empty()) return;
        n = threads < 1 ? 1 : threads;
        for (int i = 1; i < n; i++) th.emplace_back([this, i] { worker(i); });
    }
    void shutdown() {
        { std::lock_guard<std::mutex> l(m); quit = true; cv.notify_all(); }
        for (auto &t : th) if (t.joinable()) t.join();
        th.clear(); n = 1; quit = false;
    }
    void worker(int idx) {
        unsigned seen = 0;
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return quit || gen != seen; });
                if (quit) return;
                seen = gen; j = job;
            }
            doRows(j, j.dh * idx / n, j.dh * (idx + 1) / n);
            { std::lock_guard<std::mutex> l(m); if (--pending == 0) cvDone.notify_all(); }
        }
    }
    void run(const Job &j) {
        if (n <= 1) { doRows(j, 0, j.dh); return; }
        { std::lock_guard<std::mutex> l(m); job = j; pending = n - 1; gen++; cv.notify_all(); }
        doRows(j, 0, j.dh / n);
        std::unique_lock<std::mutex> l(m);
        cvDone.wait(l, [&] { return pending == 0; });
    }
};

inline Pool &pool() { static Pool p; return p; }

inline void nv12(const uint8_t *src, int sw, int sh, int pitch, int yRows, uint32_t *dst, int dw, int dh) {
    initTabs();
    Pool &p = pool();
    if (p.xmapW != dw || p.xmapS != sw) {
        p.xmap.resize(dw);
        for (int x = 0; x < dw; x++) p.xmap[x] = (int)((long long)x * sw / dw);
        p.xmapW = dw; p.xmapS = sw;
    }
    Job j; j.Y = src; j.UV = src + (size_t)pitch * yRows; j.sw = sw; j.sh = sh; j.pitch = pitch;
    j.dst = dst; j.dw = dw; j.dh = dh; j.xmap = p.xmap.data();
    p.run(j);
}

}  // namespace blit
