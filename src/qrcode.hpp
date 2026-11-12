#pragma once
// Gerador de QR Code (modo bytes, correcao de erro M, versoes 1 a 12 = ate ~230 caracteres). Sem dependencias.
// Uso: QrCode q; if (qrEncode("https://...", q)) { for y,x: q.dark(x, y) }
#include <cstdint>
#include <string>
#include <vector>

struct QrCode {
    int size = 0;                       // lado em modulos (21 + 4*(versao-1))
    int version = 0;
    std::vector<uint8_t> m;             // 1 = escuro
    bool dark(int x, int y) const { return x >= 0 && y >= 0 && x < size && y < size && m[(size_t)y * size + x]; }
};

namespace qrimpl {
// correcao de erro nivel M, indice = versao (1..12)
static const int kEccPerBlock[13] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22};
static const int kNumBlocks[13]   = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8};

inline int rawModules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) { int na = ver / 7 + 2; r -= (25 * na - 10) * na - 55; if (ver >= 7) r -= 36; }
    return r;
}
inline int dataCodewords(int ver) { return rawModules(ver) / 8 - kEccPerBlock[ver] * kNumBlocks[ver]; }

inline uint8_t gfMul(uint8_t x, uint8_t y) {          // GF(256), polinomio 0x11D
    int z = 0;
    for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
    return (uint8_t)z;
}
inline std::vector<uint8_t> rsDivisor(int degree) {
    std::vector<uint8_t> res((size_t)degree, 0);
    res[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) { res[j] = gfMul(res[j], root); if (j + 1 < degree) res[j] ^= res[j + 1]; }
        root = gfMul(root, 0x02);
    }
    return res;
}
inline std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &div) {
    std::vector<uint8_t> res(div.size(), 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ res[0];
        res.erase(res.begin()); res.push_back(0);
        for (size_t i = 0; i < div.size(); i++) res[i] ^= gfMul(div[i], f);
    }
    return res;
}

struct Builder {
    int ver, size;
    std::vector<uint8_t> mod, fn;       // modulos e mapa de "funcao" (nao recebe dados)
    Builder(int v) : ver(v), size(v * 4 + 17), mod((size_t)size * size, 0), fn((size_t)size * size, 0) {}
    void set(int x, int y, bool d) { mod[(size_t)y * size + x] = d; fn[(size_t)y * size + x] = 1; }
    bool get(int x, int y) const { return mod[(size_t)y * size + x]; }

    void finder(int cx, int cy) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                int d = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
                set(x, y, d != 2 && d != 4);
            }
    }
    void align(int cx, int cy) {
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++) { int d = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy); set(cx + dx, cy + dy, d != 1); }
    }
    std::vector<int> alignPos() const {
        std::vector<int> r;
        if (ver == 1) return r;
        int na = ver / 7 + 2, step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
        r.assign((size_t)na, 0);
        r[0] = 6;
        for (int i = na - 1, pos = size - 7; i >= 1; i--, pos -= step) r[i] = pos;
        return r;
    }
    void formatBits(int mask) {          // nivel M = 00
        int data = (0 << 3) | mask, rem = data;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        int bits = ((data << 10) | rem) ^ 0x5412;
        auto b = [&](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; i++) set(8, i, b(i));
        set(8, 7, b(6)); set(8, 8, b(7)); set(7, 8, b(8));
        for (int i = 9; i < 15; i++) set(14 - i, 8, b(i));
        for (int i = 0; i < 8; i++) set(size - 1 - i, 8, b(i));
        for (int i = 8; i < 15; i++) set(8, size - 15 + i, b(i));
        set(8, size - 8, true);
    }
    void versionBits() {
        if (ver < 7) return;
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        int bits = (ver << 12) | rem;
        for (int i = 0; i < 18; i++) {
            bool bit = ((bits >> i) & 1) != 0;
            int a = size - 11 + i % 3, b = i / 3;
            set(a, b, bit); set(b, a, bit);
        }
    }
    void functionPatterns() {
        for (int i = 0; i < size; i++) { set(6, i, i % 2 == 0); set(i, 6, i % 2 == 0); }
        finder(3, 3); finder(size - 4, 3); finder(3, size - 4);
        auto ap = alignPos();
        int n = (int)ap.size();
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;
                align(ap[i], ap[j]);
            }
        formatBits(0);
        versionBits();
    }
    void placeData(const std::vector<uint8_t> &cw) {
        size_t i = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;
            for (int vert = 0; vert < size; vert++)
                for (int j = 0; j < 2; j++) {
                    int x = right - j;
                    bool up = ((right + 1) & 2) == 0;
                    int y = up ? size - 1 - vert : vert;
                    if (!fn[(size_t)y * size + x] && i < cw.size() * 8) { mod[(size_t)y * size + x] = ((cw[i >> 3] >> (7 - (i & 7))) & 1) != 0; i++; }
                }
        }
    }
    void applyMask(int mask) {
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                bool inv = false;
                switch (mask) {
                case 0: inv = (x + y) % 2 == 0; break;
                case 1: inv = y % 2 == 0; break;
                case 2: inv = x % 3 == 0; break;
                case 3: inv = (x + y) % 3 == 0; break;
                case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                }
                if (inv && !fn[(size_t)y * size + x]) mod[(size_t)y * size + x] ^= 1;
            }
    }
    int penalty() const {
        int res = 0;
        auto runs = [&](bool horiz) {
            for (int a = 0; a < size; a++) {
                int run = 1;
                for (int b = 1; b < size; b++) {
                    bool cur = horiz ? get(b, a) : get(a, b), prev = horiz ? get(b - 1, a) : get(a, b - 1);
                    if (cur == prev) { run++; if (run == 5) res += 3; else if (run > 5) res++; } else run = 1;
                }
                // padrao 1:1:3:1:1 (parecido com o localizador)
                for (int b = 0; b + 11 <= size; b++) {
                    static const int p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                    bool m1 = true, m2 = true;
                    for (int k = 0; k < 11; k++) { int v = horiz ? get(b + k, a) : get(a, b + k); if (v != p1[k]) m1 = false; if (v != p2[k]) m2 = false; }
                    if (m1) res += 40;
                    if (m2) res += 40;
                }
            }
        };
        runs(true); runs(false);
        for (int y = 0; y + 1 < size; y++)
            for (int x = 0; x + 1 < size; x++) { bool c = get(x, y); if (c == get(x + 1, y) && c == get(x, y + 1) && c == get(x + 1, y + 1)) res += 3; }
        int dark = 0; for (uint8_t v : mod) dark += v;
        int total = size * size, k = 0;
        int diff = dark * 20 - total * 10; if (diff < 0) diff = -diff;
        while (diff > total) { diff -= total; k++; }   // passos de 5% de distancia de 50%
        // (equivale a ceil(|dark*20 - total*10| / total) - 1)
        res += (k > 0 ? k : 0) * 10;
        return res;
    }
};
}  // namespace qrimpl

