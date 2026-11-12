#pragma once
// AES-128 em modo CBC (so a decifragem) para HLS com "#EXT-X-KEY:METHOD=AES-128". Sem libs externas; testado contra o openssl.
#include <cstdint>
#include <cstring>
#include <string>

namespace aes128 {

struct Tables {
    uint8_t sbox[256], inv[256], m9[256], m11[256], m13[256], m14[256];
    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b) { uint8_t r = 0; while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; } return r; }
    Tables() {
        uint8_t p = 1, q = 1;                                  // gera a S-box (inverso multiplicativo + transformacao afim)
        do {
            p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
            if (q & 0x80) q ^= 0x09;
            auto rl = [](uint8_t v, int s) { return (uint8_t)((v << s) | (v >> (8 - s))); };
            sbox[p] = (uint8_t)(q ^ rl(q, 1) ^ rl(q, 2) ^ rl(q, 3) ^ rl(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; i++) inv[sbox[i]] = (uint8_t)i;
        for (int i = 0; i < 256; i++) { m9[i] = mul((uint8_t)i, 9); m11[i] = mul((uint8_t)i, 11); m13[i] = mul((uint8_t)i, 13); m14[i] = mul((uint8_t)i, 14); }
    }
};
inline const Tables &T() { static const Tables t; return t; }

struct Key {
    uint8_t rk[11][16];
    explicit Key(const uint8_t k[16]) {
        const Tables &t = T();
        uint8_t w[176]; memcpy(w, k, 16);
        uint8_t rc = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t tmp[4] = {w[i - 4], w[i - 3], w[i - 2], w[i - 1]};
            if (i % 16 == 0) {
                uint8_t a = tmp[0];
                tmp[0] = (uint8_t)(t.sbox[tmp[1]] ^ rc); tmp[1] = t.sbox[tmp[2]]; tmp[2] = t.sbox[tmp[3]]; tmp[3] = t.sbox[a];
                rc = Tables::xt(rc);
            }
            for (int j = 0; j < 4; j++) w[i + j] = (uint8_t)(w[i - 16 + j] ^ tmp[j]);
        }
        for (int r = 0; r < 11; r++) memcpy(rk[r], w + r * 16, 16);
    }
};

// decifra um bloco de 16 bytes
inline void decryptBlock(const Key &k, const uint8_t in[16], uint8_t out[16]) {
    const Tables &t = T();
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ k.rk[10][i]);
    for (int round = 9; round >= 0; round--) {
        uint8_t u[16];                                          // InvShiftRows + InvSubBytes
        for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) u[((c + r) & 3) * 4 + r] = t.inv[s[c * 4 + r]];
        for (int i = 0; i < 16; i++) u[i] ^= k.rk[round][i];    // AddRoundKey
        if (round == 0) { memcpy(s, u, 16); break; }
        for (int c = 0; c < 4; c++) {                           // InvMixColumns
            const uint8_t *a = u + c * 4;
            s[c * 4 + 0] = (uint8_t)(t.m14[a[0]] ^ t.m11[a[1]] ^ t.m13[a[2]] ^ t.m9[a[3]]);
            s[c * 4 + 1] = (uint8_t)(t.m9[a[0]] ^ t.m14[a[1]] ^ t.m11[a[2]] ^ t.m13[a[3]]);
            s[c * 4 + 2] = (uint8_t)(t.m13[a[0]] ^ t.m9[a[1]] ^ t.m14[a[2]] ^ t.m11[a[3]]);
            s[c * 4 + 3] = (uint8_t)(t.m11[a[0]] ^ t.m13[a[1]] ^ t.m9[a[2]] ^ t.m14[a[3]]);
        }
    }
    memcpy(out, s, 16);
}

// CBC + remocao do preenchimento PKCS#7 (como o HLS faz). Retorna false se o tamanho nao for multiplo de 16.
// 'strictPad' false: se o preenchimento for invalido, mantem os bytes (alguns servidores nao preenchem o ultimo bloco corretamente).
inline bool cbcDecrypt(const uint8_t key[16], const uint8_t iv[16], std::string &data, bool strictPad = false) {
    if (data.empty() || data.size() % 16 != 0) return false;
    Key k(key);
    uint8_t prev[16]; memcpy(prev, iv, 16);
    uint8_t *d = (uint8_t *)&data[0];
    for (size_t o = 0; o < data.size(); o += 16) {
        uint8_t c[16], p[16];
        memcpy(c, d + o, 16);
        decryptBlock(k, c, p);
        for (int i = 0; i < 16; i++) d[o + i] = (uint8_t)(p[i] ^ prev[i]);
        memcpy(prev, c, 16);
    }
    uint8_t pad = d[data.size() - 1];
    bool ok = pad >= 1 && pad <= 16 && pad <= data.size();
    if (ok) for (size_t i = data.size() - pad; i < data.size(); i++) if (d[i] != pad) { ok = false; break; }
    if (ok) data.resize(data.size() - pad);
    else if (strictPad) return false;
    return true;
}

}  // namespace aes128
