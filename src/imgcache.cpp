#include "imgcache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

// PS4/OpenOrbis: nada de thread_local (TLS no eboot derruba o app na abertura) nem de stdio dentro do stb (so decodificamos da memoria).
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

double ImgCache::nowSec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

bool imgDecodeToCover(const std::string &body, Cover &out, int tw, int th) {
    out = Cover{};
    if (body.size() < 24 || body.size() > 3 * 1024 * 1024) return false;
    const stbi_uc *data = (const stbi_uc *)body.data();
    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(data, (int)body.size(), &w, &h, &comp) || w <= 0 || h <= 0 || (long long)w * h > 6000000) return false;   // gigante demais: nao arrisca a memoria
    unsigned char *px = stbi_load_from_memory(data, (int)body.size(), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return false; }

    const uint32_t BG = 0xFF161C16u;
    out.w = tw; out.h = th;
    out.bgra.assign((size_t)tw * th, BG);

    // poster (2:3 aproximado): preenche cortando as sobras | outros formatos (logo, paisagem): cabe inteiro no meio
    double ar = (double)w / h, tar = (double)tw / th;
    bool contain = ar > 0.85 || ar < 0.5;
    int sx0 = 0, sy0 = 0, sw = w, sh = h, dx0 = 0, dy0 = 0, dw = tw, dh = th;
    if (!contain) {
        if (ar > tar) { sw = (int)(h * tar); sx0 = (w - sw) / 2; }
        else { sh = (int)(w / tar); sy0 = (h - sh) / 4; }       // corta mais embaixo que em cima (rostos ficam)
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
    } else {
        double sc = std::min(tw * 0.86 / w, th * 0.86 / h);
        dw = std::max(1, (int)(w * sc)); dh = std::max(1, (int)(h * sc));
        dx0 = (tw - dw) / 2; dy0 = (th - dh) / 2;
    }
    // reducao por media de blocos (nitida, sem serrilhado)
    for (int j = 0; j < dh; j++) {
        int ya = sy0 + (int)((long long)j * sh / dh), yb = sy0 + (int)((long long)(j + 1) * sh / dh);
        if (yb <= ya) yb = ya + 1;
        for (int i = 0; i < dw; i++) {
            int xa = sx0 + (int)((long long)i * sw / dw), xb = sx0 + (int)((long long)(i + 1) * sw / dw);
            if (xb <= xa) xb = xa + 1;
            unsigned long long sr = 0, sg = 0, sb = 0, sa = 0; unsigned cnt = 0;
            for (int y = ya; y < yb && y < h; y++) {
                const unsigned char *p = px + ((size_t)y * w + xa) * 4;
                for (int x = xa; x < xb && x < w; x++, p += 4) { unsigned a = p[3]; sr += p[0] * a; sg += p[1] * a; sb += p[2] * a; sa += a; cnt++; }
            }
            if (!cnt || !sa) continue;
            unsigned a = (unsigned)(sa / cnt);
            unsigned r = (unsigned)(sr / sa), g = (unsigned)(sg / sa), b = (unsigned)(sb / sa);
            uint32_t &d = out.bgra[(size_t)(dy0 + j) * tw + (dx0 + i)];
            unsigned inv = 255 - a;
            r = (r * a + ((d >> 16) & 255) * inv) / 255;
            g = (g * a + ((d >> 8) & 255) * inv) / 255;
            b = (b * a + (d & 255) * inv) / 255;
            d = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    stbi_image_free(px);
    out.ready = true;
    return true;
}

std::string ImgCache::diskPath(const std::string &url) {
    uint32_t h = 2166136261u, h2 = 5381u;
    for (unsigned char ch : url) { h ^= ch; h *= 16777619u; h2 = h2 * 33u + ch; }
    char b[64];
    snprintf(b, sizeof b, "/q%08x%08x.cv", h, h2);          // "q": formato compacto (RGB565); os formatos antigos sao ignorados
    return diskDir() + b;
}

bool ImgCache::diskHas(const std::string &url) {
    struct stat st;
    return stat(diskPath(url).c_str(), &st) == 0 && st.st_size > 16;
}

// arquivo: largura, altura (int32) + w*h pixels RGB565 (metade do tamanho do BGRA, e a diferenca nao aparece em poster)
bool ImgCache::loadDisk(const std::string &url, Cover &out) {
    FILE *f = fopen(diskPath(url).c_str(), "rb");
    if (!f) return false;
    int w = 0, h = 0;
    if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 || w != kThumbW || h != kThumbH) { fclose(f); return false; }
    std::vector<uint16_t> raw((size_t)w * h);
    size_t n = fread(raw.data(), 2, raw.size(), f);
    fclose(f);
    if (n != raw.size()) return false;
    out.w = w; out.h = h;
    out.bgra.resize(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        unsigned v = raw[i], r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        out.bgra[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    out.ready = true;
    return true;
}

void ImgCache::saveDisk(const std::string &url, const Cover &c) {
    if (!c.ready || c.bgra.empty()) return;
    mkdir(diskDir().c_str(), 0777);
    std::string path = diskPath(url), tmp = path + ".tmp";      // grava num temporario: arquivo pela metade nunca vira capa
    std::vector<uint16_t> raw(c.bgra.size());
    for (size_t i = 0; i < raw.size(); i++) { uint32_t p = c.bgra[i]; raw[i] = (uint16_t)((((p >> 16) & 255) >> 3) << 11 | (((p >> 8) & 255) >> 2) << 5 | ((p & 255) >> 3)); }
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(&c.w, 4, 1, f) == 1 && fwrite(&c.h, 4, 1, f) == 1 && fwrite(raw.data(), 2, raw.size(), f) == raw.size();
    ok = fclose(f) == 0 && ok;
    if (ok) rename(tmp.c_str(), path.c_str()); else remove(tmp.c_str());
}

void ImgCache::loadOne(const std::string &url, int worker) {
    Cover c;
    if (loadDisk(url, c)) { finish(url, std::move(c), true); return; }
    if (!fetch_) { finish(url, Cover{}, false); return; }
    std::string body;
    bool ok = fetch_(worker, url, body) && body.size() > 32 && body.size() < 3 * 1024 * 1024;
    if (ok) ok = imgDecodeToCover(body, c, kThumbW, kThumbH);
    if (ok) saveDisk(url, c);
    finish(url, std::move(c), ok);
}
