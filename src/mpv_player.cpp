// KICKIPTV_STREAM_CB_V3 - loadfile DEVE usar khttp:// (HTTP do app, nao FFmpeg DNS)
#include "mpv_player.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifndef USE_LIBMPV

namespace mpv_player {
void setStreamIo(const StreamIo &) {}
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
#include <mpv/stream_cb.h>
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
StreamIo g_io{};
bool g_cb_registered = false;

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
    info->seek_fn = cb_seek;
    info->size_fn = cb_size;
    info->close_fn = cb_close;
    info->cancel_fn = nullptr;
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

static void drainEventsUnlocked(bool *gotLoaded, bool *gotFail) {
    while (true) {
        mpv_event *ev = mpv_wait_event(g_mpv, 0);
        if (!ev || ev->event_id == MPV_EVENT_NONE) break;
        switch (ev->event_id) {
        case MPV_EVENT_SHUTDOWN:
            g_open = false;
            if (gotFail) *gotFail = true;
            break;
        case MPV_EVENT_FILE_LOADED:
            if (gotLoaded) *gotLoaded = true;
            g_failed = false;
            g_err.clear();
            break;
        case MPV_EVENT_END_FILE: {
            auto *e = (mpv_event_end_file *)ev->data;
            if (!e) break;
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
            if (!prop || !prop->data) break;
            if (prop->format == MPV_FORMAT_DOUBLE) {
                if (!std::strcmp(prop->name, "time-pos")) g_pos = *(double *)prop->data;
                if (!std::strcmp(prop->name, "duration")) g_dur = *(double *)prop->data;
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
}

} // namespace

void setStreamIo(const StreamIo &io) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_io = io;
}

bool available() { return true; }

bool boot() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_mpv) return true;
    g_mpv = mpv_create();
    if (!g_mpv) {
        setErr("mpv_create falhou");
        return false;
    }

    mpv_set_option_string(g_mpv, "log-file", "/data/kickiptv_mpv.log");
    mpv_set_option_string(g_mpv, "msg-level", "ffmpeg=v,cplayer=v,stream=v,demux=v");
    mpv_set_option_string(g_mpv, "ytdl", "no");
    mpv_set_option_string(g_mpv, "osd-level", "0");
    mpv_set_option_string(g_mpv, "vo", "libmpv");
    mpv_set_option_string(g_mpv, "hwdec", "no");
    mpv_set_option_string(g_mpv, "vd-lavc-threads", "4");
    mpv_set_option_string(g_mpv, "vd-lavc-fast", "yes");
    mpv_set_option_string(g_mpv, "cache", "yes");
    mpv_set_option_string(g_mpv, "demuxer-max-bytes", "96MiB");
    mpv_set_option_string(g_mpv, "demuxer-max-back-bytes", "32MiB");
    mpv_set_option_string(g_mpv, "force-seekable", "yes");
    mpv_set_option_string(g_mpv, "audio-channels", "stereo");
    mpv_set_option_string(g_mpv, "ao", "sdl,null");
    mpv_set_option_string(g_mpv, "demuxer-lavf-analyzeduration", "5");
    mpv_set_option_string(g_mpv, "demuxer-lavf-probesize", "5242880");
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

bool play(const std::string &url) {
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

        g_eof = false;
        g_failed = false;
        g_buffering = true;
        g_bufPct = 0;
        g_pos = 0;
        g_dur = 0;
        g_err.clear();
        ensureRender();

        // Se esta linha nao aparecer no /data/kickiptv_mpv.log como khttp://... o PKG e velho
        std::string wrap = std::string("khttp://") + url;
        if (wrap.find("khttp://http://") != 0 && wrap.find("khttp://https://") != 0) {
            // url relativa? ainda assim prefixa
            wrap = std::string("khttp://") + url;
        }
        // Dica de formato pro lavf
        std::string opts = "force-seekable=yes";
        auto lower = url;
        for (char &c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (lower.find(".mp4") != std::string::npos || lower.find("/movie/") != std::string::npos)
            opts += ",demuxer-lavf-format=mp4";
        else if (lower.find(".mkv") != std::string::npos)
            opts += ",demuxer-lavf-format=matroska";
        else if (lower.find(".m3u8") != std::string::npos || lower.find(".ts") != std::string::npos)
            opts += ",demuxer-lavf-format=mpegts";
        else
            opts += ",demuxer-lavf-format=mp4"; // painel IPTV: maioria e mp4 progressivo

        const char *cmd[] = {"loadfile", wrap.c_str(), "replace", "0", opts.c_str(), nullptr};
        if (chk(mpv_command(g_mpv, cmd)) < 0) {
            // fallback sem opts
            const char *cmd2[] = {"loadfile", wrap.c_str(), "replace", nullptr};
            if (chk(mpv_command(g_mpv, cmd2)) < 0) {
                g_open = false;
                return false;
            }
        }
    }
    // IMPORTANTE: nao segurar g_mx enquanto o demuxer le via stream_cb

    bool gotLoaded = false, gotFail = false;
    for (int i = 0; i < 400 && !gotLoaded && !gotFail; i++) {
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_mpv) return false;
            drainEventsUnlocked(&gotLoaded, &gotFail);
        }
        if (gotLoaded || gotFail) break;
        mpv_event *ev = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_mpv) return false;
            ev = mpv_wait_event(g_mpv, 0.1);
            if (ev && ev->event_id != MPV_EVENT_NONE) {
                if (ev->event_id == MPV_EVENT_FILE_LOADED) {
                    gotLoaded = true;
                    g_failed = false;
                    g_err.clear();
                } else if (ev->event_id == MPV_EVENT_END_FILE) {
                    auto *e = (mpv_event_end_file *)ev->data;
                    if (e && e->reason == MPV_END_FILE_REASON_ERROR) {
                        gotFail = true;
                        g_failed = true;
                        g_err = std::string("end-file: ") + mpv_error_string(e->error);
                    }
                } else if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                    auto *prop = (mpv_event_property *)ev->data;
                    if (prop && prop->data && prop->format == MPV_FORMAT_DOUBLE) {
                        if (!std::strcmp(prop->name, "duration") && *(double *)prop->data > 0.5) {
                            gotLoaded = true;
                            g_dur = *(double *)prop->data;
                        }
                        if (!std::strcmp(prop->name, "time-pos")) g_pos = *(double *)prop->data;
                    }
                }
            }
        }
    }

    if (gotFail || g_failed || !gotLoaded) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_open = false;
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
    g_buffering = false;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    mpv_command_string(g_mpv, "quit");
    destroyRender();
    mpv_terminate_destroy(g_mpv);
    g_mpv = nullptr;
    g_cb_registered = false;
    g_open = false;
    g_paused = false;
    g_eof = false;
}

void poll() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv) return;
    drainEventsUnlocked(nullptr, nullptr);
}

bool renderToBgra(uint8_t *dst, int width, int height, int strideBytes) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mpv || !g_open || !dst || width <= 0 || height <= 0) return false;
    if (!ensureRender()) return false;
    uint64_t flags = mpv_render_context_update(g_rr);
    if (!(flags & MPV_RENDER_UPDATE_FRAME)) return false;
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
