// TESTE 5 - PLAYER com AvPlayer (video + audio) + teclado na tela + diagnostico.
//   HLS (.m3u8): o app baixa playlist/segmentos e entrega ao AvPlayer por callbacks de arquivo (hls.hpp / streambuf.hpp).
//   Aba Kick (abre por padrao):
//     X em "Digitar @streamer" = teclado na tela (D-pad move, X digita, QUADRADO apaga, R1 = OK, O = cancela)
//     X em "TESTE do player"   = toca um video HLS publico (valida o player sem depender da Kick)
//   Durante o video: TRIANGULO = muda o "modo de stride" do desenho (se a imagem sair torta) | O = voltar | OPTIONS = sair
//   Menu: QUADRADO = teste de rede
// Precisa do src/font8x16.hpp. Makefile liga -lSceAvPlayer e -lSceAudioOut.
// Headers do C++ primeiro (mesma ordem do sample "threading" do OpenOrbis): evita conflito de tipos com os headers orbis.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hls.hpp"
#include "streambuf.hpp"
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include "ui.hpp"
#include "font8x16.hpp"

// ---------- Cliente HTTP (libSceNet + libSceSsl + libSceHttp, ligadas no link) ----------
// Prototipos declarados aqui para nao depender dos nomes dos headers do toolchain.
extern "C" {
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t poolSize);
int sceHttpInit(int libnetMemId, int libsslCtxId, size_t poolSize);
int sceHttpCreateTemplate(int libhttpCtxId, const char *userAgent, int httpVer, int isAutoProxyConf);
int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int enableKeepalive);
int sceHttpCreateRequestWithURL(int connId, int method, const char *url, unsigned long long contentLength);
int sceHttpSendRequest(int reqId, const void *postData, size_t size);
int sceHttpGetStatusCode(int reqId, int *statusCode);
int sceHttpReadData(int reqId, void *data, size_t size);
int sceHttpDeleteRequest(int id);
int sceHttpDeleteConnection(int id);
int sceHttpDeleteTemplate(int id);
int sceHttpsDisableOption(int id, unsigned flags);
int sceHttpSetAutoRedirect(int id, int enable);
int sceHttpAddRequestHeader(int id, const char *name, const char *value, int mode);
int sceHttpSetConnectTimeOut(int id, unsigned usec);
int sceHttpSetSendTimeOut(int id, unsigned usec);
int sceHttpSetRecvTimeOut(int id, unsigned usec);
int sceHttpSetResolveTimeOut(int id, unsigned usec);
}

struct NetClient {
    std::vector<std::string> *log = nullptr;
    bool ready = false, failed = false;
    int httpCtx = -1;

    void L(const std::string &s) { if (log) log->push_back(s); }
    static std::string hex(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    bool init() {
        if (ready) return true;
        if (failed) { L("(inicializacao ja falhou acima)"); return false; }
        failed = true;
        // IDs internos de modulo do PS4: NetCtl 0x80000009, Http 0x8000000A, Ssl 0x8000000B, Net 0x8000001C
        const uint32_t ids[4] = {0x8000001C, 0x80000009, 0x8000000A, 0x8000000B};
        const char *names[4] = {"Net", "NetCtl", "Http", "Ssl"};
        for (int i = 0; i < 4; i++) L(std::string("carregar modulo ") + names[i] + ": " + hex(sceSysmoduleLoadModuleInternal(ids[i])));
        int rc = sceNetInit();                         L("sceNetInit: " + hex(rc));
        int netMem = sceNetPoolCreate("kickiptv", 64 * 1024, 0); L("sceNetPoolCreate: " + hex(netMem));
        int ssl = sceSslInit(300 * 1024);              L("sceSslInit: " + hex(ssl));
        httpCtx = sceHttpInit(netMem, ssl, 128 * 1024); L("sceHttpInit: " + hex(httpCtx));
        if (httpCtx < 0) return false;
        ready = true;
        failed = false;
        return true;
    }

    // GET simples. Retorna true se conseguiu falar com o servidor (mesmo com status de erro).
    bool get(const std::string &url, std::string &body, int &status, size_t maxBytes = 1536 * 1024) {
        status = 0; body.clear();
        if (!init()) return false;
        const char *ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        int tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        L("  CreateTemplate: " + hex(tmpl));
        if (tmpl < 0) return false;
        sceHttpsDisableOption(tmpl, 0x1F);   // sem verificacao de certificado (o app nao carrega os CAs do PS4)
        sceHttpSetAutoRedirect(tmpl, 1);
        const unsigned T = 15 * 1000 * 1000;
        sceHttpSetResolveTimeOut(tmpl, T);
        sceHttpSetConnectTimeOut(tmpl, T);
        sceHttpSetSendTimeOut(tmpl, T);
        sceHttpSetRecvTimeOut(tmpl, T);
        int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1);
        L("  CreateConnection: " + hex(conn));
        bool ok = false;
        if (conn >= 0) {
            int req = sceHttpCreateRequestWithURL(conn, 0 /*GET*/, url.c_str(), 0);
            L("  CreateRequest: " + hex(req));
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "application/json, text/plain, */*", 0);
                sceHttpAddRequestHeader(req, "Accept-Language", "pt-BR,pt;q=0.9,en;q=0.8", 0);
                int rc = sceHttpSendRequest(req, NULL, 0);
                L("  SendRequest: " + hex(rc));
                if (rc >= 0) {
                    sceHttpGetStatusCode(req, &status);
                    std::vector<char> buf(32 * 1024);
                    for (;;) {
                        int n = sceHttpReadData(req, buf.data(), buf.size());
                        if (n <= 0) break;
                        body.append(buf.data(), n);
                        if (body.size() > maxBytes) break;
                    }
                    ok = true;
                }
                sceHttpDeleteRequest(req);
            }
            sceHttpDeleteConnection(conn);
        }
        sceHttpDeleteTemplate(tmpl);
        return ok;
    }
};

