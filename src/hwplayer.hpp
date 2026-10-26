// hwplayer.hpp — protótipo de player próprio (fase 1):
//   arquivo local H.264 Annex-B  →  sceVideodec  →  NV12  →  tela (blit existente)
//
// Fases futuras (ainda NÃO implementadas aqui):
//   2) HTTP Range + seek
//   3) live HLS / IPTV
//
// Se o módulo de hardware não carregar, open() retorna false e o app
// deve continuar no AvPlayer (caminho atual).
#pragma once

#include "videodec.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hwplayer {

struct Frame {
    const uint8_t *nv12 = nullptr; // Y + UV intercalado (ou planar conforme o decoder)
    int width  = 0;
    int height = 0;
    int pitch  = 0;   // bytes por linha Y
    int yRows  = 0;
    uint64_t ptsMs = 0;
    bool valid = false;
};

class Player {
public:
    Player() = default;
    ~Player() { close(); }

    // Tenta carregar libSceVideodec (ou variantes de nome no firmware).
    // Chame uma vez no boot do app.
    static bool tryLoadApi();

    // true se a API de hardware está disponível neste console.
    static bool hwAvailable();

    // Abre um arquivo .h264 / .264 em Annex-B (NAL com start codes 00 00 01).
    // MP4/MKV ainda não: precisa demux (fase 2).
    bool openFile(const std::string &path, int maxW = 1920, int maxH = 1080);

    // Alimenta o próximo access unit e, se houver quadro pronto, preenche 'out'.
    // Retorna true se 'out' tem frame novo.
    bool pump(Frame &out);

    void close();

    bool isOpen() const { return open_; }
    const std::string &lastError() const { return err_; }

private:
    bool initDecoder(int maxW, int maxH);
    bool nextAccessUnit(std::vector<uint8_t> &au);
    static size_t findStartCode(const uint8_t *p, size_t n, size_t from, size_t *scLen);

    bool open_ = false;
    std::string err_;
    std::vector<uint8_t> file_;
    size_t pos_ = 0;

    vdec::Ctrl ctrl_{};
    vdec::ResourceInfo res_{};
    std::vector<uint8_t> cpuMem_;
    std::vector<uint8_t> cpuGpuMem_;
    std::vector<uint8_t> frameBuf_;
    uint64_t ptsTick_ = 0;
};

} // namespace hwplayer
