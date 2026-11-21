#pragma once
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

// Backend HTTP do app. O cookie e opaco e vive ate close().
struct StreamIo {
    // Abre a URL; devolve cookie != nullptr em sucesso. size_out pode ficar 0 se desconhecido.
    void *(*open)(void *ud, const char *url, uint64_t *size_out) = nullptr;
    // Le a partir de offset (seek absoluto por leitura). n bytes no maximo.
    int64_t (*read)(void *cookie, uint64_t offset, char *buf, uint64_t n) = nullptr;
    void (*close)(void *cookie) = nullptr;
    // Opcional: o mpv pede para abortar (stop/trocar de video) com uma leitura bloqueada em outra thread. Deve fazer o read voltar logo.
    void (*cancel)(void *cookie) = nullptr;
    void *ud = nullptr;
};

void setStreamIo(const StreamIo &io);
void setTrace(void (*fn)(const char *));   // passos em /data/kickiptv.log

bool available();
// Inicia em SEGUNDO PLANO (thread propria, sem bloquear ninguem) o teste do audio SDL. Chame cedo (ex.: ao abrir o menu):
// quando o 1o video abrir o resultado ja esta pronto e o play() nao espera. Pode chamar varias vezes.
void prewarm();
bool boot();
// live=true: live da Kick ou canal ao vivo do IPTV -> tenta o decodificador de HARDWARE (h264_videodec2).
// live=false (filme/serie/VOD): sempre software. O pedido e refeito a cada play, nada vaza de um video para o outro.
bool play(const std::string &url, bool live = false);
// true UMA vez quando o videodec2 abriu mas nao entregou video: o app deve reabrir o canal (ja volta em software, ate fechar o app).
bool hwFallbackRequested();
void pause(bool on);
void setMute(bool on);
void setSpeed(double rate);
bool seekSec(double sec);
bool seekRelative(double deltaSec);
void stop();
void shutdown();
Status status();
bool isOpen();
void poll();
// Desenha o quadro atual do mpv em dst (BGRA). Sem 'force' so desenha se chegou quadro novo (false = nada novo).
// force=true: redesenha o ULTIMO quadro mesmo sem quadro novo (OSD com o video pausado) - assim nao precisa guardar uma copia de 8 MB.
bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes, bool force = false);

} // namespace mpv_player
