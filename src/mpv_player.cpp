// KICKIPTV_STREAM_CB_V3 - loadfile DEVE usar khttp:// (HTTP do app, nao FFmpeg DNS)
#include "mpv_player.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <pthread.h>
#include <vector>

#ifndef USE_LIBMPV

namespace mpv_player {
void setStreamIo(const StreamIo &) {}
void setTrace(void (*)(const char *)) {}
bool available() { return false; }
void prewarm() {}
bool boot() { return false; }
bool play(const std::string &, bool) { return false; }
bool hwFallbackRequested() { return false; }
void pause(bool) {}
void setMute(bool) {}
void setSpeed(double) {}
bool seekSec(double) { return false; }
bool seekRelative(double) { return false; }
void stop() {}
void shutdown() {}
Status status() { return {}; }
bool isOpen() { return false; }
void poll() {}
bool renderToBgra(uint8_t *, int, int, int, bool) { return false; }
}

#else

extern "C" {
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/stream_cb.h>
}
#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#define KICK_HAVE_SDL_PRETEST 1
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#if __has_include(<orbis/AudioOut.h>)
#include <orbis/AudioOut.h>
#include <orbis/libkernel.h>
#define KICK_SDL_SHIM 1      // audio do mpv via backend proprio (sceAudioOut): o SDL_InitSubSystem(AUDIO) real trava no PS4
#endif
#endif
#endif

