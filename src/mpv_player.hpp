#pragma once
// Player estilo GMCA/Switchfin: libmpv + FFmpeg (PacBrew). Saida limpa via mpv_terminate_destroy.
#include <cstdint>
#include <string>
#include <vector>

namespace mpv_player {

struct Status {
    bool open = false;
    bool paused = false;
    bool eof = false;
    bool failed = false;
    double positionSec = 0;
    double durationSec = 0;
    std::string error;
};

bool available();
bool boot();
bool play(const std::string &url);
void pause(bool on);
bool seekSec(double sec);
bool seekRelative(double deltaSec);
void stop();
void shutdown(); // quit + terminate_destroy (igual GMCA)
Status status();
bool isOpen();
void poll();

// Software render -> buffer BGRA/RGB0 para blit no VideoOut do Kick
// width/height: tamanho do framebuffer (ex.: 1920x1080). Devolve true se pintou.
bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes);

} // namespace mpv_player
