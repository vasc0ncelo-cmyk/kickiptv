#include "../src/blit.hpp"
#include <cassert>
#include <iostream>
#include <vector>

// Monta um quadro NV12 sintetico "de verdade" (gradientes + detalhes) com o layout dado e testa a deteccao de stride.
static std::vector<uint8_t> make(int w, int h, int pitch, int yRows) {
    std::vector<uint8_t> b((size_t)pitch * yRows + (size_t)pitch * ((h + 1) / 2) + 64, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) b[(size_t)y * pitch + x] = (uint8_t)(16 + ((x * 3 + y) % 200) + ((x / 7 + y / 5) % 13));
    for (int y = 0; y < h / 2; y++) for (int x = 0; x < w; x++) b[(size_t)pitch * yRows + (size_t)y * pitch + x] = (uint8_t)(100 + ((x / 2 * 5 + y * 2) % 50));
    return b;
}
// devolve o layout (pitch, yRows) que a deteccao escolheu para um quadro montado com 'mode'
static bool run(int w, int h, int mode, int expectPitch, int expectRows) {
    int p, r; blit::strideCandidate(mode, w, h, p, r);
    auto b = make(w, h, p, r);
    int d = blit::detectStride(b.data(), b.size(), w, h);
    if (d < 0) return false;
    int dp, dr; blit::strideCandidate(d, w, h, dp, dr);
    return dp == expectPitch && dr == expectRows;
}
int main() {
    assert(run(1280, 720, 0, 1280, 720));        // 1280x720: layout igual em todos os modos
    assert(run(864, 480, 3, 1024, 480));         // caso do video: 864 alinhado a 256 -> 1024
    assert(run(864, 480, 2, 896, 480));          // 864 alinhado a 64 -> 896
    assert(run(864, 480, 0, 864, 480));          // sem alinhamento
    assert(run(1920, 1080, 1, 1920, 1088));      // altura 1088 no plano Y
    assert(run(852, 480, 3, 1024, 480));
    assert(run(1920, 1080, 3, 2048, 1088));
    // quadro todo preto: nao ha como decidir
    std::vector<uint8_t> z(2048 * 1100 * 2, 0);
    assert(blit::detectStride(z.data(), z.size(), 864, 480) == -1);
    // buffer curto: so os modos que cabem sao avaliados
    auto b = make(864, 480, 864, 480);
    int d = blit::detectStride(b.data(), 864 * 480 * 3 / 2, 864, 480);
    assert(d == 1);
    std::cout << "OK\n";
}