namespace mpv_player {
namespace {

std::mutex g_mx;
mpv_handle *g_mpv = nullptr;
mpv_render_context *g_rr = nullptr;
std::atomic<bool> g_open{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_eof{false};
std::atomic<bool> g_failed{false};
std::atomic<bool> g_buffering{false};
std::atomic<int> g_bufPct{0};
std::string g_err;
double g_pos = 0, g_dur = 0;
StreamIo g_io{};
bool g_cb_registered = false;
// Troca de midia: cada arquivo carregado tem um "playlist_entry_id". Eventos (fim/erro/eof) de um arquivo ANTIGO que chegam depois que o
// novo comecou NAO podem valer para o novo (era isso que dava EOF/falha logo apos a 1a reproducao e deixava a imagem parada).
std::atomic<int64_t> g_curEntry{-1};
std::atomic<bool> g_startSeen{false};
bool g_started = false;                                  // um arquivo foi aberto e a parada ainda nao foi confirmada (mpv "idle")
std::atomic<unsigned> g_audCb{0};                        // vezes que o backend de audio chamou o callback do mpv (diagnostico)
std::atomic<int> g_audPause{1};                          // estado de pausa que o mpv pediu ao audio (1 = pausado)
unsigned g_audCbLast = 0;
int g_stallKicks = 0;                                    // tentativas de destravar o video nesta reproducao
std::chrono::steady_clock::time_point g_lastFrameT = std::chrono::steady_clock::now();
// quadro reduzido (render interno <= 1280 de largura) e mapa da ampliacao: zerados a cada troca de midia
static std::vector<uint32_t> g_lo;
static int g_loW = 0, g_loH = 0;
static std::vector<int> g_xmap;
static int g_xmapW = 0, g_xmapLw = 0;
static void resetFrameState() {
    g_lo.clear(); g_lo.shrink_to_fit(); g_loW = g_loH = 0;
    g_xmap.clear(); g_xmapW = g_xmapLw = 0;
}

static void setErr(const char *s) {
    g_err = s ? s : "";
    g_failed = true;
}
static int chk(int st) {
    if (st < 0) {
        setErr(mpv_error_string(st));
        return st;
    }
    return st;
}

struct StreamState {
    void *io_cookie = nullptr; // do StreamIo::open
    uint64_t pos = 0;
    uint64_t size = 0;
    bool size_known = false;
};

static int64_t cb_read(void *cookie, char *buf, uint64_t nbytes) {
    auto *st = (StreamState *)cookie;
    if (!st || !st->io_cookie || !g_io.read || !buf || nbytes == 0) return -1;
    int64_t n = g_io.read(st->io_cookie, st->pos, buf, nbytes);
    if (n < 0) return -1;
    if (n == 0) return 0;
    st->pos += (uint64_t)n;
    return n;
}

static int64_t cb_seek(void *cookie, int64_t offset) {
    auto *st = (StreamState *)cookie;
    if (!st) return MPV_ERROR_GENERIC;
    if (offset < 0) return MPV_ERROR_GENERIC;
    if (st->size_known && (uint64_t)offset > st->size) return MPV_ERROR_GENERIC;
    st->pos = (uint64_t)offset;
    return 0;
}

static int64_t cb_size(void *cookie) {
    auto *st = (StreamState *)cookie;
    if (!st || !st->size_known || st->size == 0) return MPV_ERROR_UNSUPPORTED;
    return (int64_t)st->size;
}

static void cb_cancel(void *cookie) {
    auto *st = (StreamState *)cookie;
    if (st && st->io_cookie && g_io.cancel) g_io.cancel(st->io_cookie);
}

static void cb_close(void *cookie) {
    auto *st = (StreamState *)cookie;
    if (st) {
        if (st->io_cookie && g_io.close) g_io.close(st->io_cookie);
        delete st;
    }
}

static int cb_open(void * /*user_data*/, char *uri, mpv_stream_cb_info *info) {
    if (!uri || !info || !g_io.open || !g_io.read || !g_io.close)
        return MPV_ERROR_UNSUPPORTED;

    const char *u = uri;
    if (!strncmp(u, "khttp://", 8)) u += 8;
    if (!*u) return MPV_ERROR_INVALID_PARAMETER;

    uint64_t sz = 0;
    void *io_cookie = g_io.open(g_io.ud, u, &sz);
    if (!io_cookie) return MPV_ERROR_LOADING_FAILED;

    auto *st = new StreamState();
    st->io_cookie = io_cookie;
    st->pos = 0;
    if (sz > 0) {
        st->size = sz;
        st->size_known = true;
    }

    info->cookie = st;
    info->read_fn = cb_read;
    // Live da Kick (kicklive://): fluxo continuo SEM posicao. O cb_seek dizia "ok" para qualquer seek, mas a leitura ignora o offset
    // (le sempre o proximo byte do alimentador): o mpv achava que tinha voltado no arquivo e recebia bytes do ponto errado.
    // Sem seek_fn o mpv trata como transmissao ao vivo (e nunca tenta voltar no cache).
    info->seek_fn = !strncmp(u, "kicklive://", 11) ? nullptr : cb_seek;
    info->size_fn = cb_size;
    info->close_fn = cb_close;
    info->cancel_fn = g_io.cancel ? cb_cancel : nullptr;
    return 0;
}

static void destroyRender() {
    if (g_rr) {
        mpv_render_context_free(g_rr);
        g_rr = nullptr;
    }
}

static bool ensureRender() {
    if (g_rr) return true;
    if (!g_mpv) return false;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    if (mpv_render_context_create(&g_rr, g_mpv, params) < 0) {
        g_rr = nullptr;
        return false;
    }
    return true;
}


static void (*g_trace)(const char *) = nullptr;
static void tr(const char *s) { if (g_trace) g_trace(s); }

// ---------- Audio SDL: teste em segundo plano ----------
// O SDL de audio do PS4 trava (o log parava em "sdl-pretest: SDL_InitSubSystem"). Antes o teste rodava DENTRO do boot do mpv,
// bloqueando a abertura do video por 6 s e, se travasse, deixava o video sem som para sempre. Agora:
//  1) o teste roda numa thread propria (pilha de 1 MiB: a padrao da musl tem so 128 KiB) iniciada CEDO pelo app (prewarm());
//  2) o play() so espera um pouco (ate 2 s) e nunca fica preso: se o SDL nao estiver pronto, o video abre com ao=null;
//  3) assim que o SDL ficar pronto, o ao e trocado para sdl em execucao (poll()/proximo play()): o som entra sem reabrir o video;
//  4) o teste termina com SDL_QuitSubSystem: o ao_sdl do mpv RECUSA iniciar ("already initialized") se o audio do SDL ja
//     estiver inicializado - o teste antigo nunca fechava, entao o ao=sdl nunca poderia funcionar.
enum { SDL_ST_IDLE = 0, SDL_ST_RUN = 1, SDL_ST_OK = 2, SDL_ST_FAIL = 3 };
static std::atomic<int> g_sdlState{SDL_ST_IDLE};
static std::atomic<int> g_sdlTries{0};
static std::atomic<bool> g_aoUpgrade{false};   // o video abriu com ao=null: trocar para sdl quando o teste passar

#ifdef KICK_HAVE_SDL_PRETEST
static void *sdlProbeMain(void *) {
    SDL_SetMainReady();
    tr("sdl-probe: SDL_InitSubSystem");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { tr("sdl-probe: init falhou"); g_sdlState = SDL_ST_FAIL; return nullptr; }
    {
        const char *drv = SDL_GetCurrentAudioDriver();
        char b[96]; snprintf(b, sizeof b, "sdl-probe: init ok driver=%s", drv ? drv : "?"); tr(b);
    }
    SDL_AudioSpec want, have;
    SDL_zero(want); SDL_zero(have);
    want.freq = 48000; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    tr("sdl-probe: SDL_OpenAudioDevice");
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (d == 0) { tr("sdl-probe: open falhou"); SDL_QuitSubSystem(SDL_INIT_AUDIO); g_sdlState = SDL_ST_FAIL; return nullptr; }
    tr("sdl-probe: open ok");
    SDL_CloseAudioDevice(d);
    tr("sdl-probe: close ok");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);          // obrigatorio: sem isso o ao_sdl do mpv diz "already initialized"
    tr("sdl-probe: quit ok");
    g_sdlState = SDL_ST_OK;
    return nullptr;
}
static void sdlStartProbe() {
#ifdef KICK_SDL_SHIM
    g_sdlState = SDL_ST_OK;      // backend proprio no fim do arquivo: nao chama o SDL real (que trava no init)
    return;
#endif
    int st = g_sdlState.load();
    if (st == SDL_ST_RUN || st == SDL_ST_OK) return;
    if (st == SDL_ST_FAIL && g_sdlTries.load() >= 3) return;      // falhou (sem travar) 3 vezes: desiste
    g_sdlTries++;
    g_sdlState = SDL_ST_RUN;
    pthread_attr_t at;
    pthread_t th;
    bool ok = false;
    if (pthread_attr_init(&at) == 0) {
        pthread_attr_setstacksize(&at, 1u << 20);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        ok = pthread_create(&th, &at, sdlProbeMain, nullptr) == 0;
        pthread_attr_destroy(&at);
    }
    if (!ok) {
        try { std::thread([] { sdlProbeMain(nullptr); }).detach(); ok = true; } catch (...) {}
    }
    if (!ok) { tr("sdl-probe: nao conseguiu criar a thread"); g_sdlState = SDL_ST_FAIL; }
}
#else
static void sdlStartProbe() { g_sdlState = SDL_ST_OK; }          // sem o header do SDL: como antes, assume que o ao=sdl abre
#endif

// Espera no maximo maxMs pelo teste (nunca indefinidamente). Devolve o estado.
static int sdlAudioWait(int maxMs) {
    sdlStartProbe();
    for (int i = 0; i < maxMs / 50 && g_sdlState.load() == SDL_ST_RUN; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return g_sdlState.load();
}

// O video abriu sem som e o SDL ficou pronto depois: troca o ao em execucao (chamar com g_mx).
static void applyAoUpgradeLocked() {
    if (!g_aoUpgrade.load() || !g_mpv) return;
    sdlStartProbe();                                   // se falhou (sem travar), tenta de novo (ate 3 vezes)
    if (g_sdlState.load() != SDL_ST_OK) return;
    g_aoUpgrade = false;
    int r = mpv_set_property_string(g_mpv, "ao", "sdl,null");
    tr(r >= 0 ? "mpv-ao: trocado para sdl (som ligado)" : "mpv-ao: nao foi possivel trocar para sdl");
}

// desempenho do desenho (aparece em mpv-perf): tempo medio de mpv_render_context_render
static std::atomic<long long> g_renderUs{0};
static std::atomic<int> g_renderN{0};
static bool g_everRendered = false;     // ja desenhou um quadro desde o ultimo play (so entao 'force' faz sentido)

// ---------- Decodificador de HARDWARE (h264_videodec2): so lives da Kick e canais ao vivo ----------
static bool g_hwWanted = false;                    // ESTE play pediu o videodec2
static bool g_hwChecked = false;                   // o decoder REAL deste play ja foi conferido e logado
static bool g_hwBad = false;                       // videodec2 abriu mas nao deu video: software ate fechar o app
static std::atomic<bool> g_hwFallbackAsk{false};   // pedido (1 vez) para o app reabrir o canal em software
static std::chrono::steady_clock::time_point g_hwT0 = std::chrono::steady_clock::now();

static bool fileThere(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return false; fclose(f); return true; }

static std::string propStr(const char *name) {     // propriedade do mpv como texto ("-" se nao existe); chamar com g_mx
    char *s = g_mpv ? mpv_get_property_string(g_mpv, name) : nullptr;
    std::string r = s ? s : "-";
    if (s) mpv_free(s);
    return r;
}

// Grava no /data/kickiptv.log (tr) E no /data/kickiptv_mpv.log (print-text do mpv). Prefixo fixo: grep "\[kick-hw\]".
static void logBoth(const char *msg) {
    tr(msg);
    if (g_mpv) { const char *c[] = {"print-text", msg, nullptr}; mpv_command_async(g_mpv, 0, c); }
}

// ---- Protecao contra queda do app ----
// O decoder h264_videodec2 do ffmpeg carrega 5 modulos do sistema e, se eles NAO carregam, segue em frente e chama funcoes que nao
// existem: o app INTEIRO cai (nao e erro de codec, entao o mpv nem chega a cair no h264 por software). Foi o que o log mostrou:
// "videodec2: LoadStartModule(/system/common/lib/libSceVdecCore.sprx) => 0x80020002" (ENOENT) e o log parou ali.
// Por isso: (1) conferimos os MESMOS modulos antes (so le o codigo de retorno); (2) um marcador em disco liga o modo seguro se
// mesmo assim cair.
extern "C" int mpvhw_LoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, void *opt, int *res) __asm__("sceKernelLoadStartModule");

static bool hwModulesOk(std::string &why) {
    static int s_state = -1;                      // o resultado nao muda durante a execucao: confere uma vez so
    static std::string s_why;
    if (s_state >= 0) { why = s_why; return s_state == 1; }
    static const char *kMods[] = {"/system/common/lib/libSceVdecCore.sprx", "/system/common/lib/libSceVdecSavc.sprx",
                                  "/system/common/lib/libSceVdecSavc2.sprx", "/system/common/lib/libSceVdecwrap.sprx",
                                  "/system/common/lib/libSceVideodec2.sprx"};
    bool ok = true;
    char b[220];
    for (const char *p : kMods) {
        int res = 0;
        int rc = mpvhw_LoadStartModule(p, 0, nullptr, 0, nullptr, &res);
        snprintf(b, sizeof b, "[kick-hw] preflight: %s rc=0x%08x res=%d", p, (unsigned)rc, res);
        logBoth(b);
        if (rc < 0) {                             // 2a chance: pelo nome (pode ja estar carregado) e depois repete o caminho completo
            const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
            const int rc2 = mpvhw_LoadStartModule(base, 0, nullptr, 0, nullptr, &res);
            snprintf(b, sizeof b, "[kick-hw] preflight: %s (pelo nome) rc=0x%08x", base, (unsigned)rc2);
            logBoth(b);
            if (rc2 >= 0) {
                rc = mpvhw_LoadStartModule(p, 0, nullptr, 0, nullptr, &res);
                snprintf(b, sizeof b, "[kick-hw] preflight: %s (de novo) rc=0x%08x", p, (unsigned)rc);
                logBoth(b);
            }
        }
        if (rc < 0) { ok = false; if (s_why.empty()) s_why = std::string("nao carregou ") + p; }
    }
    s_state = ok ? 1 : 0;
    why = s_why;
    logBoth(ok ? "[kick-hw] preflight OK: modulos do videodec2 carregam"
               : "[kick-hw] preflight FALHOU: lives ficam em SOFTWARE (com estes modulos ausentes o ffmpeg derruba o app)");
    return ok;
}

static const char *kHwGuard = "/data/kickiptv_hwdec_guard";      // existe = o ultimo videodec2 derrubou o app
static const char *kHwOffFile = "/data/kickiptv_nohwdec";        // existe = tudo em software (manual ou automatico)
static void guardSet() { FILE *f = fopen(kHwGuard, "wb"); if (f) { fputs("tentando videodec2", f); fclose(f); } }
static void guardClear() { remove(kHwGuard); }

// Quem DE FATO decodifica. "No hardware decoding requested" / "Using software decoding" do mpv so falam do hwaccel (--hwdec) e saem
// mesmo com o videodec2 rodando; hwdec-current tambem fica "no". A prova e current-tracks/video/decoder.
static std::string logDecoderLocked(const char *tag) {
    const std::string dec = propStr("current-tracks/video/decoder");
    const bool isHw = dec.find("videodec2") != std::string::npos;
    char b[320];
    snprintf(b, sizeof b, "[kick-hw] %s: decoder-real=%s => %s | hwdec-current=%s | options: vd=%s hwdec=%s threads=%s",
             tag, dec.c_str(), isHw ? "HARDWARE" : "SOFTWARE", propStr("hwdec-current").c_str(), propStr("options/vd").c_str(),
             propStr("options/hwdec").c_str(), propStr("options/vd-lavc-threads").c_str());
    logBoth(b);
    snprintf(b, sizeof b, "[kick-hw] %s: %s | codec=%s | fmt=%s hw-fmt=%s | %sx%s | fps=%s drop-dec=%s",
             tag, propStr("current-tracks/video/decoder-desc").c_str(), propStr("video-codec").c_str(),
             propStr("video-params/pixelformat").c_str(), propStr("video-params/hw-pixelformat").c_str(),
             propStr("video-params/w").c_str(), propStr("video-params/h").c_str(), propStr("estimated-vf-fps").c_str(),
             propStr("decoder-frame-drop-count").c_str());
    logBoth(b);
    return dec;
}

static void handleEvent(mpv_event *ev, bool *gotLoaded, bool *gotFail) {
    switch (ev->event_id) {
    case MPV_EVENT_SHUTDOWN:
        g_open = false;
        if (gotFail) *gotFail = true;
        break;
    case MPV_EVENT_START_FILE: {
        auto *sf = (mpv_event_start_file *)ev->data;
        g_curEntry = sf ? sf->playlist_entry_id : 0;
        g_startSeen = true;                           // daqui em diante os eventos sao do arquivo NOVO
        g_eof = false;
        break;
    }
    case MPV_EVENT_FILE_LOADED:
        if (!g_startSeen.load()) break;
        if (gotLoaded) *gotLoaded = true;
        g_failed = false;
        g_err.clear();
        break;
    case MPV_EVENT_END_FILE: {
        auto *e = (mpv_event_end_file *)ev->data;
        if (!e) break;
        // fim de um arquivo ANTIGO (parado/abortado na troca): ignora
        if (!g_startSeen.load() || e->playlist_entry_id != g_curEntry.load()) break;
        if (e->reason == MPV_END_FILE_REASON_EOF) g_eof = true;
        else if (e->reason == MPV_END_FILE_REASON_ERROR) {
            g_failed = true;
            g_err = std::string("end-file: ") + mpv_error_string(e->error);
            if (gotFail) *gotFail = true;
        }
        break;
    }
    case MPV_EVENT_PROPERTY_CHANGE: {
        auto *prop = (mpv_event_property *)ev->data;
        if (!prop || !prop->data || !g_startSeen.load()) break;
        if (prop->format == MPV_FORMAT_DOUBLE) {
            if (!std::strcmp(prop->name, "time-pos")) g_pos = *(double *)prop->data;
            if (!std::strcmp(prop->name, "duration")) {
                g_dur = *(double *)prop->data;
                if (gotLoaded && g_dur > 0.5) *gotLoaded = true;
            }
        } else if (prop->format == MPV_FORMAT_INT64) {
            if (!std::strcmp(prop->name, "cache-buffering-state"))
                g_bufPct = (int)*(int64_t *)prop->data;
        } else if (prop->format == MPV_FORMAT_FLAG) {
            if (!std::strcmp(prop->name, "paused-for-cache"))
                g_buffering = *(int *)prop->data != 0;
            if (!std::strcmp(prop->name, "pause")) g_paused = *(int *)prop->data != 0;
            if (!std::strcmp(prop->name, "eof-reached")) g_eof = *(int *)prop->data != 0;
        }
        break;
    }
    default:
        break;
    }
}

static void drainEventsUnlocked(bool *gotLoaded, bool *gotFail) {
    while (true) {
        mpv_event *ev = mpv_wait_event(g_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        handleEvent(ev, gotLoaded, gotFail);
    }
}

// Confirma que o arquivo anterior parou DE VERDADE (mpv "idle") antes de abrir o proximo, e entao recria o contexto de render:
// o stop() e assincrono e o loadfile seguinte corria junto com ele (imagem parada, audio morto, EOF/pos=0 na 2a reproducao).
// Chamar com g_mx. As leituras de rede do arquivo antigo ja foram canceladas pelo app (cancel_fn / abort), entao volta rapido.
static void quiesceLocked() {
    if (!g_mpv || !g_started) return;
    const char *stopc[] = {"stop", nullptr};
    mpv_command_async(g_mpv, 0, stopc);                   // repetir e inofensivo
    bool idle = false;
    for (int i = 0; i < 150 && !idle; i++) {              // ate ~3 s
        mpv_event *ev = mpv_wait_event(g_mpv, 0.02);
        while (ev && ev->event_id != MPV_EVENT_NONE) ev = mpv_wait_event(g_mpv, 0);   // eventos do arquivo antigo: descartados
        int f = 0;
        if (mpv_get_property(g_mpv, "idle-active", MPV_FORMAT_FLAG, &f) >= 0 && f) idle = true;
    }
    if (idle) {
        destroyRender();                                  // video novo = contexto de render novo (nada de estado do video anterior)
        ensureRender();
        g_started = false;
        tr("mpv-troca: arquivo anterior parado, render recriado");
    } else {
        tr("mpv-troca: o stop demorou mais de 3 s; segue com o render atual");
    }
}

} // namespace

void setTrace(void (*fn)(const char *)) { g_trace = fn; }

void setStreamIo(const StreamIo &io) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_io = io;
}

bool available() { return true; }

void prewarm() { sdlStartProbe(); }

bool boot() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_mpv) return true;
    if (fileThere(kHwGuard)) {       // a execucao anterior caiu DENTRO do videodec2: nunca mais tenta (ate apagar o arquivo abaixo)
        g_hwBad = true;
        guardClear();
        FILE *f = fopen(kHwOffFile, "wb");
        if (f) { fputs("automatico: o videodec2 derrubou o app. Apague este arquivo para tentar o hardware de novo.", f); fclose(f); }
        tr("[kick-hw] a execucao anterior caiu no videodec2: hardware desligado (criado /data/kickiptv_nohwdec)");
    }
    g_mpv = mpv_create();
    if (!g_mpv) {
        setErr("mpv_create falhou");
        return false;
    }

    mpv_set_option_string(g_mpv, "log-file", "/data/kickiptv_mpv.log");
    mpv_set_option_string(g_mpv, "msg-level", "ffmpeg=v,cplayer=v,stream=v,demux=v,ao=v,sdl=v,vo=v,vd=v");
    mpv_set_option_string(g_mpv, "ytdl", "no");
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "sid", "no");
    mpv_set_option_string(g_mpv, "sub-auto", "no");
    mpv_set_option_string(g_mpv, "vo", "libmpv");
    mpv_set_option_string(g_mpv, "hwdec", "no");
    mpv_set_option_string(g_mpv, "vd-lavc-threads", "6");
    // ---- VIDEO ----
    // O vo=libmpv por software converte (YUV->BGRA) e escala TODO quadro na CPU. O que pesava: o swscale padrao do mpv e o "bicubic"
    // (~150 ms por quadro 1080p no PS4 => ~7 quadros/s). As opcoes de GPU (scale/cscale/dscale/dither/interpolation...) nao valem
    // para este caminho; quem manda aqui sao as sws-*: bilinear rapido, sem zimg (mais lento nessa CPU).
    // ATENCAO (queda da live Kick): NAO usar "fast-bilinear". Com o video menor que a tela (live 720p -> tela 1080p) o swscale liga o
    // escalador MMXEXT que GERA CODIGO em memoria executavel (mmap + mprotect PROT_EXEC). No PS4 isso derruba o app (SIGBUS) logo no
    // 1o quadro desenhado: o mpv_kickiptv.log parava em "starting audio playback" ~1 s depois de abrir. O "bilinear" normal usa so
    // as rotinas SIMD fixas (sem gerar codigo) e e bem mais leve que o bicubic padrao.
    mpv_set_option_string(g_mpv, "sws-scaler", "bilinear");
    mpv_set_option_string(g_mpv, "sws-fast", "no");
    mpv_set_option_string(g_mpv, "sws-allow-zimg", "no");
    mpv_set_option_string(g_mpv, "sw-fast", "no");
    mpv_set_option_string(g_mpv, "scale", "bilinear");
    mpv_set_option_string(g_mpv, "cscale", "bilinear");
    mpv_set_option_string(g_mpv, "dscale", "bilinear");
    mpv_set_option_string(g_mpv, "correct-downscaling", "no");
    mpv_set_option_string(g_mpv, "dither-depth", "no");
    mpv_set_option_string(g_mpv, "interpolation", "no");
    mpv_set_option_string(g_mpv, "video-sync", "audio");
    mpv_set_option_string(g_mpv, "vd-lavc-skiploopfilter", "nonref");   // "all" desligava o filtro anti-blocos: canais IPTV (pouco bitrate) ficavam quadriculados
    mpv_set_option_string(g_mpv, "vd-lavc-dr", "no");
    mpv_set_option_string(g_mpv, "framedrop", "decoder+vo");
    mpv_set_option_string(g_mpv, "vd-lavc-fast", "yes");
    // ---- CACHE ----
    // Leitura em blocos grandes (o HTTP do app ja busca 1 MB por vez); a memoria "de tras" so precisa cobrir os saltos de +-10 s;
    // Colchao maior na frente, pouco para tras (memoria do PS4 e curta).
    // Pausa so quando o cache zera de verdade e retoma com folga (menos engasgo curto).
    mpv_set_option_string(g_mpv, "cache", "yes");
    mpv_set_option_string(g_mpv, "demuxer-max-bytes", "160MiB");
    mpv_set_option_string(g_mpv, "demuxer-max-back-bytes", "8MiB");
    mpv_set_option_string(g_mpv, "demuxer-readahead-secs", "45");
    mpv_set_option_string(g_mpv, "cache-secs", "180");
    mpv_set_option_string(g_mpv, "stream-buffer-size", "2MiB");
    mpv_set_option_string(g_mpv, "cache-pause", "yes");
    mpv_set_option_string(g_mpv, "cache-pause-initial", "no");
    mpv_set_option_string(g_mpv, "cache-pause-wait", "2");
    mpv_set_option_string(g_mpv, "network-timeout", "20");
    mpv_set_option_string(g_mpv, "force-seekable", "yes");
    // ---- AUDIO ----
    mpv_set_option_string(g_mpv, "audio-channels", "stereo");
    mpv_set_option_string(g_mpv, "audio-samplerate", "48000");     // o mesmo formato que o teste do SDL abriu (s16 estereo 48 kHz)
    mpv_set_option_string(g_mpv, "audio-format", "s16");
    mpv_set_option_string(g_mpv, "audio-fallback-to-null", "yes");
    {
        // Nao trava a abertura: espera no maximo 2 s pelo teste (que ja deve ter rodado em segundo plano desde o inicio do app).
        const bool sdlOk = sdlAudioWait(2000) == SDL_ST_OK;
        mpv_set_option_string(g_mpv, "ao", sdlOk ? "sdl,null" : "null");
        g_aoUpgrade = !sdlOk;      // sem som por enquanto: troca para sdl em execucao quando o teste passar
        tr(sdlOk ? "mpv-ao=sdl" : "mpv-ao=null (SDL ainda nao pronto: o som liga sozinho quando ficar)");
    }
    mpv_set_option_string(g_mpv, "demuxer-lavf-analyzeduration", "2");
    mpv_set_option_string(g_mpv, "demuxer-lavf-probesize", "3145728");
    mpv_set_option_string(g_mpv, "demuxer-lavf-probescore", "1");
    // stream_cb e tratado como arquivo: evita lavf tentar reabrir como http
    mpv_set_option_string(g_mpv, "demuxer", "lavf");

    if (chk(mpv_initialize(g_mpv)) < 0) {
        mpv_terminate_destroy(g_mpv);
        g_mpv = nullptr;
        return false;
    }

    if (!g_cb_registered) {
        // Assinatura correta: (ctx, protocol, user_data, open_fn)
        if (mpv_stream_cb_add_ro(g_mpv, "khttp", nullptr, cb_open) < 0) {
            setErr("stream_cb_add_ro falhou");
        } else {
            g_cb_registered = true;
        }
    }

    mpv_observe_property(g_mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 5, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 6, "cache-buffering-state", MPV_FORMAT_INT64);

    ensureRender();
    g_failed = false;
    g_err.clear();
    return true;
}

