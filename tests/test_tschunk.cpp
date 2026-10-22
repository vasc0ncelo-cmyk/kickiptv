// Cortador de MPEG-TS continuo + conversao TS->MP4 dos pedacos.
#include "../src/tschunk.hpp"
#include <cassert>
#include <iostream>
#include <vector>

static std::string pkt(int pid, bool pusi, const std::vector<uint8_t> &payload, int cc) {
    std::string s(188, (char)0xFF);
    s[0] = 0x47; s[1] = (char)((pusi ? 0x40 : 0) | (pid >> 8)); s[2] = (char)(pid & 255); s[3] = (char)(0x10 | (cc & 15));
    for (size_t i = 0; i < payload.size() && 4 + i < 188; i++) s[4 + i] = (char)payload[i];
    return s;
}
static std::string pat(int pmt) {
    std::vector<uint8_t> v = {0, 0, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, (uint8_t)(0xE0 | (pmt >> 8)), (uint8_t)(pmt & 255), 0, 0, 0, 0};
    return pkt(0, true, v, 0);
}
static std::string pmtPkt(int pmt, int vpid) {
    std::vector<uint8_t> v = {0, 2, 0xB0, 18, 0, 1, 0xC1, 0, 0, (uint8_t)(0xE0 | (vpid >> 8)), (uint8_t)(vpid & 255), 0xF0, 0, 0x1B, (uint8_t)(0xE0 | (vpid >> 8)), (uint8_t)(vpid & 255), 0xF0, 0, 0, 0, 0, 0};
    return pkt(pmt, true, v, 0);
}
static std::string video(int vpid, bool key, int cc) {       // inicio de PES com AUD + (SPS+PPS+IDR | slice)
    std::vector<uint8_t> v = {0, 0, 1, 0xE0, 0, 0, 0x80, 0, 0, 0, 0, 1, 9, 0xF0, 0, 0, 1, (uint8_t)(key ? 0x67 : 0x41), 1, 2, 3};
    return pkt(vpid, true, v, cc);
}
static std::string filler(int vpid, int cc) { return pkt(vpid, false, {1, 2, 3}, cc); }

int main() {
    const int PMT = 0x100, V = 0x101;
    std::string stream;
    int cc = 0;
    for (int gop = 0; gop < 10; gop++) {                     // 10 GOPs de 1 s
        stream += pat(PMT) + pmtPkt(PMT, V);
        stream += video(V, true, cc++);
        for (int f = 0; f < 24; f++) { stream += video(V, false, cc++); stream += filler(V, cc++); }
    }
    std::string junk = std::string("\x01\x02\x03 lixo antes", 14);

    // o fluxo chega em pedacos de tamanho qualquer, comecando no meio de um pacote; relogio simulado: 1 GOP = 1 s
    for (size_t chunk : {1u, 100u, 188u, 1000u, 65536u}) {
        std::vector<std::string> out;
        TsChunker c; c.emit = [&](std::string &&s) { out.push_back(std::move(s)); };
        std::string data = junk + stream.substr(50);          // comeco cortado
        double t = 0; size_t perGop = stream.size() / 10;
        for (size_t i = 0; i < data.size(); i += chunk) { c.feed(data.data() + i, std::min(chunk, data.size() - i), t); t = (double)i / perGop; }
        c.flush();
        assert(!c.notTs && c.vPid == V);
        assert(out.size() >= 1);
        if (chunk <= 1000) assert(out.size() >= 3 && out.size() <= 6);      // blocos enormes chegam de uma vez (rajada): um pedaco so, tudo bem
        for (auto &o : out) {
            assert(o.size() % 188 == 0);
            assert((uint8_t)o[0] == 0x47 && ((o[1] & 0x1F) << 8 | (uint8_t)o[2]) == 0);                 // comeca na PAT
            assert(((o[188 + 1] & 0x1F) << 8 | (uint8_t)o[188 + 2]) == PMT);                            // depois a PMT
            const uint8_t *k = (const uint8_t *)o.data() + 376;                                           // depois o quadro-chave
            assert((((k[1] & 0x1F) << 8) | k[2]) == V && k[4 + 17] == 0x67);
        }
    }
    // lixo (pagina HTML) nao e TS
    {
        TsChunker c; c.maxPending = 5000; int n = 0; c.emit = [&](std::string &&) { n++; };
        std::string html(20000, 'x'); c.feed(html.data(), html.size(), 0);
        assert(c.notTs && n == 0);
    }
    std::cout << "OK\n";
}
