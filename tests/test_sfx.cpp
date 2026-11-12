// Sons da interface: tamanho em blocos, volume contido, silencio no fim, todos os tipos geram audio.
#include "../src/sfx.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
int main() {
    for (int k = 0; k < sfx::Count; k++) {
        std::vector<int16_t> p = sfx::render(k);
        assert(!p.empty() && p.size() % 512 == 0);                   // blocos de 256 quadros estereo
        int peak = 0; long long energy = 0;
        for (int16_t s : p) { int a = std::abs((int)s); if (a > peak) peak = a; energy += (long long)a; }
        assert(peak > 500 && peak < 32767 * 0.30);                   // audivel mas discreto
        assert(energy > 100000);
        for (size_t i = p.size() - 512; i < p.size(); i++) assert(p[i] == 0);   // termina em silencio
        assert(sfx::render(k) == p);                                 // deterministico
    }
    assert(sfx::render(sfx::Boot).size() > sfx::render(sfx::Nav).size());
    assert(sfx::render(sfx::Nav, 0.0f)[10] == 0);                    // volume 0 = mudo
    std::cout << "test_sfx OK\n";
}