bool play(const std::string &url, bool live) {
    if (!boot()) return false;
    if (url.empty()) {
        setErr("URL vazia");
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_mpv) return false;
        if (!g_io.open || !g_io.read || !g_io.close) {
            setErr("StreamIo nao configurado (HTTP do app)");
            return false;
        }
        if (!g_cb_registered) {
            setErr("stream_cb nao registrado");
            return false;
        }

        quiesceLocked();                                   // 1) o que tocava parou de verdade, 2) render novo
        g_eof = false;
        g_failed = false;
        g_buffering = true;
        g_bufPct = 0;
        g_pos = 0;
        g_dur = 0;
        g_err.clear();
        g_everRendered = false;
        g_startSeen = false; g_curEntry = -1;              // eventos do arquivo antigo (fim/erro/eof) nao valem para este
        g_stallKicks = 0; g_lastFrameT = std::chrono::steady_clock::now();
        mpv_set_property_string(g_mpv, "video-sync", "audio");     // a anterior pode ter caido para "desync" (relogio de audio parado)
        resetFrameState();
        applyAoUpgradeLocked();       // SDL ficou pronto depois do 1o boot: este video ja abre com som
        ensureRender();

        // khttp:// = HTTP do app (sceHttp). Nao passar http:// cru (FFmpeg DNS no PS4 falha).
        std::string wrap = std::string("khttp://") + url;
        // NAO usar force-seekable=yes aqui: o mpv vai no fim do arquivo e muitos paineis IPTV
        // recusam o Range grande -> end-file: loading failed. Seek e feito depois, sob demanda.
        // Live da Kick: memoria curta (o PS4 fechava o app depois de alguns minutos: 160 MiB de demuxer + 45 s de readahead + buffers do
        // alimentador somavam mais do que o app pode ter) e sem force-seekable (nao e arquivo).
        const bool kickLive = url.compare(0, 11, "kicklive://") == 0;
        // ---- Decodificacao por HARDWARE: SO lives da Kick e canais ao vivo (live=true). Filme/serie/VOD: software. ----
        // (1) Por que caia em software: em mpv 0.36 "vd" e uma lista de NOMES de decoder ("h264_videodec2"). O prefixo "lavc:" (sintaxe
        //     antiga) nao casa com nenhum decoder e e ignorado em silencio -> ordem padrao (h264 por software vem primeiro).
        //     Sem "+": se o videodec2 nao abrir, o mpv cai sozinho no h264 (fallback seguro, mesmo arquivo, sem reabrir).
        // (2) hwdec fica "no": o videodec2 e um decoder proprio do ffmpeg (como h264_cuvid), nao um hwaccel do mpv. Por isso o mpv
        //     escreve "No hardware decoding requested / Using software decoding" MESMO com ele ativo. Vale "Selected codec:" e [kick-hw].
        // (3) vd e threads sao reaplicados A CADA play (live: videodec2 + 1 thread; o resto: padrao) -> nada vaza para filme/serie.
        const bool hwOff = fileThere(kHwOffFile);                              // chave de seguranca: arquivo vazio = tudo em software
        std::string hwWhyNot;
        const bool hw = live && !g_hwBad && !hwOff && hwModulesOk(hwWhyNot);   // so lives chegam a tocar nos modulos do sistema
        if (hw) guardSet(); else guardClear();                                 // marcador em disco: se o app cair daqui ate o 1o quadro, o proximo start vai em software
        mpv_set_property_string(g_mpv, "hwdec", "no");
        const int vdRc = mpv_set_property_string(g_mpv, "vd", hw ? "h264_videodec2" : "");
        mpv_set_property_string(g_mpv, "vd-lavc-threads", hw ? "1" : "6");
        {
            char b[240];
            snprintf(b, sizeof b, "[kick-hw] play: live=%d pedido=%s set-rc=%d vd(lido)=%s%s%s", live ? 1 : 0, hw ? "h264_videodec2" : "software",
                     vdRc, propStr("options/vd").c_str(), g_hwBad ? " [bloqueado: falhou antes]" : "", hwOff ? " [/data/kickiptv_nohwdec]" : (!hwWhyNot.empty() && live ? (" [" + hwWhyNot + "]").c_str() : ""));
            logBoth(b);
        }
        g_hwWanted = hw; g_hwChecked = false; g_hwT0 = std::chrono::steady_clock::now();
        std::string liveOpts = std::string("force-seekable=no,demuxer-max-bytes=24MiB,demuxer-max-back-bytes=1MiB,demuxer-readahead-secs=15,vd-lavc-threads=") + (hw ? "1" : "4");
        const char *cmd[] = {"loadfile", wrap.c_str(), "replace", kickLive ? liveOpts.c_str() : nullptr, nullptr};   // mpv 0.36: sem o indice "0"
        int lrc = mpv_command(g_mpv, cmd);
        if (lrc < 0 && kickLive) {                                  // opcao nao aceita nesta versao: abre sem elas
            const char *cmd2[] = {"loadfile", wrap.c_str(), "replace", nullptr};
            lrc = mpv_command(g_mpv, cmd2);
        }
        if (chk(lrc) < 0) {
            g_open = false;
            guardClear();
            return false;
        }
        g_started = true;
    }
    // IMPORTANTE: nao segurar g_mx enquanto o demuxer le via stream_cb

    bool gotLoaded = false, gotFail = false;
    for (int i = 0; i < 400 && !gotLoaded && !gotFail; i++) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_mpv) return false;
        mpv_event *ev = mpv_wait_event(g_mpv, 0.1);
        while (ev && ev->event_id != MPV_EVENT_NONE) {
            handleEvent(ev, &gotLoaded, &gotFail);
            if (gotLoaded || gotFail) break;
            ev = mpv_wait_event(g_mpv, 0);
        }
    }

    if (gotFail || g_failed || !gotLoaded) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_open = false;
        guardClear();
        if (g_err.empty()) setErr("loading failed (stream_cb)");
        if (g_mpv) {
            const char *stopc[] = {"stop", nullptr};
            mpv_command(g_mpv, stopc);
        }
        return false;
    }

    g_open = true;
    g_paused = false;
    g_buffering = false;
    g_failed = false;
    g_err.clear();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_hwT0 = std::chrono::steady_clock::now();         // o relogio do "sem video" conta a partir de agora (abertura ja terminou)
        if (g_mpv) logDecoderLocked("carregado");          // o decoder definitivo e conferido de novo no 1o quadro (poll)
    }
    return true;
}

