#pragma once
// =============================================================================
// mpv_player — mesmo caminho do GMCA / Switchfin no PS4
// =============================================================================
// O "player próprio" do GMCA NÃO é sceAvPlayer: é libmpv + FFmpeg (PacBrew).
// Este módulo espelha o ciclo de vida do MPVCore do GMCA:
//   mpv_create -> options -> mpv_initialize -> loadfile -> ...
//   stop -> quit -> mpv_terminate_destroy  (saída limpa, sem CE do AvPlayer)
//
// Build: compile com -DUSE_LIBMPV e linke as libs PacBrew (ver MPV_PS4.md).
// Sem USE_LIBMPV este header vira stub (sempre unavailable).
// =============================================================================

#include <cstdint>
#include <string>

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

// true se o binário foi linkado com libmpv
bool available();

// Cria o contexto mpv (uma vez). Idempotente.
bool boot();

// Abre URL http(s)/file e começa a tocar (direto, como loadfile do GMCA).
bool play(const std::string &url);

void pause(bool on);
bool seekSec(double sec);           // absoluto
bool seekRelative(double deltaSec); // +/- segundos

void stop();                        // mpv "stop" (mantém contexto)
void shutdown();                    // quit + mpv_terminate_destroy (saída limpa)

Status status();
bool isOpen();

// Processa eventos do mpv (chamar no loop principal).
void poll();

} // namespace mpv_player
