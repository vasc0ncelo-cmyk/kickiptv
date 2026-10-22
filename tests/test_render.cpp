// Testes do renderizador de software (PC): o desenho rapido tem que dar o mesmo resultado do original.
#include "../src/keyboard.hpp"
#include "../src/fbrender.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>

// FbRenderer sem o atalho do brilho: forca o caminho antigo (camadas sobrepostas) para comparar.
struct SlowGlow : FbRenderer {
    using FbRenderer::FbRenderer;
    bool glow(int, int, int, int, int, unsigned, int, int, bool) override { return false; }
};
static int maxDiff(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, int x0, int y0, int x1, int y1, int W) {
    int m = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint32_t p = a[(size_t)y * W + x], q = b[(size_t)y * W + x];
        for (int sh = 0; sh <= 16; sh += 8) { int d = (int)((p >> sh) & 255) - (int)((q >> sh) & 255); if (d < 0) d = -d; if (d > m) m = d; }
    }
    return m;
}

int main() {
    // blendPx == arredondamento exato de v/255
    for (int a = 0; a <= 255; a += 5)
        for (int d = 0; d < 256; d += 17)
            for (int s = 0; s < 256; s += 23) {
                uint32_t out = blendPx(0xFF000000u | (d << 16) | (d << 8) | d, 0xFF000000u | (s << 16) | (s << 8) | s, a);
                int want = (s * a + d * (255 - a) + 127) / 255;
                int got = out & 255;
                assert(got == want || got == want + 1 || got == want - 1);
                assert(((out >> 16) & 255) == (unsigned)got && ((out >> 8) & 255) == (unsigned)got && (out >> 24) == 0xFF);
            }
    const int W = 640, H = 480;
    // brilho rapido ~= brilho por camadas (diferenca so de arredondamento)
    for (int inside = 0; inside < 2; inside++) {
        std::vector<uint32_t> fast((size_t)W * H, 0xFF000000u), slow((size_t)W * H, 0xFF000000u);
        { FbRenderer r(fast.data(), W, H); uiGlow(r, 150, 120, 300, 180, 24, theme::GREEN, 18, 130, inside != 0); }
        { SlowGlow r(slow.data(), W, H); uiGlow(r, 150, 120, 300, 180, 24, theme::GREEN, 18, 130, inside != 0); }
        // fora do retangulo: sempre igual (no maximo ~6 niveis de diferenca)
        assert(maxDiff(fast, slow, 100, 70, 500, 120 - 1, W) <= 6);
        assert(maxDiff(fast, slow, 100, 70, 150 - 1, 350, W) <= 6);
        assert(maxDiff(fast, slow, 460, 70, 500, 350, W) <= 6);
        if (inside) assert(maxDiff(fast, slow, 200, 160, 400, 260, W) <= 6);
    }
    // fundo em cache == fundo desenhado direto
    {
        std::vector<uint32_t> a((size_t)W * H), b((size_t)W * H);
        { FbRenderer r(a.data(), W, H); r.buildingBg = true; g_fbTrunc = true; uiBackground(r); g_fbTrunc = false; }
        { FbRenderer r(b.data(), W, H); uiBackground(r); }
        { FbRenderer r(b.data(), W, H); for (auto &p : b) p = 0x12345678; uiBackground(r); }   // 2a vez: copia do cache
        assert(a == b);
    }
    // texto nas bordas / fora da tela nao pode escrever fora do buffer
    {
        std::vector<uint32_t> g((size_t)W * H + 4096, 0xAABBCCDDu);
        FbRenderer r(g.data(), W, H);
        r.drawText(-30, -10, "Texto cortado", 0xFFFFFFFF, 36); r.drawText(W - 20, H - 8, "Fim da tela", 0x80FFFFFF, 28);
        r.drawText(W + 50, 10, "fora", 0xFFFFFFFF, 20); r.drawText(5, 5, "çãõ ÂÉ ?", 0xFFFFFFFF, 18);
        for (size_t i = (size_t)W * H; i < g.size(); i++) assert(g[i] == 0xAABBCCDDu);
        bool any = false; for (int x = 5; x < 100; x++) for (int y = 5; y < 25; y++) if (g[(size_t)y * W + x] != 0xAABBCCDDu) any = true;
        assert(any);
    }
    std::cout << "OK\n";
}
