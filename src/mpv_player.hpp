#pragma once
// libmpv no PS4 (PacBrew). Rede via stream_cb + HTTP do app (sceHttp), nao via FFmpeg.
#include <cstdint>
#include <string>

namespace mpv_player {

struct Status {
    bool open = false;
    bool paused = false;
    bool eof = false;
    bool failed = false;
    bool buffering = false;
    int bufferPct = 0;
    double positionSec = 0;
    double durationSec = 0;
    std::string error;
};

// Backend HTTP do app (preenchido em ps4_platform com g_net).
// read_at: le ate 'n' bytes a partir de 'offset'. Devolve bytes lidos, 0 = EOF, -1 = erro.
// size_out: tamanho total do recurso se conhecido (0 = desconhecido).
struct StreamIo {
    int64_t (*read_at)(void *ud, const char *url, uint64_t offset, char *buf, uint64_t n, uint64_t *size_out) = nullptr;
    void *ud = nullptr;
};

void setStreamIo(const StreamIo &io);

bool available();
bool boot();
bool play(const std::string &url);   // usa khttp:// + stream_cb
void pause(bool on);
void setMute(bool on);
bool seekSec(double sec);
bool seekRelative(double deltaSec);
void stop();
void shutdown();
Status status();
bool isOpen();
void poll();
bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes);

} // namespace mpv_player