void pause(bool on) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    int f = on ? 1 : 0;
    mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &f);
    g_paused = on;
}

void setSpeed(double rate) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open) return;
    if (rate < 0.25) rate = 0.25;
    if (rate > 4.0) rate = 4.0;
    mpv_set_property(g_mpv, "speed", MPV_FORMAT_DOUBLE, &rate);
}
void setMute(bool on) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    int f = on ? 1 : 0;
    mpv_set_property(g_mpv, "mute", MPV_FORMAT_FLAG, &f);
}

bool seekSec(double sec) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open) return false;
    char buf[32];
    snprintf(buf, sizeof buf, "%f", sec);
    const char *cmd[] = {"seek", buf, "absolute", nullptr};
    return chk(mpv_command(g_mpv, cmd)) >= 0;
}

bool seekRelative(double delta) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open) return false;
    char buf[32];
    snprintf(buf, sizeof buf, "%f", delta);
    const char *cmd[] = {"seek", buf, "relative", nullptr};
    return chk(mpv_command(g_mpv, cmd)) >= 0;
}

void stop() {
    // NAO bloqueia: o "stop" sincrono esperava o demuxer do mpv sair de uma leitura de rede (ate dezenas de segundos) com g_mx preso,
    // travando a thread principal (poll/render pedem g_mx) -> vigia derrubava o app. Assincrono: o mpv encerra em segundo plano.
    std::lock_guard<std::mutex> lk(g_mx);
    guardClear();                                  // parada normal: nao foi queda
    if (!g_mpv) return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command_async(g_mpv, 0, cmd);
    g_open = false;
    g_paused = false;
    g_buffering = false;
    g_everRendered = false;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mx);
    guardClear();
    if (!g_mpv) return;
    mpv_command_string(g_mpv, "quit");
    destroyRender();
    mpv_terminate_destroy(g_mpv);
    g_mpv = nullptr;
    g_cb_registered = false;
    g_started = false; g_startSeen = false; g_curEntry = -1;
    g_open = false;
    g_paused = false;
    g_eof = false;
    resetFrameState();
}

