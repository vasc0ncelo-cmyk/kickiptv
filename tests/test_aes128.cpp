// Confere a decifragem AES-128-CBC contra o openssl (vetores gerados na hora) e o parser de #EXT-X-KEY.
#include "../src/aes128.hpp"
#include "../src/hls.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

static std::string hexs(const unsigned char *b, int n) { std::string s; char t[3]; for (int i = 0; i < n; i++) { snprintf(t, 3, "%02x", b[i]); s += t; } return s; }

int main() {
    // vetor oficial FIPS-197: chave 000102..0f, bloco cifrado 69c4e0d86a7b0430d8cdb78070b4c55a -> 00112233445566778899aabbccddeeff
    {
        uint8_t key[16], ct[16] = {0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a}, pt[16];
        for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
        aes128::decryptBlock(aes128::Key(key), ct, pt);
        assert(hexs(pt, 16) == "00112233445566778899aabbccddeeff");
    }
    // contra o openssl (se existir): dados aleatorios de varios tamanhos
    if (system("openssl version > /dev/null 2>&1") == 0) {
        for (int size : {1, 15, 16, 17, 188, 1880, 100000}) {
            std::string plain(size, 0); for (auto &c : plain) c = (char)(rand() & 255);
            { std::ofstream f("/tmp/aes_plain.bin", std::ios::binary); f.write(plain.data(), plain.size()); }
            unsigned char key[16], iv[16]; for (int i = 0; i < 16; i++) { key[i] = (unsigned char)(rand() & 255); iv[i] = (unsigned char)(rand() & 255); }
            std::string cmd = "openssl enc -aes-128-cbc -K " + hexs(key, 16) + " -iv " + hexs(iv, 16) + " -in /tmp/aes_plain.bin -out /tmp/aes_enc.bin";
            assert(system(cmd.c_str()) == 0);
            std::ifstream f("/tmp/aes_enc.bin", std::ios::binary);
            std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            assert(enc.size() % 16 == 0);
            assert(aes128::cbcDecrypt(key, iv, enc, true));
            assert(enc == plain);
        }
        std::cout << "openssl: OK\n";
    }
    assert(!([] { std::string s(15, 'x'); unsigned char k[16] = {0}, iv[16] = {0}; return aes128::cbcDecrypt(k, iv, s); })());

    // playlist com chave: IV explicito, depois a chave troca e fica sem IV; depois NONE
    std::string m =
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://k.com/key1\",IV=0x000102030405060708090A0B0C0D0E0F\n#EXTINF:6,\na.ts\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"key2.bin\"\n#EXTINF:6,\nb.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\nc.ts\n"
        "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"x\"\n#EXTINF:6,\nd.ts\n";
    auto md = hlsParseMedia(m);
    assert(md.segs.size() == 4);
    assert(md.segs[0].keyMethod == "AES-128" && md.segs[0].keyUri == "https://k.com/key1" && md.segs[0].hasIv && md.segs[0].iv[0] == 0 && md.segs[0].iv[15] == 0x0F);
    assert(md.segs[1].keyUri == "key2.bin" && !md.segs[1].hasIv && md.segs[1].seq == 11);
    assert(md.segs[2].keyMethod.empty());
    assert(md.segs[3].keyMethod == "SAMPLE-AES");
    std::cout << "OK\n";
}
