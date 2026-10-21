// TESTE 3c - PLAYER (video apenas, sem audio) com AvPlayer + diagnostico.
//   Aba Kick: X = assistir @dekin021 (fixo neste teste)
//   Durante o video: TRIANGULO = muda o "modo de stride" do desenho (se a imagem sair torta) | O = voltar | OPTIONS = sair
//   Menu: QUADRADO = teste de rede
// Precisa do src/font8x16.hpp (o mesmo do Teste 2). Makefile liga tambem -lSceAvPlayer.
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <map>
#include <string>
#include <vector>
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
    bool get(const std::string &url, std::string &body, int &status) {
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
                    char buf[4096];
                    for (;;) {
                        int n = sceHttpReadData(req, buf, sizeof buf);
                        if (n <= 0) break;
                        body.append(buf, n);
                        if (body.size() > 256 * 1024) break;
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
    std::string pendingText;                 // pedido de "digitar @streamer" (teclado ainda nao existe: usa fixo)
    std::string pendingPlayUrl, pendingPlayTitle;
    bool httpGet(const std::string &url, std::string &body) override {
        int st = 0;
        bool ok = g_net.get(url, body, st);
        return ok && st >= 200 && st < 300;
    }
    void play(const std::string &url, const std::string &title) override { pendingPlayUrl = url; pendingPlayTitle = title; }
    void requestText(const std::string &) override { pendingText = "dekin021"; }
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
    r.drawText(40, 20, running ? "Executando teste de rede... (pode levar ate 1 minuto)" : "Diagnostico de rede", 0xFF53FC18, 28);
    int y = 70;
    size_t start = diag.size() > 36 ? diag.size() - 36 : 0;
    for (size_t i = start; i < diag.size(); i++) { r.drawText(40, y, diag[i], 0xFFFFFFFF, 20); y += 26; }
    r.drawText(40, r.H - 50, "O voltar ao menu   OPTIONS sair", 0xFF777777, 22);
}

// ---------- Dados de exemplo ----------
static void fillDemo(App &app) {
    std::string m = "#EXTM3U\n";
    for (int i = 1; i <= 40; i++)
        m += "#EXTINF:-1 group-title=\"Abertos\",Canal de Exemplo " + std::to_string(i) + "\nhttp://exemplo/live/" + std::to_string(i) + ".ts\n";
    for (int i = 1; i <= 15; i++)
        m += "#EXTINF:-1 group-title=\"Filmes\",Filme de A\xC3\xA7\xC3\xA3o " + std::to_string(i) + "\nhttp://exemplo/movie/" + std::to_string(i) + ".mp4\n";
    for (int s = 1; s <= 2; s++)
        for (int e = 1; e <= 6; e++)
            m += "#EXTINF:-1 group-title=\"Series\",Dark S0" + std::to_string(s) + "E0" + std::to_string(e) + "\nhttp://exemplo/series/d" +
                 std::to_string(s * 10 + e) + ".mp4\n";
    app.list = buildPlaylist(m);
    app.say("TESTE 3c | Kick: X = assistir dekin021 | QUADRADO = rede | OPTIONS sai");
}

// ---------- AvPlayer (libSceAvPlayer) - structs proprias para nao depender do header ----------
typedef void *(*KAvAlloc)(void *arg, uint32_t align, uint32_t size);
typedef void (*KAvFree)(void *arg, void *mem);
struct KAvInitData {
    void *memObj; KAvAlloc allocate; KAvFree deallocate; KAvAlloc allocateTexture; KAvFree deallocateTexture;   // memoryReplacement
    void *fileObj; void *fOpen; void *fClose; void *fRead; void *fSize;                                          // fileReplacement
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
typedef uintptr_t KAvHandle;
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
    uintptr_t a = ((uintptr_t)raw + sizeof(void *) + align - 1) & ~(uintptr_t)(align - 1);
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

static double nowSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

struct Player {
    KAvHandle h = 0;
    bool active = false;
    std::vector<std::string> log;
    int frames = 0, strideMode = 0;
    unsigned vw = 0, vh = 0;
    double t0 = 0, lastFps = 0; int fpsFrames = 0; double fps = 0;

    bool start(const std::string &url, const std::string &title) {
        log.clear(); frames = 0; vw = vh = 0; fps = 0; fpsFrames = 0;
        char b[64];
        log.push_back("Reproduzindo: " + title);
        g_net.log = nullptr;
        g_net.init();
        snprintf(b, sizeof b, "%08X", (unsigned)sceSysmoduleLoadModule(0x00A5)); log.push_back(std::string("modulo AvPlayer (0xA5): ") + b);
        snprintf(b, sizeof b, "%08X", (unsigned)sceSysmoduleLoadModuleInternal(0x80000050)); log.push_back(std::string("modulo AvPlayerStreaming: ") + b);
        KAvInitData id; memset(&id, 0, sizeof id);
        id.memObj = NULL; id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
        id.debugLevel = 0; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = true; id.defaultLanguage = "eng";
        h = sceAvPlayerInit(&id);
        snprintf(b, sizeof b, "%llX", (unsigned long long)h); log.push_back(std::string("sceAvPlayerInit: handle=") + b);
        if (!h) return false;
        int rc = sceAvPlayerAddSource(h, url.c_str());
        snprintf(b, sizeof b, "%08X", (unsigned)rc); log.push_back(std::string("sceAvPlayerAddSource: ") + b);
        if (rc < 0) { sceAvPlayerClose(h); h = 0; return false; }
        active = true; t0 = lastFps = nowSec();
        return true;
    }
    void stop() {
        if (h) { sceAvPlayerStop(h); sceAvPlayerClose(h); }
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
        // consome (e descarta) o audio para o relogio do player nao travar
        for (int i = 0; i < 8; i++) { KAvFrameInfo au; memset(&au, 0, sizeof au); if (!sceAvPlayerGetAudioData(h, &au)) break; }
        double t = nowSec();
        if (t - lastFps >= 1.0) { fps = fpsFrames / (t - lastFps); fpsFrames = 0; lastFps = t; }
        return drew;
    }
    void overlay(FbRenderer &r) {
        char b[160];
        snprintf(b, sizeof b, " AvPlayer ativo=%d  video=%ux%u  quadros=%d  ~%.0f fps  stride-modo=%d (TRIANGULO)  O=voltar ", sceAvPlayerIsActive(h) ? 1 : 0, vw, vh, frames, fps, strideMode);
        r.fillRect(0, 0, r.W, 22, 0xFF000000);
        r.drawText(8, 3, b, 0xFFFFFF00, 16);
    }
    void renderWaiting(FbRenderer &r) {
        r.fillRect(0, 0, r.W, r.H, 0xFF101018);
        r.drawText(40, 20, "Player", 0xFF53FC18, 28);
        int y = 70;
        for (auto &l : log) { r.drawText(40, y, l, 0xFFFFFFFF, 20); y += 26; }
        char b[160];
        snprintf(b, sizeof b, "aguardando quadros... %.0fs   ativo=%d   (O volta)", nowSec() - t0, h && sceAvPlayerIsActive(h) ? 1 : 0);
        r.drawText(40, y + 10, b, 0xFFAAAAAA, 20);
    }
};

static const uint32_t B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

enum class Mode { Menu, Diag, Play };

int main() {
    Ps4Platform pf;
    App app(pf);
    pf.app = &app;
    UI ui(app);
    fillDemo(app);

    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
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
    bool gotFrame = false;
    double lastWaitDraw = 0;
    uint32_t prev = 0;
    int heldFrames = 0, back = 0;
    bool dirty = true;

    auto flip = [&]() { sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back ^= 1; };
    auto presentMenu = [&]() { FbRenderer r(bufs[back], W, H); ui.render(r); flip(); };
    auto presentDiag = [&](bool running) { FbRenderer r(bufs[back], W, H); renderDiag(r, diag, running); flip(); };
    auto presentWait = [&]() { FbRenderer r(bufs[back], W, H); player.renderWaiting(r); flip(); };

    for (;;) {
        OrbisPadData d;
        uint32_t now = 0;
        if (pad >= 0 && scePadReadState(pad, &d) == 0) now = d.buttons;
        if (now & B_OPTIONS) break;
        uint32_t pressed = now & ~prev;
        prev = now;

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

        if (!pf.pendingText.empty()) {            // X na aba Kick: busca o canal fixo
            std::string slug = pf.pendingText; pf.pendingText.clear();
            app.say("Buscando @" + slug + " na Kick...");
            presentMenu();
            ui.onText(slug);                     // httpGet + (se ao vivo) play()
            dirty = true;
        }
        if (!pf.pendingPlayUrl.empty()) {
            std::string url = pf.pendingPlayUrl, title = pf.pendingPlayTitle;
            pf.pendingPlayUrl.clear();
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