void poll() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    drainEventsUnlocked(nullptr, nullptr);
    if (g_aoUpgrade.load()) applyAoUpgradeLocked();
    // Confere o decoder REAL no 1o quadro e vigia o videodec2: se abriu mas nao entregou video em 12 s, trava o hardware (software ate
    // fechar o app) e pede ao app para reabrir o canal (hwFallbackRequested). Se o mpv ja caiu sozinho no h264 (nao abriu), so avisa.
    if (g_open && !g_hwChecked) {
        if (g_everRendered) {
            g_hwChecked = true;
            guardClear();                                  // chegou video: o decoder nao derrubou o app
            const std::string dec = logDecoderLocked("1o-quadro");
            if (g_hwWanted && dec.find("videodec2") == std::string::npos)
                logBoth("[kick-hw] ATENCAO: pediu hardware mas o decoder real e software. Veja no mpv.log: 'Codec list:' (videodec2 deve ser o 1o), "
                        "'Opening decoder' e as linhas de erro logo depois (Could not open codec / Failed to open / profile nao suportado).");
        } else if (g_hwWanted &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_hwT0).count() > 12000) {
            g_hwChecked = true; g_hwBad = true; g_hwFallbackAsk = true;
            guardClear();
            logDecoderLocked("sem-quadro-12s");
            logBoth("[kick-hw] videodec2 ativo mas sem video em 12 s: hardware desligado ate fechar o app, reabrindo em software");
        }
    }
    // imagem parada: tocando (nao pausado, sem esperar rede) mas nenhum quadro novo ha 2 s. O video segue o RELOGIO DO AUDIO ("video-sync=audio");
    // se o dispositivo de audio nao entrega amostras o relogio para (pos=0, imagem congelada). Entao: 1) passa o video para o relogio do
    // sistema ("desync": a imagem volta na hora), 2) empurra 0,2 s. A proxima midia volta ao "audio".
    if (g_open && !g_paused && !g_buffering && g_stallKicks < 3) {
        const auto nw = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(nw - g_lastFrameT).count() >= 2000) {
            g_stallKicks++; g_lastFrameT = nw;
            char tb[120]; snprintf(tb, sizeof tb, "mpv-travado #%d: audio cb=%u (+%u) pausa=%d", g_stallKicks, g_audCb.load(), g_audCb.load() - g_audCbLast, g_audPause.load());
            tr(tb);
            if (g_stallKicks == 1) mpv_set_property_string(g_mpv, "video-sync", "desync");
            else { const char *c[] = {"seek", "0.2", "relative", nullptr}; mpv_command_async(g_mpv, 0, c); }
        }
    }
    // a cada ~5 s grava fps real e quadros descartados em /data/kickiptv.log (para ver se o decodificador da conta)
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (g_open && std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 5) {
        last = now;
        double fps = 0; int64_t vo = 0, dec = 0;
        mpv_get_property(g_mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fps);
        mpv_get_property(g_mpv, "vo-drop-frame-count", MPV_FORMAT_INT64, &vo);
        mpv_get_property(g_mpv, "decoder-frame-drop-count", MPV_FORMAT_INT64, &dec);
        int rn = g_renderN.exchange(0); long long ru = g_renderUs.exchange(0);
        unsigned cb = g_audCb.load();
        char b[200]; snprintf(b, sizeof b, "mpv-perf fps=%.1f drop-vo=%lld drop-dec=%lld pos=%.0f render=%.0fms(n=%d) audio-cb=%u pausa=%d", fps, (long long)vo, (long long)dec, g_pos, rn ? ru / 1000.0 / rn : 0.0, rn, cb - g_audCbLast, g_audPause.load());
        g_audCbLast = cb;
        tr(b);
    }
}