static NetClient g_net;

// ---------- Plataforma ----------
struct Ps4Platform : Platform {
    App *app = nullptr;
    std::string pendingText;                 // texto digitado no teclado na tela (@streamer)
    bool kbdRequested = false;               // UI pediu o teclado
    bool avDiagRequested = false;            // UI pediu o diagnostico do AvPlayer
    void command(const std::string &c) override { if (c == "avdiag") avDiagRequested = true; }
    std::string pendingPlayUrl, pendingPlayTitle;
    bool httpGet(const std::string &url, std::string &body) override {
        int st = 0;
        bool ok = g_net.get(url, body, st);
        return ok && st >= 200 && st < 300;
    }
    void play(const std::string &url, const std::string &title) override { pendingPlayUrl = url; pendingPlayTitle = title; }
    void requestText(const std::string &) override { kbdRequested = true; }
};

// ---------- Renderer (igual ao Teste 2) ----------
struct FbRenderer : Renderer {
    uint32_t *fb; int W, H;
    FbRenderer(void *p, int w, int h) : fb((uint32_t *)p), W(w), H(h) {}
    int width() const override { return W; }
    int height() const override { return H; }
    void fillRect(int x, int y, int w, int h, unsigned argb) override {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        for (int j = y0; j < y1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            for (int i = x0; i < x1; i++) row[i] = argb;
        }
    }
    void drawText(int x, int y, const std::string &text, unsigned argb, int size) override {
        float s = size / 16.0f;
        int adv = (int)(8 * s + 0.5f);
        int cx = x;
        for (size_t i = 0; i < text.size();) {
            unsigned char c = text[i];
            unsigned cp;
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c >> 5) == 6 && i + 1 < text.size()) { cp = ((c & 0x1F) << 6) | (text[i + 1] & 0x3F); i += 2; }
            else if ((c >> 4) == 14 && i + 2 < text.size()) { cp = '?'; i += 3; }
            else if ((c >> 3) == 30 && i + 3 < text.size()) { cp = '?'; i += 4; }
            else { cp = '?'; i += 1; }
            int gi;
            if (cp >= 32 && cp <= 126) gi = cp - 32;
            else if (cp >= 160 && cp <= 255) gi = 95 + (cp - 160);
            else gi = '?' - 32;
            const unsigned char *g = kFont8x16[gi];
            for (int gy = 0; gy < 16; gy++) {
                unsigned char bits = g[gy];
                if (!bits) continue;
                int py0 = y + (int)(gy * s), py1 = y + (int)((gy + 1) * s);
                if (py1 == py0) py1 = py0 + 1;
                for (int gx = 0; gx < 8; gx++) {
                    if (!(bits & (0x80 >> gx))) continue;
                    int px0 = cx + (int)(gx * s), px1 = cx + (int)((gx + 1) * s);
                    if (px1 == px0) px1 = px0 + 1;
                    fillRect(px0, py0, px1 - px0, py1 - py0, argb);
                }
            }
            cx += adv;
        }
    }
};

// ---------- Diagnostico ----------
static std::string clean(const std::string &s, size_t maxLen) {
    std::string o;
    for (size_t i = 0; i < s.size() && o.size() < maxLen; i++) {
        unsigned char c = s[i];
        o += (c >= 32 && c < 127) ? (char)c : ' ';
    }
    return o;
}
static bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

static void runNetTest(std::vector<std::string> &diag) {
    diag.clear();
    g_net.log = &diag;
    diag.push_back("== TESTE DE REDE ==");
    struct T { const char *name; const char *url; };
    T tests[] = {{"Controle (example.com)", "https://example.com/"},
                 {"Kick API", "https://kick.com/api/v2/channels/xqc"}};
    for (auto &t : tests) {
        diag.push_back("");
        diag.push_back(std::string("-- ") + t.name + ": " + t.url);
        std::string body; int st = 0;
        bool ok = g_net.get(t.url, body, st);
        diag.push_back(std::string("   resultado: ") + (ok ? "respondeu" : "FALHOU") + "  status=" + std::to_string(st) + "  bytes=" + std::to_string(body.size()));
        if (!body.empty()) {
            std::string c = clean(body, 330);
            for (size_t p = 0; p < c.size(); p += 110) diag.push_back("   > " + c.substr(p, 110));
            if (has(body, "Just a moment") || has(body, "cloudflare") || has(body, "cf-chl")) diag.push_back("   >>> PARECE BLOQUEIO DO CLOUDFLARE");
            if (has(body, "playback_url")) diag.push_back("   >>> ACHOU playback_url (a API respondeu!)");
        }
    }
    diag.push_back("");
    diag.push_back("Fim. Tire um print desta tela e mande para o Claude.");
    g_net.log = nullptr;
}

static void renderDiag(FbRenderer &r, const std::vector<std::string> &diag, bool running) {
    r.fillRect(0, 0, r.W, r.H, 0xFF101018);
    r.drawText(40, 20, running ? "Executando teste... (pode levar ate 1 minuto)" : "Diagnostico", 0xFF53FC18, 28);
    int y = 70;
    size_t start = diag.size() > 36 ? diag.size() - 36 : 0;
    for (size_t i = start; i < diag.size(); i++) { r.drawText(40, y, diag[i], 0xFFFFFFFF, 20); y += 26; }
    r.drawText(40, r.H - 50, "O voltar ao menu   OPTIONS sair", 0xFF777777, 22);
}

