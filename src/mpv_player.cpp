#include "mpv_player.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef USE_LIBMPV

namespace mpv_player {
bool available() { return false; }
bool boot() { return false; }
bool play(const std::string &) { return false; }
void pause(bool) {}
void setMute(bool) {}
bool seekSec(double) { return false; }
bool seekRelative(double) { return false; }
void stop() {}
void shutdown() {}
Status status() { return {}; }
bool isOpen() { return false; }
void poll() {}
bool renderToBgra(uint8_t *, int, int, int) { return false; }
}

#else

extern "C" {
#include <mpv/client.h>
#include <mpv/render.h>
}

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
int g_needRedraw = 0;

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
static void onRender(void *) { g_needRedraw = 1; }

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
        setErr("mpv_render_context_create(SW) falhou");
        g_rr = nullptr;
        return false;
    }
    mpv_render_context_set_update_callback(g_rr, onRender, nullptr);
    return true;
}

} // namespace

bool available() { return true; }

bool boot() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_mpv) return true;
    g_mpv = mpv_create();
    if (!g_mpv) {
        setErr("mpv_create falhou");
        return false;
    }

    // Opcoes alinhadas ao GMCA (PS4)
    mpv_set_option_string(g_mpv, "log-file", "/data/kickiptv_mpv.log");
    mpv_set_option_string(g_mpv, "msg-level", "all=v");
    mpv_set_option_string(g_mpv, "tls-verify", "no");
    mpv_set_option_string(g_mpv, "ytdl", "no");
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "video-timing-offset", "0");
    mpv_set_option_string(g_mpv, "reset-on-next-file", "speed,pause");
    mpv_set_option_string(g_mpv, "audio-channels", "stereo");
    mpv_set_option_string(g_mpv, "vo", "libmpv");
    mpv_set_option_string(g_mpv, "hwdec", "no");
    // identico ao mpv_core.cpp do GMCA para PS4 (decodificacao por software, 6 threads)
    mpv_set_option_string(g_mpv, "vd-lavc-skiploopfilter", "all");
    mpv_set_option_string(g_mpv, "vd-lavc-fast", "yes");
    mpv_set_option_string(g_mpv, "vd-lavc-dr", "yes");
    mpv_set_option_string(g_mpv, "vd-lavc-threads", "6");
    mpv_set_option_string(g_mpv, "sdl-buflen", "0.2");
    mpv_set_option_string(g_mpv, "audio-buffer", "0.5");
    mpv_set_option_string(g_mpv, "framedrop", "vo");
    mpv_set_option_string(g_mpv, "sws-fast", "yes");
    mpv_set_option_string(g_mpv, "cache", "yes");
    mpv_set_option_string(g_mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(g_mpv, "demuxer-max-back-bytes", "32MiB");
    mpv_set_option_string(g_mpv, "network-timeout", "30");
    mpv_set_option_string(g_mpv, "user-agent", "KickIPTV/mpv");
    mpv_set_option_string(g_mpv, "audio-client-name", "KickIPTV");

    if (chk(mpv_initialize(g_mpv)) < 0) {
        mpv_terminate_destroy(g_mpv);
        g_mpv = nullptr;
        return false;
    }
    mpv_observe_property(g_mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 5, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 6, "cache-buffering-state", MPV_FORMAT_INT64);

    if (!ensureRender()) {
        // segue sem video SW (audio/eventos ainda funcionam)
    }
    g_failed = false;
    g_err.clear();
    return true;
}

bool play(const std::string &url) {
    if (!boot()) return false;
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return false;
    g_eof = false;
    g_failed = false;
    g_buffering = false;
    g_pos = 0; g_dur = 0;
    g_err.clear();
    ensureRender();
    const char *cmd[] = {"loadfile", url.c_str(), "replace", nullptr};
    if (chk(mpv_command(g_mpv, cmd)) < 0) return false;
    g_open = true;
    g_paused = false;
    return true;
}

void pause(bool on) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    int f = on ? 1 : 0;
    mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &f);
    g_paused = on;
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
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    const char *cmd[] = {"stop", nullptr};
    mpv_command(g_mpv, cmd);
    g_open = false;
    g_paused = false;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    mpv_command_string(g_mpv, "quit");
    destroyRender();
    mpv_terminate_destroy(g_mpv);
    g_mpv = nullptr;
    g_open = false;
    g_paused = false;
    g_eof = false;
}

void poll() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    while (true) {
        mpv_event *ev = mpv_wait_event(g_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_SHUTDOWN:
            g_open = false;
            break;
        case MPV_EVENT_END_FILE: {
            auto *e = (mpv_event_end_file *)ev->data;
            if (e && e->reason == MPV_END_FILE_REASON_EOF) g_eof = true;
            if (e && e->reason == MPV_END_FILE_REASON_ERROR) {
                g_failed = true;
                g_err = std::string("end-file: ") + mpv_error_string(e->error);
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = (mpv_event_property *)ev->data;
            if (!prop || !prop->data) break;
            if (prop->format == MPV_FORMAT_DOUBLE) {
                if (!std::strcmp(prop->name, "time-pos")) g_pos = *(double *)prop->data;
                if (!std::strcmp(prop->name, "duration")) g_dur = *(double *)prop->data;
            } else if (prop->format == MPV_FORMAT_INT64) {
                if (!std::strcmp(prop->name, "cache-buffering-state")) g_bufPct = (int)*(int64_t *)prop->data;
            } else if (prop->format == MPV_FORMAT_FLAG) {
                if (!std::strcmp(prop->name, "paused-for-cache")) g_buffering = *(int *)prop->data != 0;
                if (!std::strcmp(prop->name, "pause")) g_paused = *(int *)prop->data != 0;
                if (!std::strcmp(prop->name, "eof-reached")) g_eof = *(int *)prop->data != 0;
            }
            break;
        }
        default:
            break;
        }
    }
}

bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open || !dst || width <= 0 || height <= 0) return false;
    if (!ensureRender()) return false;
    // Atualiza o contexto (pode liberar frames)
    uint64_t flags = mpv_render_context_update(g_rr);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) return false;      // sem quadro novo: nao redesenha (poupa CPU); o chamador mantem a imagem anterior

    int size[2] = {width, height};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, (void *)"bgr0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &strideBytes},
        {MPV_RENDER_PARAM_SW_POINTER, dst},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    int r = mpv_render_context_render(g_rr, params);
    if (r < 0) {
        // fallback rgb0
        params[1].data = (void *)"rgb0";
        r = mpv_render_context_render(g_rr, params);
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

} // namespace mpv_player

#endif