// Render interno em no maximo 1280 de largura (a CPU Jaguar do PS4 gasta ~48 ms por quadro convertendo/escalando 1080p) e
// ampliacao barata por vizinho mais proximo para o buffer da tela. Quadros repetidos ('force') reaproveitam o ultimo quadro pronto.
// Ampliacao do quadro reduzido para a tela: horizontal com interpolacao linear (suave) e vertical por repeticao de linha. Se o PS4 nao der conta
// (media > 14 ms por quadro), volta sozinho para o vizinho mais proximo (mais rapido, serrilhado) e avisa no log.
static std::vector<int> g_xi;
static std::vector<uint32_t> g_xw;
static int g_xiW = 0, g_xiLw = 0;
static bool g_smoothOn = true;
static double g_smoothMs = 0;
static int g_smoothN = 0;

static void upscaleLo(uint8_t *dst, int w, int h, int stride) {
    if (g_loW == w && g_loH == h) {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * stride, &g_lo[(size_t)y * g_loW], (size_t)w * 4);
        return;
    }
    if (g_xmapW != w || g_xmapLw != g_loW) {
        g_xmap.resize((size_t)w);
        for (int x = 0; x < w; x++) g_xmap[(size_t)x] = (int)((long long)x * g_loW / w);
        g_xmapW = w; g_xmapLw = g_loW;
    }
    if (g_xiW != w || g_xiLw != g_loW) {
        g_xi.resize((size_t)w); g_xw.resize((size_t)w);
        for (int x = 0; x < w; x++) {
            double sx = (x + 0.5) * g_loW / w - 0.5;
            if (sx < 0) sx = 0;
            int i = (int)sx; if (i > g_loW - 2) i = g_loW - 2; if (i < 0) i = 0;
            g_xi[(size_t)x] = i;
            double f = sx - i; if (f > 1) f = 1;
            g_xw[(size_t)x] = (uint32_t)(f * 256.0);
        }
        g_xiW = w; g_xiLw = g_loW;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // A linha ampliada e montada numa linha de RAM e so ENTAO copiada para dst: nunca ler de dst (memoria de video nao-cacheada: ~120 ms por quadro).
    static std::vector<uint32_t> row;
    if (row.size() < (size_t)w) row.resize((size_t)w);
    const bool smooth = g_smoothOn && g_loW >= 2;
    int prevSy = -1;
    for (int y = 0; y < h; y++) {
        int sy = (int)((long long)y * g_loH / h);
        if (sy != prevSy) {
            const uint32_t *src = &g_lo[(size_t)sy * g_loW];
            uint32_t *r = row.data();
            if (smooth) {
                const int *xi = g_xi.data(); const uint32_t *xw = g_xw.data();
                for (int x = 0; x < w; x++) {
                    const uint32_t a = src[xi[x]], b = src[xi[x] + 1], f = xw[x];
                    const uint32_t rb = (((a & 0x00FF00FFu) * (256 - f) + (b & 0x00FF00FFu) * f) >> 8) & 0x00FF00FFu;
                    const uint32_t g = (((a & 0x0000FF00u) * (256 - f) + (b & 0x0000FF00u) * f) >> 8) & 0x0000FF00u;
                    r[x] = rb | g | (a & 0xFF000000u);
                }
            } else {
                const int *xm = g_xmap.data();
                for (int x = 0; x < w; x++) r[x] = src[xm[x]];
            }
            prevSy = sy;
        }
        memcpy(dst + (size_t)y * stride, row.data(), (size_t)w * 4);
    }
    if (smooth) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_smoothMs += ms;
        if (++g_smoothN >= 30) {
            if (g_smoothMs / g_smoothN > 14.0) { g_smoothOn = false; char tb[96]; snprintf(tb, sizeof tb, "mpv-ampliacao: suave lenta (%.1f ms), voltando para rapida", g_smoothMs / g_smoothN); tr(tb); }
            g_smoothMs = 0; g_smoothN = 0;
        }
    }
}

bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes, bool force) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open || !dst || width <= 0 || height <= 0) return false;
    if (!ensureRender()) return false;
    uint64_t flags = mpv_render_context_update(g_rr);
    const bool newFrame = (flags & MPV_RENDER_UPDATE_FRAME) != 0;
    if (!newFrame && !(force && g_everRendered)) return false;
    int lw = width, lh = height;
    if (width > 1280) { lw = 1280; lh = (int)((long long)height * lw / width); if (lh < 2) lh = 2; }
    const bool scaled = (lw != width || lh != height);
    if (!newFrame && force && g_everRendered && scaled && g_loW == lw && g_loH == lh && !g_lo.empty()) {
        upscaleLo(dst, width, height, strideBytes);       // so repinta o ultimo quadro (OSD mudou): custo minimo
        return true;
    }
    uint8_t *target = dst; int tstride = strideBytes;
    if (scaled) {
        if (g_loW != lw || g_loH != lh || g_lo.size() < (size_t)lw * lh) { g_lo.assign((size_t)lw * lh, 0); g_loW = lw; g_loH = lh; }
        target = (uint8_t *)g_lo.data(); tstride = lw * 4;
    }
    int size[2] = {lw, lh};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, (void *)"bgr0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &tstride},
        {MPV_RENDER_PARAM_SW_POINTER, target},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    auto t0 = std::chrono::steady_clock::now();
    int r = mpv_render_context_render(g_rr, params);
    if (r < 0) {
        params[1].data = (void *)"rgb0";
        r = mpv_render_context_render(g_rr, params);
    }
    if (r >= 0) {
        g_everRendered = true;
        g_lastFrameT = std::chrono::steady_clock::now();
        g_renderUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        g_renderN++;
        if (scaled) upscaleLo(dst, width, height, strideBytes);
    }
    return r >= 0;
}

Status status() {
    Status s;
    s.open = g_open;
    s.paused = g_paused;
    s.eof = g_eof;
    s.failed = g_failed;
    s.buffering = g_buffering;
    s.bufferPct = g_bufPct;
    s.positionSec = g_pos;
    s.durationSec = g_dur;
    s.error = g_err;
    return s;
}

bool isOpen() { return g_open.load(); }

bool hwFallbackRequested() { return g_hwFallbackAsk.exchange(false); }

} // namespace mpv_player