// ---------- Dados de exemplo ----------
static void fillDemo(App &app) {
    std::string m = "#EXTM3U\n";
    for (int i = 1; i <= 40; i++)
        m += "#EXTINF:-1 group-title=\"Abertos\",[DEMO sem video] Canal de Exemplo " + std::to_string(i) + "\nhttp://exemplo/live/" + std::to_string(i) + ".ts\n";
    for (int i = 1; i <= 15; i++)
        m += "#EXTINF:-1 group-title=\"Filmes\",Filme de A\xC3\xA7\xC3\xA3o " + std::to_string(i) + "\nhttp://exemplo/movie/" + std::to_string(i) + ".mp4\n";
    for (int s = 1; s <= 2; s++)
        for (int e = 1; e <= 6; e++)
            m += "#EXTINF:-1 group-title=\"Series\",Dark S0" + std::to_string(s) + "E0" + std::to_string(e) + "\nhttp://exemplo/series/d" +
                 std::to_string(s * 10 + e) + ".mp4\n";
    app.list = buildPlaylist(m);
    app.say("TESTE 5 | Kick: X digita @streamer | QUADRADO = rede | OPTIONS sai");
}

// ---------- AvPlayer (libSceAvPlayer) - structs proprias para nao depender do header ----------
typedef void *(*KAvAlloc)(void *arg, uint32_t align, uint32_t size);
typedef void (*KAvFree)(void *arg, void *mem);
typedef int (*KAvFileOpen)(void *arg, const char *name);
typedef int (*KAvFileClose)(void *arg);
typedef int (*KAvFileRead)(void *arg, uint8_t *buf, uint64_t pos, uint32_t len);
typedef uint64_t (*KAvFileSize)(void *arg);
struct KAvInitData {
    void *memObj; KAvAlloc allocate; KAvFree deallocate; KAvAlloc allocateTexture; KAvFree deallocateTexture;   // memoryReplacement
    void *fileObj; KAvFileOpen fOpen; KAvFileClose fClose; KAvFileRead fRead; KAvFileSize fSize;                // fileReplacement
    void *eventObj; void *eventCb;                                                                              // eventReplacement
    int32_t debugLevel;
    uint32_t basePriority;
    int32_t numOutputVideoFrameBuffers;
    bool autoStart;
    uint8_t reserved[3];
    const char *defaultLanguage;
};
struct KAvFrameInfo {
    uint8_t *pData;
    uint32_t reserved;
    uint64_t timeStamp;
    uint8_t details[216];      // union de detalhes (video: width@0 height@4; audio: ch@0 rate@4 size@8)
};
typedef unsigned long KAvHandle;          // 64 bits (LP64); nao usa uintptr_t
extern "C" {
int sceSysmoduleLoadModule(uint16_t id);
KAvHandle sceAvPlayerInit(KAvInitData *init);
int sceAvPlayerAddSource(KAvHandle h, const char *url);
bool sceAvPlayerIsActive(KAvHandle h);
bool sceAvPlayerGetVideoData(KAvHandle h, KAvFrameInfo *fi);
bool sceAvPlayerGetAudioData(KAvHandle h, KAvFrameInfo *fi);
int sceAvPlayerStop(KAvHandle h);
int sceAvPlayerClose(KAvHandle h);
}

static void *alignedAlloc(size_t align, size_t size) {
    if (align < 16) align = 16;
    void *raw = malloc(size + align + sizeof(void *));
    if (!raw) return NULL;
    unsigned long a = ((unsigned long)raw + sizeof(void *) + align - 1) & ~(unsigned long)(align - 1);
    ((void **)a)[-1] = raw;
    return (void *)a;
}
static void alignedFree(void *p) { if (p) free(((void **)p)[-1]); }
static void *avAlloc(void *, uint32_t align, uint32_t size) { return alignedAlloc(align, size); }
static void avFree(void *, void *p) { alignedFree(p); }

// memoria "de textura" (os quadros de video): memoria direta mapeada, como nos samples da Sony
struct TexMem { off_t phys; size_t size; };
static std::map<void *, TexMem> g_tex;
static void *avAllocTex(void *, uint32_t align, uint32_t size) {
    size_t al = align < 0x10000 ? 0x10000 : align;
    size_t sz = (size + 0xFFFF) & ~(size_t)0xFFFF;
    off_t phys = 0; void *addr = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, 3, &phys) < 0) return NULL;
    if (sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, al) < 0) return NULL;
    g_tex[addr] = {phys, sz};
    return addr;
}
static void avFreeTex(void *, void *p) {
    auto it = g_tex.find(p);
    if (it == g_tex.end()) return;
    sceKernelReleaseDirectMemory(it->second.phys, it->second.size);
    g_tex.erase(it);
}

