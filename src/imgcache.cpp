#include "imgcache.hpp"
#include <algorithm>
#include <cstring>

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
    if (body.size() < 24) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char *px = stbi_load_from_memory(
        (const stbi_uc *)body.data(), (int)body.size(), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    // escala para thumb (nearest) e converte RGBA -> ARGB uint32 (0xAARRGGBB)
    out.w = tw; out.h = th;
    out.bgra.resize((size_t)tw * (size_t)th);
    for (int y = 0; y < th; y++) {
        int sy = y * h / th;
        for (int x = 0; x < tw; x++) {
            int sx = x * w / tw;
            const unsigned char *p = px + ((size_t)sy * w + sx) * 4;
            // PS4 renderer usa ARGB; stb entrega RGBA
            unsigned a = p[3], r = p[0], g = p[1], b = p[2];
            out.bgra[(size_t)y * tw + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    stbi_image_free(px);
    out.ready = true;
    return true;
}

void ImgCache::loadOne(const std::string &url) {
    Cover c;
    if (!fetch_) {
        std::lock_guard<std::mutex> l(m_);
        map_[url].failed = true;
        return;
    }
    std::string body;
    bool ok = false;
    try {
        ok = fetch_(url, body) && !body.empty() && body.size() < 4 * 1024 * 1024;
    } catch (...) {
        ok = false;
    }
    if (ok) ok = imgDecodeToCover(body, c, kThumbW, kThumbH);
    {
        std::lock_guard<std::mutex> l(m_);
        auto &slot = map_[url];
        if (ok) slot = std::move(c);
        else { slot.failed = true; slot.ready = false; }
    }
}
