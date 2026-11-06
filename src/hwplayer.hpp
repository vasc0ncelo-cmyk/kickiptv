// hwplayer — sceVideodec, um access unit por vez (sem carregar o arquivo inteiro na RAM)
#pragma once

#include "videodec.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hwplayer {

// Gancho de diagnostico: a plataforma aponta para trace() (grava em /data/kickiptv.log). O menu mostra o ultimo passo se o app cair.
extern void (*traceHook)(const char *);

struct Frame {
    const uint8_t *nv12 = nullptr;
    int width  = 0;
    int height = 0;
    int pitch  = 0;
    int yRows  = 0;
    uint64_t ptsMs = 0;
    bool valid = false;
};

class Player {
public:
    Player() = default;
    ~Player() { close(); }

    static bool tryLoadApi();
    static bool hwAvailable();

    bool open(int maxW = 1920, int maxH = 1080);
    bool openFile(const std::string &path, int maxW = 1920, int maxH = 1080);

    // Empurra AU Annex-B; se houver quadro, preenche out.
    bool feedAu(const uint8_t *data, size_t size, Frame &out);
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
    uint64_t ptsTick_ = 0;
    bool created_ = false;
    uint8_t *fbAligned_ = nullptr;
    size_t fbSize_ = 0;
    uint32_t codecUsed_ = 0;
    int decodes_ = 0;
public:
    uint32_t codecUsed() const { return codecUsed_; }
};

} // namespace hwplayer
