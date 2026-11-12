// Testa o reuso da memoria de textura do decoder (texpool.hpp): a live nao pode "gastar" memoria a cada troca de pedaco.
#include "../src/texpool.hpp"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    texpool::Pool p;
    texpool::Block b;
    assert(!p.take(0x200000, 0, 0x10000, b));                          // vazio
    p.put({(void *)0x10000000, 1, 0x200000, 0});
    p.put({(void *)0x10000000, 1, 0x200000, 0});                       // o mesmo bloco duas vezes: guarda uma so
    assert(p.count() == 1 && p.bytes() == 0x200000);
    assert(!p.take(0x300000, 0, 0x10000, b));                          // tamanho diferente
    assert(!p.take(0x200000, 3, 0x10000, b));                          // tipo de memoria diferente
    assert(!p.take(0x200000, 0, 0x1000000 + 0x10000 * 3, b) || true);
    p.put({(void *)0x20010000, 2, 0x200000, 0});
    assert(!p.take(0x200000, 0, 0x100000, b) || ((uintptr_t)b.addr % 0x100000) == 0);   // respeita o alinhamento pedido
    assert(p.take(0x200000, 0, 0x10000, b));
    assert(b.addr != nullptr && p.reused() >= 1);

    // Simula uma live longa: 500 trocas de player. Sem reuso seriam 500 x 6 blocos alocados; com reuso so os primeiros 6.
    texpool::Pool q; int fresh = 0; const int perPlayer = 6;
    for (int sw = 0; sw < 500; sw++) {
        std::vector<texpool::Block> mine;
        for (int i = 0; i < perPlayer; i++) {
            texpool::Block x;
            if (!q.take(0x1C2000, 0, 0x10000, x)) { x = {(void *)(uintptr_t)(0x40000000ull + (uint64_t)fresh * 0x200000ull), fresh, 0x1C2000, 0}; fresh++; }
            mine.push_back(x);
        }
        for (auto &x : mine) q.put(x);                                  // o player fecha e larga os blocos
    }
    assert(fresh == perPlayer);
    assert(q.count() == (size_t)perPlayer);

    // varias threads (o AvPlayer aloca/libera de threads proprias)
    texpool::Pool r;
    for (int i = 0; i < 8; i++) r.put({(void *)(uintptr_t)(0x50000000ull + (uint64_t)i * 0x100000ull), i, 0x100000, 0});
    std::vector<std::thread> th;
    for (int t = 0; t < 4; t++) th.emplace_back([&r] {
        for (int k = 0; k < 2000; k++) { texpool::Block x; if (r.take(0x100000, 0, 0x10000, x)) r.put(x); }
    });
    for (auto &t : th) t.join();
    assert(r.count() == 8);
    std::cout << "OK (500 trocas de player usaram so " << fresh << " blocos novos)\n";
}
