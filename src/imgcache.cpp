#include "imgcache.hpp"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool imgDecodeToCover(const std::string &body, Cover &out, int tw, int th) {
    out = Cover{};
    if (body.size() < 24 || body.size() > 2 * 1024 * 1024) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(
        (const stbi_uc *)body.data(), (int)body.size(), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    out.w = tw; out.h = th;
    out.bgra.resize((size_t)tw * (size_t)th);
    for (int y = 0; y < th; y++) {
        int sy = y * h / th;
        for (int x = 0; x < tw; x++) {
            int sx = x * w / tw;
            const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
            out.bgra[(size_t)y * tw + x] =
                ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | (unsigned)p[2];
        }
    }
    stbi_image_free(px);
    out.ready = true;
    return true;
}

std::string ImgCache::diskPath(const std::string &url) {
    uint32_t h = 2166136261u;
    for (unsigned char c : url) { h ^= c; h *= 16777619u; }
    char b[64];
    snprintf(b, sizeof b, "/data/kickiptv_cov/%08x.cov", h);
    return b;
}

bool ImgCache::loadDisk(const std::string &url, Cover &out) {
    FILE *f = fopen(diskPath(url).c_str(), "rb");
    if (!f) return false;
    int w = 0, h = 0;
    if (fread(&w, 4, 1, f) != 1 || fread(&h, 4, 1, f) != 1 ||
        w != kThumbW || h != kThumbH) { fclose(f); return false; }
    out.w = w; out.h = h;
    out.bgra.resize((size_t)w * h);
    size_t n = fread(out.bgra.data(), 4, out.bgra.size(), f);
    fclose(f);
    if (n != out.bgra.size()) return false;
    out.ready = true;
    return true;
}

void ImgCache::saveDisk(const std::string &url, const Cover &c) {
    if (!c.ready || c.bgra.empty()) return;
    mkdir("/data/kickiptv_cov", 0777);
    FILE *f = fopen(diskPath(url).c_str(), "wb");
    if (!f) return;
    fwrite(&c.w, 4, 1, f);
    fwrite(&c.h, 4, 1, f);
    fwrite(c.bgra.data(), 4, c.bgra.size(), f);
    fclose(f);
}

void ImgCache::loadOne(const std::string &url) {
    Cover c;
    if (loadDisk(url, c)) {
        std::lock_guard<std::mutex> l(m_);
        map_[url] = std::move(c);
        return;
    }
    if (!fetch_) {
        std::lock_guard<std::mutex> l(m_);
        map_[url].failed = true;
        return;
    }
    std::string body;
    bool ok = false;
    try {
        ok = fetch_(url, body) && body.size() > 32 && body.size() < 2 * 1024 * 1024;
        if (ok) ok = imgDecodeToCover(body, c, kThumbW, kThumbH);
    } catch (...) { ok = false; }
    if (ok) saveDisk(url, c);
    {
        std::lock_guard<std::mutex> l(m_);
        auto &slot = map_[url];
        if (ok) slot = std::move(c);
        else { slot.failed = true; slot.ready = false; }
    }
}