inline bool qrEncode(const std::string &text, QrCode &out) {
    using namespace qrimpl;
    int ver = 1;
    for (; ver <= 12; ver++) {
        int cap = dataCodewords(ver) * 8, ccbits = ver < 10 ? 8 : 16;
        if (4 + ccbits + (int)text.size() * 8 <= cap) break;
    }
    if (ver > 12 || text.empty()) return false;
    // bits de dados
    std::vector<uint8_t> bits;
    auto put = [&](unsigned v, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((v >> i) & 1); };
    put(4, 4); put((unsigned)text.size(), ver < 10 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    size_t cap = (size_t)dataCodewords(ver) * 8;
    for (int i = 0; i < 4 && bits.size() < cap; i++) bits.push_back(0);
    while (bits.size() % 8) bits.push_back(0);
    std::vector<uint8_t> data(cap / 8, 0);
    for (size_t i = 0; i < bits.size(); i++) data[i >> 3] |= (uint8_t)(bits[i] << (7 - (i & 7)));
    for (size_t i = bits.size() / 8, pad = 0xEC; i < data.size(); i++, pad ^= (0xEC ^ 0x11)) data[i] = (uint8_t)pad;
    // blocos + Reed-Solomon + intercalacao
    int nb = kNumBlocks[ver], ecc = kEccPerBlock[ver], raw = rawModules(ver) / 8;
    int numShort = nb - raw % nb, shortLen = raw / nb;
    auto div = rsDivisor(ecc);
    std::vector<std::vector<uint8_t>> blocks;
    size_t k = 0;
    for (int i = 0; i < nb; i++) {
        int len = shortLen - ecc + (i < numShort ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + len); k += len;
        auto e = rsRemainder(d, div);
        if (i < numShort) d.push_back(0);                  // espaco vazio para alinhar blocos curtos
        d.insert(d.end(), e.begin(), e.end());
        blocks.push_back(d);
    }
    std::vector<uint8_t> all;
    for (size_t i = 0; i < blocks[0].size(); i++)
        for (int j = 0; j < nb; j++)
            if (i != (size_t)(shortLen - ecc) || j >= numShort) all.push_back(blocks[j][i]);
    // monta a matriz e escolhe a melhor mascara
    Builder b(ver);
    b.functionPatterns();
    b.placeData(all);
    int bestMask = 0, bestPen = 1 << 30;
    for (int m = 0; m < 8; m++) {
        b.applyMask(m); b.formatBits(m);
        int p = b.penalty();
        if (p < bestPen) { bestPen = p; bestMask = m; }
        b.applyMask(m);                                    // desfaz
    }
    b.applyMask(bestMask); b.formatBits(bestMask);
    out.size = b.size; out.version = ver; out.m = b.mod;
    return true;
}
