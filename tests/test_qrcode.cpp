// QR Code: estrutura (localizadores, temporizacao, tamanho por versao) e uma impressao digital do desenho
// (os QR de exemplo foram conferidos com um leitor de verdade antes de gravar os valores).
#include "../src/qrcode.hpp"
#include <cassert>
#include <cstdio>

static uint32_t fnv(const QrCode &q) { uint32_t h = 2166136261u; for (uint8_t v : q.m) { h ^= v; h *= 16777619u; } return h; }

static void checkStructure(const QrCode &q) {
    int n = q.size;
    assert(n == 21 + 4 * (q.version - 1));
    // 3 localizadores (7x7: borda escura, anel claro, centro escuro 3x3)
    int cx[3] = {0, n - 7, 0}, cy[3] = {0, 0, n - 7};
    for (int k = 0; k < 3; k++)
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 7; x++) {
                bool edge = x == 0 || y == 0 || x == 6 || y == 6, core = x >= 2 && x <= 4 && y >= 2 && y <= 4;
                assert(q.dark(cx[k] + x, cy[k] + y) == (edge || core));
            }
    for (int i = 8; i < n - 8; i++) { assert(q.dark(6, i) == (i % 2 == 0)); assert(q.dark(i, 6) == (i % 2 == 0)); }   // temporizacao
    assert(q.dark(8, n - 8));                                                                                         // modulo escuro fixo
    for (int i = 0; i < 7; i++) assert(!q.dark(7, i) && !q.dark(i, 7));                                              // separador claro ao redor do localizador
}

int main() {
    QrCode q;
    assert(!qrEncode("", q));
    std::string big(400, 'x'); assert(!qrEncode(big, q));                                   // grande demais (passa do limite)
    assert(qrEncode("a", q) && q.version == 1 && q.size == 21); checkStructure(q);
    assert(qrEncode(std::string(14, 'a'), q) && q.version == 1);                            // 14 bytes cabem na versao 1-M
    assert(qrEncode(std::string(15, 'a'), q) && q.version == 2); checkStructure(q);
    assert(qrEncode("https://livepix.gg/exemplo", q) && q.version == 2 && q.size == 25); checkStructure(q);
    assert(qrEncode(std::string(100, 'z'), q) && q.version == 6); checkStructure(q);
    assert(qrEncode(std::string(200, 'z'), q) && q.version == 10 && q.size == 57); checkStructure(q);
    assert(qrEncode(std::string(230, 'z'), q) && q.version == 11 && q.size == 61); checkStructure(q);
    // impressao digital (mudou = o QR mudou; reconfira com um leitor de QR antes de atualizar o valor)
    QrCode a, b;
    assert(qrEncode("https://livepix.gg/exemplo", a) && qrEncode("https://livepix.gg/exemplo", b) && a.m == b.m);   // deterministico
    assert(qrEncode("https://livepix.gg/outro", b) && a.m != b.m);
    assert(fnv(a) == 0x7e7233e1u);                                                      // conferido com leitor de QR
    puts("OK");
    return 0;
}