// ---------- NV12 -> BGRA (vizinho mais proximo, com escala) ----------
static int g_yTab[256], g_rv[256], g_gu[256], g_gv[256], g_bu[256];
static bool g_tabInit = false;
static void initTabs() {
    if (g_tabInit) return;
    for (int i = 0; i < 256; i++) {
        int y = ((i - 16) * 298) >> 8; g_yTab[i] = y < 0 ? 0 : y;
        int c = i - 128;
        g_rv[i] = (409 * c) >> 8;   // BT.709 aproximado
        g_gu[i] = (100 * c) >> 8;
        g_gv[i] = (208 * c) >> 8;
        g_bu[i] = (516 * c) >> 8;
    }
    g_tabInit = true;
}
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static void blitNV12(const uint8_t *src, int sw, int sh, int pitch, int yRows, uint32_t *dst, int dw, int dh) {
    initTabs();
    static std::vector<int> xmap; static int xmapW = 0, xmapS = 0;
    if (xmapW != dw || xmapS != sw) { xmap.resize(dw); for (int x = 0; x < dw; x++) xmap[x] = (int)((long long)x * sw / dw); xmapW = dw; xmapS = sw; }
    const uint8_t *Y = src, *UV = src + (size_t)pitch * yRows;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * sh / dh);
        const uint8_t *yr = Y + (size_t)sy * pitch, *uvr = UV + (size_t)(sy >> 1) * pitch;
        uint32_t *o = dst + (size_t)y * dw;
        for (int x = 0; x < dw; x++) {
            int sx = xmap[x];
            int yy = g_yTab[yr[sx]];
            int u = uvr[sx & ~1], v = uvr[(sx & ~1) + 1];
            int r = clamp255(yy + g_rv[v]), g = clamp255(yy - g_gu[u] - g_gv[v]), b = clamp255(yy + g_bu[u]);
            o[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

// ---------- Audio: thread propria (sceAudioOutOutput bloqueia ate o buffer tocar) ----------
// O AvPlayer entrega PCM S16 intercalado (1, 2 ou 6+ canais, 44.1k/48k). Aqui vira estereo 48k.
struct AudioPump {
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<int> state{0};          // 0 parado | 1 aguardando audio | 2 tocando | -1 erro ao abrir o audio
    std::atomic<int> openRc{0};
    std::atomic<unsigned> rate{0}, chans{0};
    std::atomic<unsigned> chunks{0};
    KAvHandle h = 0;
    int port = -1;
    static const int GRAIN = 1024;

    void start(KAvHandle handle) {
        stop();
        h = handle; run = true; state = 1; chunks = 0; rate = chans = 0; openRc = 0;
        th = std::thread([this] { loop(); });
    }
    void stop() {
        run = false;
        if (th.joinable()) th.join();
        if (port > 0) { sceAudioOutClose(port); port = -1; }
        state = 0;
    }
    static int16_t clip(float v) { return v > 32767.f ? 32767 : (v < -32768.f ? -32768 : (int16_t)v); }

    void loop() {
        std::vector<int16_t> acc;
        float prevL = 0, prevR = 0, phase = 0;
        while (run) {
            KAvFrameInfo au; memset(&au, 0, sizeof au);
            if (!sceAvPlayerGetAudioData(h, &au) || !au.pData) { sceKernelUsleep(3000); continue; }
            unsigned ch = *(uint16_t *)(au.details + 0), sr = *(uint32_t *)(au.details + 4), sz = *(uint32_t *)(au.details + 8);
            if (ch == 0 || ch > 8 || sr < 8000 || sr > 192000 || sz < 2u * ch) continue;
            rate = sr; chans = ch;
            if (port < 0 && state != -1) {
                int p = sceAudioOutOpen(0xFF /*usuario do sistema*/, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                openRc = p;
                if (p > 0) { port = p; state = 2; } else state = -1;      // se falhar, so descarta o audio (o relogio do player continua)
            }
            if (port <= 0) continue;
            const int16_t *in = (const int16_t *)au.pData;
            size_t frames = sz / (2 * ch);
            float step = (float)sr / 48000.0f;
            for (size_t i = 0; i < frames; i++) {
                const int16_t *f = in + i * ch;
                float L, R;
                if (ch == 1) L = R = f[0];
                else if (ch == 2) { L = f[0]; R = f[1]; }
                else if (ch >= 6) { L = f[0] + 0.707f * f[2] + 0.707f * f[4]; R = f[1] + 0.707f * f[2] + 0.707f * f[5]; }
                else { L = f[0]; R = f[1]; }
                while (phase < 1.0f) {
                    acc.push_back(clip(prevL + (L - prevL) * phase));
                    acc.push_back(clip(prevR + (R - prevR) * phase));
                    phase += step;
                }
                phase -= 1.0f; prevL = L; prevR = R;
            }
            size_t used = 0;
            while (run && acc.size() - used >= (size_t)GRAIN * 2) {
                sceAudioOutOutput(port, acc.data() + used);
                used += (size_t)GRAIN * 2; chunks++;
            }
            if (used) acc.erase(acc.begin(), acc.begin() + used);
            if (acc.size() > 48000 * 2) acc.clear();            // seguranca: nunca acumula mais de ~1s
        }
    }
};

// ---------- Alimentador HLS -> AvPlayer ----------
// O AvPlayer do PS4 nao abre https/HLS direto (AddSource=806A0002). Entao o app baixa o HLS (playlist + segmentos .ts)
// com o cliente HTTP proprio e entrega os bytes ao player pelos callbacks de arquivo ("fileReplacement").
struct HlsFeeder {
    StreamBuf sb;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<int> segs{0}, opens{0}, reads{0}, failed{0};
    std::mutex lm;
    std::string status;
    std::string url;

    void setStatus(const std::string &t) { std::lock_guard<std::mutex> l(lm); status = t; }
    std::string getStatus() { std::lock_guard<std::mutex> l(lm); return status; }
    void fail(const std::string &t) { setStatus("ERRO: " + t); failed = 1; }

    void start(const std::string &u) {
        join(); sb.reset();
        url = u; segs = opens = reads = failed = 0; run = true; setStatus("iniciando");
        th = std::thread([this] { loop(); });
    }
    void abort() { run = false; sb.abort(); }                // libera leituras bloqueadas do player
    void join() { abort(); if (th.joinable()) th.join(); }

    static void sleepMs(int ms, std::atomic<bool> &run) { for (int i = 0; i < ms / 50 && run; i++) sceKernelUsleep(50000); }

    void loop() {
        std::string body; int st = 0;
        setStatus("baixando playlist mestre");
        if (!g_net.get(url, body, st) || st < 200 || st >= 300) { fail("playlist mestre HTTP " + std::to_string(st) + (body.empty() ? " (sem resposta)" : "")); return; }
        std::string mediaUrl = url;
        if (hlsIsMaster(body)) {
            auto v = hlsParseMaster(body);
            int i = hlsPickVariant(v, 720);
            if (i < 0) { fail("playlist mestre sem variantes"); return; }
            mediaUrl = hlsResolve(url, v[i].uri);
            setStatus("variante " + std::to_string(v[i].width) + "x" + std::to_string(v[i].height) + " bw=" + std::to_string(v[i].bandwidth));
        }
        unsigned long long next = 0; bool first = true; int errs = 0;
        while (run) {
            if (!g_net.get(mediaUrl, body, st) || st < 200 || st >= 300) {
                if (++errs > 6) { fail("playlist de midia HTTP " + std::to_string(st)); return; }
                sleepMs(1000, run); continue;
            }
            errs = 0;
            HlsMedia md = hlsParseMedia(body);
            if (md.hasMap) { fail("fMP4 (EXT-X-MAP) ainda nao suportado"); return; }
            if (md.segs.empty()) { sleepMs(1000, run); continue; }
            if (first) { size_t k = md.segs.size() > 3 ? md.segs.size() - 3 : 0; next = md.segs[k].seq; first = false; }
            bool got = false;
            for (auto &sg : md.segs) {
                if (!run) break;
                if (sg.seq < next) continue;
                std::string seg; int ss = 0;
                if (!g_net.get(hlsResolve(mediaUrl, sg.uri), seg, ss, 12u << 20) || ss < 200 || ss >= 300 || seg.empty()) {
                    if (++errs > 6) { fail("segmento HTTP " + std::to_string(ss)); return; }
                    break;
                }
                sb.append((const uint8_t *)seg.data(), seg.size());
                next = sg.seq + 1; segs++; got = true;
                setStatus("ao vivo: segmento " + std::to_string(sg.seq) + " (" + std::to_string(seg.size() / 1024) + " KB)");
            }
            if (md.endList && !got) { setStatus("fim da transmissao"); return; }
            if (!got) { int ms = (int)(md.target * 500); sleepMs(ms < 300 ? 300 : (ms > 1500 ? 1500 : ms), run); }
        }
    }
};
static HlsFeeder g_feed;
static int feedOpen(void *, const char *) { g_feed.opens++; return 0; }
static int feedClose(void *) { return 0; }
static int feedRead(void *, uint8_t *dst, uint64_t pos, uint32_t len) { g_feed.reads++; return g_feed.sb.read(dst, pos, len); }
static uint64_t feedSize(void *) { return 1ULL << 40; }       // ao vivo: tamanho "infinito"

static double nowSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

struct Player {
    KAvHandle h = 0;
    bool active = false;
    AudioPump audio;
    int addRc = 0;
    bool failed = false, useFeed = false;
    std::vector<std::string> log;
    int frames = 0, strideMode = 0;
    unsigned vw = 0, vh = 0;
    double t0 = 0, lastFps = 0; int fpsFrames = 0; double fps = 0;

    bool start(const std::string &url, const std::string &title) {
        log.clear(); frames = 0; vw = vh = 0; fps = 0; fpsFrames = 0; failed = false; addRc = 0;
        char b[64];
        log.push_back("Reproduzindo: " + title);
        {   // host e tamanho da URL (o AvPlayer pode ter limite de tamanho; o link da Kick tem ~1000 caracteres)
            size_t a = url.find("://"); a = a == std::string::npos ? 0 : a + 3;
            size_t e = url.find('/', a);
            log.push_back("URL: " + url.substr(0, 8) + "... host=" + url.substr(a, e == std::string::npos ? std::string::npos : e - a) +
                          "  tamanho=" + std::to_string(url.size()));
        }
        g_net.log = nullptr;
        g_net.init();
        snprintf(b, sizeof b, "%08X", (unsigned)sceSysmoduleLoadModule(0x00A5)); log.push_back(std::string("modulo AvPlayer (0xA5): ") + b);
        snprintf(b, sizeof b, "%08X", (unsigned)sceSysmoduleLoadModuleInternal(0x80000050)); log.push_back(std::string("modulo AvPlayerStreaming: ") + b);
        KAvInitData id; memset(&id, 0, sizeof id);
        id.memObj = NULL; id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
        useFeed = url.find(".m3u8") != std::string::npos;     // HLS: o app baixa e entrega os bytes ao player
        if (useFeed) { id.fileObj = NULL; id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
        log.push_back(useFeed ? "modo: HLS baixado pelo app (leitura propria)" : "modo: URL direta no AvPlayer");
        id.debugLevel = 0; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = true; id.defaultLanguage = "eng";
        h = sceAvPlayerInit(&id);
        snprintf(b, sizeof b, "%llX", (unsigned long long)h); log.push_back(std::string("sceAvPlayerInit: handle=") + b);
        if (!h) { failed = true; log.push_back("ERRO: sceAvPlayerInit falhou"); return false; }
        if (useFeed) g_feed.start(url);
        int rc = sceAvPlayerAddSource(h, useFeed ? "live.ts" : url.c_str());
        addRc = rc;
        snprintf(b, sizeof b, "%08X", (unsigned)rc); log.push_back(std::string("sceAvPlayerAddSource: ") + b);
        if (rc < 0) {
            failed = true;
            log.push_back("ERRO: o player nao conseguiu abrir esta URL (veja o codigo acima).");
            g_feed.join();
            sceAvPlayerClose(h); h = 0; return false;
        }
        audio.start(h);
        active = true; t0 = lastFps = nowSec();
        return true;
    }
    void stop() {
        g_feed.abort();                     // libera leituras bloqueadas do player (senao o Close poderia travar)
        audio.stop();                       // a thread de audio usa o handle: para antes de fechar
        if (h) { sceAvPlayerStop(h); sceAvPlayerClose(h); }
        g_feed.join();
        h = 0; active = false;
    }
    // chama a cada iteracao; retorna true se desenhou um quadro novo
    bool poll(FbRenderer &r) {
        KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
        bool drew = false;
        if (sceAvPlayerGetVideoData(h, &fi) && fi.pData) {
            uint32_t w = *(uint32_t *)(fi.details + 0), hh = *(uint32_t *)(fi.details + 4);
            if (w > 0 && hh > 0 && w <= 4096 && hh <= 4096) {
                vw = w; vh = hh;
                int a16h = (hh + 15) & ~15;
                int pitch = w, yRows = hh;
                switch (strideMode) {
                case 1: yRows = a16h; break;
                case 2: pitch = (w + 63) & ~63; yRows = a16h; break;
                case 3: pitch = (w + 255) & ~255; yRows = a16h; break;
                }
                blitNV12(fi.pData, w, hh, pitch, yRows, r.fb, r.W, r.H);
                frames++; fpsFrames++;
                drew = true;
            }
        }
        double t = nowSec();
        if (t - lastFps >= 1.0) { fps = fpsFrames / (t - lastFps); fpsFrames = 0; lastFps = t; }
        return drew;
    }
    void overlay(FbRenderer &r) {
        char b[160];
        snprintf(b, sizeof b, " ativo=%d video=%ux%u quadros=%d ~%.0ffps stride=%d(TRIANGULO) audio=%d %uHz/%uch blocos=%u O=voltar ",
                 sceAvPlayerIsActive(h) ? 1 : 0, vw, vh, frames, fps, strideMode, audio.state.load(), audio.rate.load(), audio.chans.load(), audio.chunks.load());
        r.fillRect(0, 0, r.W, 22, 0xFF000000);
        r.drawText(8, 3, b, 0xFFFFFF00, 16);
        if (useFeed && g_feed.failed) { r.fillRect(0, 22, r.W, 22, 0xFF000000); r.drawText(8, 25, g_feed.getStatus(), 0xFFFF6060, 16); }
    }
    void renderWaiting(FbRenderer &r) {
        r.fillRect(0, 0, r.W, r.H, 0xFF101018);
        r.drawText(40, 20, "Player", 0xFF53FC18, 28);
        int y = 70;
        for (auto &l : log) { r.drawText(40, y, l, 0xFFFFFFFF, 20); y += 26; }
        char b[160];
        if (failed) snprintf(b, sizeof b, "FALHOU (AddSource=%08X).  O volta ao menu", (unsigned)addRc);
        else snprintf(b, sizeof b, "aguardando quadros... %.0fs   ativo=%d   audio=%d (%uHz %uch)   (O volta)", nowSec() - t0,
                      h && sceAvPlayerIsActive(h) ? 1 : 0, audio.state.load(), audio.rate.load(), audio.chans.load());
        r.drawText(40, y + 10, b, failed ? 0xFFFF6060 : 0xFFAAAAAA, 20);
        if (useFeed) {
            snprintf(b, sizeof b, "HLS: segmentos=%d  bytes=%llu  open=%d  leituras=%d", g_feed.segs.load(), (unsigned long long)g_feed.sb.total(), g_feed.opens.load(), g_feed.reads.load());
            r.drawText(40, y + 78, b, 0xFFAAAAAA, 20);
            r.drawText(40, y + 104, "HLS: " + g_feed.getStatus(), g_feed.failed ? 0xFFFF6060 : 0xFFAAAAAA, 20);
        }
        if (!failed && nowSec() - t0 > 15) r.drawText(40, y + 44, "Mais de 15s sem video: tire um print desta tela e mande para o Claude.", 0xFFFFFF00, 20);
    }
};

// ---------- Diagnostico do AvPlayer: tenta varias formas de abrir uma fonte e mostra cada resultado ----------
static std::string hx(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

// repl: usa callbacks de arquivo | path: nome passado ao AddSource | hls: se != NULL, alimenta o buffer com este HLS
static void avAttempt(std::vector<std::string> &d, const std::function<void()> &refresh, const char *name, bool repl, const char *path,
                      const char *hls, bool autoStart) {
    d.push_back(std::string("-- ") + name);
    refresh();
    KAvInitData id; memset(&id, 0, sizeof id);
    id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
    id.debugLevel = 0; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = autoStart; id.defaultLanguage = "eng";
    if (repl) { id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
    g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0;
    if (hls) g_feed.start(hls);
    KAvHandle h = sceAvPlayerInit(&id);
    d.push_back("   Init: handle=" + hx((int)(h & 0xFFFFFFFF)) + (h ? "" : "  (FALHOU)"));
    int rc = h ? sceAvPlayerAddSource(h, path) : -1;
    d.push_back("   AddSource(\"" + std::string(path).substr(0, 60) + "\") = " + hx(rc));
    refresh();
    if (h && rc >= 0) {
        double t0 = nowSec(); int frames = 0, vw = 0, vh = 0, act = 0; unsigned ach = 0, asr = 0; int aframes = 0;
        while (nowSec() - t0 < 7.0) {
            KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
            if (sceAvPlayerGetVideoData(h, &fi) && fi.pData) { frames++; vw = *(int *)(fi.details + 0); vh = *(int *)(fi.details + 4); }
            KAvFrameInfo au; memset(&au, 0, sizeof au);
            if (sceAvPlayerGetAudioData(h, &au) && au.pData) { aframes++; ach = *(uint16_t *)(au.details + 0); asr = *(uint32_t *)(au.details + 4); }
            if (sceAvPlayerIsActive(h)) act = 1;
            sceKernelUsleep(8000);
        }
        char b[220];
        snprintf(b, sizeof b, "   apos 7s: ativo=%d  video: quadros=%d %dx%d  audio: blocos=%d %uHz/%uch  open=%d leituras=%d segs=%d bytes=%llu",
                 act, frames, vw, vh, aframes, asr, ach, g_feed.opens.load(), g_feed.reads.load(), g_feed.segs.load(), (unsigned long long)g_feed.sb.total());
        d.push_back(b);
        if (hls) d.push_back("   HLS: " + g_feed.getStatus());
    } else if (hls) {
        d.push_back("   (open=" + std::to_string(g_feed.opens.load()) + " leituras=" + std::to_string(g_feed.reads.load()) + ")");
    }
    g_feed.abort();
    if (h) { sceAvPlayerStop(h); sceAvPlayerClose(h); }
    g_feed.join();
    refresh();
}

static void runAvTest(std::vector<std::string> &d, const std::function<void()> &refresh) {
    d.clear();
    d.push_back("== DIAGNOSTICO DO AVPLAYER ==  (cada teste leva ate ~10s)");
    g_net.log = nullptr; g_net.init();
    d.push_back("modulo AvPlayer (0xA5): " + hx(sceSysmoduleLoadModule(0x00A5)));
    const char *hls = kTestHlsHttps();
    avAttempt(d, refresh, "A) leitura propria + /app0/live.ts  (HLS publico baixado pelo app)", true, "/app0/live.ts", hls, true);
    avAttempt(d, refresh, "B) leitura propria + live.ts (sem barra)", true, "live.ts", hls, true);
    avAttempt(d, refresh, "C) leitura propria + /app0/live.ts, autoStart desligado", true, "/app0/live.ts", hls, false);
    avAttempt(d, refresh, "D) sem leitura propria + arquivo local /app0/eboot.bin (existe, mas nao e video)", false, "/app0/eboot.bin", NULL, true);
    avAttempt(d, refresh, "E) sem leitura propria + http direto (m3u8)", false, "http://devimages.apple.com/iphone/samples/bipbop/bipbopall.m3u8", NULL, true);
    avAttempt(d, refresh, "F) sem leitura propria + https direto (m3u8)", false, kTestHlsHttps(), NULL, true);
    d.push_back("Fim. Tire UM print desta tela e mande para o Claude.  (O volta ao menu)");
}

// ---------- Teclado na tela (para digitar o @streamer) ----------
struct Keyboard {
    static const int COLS = 10, ROWS = 4;
    std::string text;
    int cx = 0, cy = 0;
    // 40 teclas: a-z, _ - 0-9, "<" (apagar), "OK"
    static std::string keyAt(int x, int y) {
        static const char *k[ROWS * COLS] = {
            "a","b","c","d","e","f","g","h","i","j",
            "k","l","m","n","o","p","q","r","s","t",
            "u","v","w","x","y","z","_","-","0","1",
            "2","3","4","5","6","7","8","9","<","OK"};
        return k[y * COLS + x];
    }
    void open(const std::string &initial) { text = initial; cx = cy = 0; }
    void move(int dx, int dy) { cx = (cx + dx + COLS) % COLS; cy = (cy + dy + ROWS) % ROWS; }
    void backspace() { if (!text.empty()) text.pop_back(); }
    // retorna true se confirmou (OK)
    bool press() {
        std::string k = keyAt(cx, cy);
        if (k == "OK") return true;
        if (k == "<") backspace();
        else if (text.size() < 25) text += k;
        return false;
    }
    void render(FbRenderer &r) const {
        r.fillRect(0, 0, r.W, r.H, 0xFF101018);
        r.drawText(40, 30, "Digite o @ do streamer da Kick", 0xFF53FC18, 32);
        r.fillRect(40, 100, 900, 60, 0xFF2A2A38);
        r.drawText(60, 112, "@" + text + "_", 0xFFFFFFFF, 36);
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                bool on = (x == cx && y == cy);
                int px = 40 + x * 92, py = 200 + y * 80;
                r.fillRect(px, py, 84, 70, on ? 0xFF53FC18 : 0xFF2A2A38);
                r.drawText(px + 24, py + 20, keyAt(x, y), on ? 0xFF000000 : 0xFFFFFFFF, 32);
            }
        r.drawText(40, 560, "D-pad move   X digita   QUADRADO apaga   R1 ou tecla OK = confirmar   O cancela", 0xFFAAAAAA, 22);
    }
};

static const uint32_t B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_LEFT = 0x0080, B_RIGHT = 0x0020, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

enum class Mode { Menu, Diag, Play, Kbd };

int main() {
    Ps4Platform pf;
    App app(pf);
    pf.app = &app;
    UI ui(app);
    fillDemo(app);
    ui.tab = Tab::Kick;                       // abre direto na aba Kick (os canais M3U sao so demonstracao)

    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
    sceAudioOutInit();
    int pad = scePadOpen(userId, 0, 0, NULL);

    const int W = 1920, H = 1080;
    int vh = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, NULL);
    if (vh < 0) return 1;
    const size_t bufSize = 0x800000, total = bufSize * 2;
    off_t phys = 0;
    void *mem = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &phys) < 0) return 2;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, 0x200000) < 0) return 3;
    void *bufs[2] = {mem, (char *)mem + bufSize};
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    if (sceVideoOutRegisterBuffers(vh, 0, bufs, 2, &attr) < 0) return 4;

    std::vector<std::string> diag;
    Mode mode = Mode::Menu;
    Player player;
    Keyboard kbd;
    bool gotFrame = false;
    double lastWaitDraw = 0;
    uint32_t prev = 0;
    int heldFrames = 0, back = 0;
    bool dirty = true;

    auto flip = [&]() { sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back ^= 1; };
    auto presentMenu = [&]() { FbRenderer r(bufs[back], W, H); ui.render(r); flip(); };
    auto presentDiag = [&](bool running) { FbRenderer r(bufs[back], W, H); renderDiag(r, diag, running); flip(); };
    auto presentWait = [&]() { FbRenderer r(bufs[back], W, H); player.renderWaiting(r); flip(); };
    auto presentKbd = [&]() { FbRenderer r(bufs[back], W, H); kbd.render(r); flip(); };

    for (;;) {
        OrbisPadData d;
        uint32_t now = 0;
        if (pad >= 0 && scePadReadState(pad, &d) == 0) now = d.buttons;
        if (now & B_OPTIONS) break;
        uint32_t pressed = now & ~prev;
        prev = now;

        if (mode == Mode::Kbd) {
            bool changed = false, done = false;
            if (pressed & B_UP)     { kbd.move(0, -1); changed = true; }
            if (pressed & B_DOWN)   { kbd.move(0, 1);  changed = true; }
            if (pressed & B_LEFT)   { kbd.move(-1, 0); changed = true; }
            if (pressed & B_RIGHT)  { kbd.move(1, 0);  changed = true; }
            if (pressed & B_SQUARE) { kbd.backspace(); changed = true; }
            if (pressed & B_CROSS)  { done = kbd.press(); changed = true; }
            if (pressed & B_R1)     done = true;
            if (pressed & B_CIRCLE) { mode = Mode::Menu; dirty = true; continue; }
            if (done) {
                mode = Mode::Menu; dirty = true;
                if (!kbd.text.empty()) pf.pendingText = kbd.text;
                continue;
            }
            if (changed || dirty) { presentKbd(); dirty = false; }
            sceKernelUsleep(16000);
            continue;
        }

        if (mode == Mode::Play) {
            if (pressed & B_CIRCLE) { player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(16000); continue; }
            if (pressed & B_TRIANGLE) player.strideMode = (player.strideMode + 1) % 4;
            if (player.h) {
                FbRenderer r(bufs[back], W, H);
                if (player.poll(r)) { gotFrame = true; player.overlay(r); flip(); }
                else if (!gotFrame && nowSec() - lastWaitDraw > 0.5) { lastWaitDraw = nowSec(); presentWait(); }
            } else if (nowSec() - lastWaitDraw > 0.5) { lastWaitDraw = nowSec(); presentWait(); }
            sceKernelUsleep(4000);
            continue;
        }

        if (mode == Mode::Diag) {
            if (pressed & B_CIRCLE) { mode = Mode::Menu; dirty = true; }
            if (pressed & B_SQUARE) { diag.assign(1, "Executando..."); presentDiag(true); runNetTest(diag); dirty = true; }
            if (dirty) { presentDiag(false); dirty = false; }
            sceKernelUsleep(16000);
            continue;
        }

        // ---- Menu ----
        if (now & (B_UP | B_DOWN)) {
            heldFrames++;
            if (heldFrames > 20 && heldFrames % 4 == 0) pressed |= now & (B_UP | B_DOWN);
        } else heldFrames = 0;
        if (pressed & B_UP)     { ui.handle(Input::Up);       dirty = true; }
        if (pressed & B_DOWN)   { ui.handle(Input::Down);     dirty = true; }
        if (pressed & B_L1)     { ui.handle(Input::PrevTab);  dirty = true; }
        if (pressed & B_R1)     { ui.handle(Input::NextTab);  dirty = true; }
        if (pressed & B_L2)     { ui.handle(Input::PageUp);   dirty = true; }
        if (pressed & B_R2)     { ui.handle(Input::PageDown); dirty = true; }
        if (pressed & B_CROSS)  { ui.handle(Input::Confirm);  dirty = true; }
        if (pressed & B_CIRCLE) { ui.handle(Input::Back);     dirty = true; }
        if (pressed & B_SQUARE) { mode = Mode::Diag; diag.assign(1, "Executando..."); presentDiag(true); runNetTest(diag); dirty = true; }

        if (pf.avDiagRequested) {                 // DIAGNOSTICO do AvPlayer
            pf.avDiagRequested = false;
            mode = Mode::Diag; diag.assign(1, "Executando...");
            presentDiag(true);
            runAvTest(diag, [&]() { presentDiag(true); });
            dirty = true;
            continue;
        }
        if (pf.kbdRequested) {                    // X em "Digitar @streamer": abre o teclado na tela
            pf.kbdRequested = false;
            kbd.open(ui.kickLast);
            mode = Mode::Kbd; dirty = true;
            continue;
        }
        if (!pf.pendingText.empty()) {            // texto confirmado no teclado: busca o canal
            std::string slug = pf.pendingText; pf.pendingText.clear();
            app.say("Buscando @" + slug + " na Kick...");
            presentMenu();
            ui.onText(slug);                     // httpGet + (se ao vivo) play()
            dirty = true;
        }
        if (!pf.pendingPlayUrl.empty()) {
            std::string url = pf.pendingPlayUrl, title = pf.pendingPlayTitle;
            pf.pendingPlayUrl.clear();
            if (url.compare(0, 15, "http://exemplo/") == 0) {   // item de demonstracao: nao existe video
                app.say("Isto e um item de DEMONSTRACAO (sem video). Use a aba Kick.");
                dirty = true;
                continue;
            }
            mode = Mode::Play; gotFrame = false; lastWaitDraw = 0;
            presentWait();                       // mostra "Player" antes de bloquear
            player.start(url, title);
            presentWait();
            continue;
        }
        if (dirty) { presentMenu(); dirty = false; }
        sceKernelUsleep(16000);
    }
    return 0;
}
