#include "mpv_player.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#ifndef USE_LIBMPV

namespace mpv_player {
bool available() { return false; }
bool boot() { return false; }
bool play(const std::string &) { return false; }
void pause(bool) {}
bool seekSec(double) { return false; }
bool seekRelative(double) { return false; }
void stop() {}
void shutdown() {}
Status status() { return {}; }
bool isOpen() { return false; }
void poll() {}
}

#else

extern "C" {
#include <mpv/client.h>
}

namespace mpv_player {
namespace {

std::mutex g_mx;
mpv_handle *g_mpv = nullptr;
std::atomic<bool> g_open{false};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_eof{false};
std::atomic<bool> g_failed{false};
std::string g_err;
double g_pos = 0, g_dur = 0;

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

} // namespace

bool available() { return true; }

bool boot() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_mpv) return true;
    g_mpv = mpv_create();
    if (!g_mpv) { setErr("mpv_create falhou"); return false; }

    // Opções alinhadas ao GMCA (mpv_core.cpp) no PS4
    mpv_set_option_string(g_mpv, "ytdl", "no");
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "video-timing-offset", "0");
    mpv_set_option_string(g_mpv, "reset-on-next-file", "speed,pause");
    mpv_set_option_string(g_mpv, "audio-channels", "stereo");
    mpv_set_option_string(g_mpv, "vo", "libmpv");           // render API (precisa GL como no GMCA)
    mpv_set_option_string(g_mpv, "hwdec", "no");            // FFmpeg software no PS4 homebrew
    mpv_set_option_string(g_mpv, "vd-lavc-threads", "2");
    mpv_set_option_string(g_mpv, "cache", "yes");
    mpv_set_option_string(g_mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(g_mpv, "demuxer-max-back-bytes", "32MiB");
    mpv_set_option_string(g_mpv, "network-timeout", "30");
    mpv_set_option_string(g_mpv, "user-agent", "KickIPTV/mpv");

    if (chk(mpv_initialize(g_mpv)) < 0) {
        mpv_terminate_destroy(g_mpv);
        g_mpv = nullptr;
        return false;
    }
    mpv_observe_property(g_mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 2, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(g_mpv, 3, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(g_mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
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
    g_err.clear();
    // loadfile <url> replace
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
    // IGUAL ao GMCA MPVCore::clean(): quit + terminate_destroy
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    mpv_command_string(g_mpv, "quit");
    mpv_terminate_destroy(g_mpv);  // chave da saída sem CE
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
                g_err = "end-file error";
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = (mpv_event_property *)ev->data;
            if (!prop || !prop->data) break;
            if (prop->format == MPV_FORMAT_DOUBLE) {
                if (std::strcmp(prop->name, "time-pos") == 0) g_pos = *(double *)prop->data;
                if (std::strcmp(prop->name, "duration") == 0) g_dur = *(double *)prop->data;
            } else if (prop->format == MPV_FORMAT_FLAG) {
                if (std::strcmp(prop->name, "pause") == 0) g_paused = *(int *)prop->data != 0;
                if (std::strcmp(prop->name, "eof-reached") == 0) g_eof = *(int *)prop->data != 0;
            }
            break;
        }
        default:
            break;
        }
    }
}

Status status() {
    Status s;
    s.open = g_open;
    s.paused = g_paused;
    s.eof = g_eof;
    s.failed = g_failed;
    s.positionSec = g_pos;
    s.durationSec = g_dur;
    s.error = g_err;
    return s;
}

bool isOpen() { return g_open.load(); }

} // namespace mpv_player

#endif // USE_LIBMPV