#ifdef KICK_SDL_SHIM
// ---------- Backend de audio SDL -> sceAudioOut ----------
// O SDL_InitSubSystem(SDL_INIT_AUDIO) do SDL2 do PacBrew nunca retorna no PS4 (o log para em "sdl-probe: SDL_InitSubSystem"),
// entao o mpv ficava em ao=null (sem som). Estas funcoes substituem as do SDL (o link usa --allow-multiple-definition e o nosso
// objeto vem antes da libSDL2.a): o ao_sdl do mpv continua igual, mas o PCM sai direto pela porta do sceAudioOut.
// Formato fixo: s16 estereo 48 kHz, blocos de 1024 quadros (o mpv ja esta configurado para isso: audio-format/samplerate/channels).
namespace {
const int kShGrain = 1024;
// Instancia do audio: pertence a thread (se o fechamento nao conseguir juntar a thread a tempo, ela e solta e se apaga sozinha).
struct ShInst {
    std::atomic<bool> run{true}, done{false}, orphan{false};
    std::recursive_mutex *cb = new std::recursive_mutex;   // SDL_LockAudio DESTA instancia (nunca apagado: quem destrava pode chegar depois do fechamento)
    int lockFails = 0;
    bool warned = false;
    int port = -1;
    SDL_AudioCallback fn = nullptr;
    void *ud = nullptr;
    pthread_t th;
};
std::mutex g_shCtl;                         // abrir/fechar uma por vez (nunca segurado enquanto espera o callback)
std::recursive_mutex g_shDummy;             // SDL_LockAudio sem instancia aberta. Cada instancia tem o proprio (RECURSIVO como no SDL real: o std::mutex do PS4 e "errorcheck")
thread_local std::recursive_mutex *tlLockM = nullptr;   // mutex travado por esta thread via SDL_LockAudio
thread_local int tlLockDepth = 0;
std::atomic<bool> g_shInit{false}, g_shPause{true};
ShInst *g_shCur = nullptr;

void *shAudioMain(void *arg) {
    ShInst *in = (ShInst *)arg;
    // buffer alinhado em 256 bytes: o sceAudioOutOutput le por DMA e um ponteiro mal alinhado pode derrubar o app
    int16_t *abuf = nullptr;
    if (posix_memalign((void **)&abuf, 256, (size_t)kShGrain * 2 * sizeof(int16_t)) != 0 || !abuf) abuf = nullptr;
    if (abuf) {
        while (in->run.load()) {
            std::memset(abuf, 0, (size_t)kShGrain * 2 * sizeof(int16_t));              // pausado/sem dados = silencio
            if (!g_shPause.load() && in->fn && in->run.load()) {
                // try_lock: se o mpv esta com o SDL_LockAudio (ex.: fechando o ao) NAO fica esperando (isso travava o fechamento). Mas se o lock
                // ficar preso (o mpv trava e nao destrava antes de fechar o dispositivo; o SDL real zera isso), o callback NUNCA era chamado:
                // relogio de audio parado, video congelado em pos=0. Depois de 3 blocos sem conseguir, chama o callback mesmo assim.
                const bool locked = in->cb->try_lock();
                if (locked || ++in->lockFails >= 3) {
                    if (locked) in->lockFails = 0;
                    else if (!in->warned) { in->warned = true; mpv_player::tr("sdl-shim: SDL_LockAudio preso; chamando o callback mesmo assim"); }
                    in->fn(in->ud, (Uint8 *)abuf, kShGrain * 4);
                    mpv_player::g_audCb++;
                }
                if (locked) in->cb->unlock();
            }
            sceAudioOutOutput(in->port, abuf);                                         // bloqueia ate o bloco tocar: e o relogio do audio
        }
        free(abuf);
    }
    in->done.store(true);
    if (in->orphan.exchange(true)) delete in;      // fechamento ja desistiu de esperar: quem apaga e a thread
    return nullptr;
}
bool shOpen(const SDL_AudioSpec *want, SDL_AudioSpec *got) {
    std::lock_guard<std::mutex> cl(g_shCtl);
    if (g_shCur || !want || !want->callback) return false;
    int p = sceAudioOutOpen(0xFF /*usuario do sistema*/, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kShGrain, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (p <= 0) { char b[80]; snprintf(b, sizeof b, "sdl-shim: sceAudioOutOpen falhou rc=%d", p); mpv_player::tr(b); return false; }
    ShInst *in = new ShInst;
    in->port = p; in->fn = want->callback; in->ud = want->userdata;
    g_shPause.store(true);
    pthread_attr_t at; bool ok = false;
    if (pthread_attr_init(&at) == 0) {
        pthread_attr_setstacksize(&at, 1u << 19);
        ok = pthread_create(&in->th, &at, shAudioMain, in) == 0;
        pthread_attr_destroy(&at);
    }
    if (!ok) { sceAudioOutClose(p); delete in; mpv_player::tr("sdl-shim: nao criou a thread"); return false; }
    g_shCur = in;
    if (got) {
        *got = *want;
        got->freq = 48000; got->format = AUDIO_S16SYS; got->channels = 2; got->silence = 0;
        got->samples = kShGrain; got->size = kShGrain * 4;
    }
    char b[80]; snprintf(b, sizeof b, "sdl-shim: audio aberto port=%d (s16 2ch 48000)", p); mpv_player::tr(b);
    return true;
}
void shClose() {
    ShInst *in = nullptr;
    { std::lock_guard<std::mutex> cl(g_shCtl); in = g_shCur; g_shCur = nullptr; }
    if (!in) return;
    in->run.store(false);
    in->fn = nullptr;
    // espera ate ~1,5 s a thread sair. Antes: pthread_join sem limite -> se o callback do mpv estava preso em um lock que quem fecha
    // segurava, os dois esperavam um pelo outro (deadlock no fechamento/troca de canal).
    for (int i = 0; i < 150 && !in->done.load(); i++) sceKernelUsleep(10000);
    const int port = in->port;
    if (in->done.load()) {
        pthread_join(in->th, nullptr);
        sceAudioOutClose(port);
        delete in;
    } else {
        pthread_detach(in->th);
        mpv_player::tr("sdl-shim: thread de audio demorou a sair (solta)");
        // a porta so fecha quando a thread sair do sceAudioOutOutput: fecha agora (desbloqueia a saida) e a thread se apaga
        sceAudioOutClose(port);
        if (in->orphan.exchange(true)) delete in;
    }
    mpv_player::tr("sdl-shim: audio fechado");
}
}  // namespace

extern "C" {
int SDL_InitSubSystem(Uint32 flags) { if (flags & SDL_INIT_AUDIO) g_shInit.store(true); return 0; }
void SDL_QuitSubSystem(Uint32 flags) { if (flags & SDL_INIT_AUDIO) { shClose(); g_shInit.store(false); } }
Uint32 SDL_WasInit(Uint32 flags) { return g_shInit.load() && (flags == 0 || (flags & SDL_INIT_AUDIO)) ? SDL_INIT_AUDIO : 0; }
int SDL_GetNumAudioDrivers(void) { return 1; }
const char *SDL_GetAudioDriver(int i) { return i == 0 ? "ps4" : nullptr; }
const char *SDL_GetCurrentAudioDriver(void) { return g_shInit.load() ? "ps4" : nullptr; }
int SDL_GetNumAudioDevices(int cap) { return cap ? 0 : 1; }
const char *SDL_GetAudioDeviceName(int i, int cap) { return !cap && i == 0 ? "PS4" : nullptr; }
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) { return shOpen(desired, obtained) ? 0 : -1; }
SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int cap, const SDL_AudioSpec *desired, SDL_AudioSpec *obtained, int) {
    return !cap && shOpen(desired, obtained) ? 2 : 0;
}
void SDL_PauseAudio(int on) { g_shPause.store(on != 0); mpv_player::g_audPause = on != 0; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID, int on) { g_shPause.store(on != 0); mpv_player::g_audPause = on != 0; }
SDL_AudioStatus SDL_GetAudioStatus(void) { return !g_shCur ? SDL_AUDIO_STOPPED : g_shPause.load() ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING; }
SDL_AudioStatus SDL_GetAudioDeviceStatus(SDL_AudioDeviceID) { return SDL_GetAudioStatus(); }
static void shLock() {
    if (tlLockDepth == 0) { ShInst *c = g_shCur; tlLockM = c ? c->cb : &g_shDummy; }   // o mesmo mutex ate o ultimo Unlock desta thread
    tlLockM->lock(); tlLockDepth++;
}
static void shUnlock() { if (tlLockDepth > 0 && tlLockM) { tlLockM->unlock(); if (--tlLockDepth == 0) tlLockM = nullptr; } }
void SDL_LockAudio(void) { shLock(); }
void SDL_UnlockAudio(void) { shUnlock(); }
void SDL_LockAudioDevice(SDL_AudioDeviceID) { shLock(); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID) { shUnlock(); }
void SDL_CloseAudio(void) { shClose(); }
void SDL_CloseAudioDevice(SDL_AudioDeviceID) { shClose(); }
}
#endif

#endif
