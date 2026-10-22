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
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hls.hpp"
#include "streambuf.hpp"
#include "tsmux.hpp"
#include "blit.hpp"
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
// Matematica sem libm (o app original nao importava nenhuma funcao de matematica do libc)
static inline int ifloor(float f) { int i = (int)f; return ((float)i > f) ? i - 1 : i; }
static inline float fsqrt(float v) {
    if (v <= 0.f) return 0.f;
    float g = v > 1.f ? v * 0.5f + 0.5f : 1.f;
    for (int i = 0; i < 8; i++) g = 0.5f * (g + v / g);
    return g;
}

static inline uint32_t blendPx(uint32_t d, uint32_t s, int a) {   // a: 0..255
    if (a <= 0) return d;
    if (a >= 255) return 0xFF000000u | (s & 0xFFFFFFu);
    uint32_t ia = 255 - a;
    uint32_t rr = (((s >> 16) & 255) * a + ((d >> 16) & 255) * ia) / 255;
    uint32_t gg = (((s >> 8) & 255) * a + ((d >> 8) & 255) * ia) / 255;
    uint32_t bb = ((s & 255) * a + (d & 255) * ia) / 255;
    return 0xFF000000u | (rr << 16) | (gg << 8) | bb;
}

struct FbRenderer : Renderer {
    uint32_t *fb; int W, H;
    FbRenderer(void *p, int w, int h) : fb((uint32_t *)p), W(w), H(h) {}
    int width() const override { return W; }
    int height() const override { return H; }
    void fillRect(int x, int y, int w, int h, unsigned argb) override {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        int a = argb >> 24;
        for (int j = y0; j < y1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            if (a >= 255) for (int i = x0; i < x1; i++) row[i] = argb;
            else for (int i = x0; i < x1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    // retangulo com cantos arredondados (bordas suavizadas)
    void roundRect(int x, int y, int w, int h, int rad, unsigned argb) override {
        if (rad * 2 > h) rad = h / 2;
        if (rad * 2 > w) rad = w / 2;
        if (rad <= 0) { fillRect(x, y, w, h, argb); return; }
        int a = argb >> 24;
        int j0 = y < 0 ? 0 : y, j1 = y + h > H ? H : y + h;
        for (int j = j0; j < j1; j++) {
            uint32_t *row = fb + (size_t)j * W;
            bool top = j < y + rad, bot = j >= y + h - rad;
            int mx0 = x, mx1 = x + w;
            if (top || bot) {
                float cy = top ? (float)(y + rad) : (float)(y + h - rad);
                float dy = (j + 0.5f) - cy;
                mx0 = x + rad; mx1 = x + w - rad;
                for (int side = 0; side < 2; side++) {
                    int px0 = side ? x + w - rad : x, px1 = side ? x + w : x + rad;
                    float cx = side ? (float)(x + w - rad) : (float)(x + rad);
                    for (int i = px0; i < px1; i++) {
                        if (i < 0 || i >= W) continue;
                        float dx = (i + 0.5f) - cx;
                        float cov = (float)rad - fsqrt(dx * dx + dy * dy) + 0.5f;
                        if (cov <= 0.f) continue;
                        if (cov > 1.f) cov = 1.f;
                        row[i] = blendPx(row[i], argb, (int)(a * cov));
                    }
                }
            }
            int i0 = mx0 < 0 ? 0 : mx0, i1 = mx1 > W ? W : mx1;
            if (a >= 255) for (int i = i0; i < i1; i++) row[i] = argb;
            else for (int i = i0; i < i1; i++) row[i] = blendPx(row[i], argb, a);
        }
    }
    void gradient(int x, int y, int w, int h, unsigned top, unsigned bot) override {
        int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
        for (int j = y0; j < y1; j++) {
            int t = h > 1 ? (j - y) * 255 / (h - 1) : 0;
            uint32_t c = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                int a = (top >> sh) & 255, b = (bot >> sh) & 255;
                c |= (uint32_t)((a * (255 - t) + b * t) / 255) << sh;
            }
            uint32_t *row = fb + (size_t)j * W;
            for (int i = x0; i < x1; i++) row[i] = c;
        }
    }
    int textWidth(const std::string &t, int size) const override {
        int n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) n++;
        return n * (int)(8 * (size / 16.0f) + 0.5f);
    }
    // texto com suavizacao (a fonte 8x16 e ampliada com interpolacao bilinear)
    void drawText(int x, int y, const std::string &text, unsigned argb, int size) override {
        float s = size / 16.0f;
        int adv = (int)(8 * s + 0.5f);
        int gw = (int)(8 * s) + 2, gh = (int)(16 * s) + 2;
        int A = argb >> 24;
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
            if (cp != 32) {
                const unsigned char *g = kFont8x16[gi];
                auto bit = [&](int gx, int gy) -> float {
                    if (gx < 0 || gx > 7 || gy < 0 || gy > 15) return 0.f;
                    return (float)((g[gy] >> (7 - gx)) & 1);
                };
                for (int dy = 0; dy < gh; dy++) {
                    int py = y + dy;
                    if (py < 0 || py >= H) continue;
                    float v = (dy + 0.5f) / s - 0.5f; int v0 = ifloor(v); float fv = v - v0;
                    uint32_t *row = fb + (size_t)py * W;
                    for (int dx = 0; dx < gw; dx++) {
                        int px = cx + dx;
                        if (px < 0 || px >= W) continue;
                        float u = (dx + 0.5f) / s - 0.5f; int u0 = ifloor(u); float fu = u - u0;
                        float cv = (bit(u0, v0) * (1 - fu) + bit(u0 + 1, v0) * fu) * (1 - fv) +
                                   (bit(u0, v0 + 1) * (1 - fu) + bit(u0 + 1, v0 + 1) * fu) * fv;
                        if (cv <= 0.03f) continue;
                        cv = (cv - 0.12f) * 1.45f;                       // afina um pouco as bordas
                        if (cv <= 0.f) continue;
                        if (cv > 1.f) cv = 1.f;
                        row[px] = blendPx(row[px], argb, (int)(A * cv));
                    }
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
    using namespace theme;
    uiBackground(r);
    uiLogo(r, 48, 28, 52);
    uiText(r, 118, 30, running ? "Executando teste..." : "Diagnóstico", TEXT, 32, true);
    uiText(r, 120, 68, running ? "pode levar até 1 minuto" : "tire um print e mande para o Claude", MUTED, 18);
    r.roundRect(40, 100, r.W - 80, r.H - 200, 22, CARD);
    int y = 122;
    size_t start = diag.size() > 33 ? diag.size() - 33 : 0;
    for (size_t i = start; i < diag.size(); i++) {
        const std::string &l = diag[i];
        unsigned c = TEXT; bool bold = false;
        if (l.compare(0, 2, "--") == 0 || l.compare(0, 2, "==") == 0) { c = GREEN; bold = true; }
        else if (l.find("ERRO") != std::string::npos || l.find("!!") != std::string::npos || l.find("FALHOU") != std::string::npos) c = RED;
        else if (l.find("[av]") != std::string::npos) c = MUTED;
        uiText(r, 66, y, uiFit(r, l, 18, r.W - 140), c, 18, bold);
        y += 24;
    }
    int cy = r.H - 50;
    uiHints(r, r.W - 48, cy, {{"O", "Voltar ao menu"}, {"OPTIONS", "Sair"}});
}

// ---------- Dados de exemplo ----------
// ---------- Tela de inicializacao (escrita direta no video, sem ler nada) ----------
// Mostra cada passo da abertura. Se o app travar/cair, a foto da tela mostra ate onde chegou.
struct BootScreen {
    uint32_t *fb = nullptr; int W = 0, H = 0, y = 40;
    void init(void *p, int w, int h) {
        fb = (uint32_t *)p; W = w; H = h;
        for (size_t i = 0; i < (size_t)W * H; i++) fb[i] = 0xFF090B10u;
        text(48, 28, "Kick IPTV", 0xFF53FC18u, 4);
        y = 120;
    }
    void text(int x, int yy, const char *t, uint32_t c, int sc) {
        for (; *t; t++, x += 8 * sc) {
            unsigned ch = (unsigned char)*t;
            if (ch < 32 || ch > 126) ch = '?';
            const unsigned char *g = kFont8x16[ch - 32];
            for (int r = 0; r < 16; r++)
                for (int k = 0; k < 8; k++)
                    if ((g[r] >> (7 - k)) & 1)
                        for (int dy = 0; dy < sc; dy++)
                            for (int dx = 0; dx < sc; dx++) {
                                int px = x + k * sc + dx, py = yy + r * sc + dy;
                                if (px >= 0 && px < W && py >= 0 && py < H) fb[(size_t)py * W + px] = c;
                            }
        }
    }
    void line(const std::string &t, uint32_t c = 0xFFE8ECF4u) { if (fb) { text(48, y, t.c_str(), c, 2); y += 40; } }
};

// ---------- Registro de passos em /data/kickiptv.log ----------
// Se o app cair, na proxima abertura o menu mostra em qual passo ele parou.
static std::string g_prevStage;           // (leitura do log anterior removida: usava fread, que o app original nao importava)
static void traceInit() {
    FILE *f = fopen("/data/kickiptv.log", "wb");
    if (f) { fwrite("inicio\n", 1, 7, f); fclose(f); }
}
static void trace(const char *stage) {
    FILE *f = fopen("/data/kickiptv.log", "ab");
    if (f) { fwrite(stage, 1, strlen(stage), f); fwrite("\n", 1, 1, f); fclose(f); }
}

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
    app.say("Pronto");
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
typedef int (*KAvLogCb)(void *ud, const char *fmt, va_list ap);
int sceAvPlayerSetLogCallback(KAvLogCb cb, void *ud);
}

// Quantas desmontagens (fechar player/threads) estao rodando em segundo plano. O botao O nao espera por elas.
static std::atomic<int> g_tearing{0};

// Stop+Close em outra thread com limite de tempo: se o player travar, abandona o handle e segue em frente
static bool avStopClose(KAvHandle h, int timeoutMs = 5000) {
    if (!h) return true;
    std::atomic<bool> *done = new std::atomic<bool>(false);
    std::thread t([h, done] { sceAvPlayerStop(h); sceAvPlayerClose(h); done->store(true); });
    for (int i = 0; i < timeoutMs / 5 && !done->load(); i++) sceKernelUsleep(5000);
    if (done->load()) { t.join(); delete done; return true; }
    t.detach();                               // vaza de proposito: a thread ainda usa 'done'
    return false;
}

// Log interno do AvPlayer (capturado para o diagnostico) e contadores dos alocadores
static std::mutex g_avlogM;
static std::vector<std::string> g_avlog;
static int avLogCb(void *, const char *fmt, va_list ap) {
    char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    std::string t = b;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    std::lock_guard<std::mutex> l(g_avlogM);
    if (g_avlog.size() < 400) g_avlog.push_back(t);
    return 0;
}
static std::atomic<int> g_allocN{0}, g_allocFail{0}, g_texN{0}, g_texFail{0};

static void *alignedAlloc(size_t align, size_t size) {
    if (align < 16) align = 16;
    void *raw = malloc(size + align + sizeof(void *));
    if (!raw) return NULL;
    unsigned long a = ((unsigned long)raw + sizeof(void *) + align - 1) & ~(unsigned long)(align - 1);
    ((void **)a)[-1] = raw;
    return (void *)a;
}
static void alignedFree(void *p) { if (p) free(((void **)p)[-1]); }
static void *avAlloc(void *, uint32_t align, uint32_t size) { g_allocN++; void *p = alignedAlloc(align, size); if (!p) g_allocFail++; return p; }
static void avFree(void *, void *p) { alignedFree(p); }

// memoria "de textura" (os quadros de video): memoria direta mapeada, como nos samples da Sony
struct TexMem { off_t phys; size_t size; };
static std::map<void *, TexMem> g_tex;
static void *avAllocTex(void *, uint32_t align, uint32_t size) {
    g_texN++;
    size_t al = align < 0x10000 ? 0x10000 : align;
    size_t sz = (size + 0xFFFF) & ~(size_t)0xFFFF;
    off_t phys = 0; void *addr = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, 3, &phys) < 0) { g_texFail++; return NULL; }
    if (sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, al) < 0) { g_texFail++; return NULL; }
    g_tex[addr] = {phys, sz};
    return addr;
}
static void avFreeTex(void *, void *p) {
    auto it = g_tex.find(p);
    if (it == g_tex.end()) return;
    sceKernelReleaseDirectMemory(it->second.phys, it->second.size);
    g_tex.erase(it);
}

// ---------- NV12 -> BGRA: ver blit.hpp (rapido, em varias threads) ----------
static void blitNV12(const uint8_t *src, int sw, int sh, int pitch, int yRows, uint32_t *dst, int dw, int dh) {
    blit::pool().start(4);                     // 3 trabalhadores + a thread principal (so cria na primeira vez)
    blit::nv12(src, sw, sh, pitch, yRows, dst, dw, dh);
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
    FILE *outFile = nullptr;                    // modo "arquivo que cresce": segmentos vao para /data/live.ts
    std::atomic<unsigned long long> outBytes{0};
    std::mutex lm;
    std::string status;
    std::string url;
    std::mutex rm;                                                   // ultimos segmentos (para tocar em "pedacos")
    std::deque<std::pair<unsigned long long, std::string>> recent;

    void setStatus(const std::string &t) { std::lock_guard<std::mutex> l(lm); status = t; }
    std::string getStatus() { std::lock_guard<std::mutex> l(lm); return status; }
    void fail(const std::string &t) { setStatus("ERRO: " + t); failed = 1; }

    void start(const std::string &u) {
        join(); sb.reset();
        { std::lock_guard<std::mutex> l(rm); recent.clear(); }
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
                if (outFile) {
                    fwrite(seg.data(), 1, seg.size(), outFile); fflush(outFile); outBytes += seg.size();
                    if (outBytes > 1200000000ULL) { fail("limite de gravacao (1,2 GB) atingido"); return; }
                } else sb.append((const uint8_t *)seg.data(), seg.size());
                { std::lock_guard<std::mutex> l(rm); recent.push_back({sg.seq, seg}); while (recent.size() > 8) recent.pop_front(); }
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
static std::atomic<unsigned long long> g_feedSize{1ULL << 40};
static uint64_t feedSize(void *) { return g_feedSize.load(); }   // ao vivo: tamanho "infinito"; MP4 de teste: tamanho real
static void closeFeedFile() {                 // para o alimentador, fecha e apaga /data/live.ts
    g_feed.join();
    if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; remove("/data/live.ts"); }
}

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

    // ---- modo "pedacos": o AvPlayer toca arquivos locais (provado pelo teste F); no ao vivo, a cada fim de arquivo
    //      gravamos os segmentos mais novos em outro arquivo e abrimos de novo.
    bool chunkMode = false, remux = false;
    std::string chunkExt;
    int chunkIdx = 0;
    unsigned long long lastSeq = 0;
    double inactiveSince = 0, lastFrameT = 0, hudUntil = 0;
    std::string title;

    // ---- pipeline do ao vivo por pedacos: preparo assincrono + segundo player (troca sem emenda) ----
    double curDur = 0;                       // duracao (s) do pedaco que toca agora (0 = desconhecida)
    double chunkT0 = 0;                      // instante do primeiro quadro do pedaco atual
    KAvHandle h2 = 0;                        // proximo pedaco ja aberto num segundo player
    bool gapless = true;                     // desliga sozinho se o segundo player falhar
    std::atomic<bool> prepBusy{false}, nextReady{false};
    std::string nextPath; unsigned long long nextLast = 0; double nextDur = 0; int nextIdx = 0;
    std::thread prepTh, openTh;
    std::atomic<int> bState{0};              // segundo player: 0 nada | 1 abrindo | 2 pronto | 3 falhou
    KAvHandle bHandle = 0;
    int restarts = 0, switchesOk = 0;
    double lastGapMs = 0, gapStartT = 0, switchedAt = 0, blitMs = 0;
    std::function<bool()> cancelCb;          // main liga: true = o usuario cancelou (O) ou pediu para sair
    bool cancelled = false;

    static std::string chunkPath(const std::string &ext, int idx) { return "/data/live" + std::to_string(idx) + "." + ext; }
    static std::string hx8(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    // junta segmentos: first=true -> os 3 mais novos; senao -> so os mais novos que lastSeq. Retorna quantos.
    int gatherChunk(bool first, std::string &out, unsigned long long &lastOut) {
        std::lock_guard<std::mutex> l(g_feed.rm);
        size_t from = 0;
        if (first) from = g_feed.recent.size() > 3 ? g_feed.recent.size() - 3 : 0;
        int n = 0; out.clear();
        for (size_t i = from; i < g_feed.recent.size(); i++) {
            if (!first && g_feed.recent[i].first <= lastSeq) continue;
            out += g_feed.recent[i].second; lastOut = g_feed.recent[i].first; n++;
        }
        return n;
    }
    static bool writeFile(const std::string &path, const std::string &data) {
        FILE *f = fopen(path.c_str(), "wb"); if (!f) return false;
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size(); fclose(f); return ok;
    }

    // Abre uma fonte. waitMs>0: so aceita se chegar um quadro de video nesse tempo. Guarda o handle em 'h'.
    bool openSource(const std::string &path, bool cb, int dbg, int waitMs, bool note, KAvHandle *target = nullptr) {
        KAvInitData id; memset(&id, 0, sizeof id);
        id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
        id.debugLevel = dbg; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = true; id.defaultLanguage = "eng";
        if (cb) { id.fileObj = (void *)&g_feed; id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
        { std::lock_guard<std::mutex> l(g_avlogM); g_avlog.clear(); }
        if (dbg) sceAvPlayerSetLogCallback(avLogCb, NULL);
        g_allocN = g_allocFail = g_texN = g_texFail = 0;
        g_feed.opens = g_feed.reads = 0;
        KAvHandle hh = sceAvPlayerInit(&id);
        if (!hh) { if (note) log.push_back("   Init falhou"); return false; }
        int rc = sceAvPlayerAddSource(hh, path.c_str());
        addRc = rc;
        if (note || waitMs > 0) sceKernelUsleep(300000);
        if (note) {
            log.push_back("   AddSource(" + path + ") = " + hx8(rc) + "  open=" + std::to_string(g_feed.opens.load()) + " leituras=" + std::to_string(g_feed.reads.load()));
            std::lock_guard<std::mutex> l(g_avlogM);
            int shown = 0;
            for (auto &m : g_avlog) {
                if (shown >= 3) break;
                if (m.find("OpenCheck") != std::string::npos || m.find("rror") != std::string::npos || m.find("ail") != std::string::npos ||
                    m.find("nsupport") != std::string::npos || m.find("nvalid") != std::string::npos) { log.push_back("      [av] " + m.substr(0, 150)); shown++; }
            }
        }
        if (rc < 0) { avStopClose(hh); return false; }
        if (waitMs > 0) {
            double t0 = nowSec(); bool got = false;
            while (nowSec() - t0 < waitMs / 1000.0 && !got) {
                KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
                if (sceAvPlayerGetVideoData(hh, &fi) && fi.pData) got = true;
                KAvFrameInfo au; memset(&au, 0, sizeof au);
                sceAvPlayerGetAudioData(hh, &au);
                sceKernelUsleep(8000);
                if (cancelCb && cancelCb()) { cancelled = true; break; }
            }
            if (note) log.push_back(got ? "      -> chegou video!" : "      -> aceitou, mas nenhum quadro em " + std::to_string(waitMs / 1000) + "s");
            if (!got) { avStopClose(hh); return false; }
        }
        (target ? *target : h) = hh;
        return true;
    }

    bool start(const std::string &url, const std::string &title) {
        log.clear(); frames = 0; vw = vh = 0; fps = 0; fpsFrames = 0; failed = false; addRc = 0; chunkMode = false; chunkIdx = 0; inactiveSince = 0;
        cancelled = false; restarts = switchesOk = 0; lastGapMs = 0; gapStartT = switchedAt = 0; curDur = 0; chunkT0 = 0; blitMs = 0;
        h2 = 0; bHandle = 0; bState = 0; nextReady = false; prepBusy = false; gapless = true;
        if (g_tearing.load() > 0) {
            log.push_back("Encerrando a exibicao anterior...");
            while (g_tearing.load() > 0) {
                if (cancelCb && cancelCb()) { cancelled = true; failed = true; return false; }
                presentCb(); sceKernelUsleep(30000);
            }
        }
        this->title = title; hudUntil = nowSec() + 8;
        log.push_back("Reproduzindo: " + title);
        {   // host e tamanho da URL
            size_t a = url.find("://"); a = a == std::string::npos ? 0 : a + 3;
            size_t e = url.find('/', a);
            log.push_back("URL: " + url.substr(0, 8) + "... host=" + url.substr(a, e == std::string::npos ? std::string::npos : e - a) +
                          "  tamanho=" + std::to_string(url.size()));
        }
        g_net.log = nullptr;
        g_net.init();
        log.push_back("modulo AvPlayer (0xA5): " + hx8(sceSysmoduleLoadModule(0x00A5)));
        log.push_back("modulo AvPlayerStreaming: " + hx8(sceSysmoduleLoadModuleInternal(0x80000050)));
        useFeed = url.find(".m3u8") != std::string::npos;     // HLS: o app baixa e entrega os bytes ao player
        if (!useFeed) {                                         // URL direta (o PS4 normalmente recusa)
            if (!openSource(url, false, 3, 0, true)) { failed = true; log.push_back("ERRO: o player nao abriu esta URL."); return false; }
            audio.start(h); active = true; t0 = lastFps = lastFrameT = nowSec();
            return true;
        }
        g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0; g_feedSize = 1ULL << 40;
        g_feed.start(url);                                      // sem outFile: segmentos vao para o buffer (callbacks) e para 'recent' (arquivos)
        for (int i = 0; i < 400 && g_feed.segs.load() < 3 && !g_feed.failed.load(); i++) {
            sceKernelUsleep(50000);
            if (cancelCb && cancelCb()) { cancelled = true; break; }
            if (i % 2 == 0) presentCb();                    // a animacao de "Conectando..." continua rodando
        }
        if (cancelled) { failed = true; stop(); return false; }
        log.push_back("HLS: segmentos baixados=" + std::to_string(g_feed.segs.load()) + "  " + g_feed.getStatus());
        if (g_feed.segs.load() == 0) { failed = true; log.push_back("ERRO: nenhum segmento baixado"); g_feed.join(); return false; }

        std::string chunk; unsigned long long last = 0;
        int n = gatherChunk(true, chunk, last);
        {   // formato do que baixamos (um .ts valido tem 0x47 a cada 188 bytes)
            bool ts = chunk.size() > 376 && (uint8_t)chunk[0] == 0x47 && (uint8_t)chunk[188] == 0x47 && (uint8_t)chunk[376] == 0x47;
            log.push_back("dados: " + std::to_string(n) + " segmentos, " + std::to_string(chunk.size() / 1024) + " KB, MPEG-TS valido=" + (ts ? "sim" : "NAO"));
        }
        // Tentativa principal: converter o TS em MP4 (o AvPlayer toca MP4 local - provado pelo teste F)
        remux = false;
        {
            std::string mp4, err; tsmux::Info ti;
            double c0 = nowSec();
            if (tsmux::convert(chunk, mp4, err, &ti)) {
                char b[160];
                snprintf(b, sizeof b, "conversao TS->MP4 ok: %dx%d, %d quadros, %d audio (%dHz), %.1fs, %d KB, %.0f ms",
                         ti.width, ti.height, ti.vSamples, ti.aSamples, ti.sampleRate, ti.seconds, (int)(mp4.size() / 1024), (nowSec() - c0) * 1000);
                log.push_back(b);
                std::string path = chunkPath("mp4", 0);
                log.push_back("Tentando MP4 convertido " + path + " ...");
                presentCb();
                if (writeFile(path, mp4) && openSource(path, false, 3, 7000, true)) {
                    chunkMode = true; remux = true; chunkExt = "mp4"; chunkIdx = 0; lastSeq = last; curDur = ti.seconds;
                    log.push_back("OK: tocando MP4 convertido");
                } else remove(path.c_str());
            } else log.push_back("ERRO conversao TS->MP4: " + err);
        }
        if (cancelled) { failed = true; stop(); return false; }
        // Plano B: escada de tentativas com o TS cru (varias extensoes) e depois leitura propria (callbacks)
        static const char *exts[] = {"mp4", "m2ts", "mpg", "ts", "mkv"};
        if (!chunkMode)
        for (const char *ext : exts) {
            if (cancelled) break;
            std::string path = chunkPath(ext, 0);
            log.push_back("Tentando arquivo " + path + " ...");
            presentCb();
            if (!writeFile(path, chunk)) { log.push_back("   nao consegui gravar"); continue; }
            if (openSource(path, false, 3, 5000, true)) {
                chunkMode = true; chunkExt = ext; chunkIdx = 0; lastSeq = last;
                log.push_back("OK: tocando por arquivos (." + std::string(ext) + ")");
                break;
            }
            remove(path.c_str());
        }
        if (!chunkMode) {
            static const char *names[] = {"live.mp4", "live.ts", "live.m2ts"};
            for (const char *nm : names) {
                if (cancelled) break;
                log.push_back(std::string("Tentando leitura propria, nome ") + nm + " ...");
                presentCb();
                g_feedSize = 1ULL << 40;
                if (openSource(nm, true, 3, 6000, true)) { log.push_back("OK: tocando por leitura propria"); break; }
            }
        }
        if (cancelled) { failed = true; stop(); return false; }
        if (!h) {
            failed = true;
            log.push_back("ERRO: nenhuma forma funcionou. Tire print desta tela.");
            g_feed.join();
            return false;
        }
        audio.start(h);
        active = true; t0 = lastFps = lastFrameT = nowSec();
        return true;
    }
    std::function<void()> presentCb = [] {};   // main liga isto para redesenhar a tela de espera durante as tentativas

    // ---- preparo assincrono do proximo pedaco: junta, converte e grava enquanto o atual ainda toca ----
    void startPrep() {
        if (prepBusy.load() || nextReady.load()) return;
        std::string data; unsigned long long last = lastSeq;
        if (gatherChunk(false, data, last) == 0) return;
        lastSeq = last;                                      // esses segmentos ja tem dono (o pedaco em preparo)
        int idx = (chunkIdx + 1) % 4;
        bool rm = remux; std::string ext = chunkExt;
        if (prepTh.joinable()) prepTh.join();
        prepBusy = true;
        prepTh = std::thread([this, data, last, idx, rm, ext]() {
            std::string out, err; tsmux::Info ti; bool ok = true;
            if (rm) ok = tsmux::convert(data, out, err, &ti); else out = data;
            std::string path = chunkPath(ext, idx);
            if (ok && writeFile(path, out)) { nextPath = path; nextLast = last; nextDur = rm ? ti.seconds : 0; nextIdx = idx; nextReady = true; }
            prepBusy = false;
        });
    }
    // abre o proximo pedaco num SEGUNDO player, em segundo plano, um pouco antes do atual acabar
    void openNextAsync() {
        if (openTh.joinable()) openTh.join();
        bState = 1;
        std::string path = nextPath;
        openTh = std::thread([this, path] { KAvHandle nh = 0; bool ok = openSource(path, false, 0, 0, false, &nh); bHandle = nh; bState = ok ? 2 : 3; });
    }
    static void closeLater(KAvHandle old, const std::string &oldPath) {
        g_tearing++;
        std::thread([old, oldPath] { avStopClose(old); remove(oldPath.c_str()); g_tearing--; }).detach();
    }
    // troca para o proximo pedaco (ja pronto em disco). Com segundo player aberto nao ha emenda.
    bool switchChunk(double t) {
        if (!nextReady.load()) return false;
        std::string oldPath = chunkPath(chunkExt, chunkIdx);
        gapStartT = lastFrameT > 0 ? lastFrameT : t;
        if (h2) {
            audio.stop();
            KAvHandle old = h; h = h2; h2 = 0;
            audio.start(h);
            closeLater(old, oldPath);
            switchesOk++;
        } else {
            audio.stop();
            avStopClose(h); h = 0;
            remove(oldPath.c_str());
            if (!openSource(nextPath, false, 0, 0, false)) { failed = true; log.push_back("ERRO ao reabrir pedaco: " + hx8(addRc)); nextReady = false; return false; }
            audio.start(h);
            restarts++;
        }
        chunkIdx = nextIdx; curDur = nextDur; nextReady = false;
        chunkT0 = 0; inactiveSince = 0; lastFrameT = t; switchedAt = t;
        return true;
    }
    void managePipeline(double t) {
        bool inactive = !sceAvPlayerIsActive(h);
        if (inactive && inactiveSince == 0) inactiveSince = t;
        int fresh = 0; { std::lock_guard<std::mutex> l(g_feed.rm); for (auto &sg : g_feed.recent) if (sg.first > lastSeq) fresh++; }
        // 1) comeca a preparar o proximo pedaco assim que houver 2 segmentos novos
        if (!prepBusy.load() && !nextReady.load() && !h2 && fresh >= 2) startPrep();
        // 2) segundo player
        if (bState.load() == 2) { h2 = bHandle; bState = 0; }
        else if (bState.load() == 3) { bState = 0; gapless = false; log.push_back("segundo player nao abriu: troca com emenda"); }
        double el = chunkT0 > 0 ? t - chunkT0 : 0;
        if (gapless && nextReady.load() && !h2 && bState.load() == 0 && chunkT0 > 0 && curDur > 0 && el >= curDur - 0.8) openNextAsync();
        // 3) troca
        bool ended = inactive && t - inactiveSince > 0.03;
        bool overdue = curDur > 0 && chunkT0 > 0 && el > curDur + 0.6;
        bool stalled = t - lastFrameT > 3.0;
        if (nextReady.load()) {
            if (h2 && (ended || overdue || stalled)) switchChunk(t);
            else if (!h2 && bState.load() == 0 && (ended || stalled)) switchChunk(t);
        } else if ((ended || stalled) && !prepBusy.load() && fresh >= 1) startPrep();
        // segundo player que nao entregou video depois da troca: desliga o modo sem emenda
        if (switchedAt > 0 && chunkT0 == 0 && t - switchedAt > 2.5 && gapless) { gapless = false; log.push_back("sem quadros apos a troca: modo sem emenda desligado"); }
    }
    // Desmonta TUDO em segundo plano: o botao O volta ao menu na hora (antes travava ate a rede responder).
    void stop() {
        KAvHandle hh = h, hh2 = h2;
        h = 0; h2 = 0; active = false; chunkMode = false;
        g_feed.abort();                                      // acorda leituras bloqueadas
        g_tearing++;
        std::thread([this, hh, hh2] {
            audio.stop();
            if (openTh.joinable()) openTh.join();
            KAvHandle hb = bState.load() == 2 ? bHandle : 0;
            bState = 0;
            avStopClose(hh);
            if (hh2) avStopClose(hh2);
            if (hb) avStopClose(hb);
            if (prepTh.joinable()) prepTh.join();
            g_feed.join();
            for (int i = 0; i < 4; i++) { static const char *e[] = {"mp4","m2ts","mpg","ts","mkv"}; for (const char *x : e) remove(chunkPath(x, i).c_str()); }
            if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; }
            remove("/data/live.ts");
            nextReady = false; prepBusy = false;
            g_tearing--;
        }).detach();
    }
    // espera as desmontagens terminarem (usado ao fechar o app)
    static void waitTeardown(double maxSec) {
        double t0 = nowSec();
        while (g_tearing.load() > 0 && nowSec() - t0 < maxSec) sceKernelUsleep(20000);
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
                double b0 = nowSec();
                blitNV12(fi.pData, w, hh, pitch, yRows, r.fb, r.W, r.H);
                double bm = (nowSec() - b0) * 1000.0;
                blitMs = blitMs == 0 ? bm : blitMs * 0.9 + bm * 0.1;
                frames++; fpsFrames++;
                drew = true;
            }
        }
        double t = nowSec();
        if (drew) {
            lastFrameT = t; inactiveSince = 0;
            if (chunkT0 == 0) { chunkT0 = t; if (gapStartT > 0) { lastGapMs = (t - gapStartT) * 1000.0; gapStartT = 0; } }
        }
        if (chunkMode && h && !failed) managePipeline(t);
        if (t - lastFps >= 1.0) { fps = fpsFrames / (t - lastFps); fpsFrames = 0; lastFps = t; }
        return drew;
    }
    bool hudActive() const { return nowSec() <= hudUntil || (useFeed && g_feed.failed); }
    // HUD moderno sobre o video (aparece nos primeiros segundos e quando qualquer botao e apertado)
    void overlay(FbRenderer &r) {
        using namespace theme;
        if (nowSec() > hudUntil && !(useFeed && g_feed.failed)) return;
        if (nowSec() <= hudUntil) {
            char b[96];
            snprintf(b, sizeof b, "%ux%u  |  %.0f fps  |  desenho %.0f ms", vw, vh, fps, blitMs);
            std::string stats = b;
            int tw = r.textWidth(title, 24), sw = r.textWidth(stats, 18);
            int pw = 24 + 92 + 16 + tw + 24 + sw + 28;
            r.roundRect(32, 28, pw, 60, 30, 0xB8000000);
            r.roundRect(44, 42, 92, 32, 16, RED);
            uiCenter(r, 44 + 46, 58, "AO VIVO", TEXT, 16, true);
            uiText(r, 44 + 92 + 16, 58 - 12, title, TEXT, 24, true);
            uiText(r, 44 + 92 + 16 + tw + 24, 58 - 9, stats, MUTED, 18);
            int hw = r.textWidth("O  Sair     TRIÂNGULO  Ajustar imagem", 18) + 48;
            r.roundRect(r.W - 32 - hw, r.H - 76, hw, 44, 22, 0xB8000000);
            uiCenter(r, r.W - 32 - hw / 2, r.H - 54, "O  Sair     TRIÂNGULO  Ajustar imagem", SOFT, 18);
            snprintf(b, sizeof b, "stride %d  |  audio %d %uHz  |  trocas: %d sem emenda, %d com emenda  |  ultima %.0f ms%s", strideMode, audio.state.load(), audio.rate.load(), switchesOk, restarts, lastGapMs, gapless ? "" : "  (sem-emenda OFF)");
            r.roundRect(32, r.H - 76, r.textWidth(b, 16) + 40, 40, 20, 0x90000000);
            uiText(r, 52, r.H - 76 + 12, b, DIM, 16);
        }
        if (useFeed && g_feed.failed) {
            std::string m = uiFit(r, g_feed.getStatus(), 20, r.W - 160);
            r.roundRect(32, 100, r.textWidth(m, 20) + 48, 44, 22, 0xD0801520);
            uiText(r, 56, 100 + 12, m, TEXT, 20);
        }
    }
    void renderWaiting(FbRenderer &r) {
        using namespace theme;
        uiBackground(r);
        // cabecalho: spinner (ou alerta) + titulo
        const int scx = 96, scy = 112;
        if (failed) {
            r.roundRect(scx - 34, scy - 34, 68, 68, 34, RED);
            uiCenter(r, scx, scy, "!", TEXT, 40, true);
        } else {
            double tt = nowSec();
            for (int i = 0; i < 12; i++) {
                static const float kc[12] = {1.f, .8660254f, .5f, 0.f, -.5f, -.8660254f, -1.f, -.8660254f, -.5f, 0.f, .5f, .8660254f};
                static const float ks[12] = {0.f, .5f, .8660254f, 1.f, .8660254f, .5f, 0.f, -.5f, -.8660254f, -1.f, -.8660254f, -.5f};
                float xx = (float)tt * 1.3f - i / 12.0f + 4.0f;
                float ph = xx - (float)(int)xx;
                int al = 40 + (int)(215 * (1.0f - ph));
                int px = scx + (int)(30 * kc[i]), py = scy + (int)(30 * ks[i]);
                r.roundRect(px - 6, py - 6, 12, 12, 6, ((unsigned)al << 24) | 0x0053FC18);
            }
        }
        uiText(r, 164, 70, failed ? "Não foi possível abrir" : "Conectando...", failed ? RED : TEXT, 38, true);
        char b[160];
        if (failed) snprintf(b, sizeof b, "AddSource = %08X   |   aperte O para voltar ao menu", (unsigned)addRc);
        else snprintf(b, sizeof b, "%s   |   %.0fs   |   O para cancelar", title.c_str(), nowSec() - t0);
        uiText(r, 166, 122, uiFit(r, b, 20, r.W - 240), MUTED, 20);
        if (!failed && nowSec() - t0 > 15) uiText(r, 166, 150, "Demorando mais que o normal: tire um print desta tela e mande para o Claude.", AMBER, 20);

        // cartao com o registro das tentativas
        const int cy0 = 196, ch = r.H - cy0 - 110;
        r.roundRect(48, cy0, r.W - 96, ch, 22, CARD);
        int y = cy0 + 22;
        size_t from = log.size() > 29 ? log.size() - 29 : 0;
        for (size_t i = from; i < log.size(); i++) {
            const std::string &l = log[i];
            unsigned c = SOFT; bool bold = false;
            if (l.compare(0, 4, "ERRO") == 0 || l.find("nenhum quadro") != std::string::npos) c = RED;
            else if (l.compare(0, 2, "OK") == 0 || l.find("chegou video") != std::string::npos) { c = GREEN; bold = true; }
            else if (l.compare(0, 8, "Tentando") == 0) c = AMBER;
            else if (l.find("[av]") != std::string::npos) c = DIM;
            else if (l.compare(0, 3, "   ") == 0) c = MUTED;
            uiText(r, 76, y, uiFit(r, l, 18, r.W - 160), c, 18, bold);
            y += 24;
        }
        if (useFeed) {
            snprintf(b, sizeof b, "HLS: %d segmentos  |  %s", g_feed.segs.load(), g_feed.getStatus().c_str());
            uiText(r, 52, r.H - 82, uiFit(r, b, 18, r.W - 420), g_feed.failed ? RED : MUTED, 18);
        }
        uiHints(r, r.W - 48, r.H - 50, {{"O", "Voltar"}});
    }
};

// ---------- Diagnostico do AvPlayer: tenta varias formas de abrir uma fonte e mostra cada resultado ----------
static std::string hx(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

// repl: usa callbacks de arquivo | path: nome passado ao AddSource | hls: se != NULL, alimenta o buffer com este HLS
static void avAttempt(std::vector<std::string> &d, const std::function<void()> &refresh, const char *name, bool repl, const char *path,
                      const char *hls, bool autoStart, const std::string *blob = nullptr, bool keepFeed = false) {
    d.push_back(std::string("-- ") + name);
    refresh();
    KAvInitData id; memset(&id, 0, sizeof id);
    id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
    id.debugLevel = 3; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = autoStart; id.defaultLanguage = "eng";
    { std::lock_guard<std::mutex> l(g_avlogM); g_avlog.clear(); }
    g_allocN = g_allocFail = g_texN = g_texFail = 0;
    sceAvPlayerSetLogCallback(avLogCb, NULL);
    if (repl) { id.fileObj = (void *)&g_feed; id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
    if (!keepFeed) {
        g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0;
        g_feedSize = 1ULL << 40;
        if (blob) { g_feed.sb.reset(); g_feed.sb.append((const uint8_t *)blob->data(), blob->size()); g_feedSize = blob->size(); }
        if (hls) g_feed.start(hls);
    }
    KAvHandle h = sceAvPlayerInit(&id);
    d.push_back("   Init: handle=" + hx((int)(h & 0xFFFFFFFF)) + (h ? "" : "  (FALHOU)"));
    int rc = h ? sceAvPlayerAddSource(h, path) : -1;
    sceKernelUsleep(400000);                       // mensagens do player chegam de outra thread
    d.push_back("   AddSource(\"" + std::string(path).substr(0, 60) + "\") = " + hx(rc) + "   alloc=" + std::to_string(g_allocN.load()) + "(falhas " +
                std::to_string(g_allocFail.load()) + ") textura=" + std::to_string(g_texN.load()) + "(falhas " + std::to_string(g_texFail.load()) + ")  open=" +
                std::to_string(g_feed.opens.load()) + " leituras=" + std::to_string(g_feed.reads.load()));
    {   // mensagens internas do proprio AvPlayer (as primeiras)
        std::lock_guard<std::mutex> l(g_avlogM);
        for (size_t i = 0; i < g_avlog.size() && i < 6; i++) d.push_back("   [av] " + g_avlog[i].substr(0, 170));
        if (g_avlog.size() > 6) d.push_back("   [av] ... (+" + std::to_string(g_avlog.size() - 6) + " mensagens)");
    }
    refresh();
    if (h && rc >= 0) {
        double t0 = nowSec(); int frames = 0, vw = 0, vh = 0, act = 0; unsigned ach = 0, asr = 0; int aframes = 0;
        d.push_back("   (tocando...)"); size_t pi = d.size() - 1; int lastSec = 0;
        while (nowSec() - t0 < 7.0) {
            int sec = (int)(nowSec() - t0);
            if (sec != lastSec) { lastSec = sec; d[pi] = "   t=" + std::to_string(sec) + "s quadros=" + std::to_string(frames) + " audio=" + std::to_string(aframes); refresh(); }
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
    refresh();
    g_feed.abort();
    bool okClose = avStopClose(h);
    if (!okClose) d.push_back("   !! sceAvPlayerStop/Close travou (mais de 5s): seguindo adiante");
    if (!keepFeed) g_feed.join();
    refresh();
}

static void avTailAttempt(std::vector<std::string> &d, const std::function<void()> &refresh, const char *hls) {
    d.push_back("-- I) HLS gravado em arquivo que cresce (/data/live.ts), sem leitura propria");
    refresh();
    g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0; g_feedSize = 1ULL << 40;
    g_feed.outFile = fopen("/data/live.ts", "wb"); g_feed.outBytes = 0;
    if (!g_feed.outFile) { d.push_back("   nao consegui criar /data/live.ts"); refresh(); return; }
    g_feed.start(hls);
    for (int i = 0; i < 75 && g_feed.segs.load() < 2 && !g_feed.failed.load(); i++) sceKernelUsleep(200000);
    d.push_back("   segmentos baixados=" + std::to_string(g_feed.segs.load()) + "  bytes=" + std::to_string(g_feed.outBytes.load()) + "  " + g_feed.getStatus());
    refresh();
    if (g_feed.segs.load() > 0) avAttempt(d, refresh, "   I2) AddSource do arquivo que cresce", false, "/data/live.ts", NULL, true, nullptr, true);
    closeFeedFile();
    refresh();
}

static void runAvTest(std::vector<std::string> &d, const std::function<void()> &refresh) {
    d.clear();
    d.push_back("== DIAGNOSTICO DO AVPLAYER v2 ==  (cada teste leva ate ~10s)");
    g_net.log = nullptr; g_net.init();
    d.push_back("modulo AvPlayer (0xA5): " + hx(sceSysmoduleLoadModule(0x00A5)));
    d.push_back("modulo AvPlayerStreaming: " + hx(sceSysmoduleLoadModuleInternal(0x80000050)));
    refresh();

    // E) baixa um MP4 pequeno e conhecido (H.264/AAC): serve de CONTROLE POSITIVO
    const char *mp4Url = "https://interactive-examples.mdn.mozilla.net/media/cc0-videos/flower.mp4";
    std::string mp4; int st = 0;
    bool got = g_net.get(mp4Url, mp4, st, 8u << 20);
    d.push_back("-- E) baixar flower.mp4: ok=" + std::to_string(got ? 1 : 0) + " http=" + std::to_string(st) + " bytes=" + std::to_string(mp4.size()));
    refresh();
    bool wrote = false;
    if (got && st == 200 && mp4.size() > 10000) {
        FILE *f = fopen("/data/teste.mp4", "wb");
        if (f) { wrote = fwrite(mp4.data(), 1, mp4.size(), f) == mp4.size(); fclose(f); }
        d.push_back(std::string("   gravado em /data/teste.mp4: ") + (wrote ? "sim" : "NAO"));
        refresh();
    }
    // F) o teste que decide: MP4 local, sem callbacks proprios
    if (wrote) avAttempt(d, refresh, "F) CONTROLE: MP4 local /data/teste.mp4, sem leitura propria", false, "/data/teste.mp4", NULL, true);
    // H) o mesmo MP4 por URL https direta (o player abre a rede sozinho)
    avAttempt(d, refresh, "H) MP4 por URL https direta, sem leitura propria", false, mp4Url, NULL, true);
    // A) HLS publico pela leitura propria (o teste original)
    const char *hls = kTestHlsHttps();
    avTailAttempt(d, refresh, hls);
    avAttempt(d, refresh, "A) HLS publico: leitura propria + /app0/live.ts", true, "/app0/live.ts", hls, true);

    // G) pode FECHAR o app (o teste equivalente antigo derrubava): da tempo de tirar o print antes
    if (got && st == 200 && mp4.size() > 10000) {
        d.push_back("TIRE O PRINT AGORA. Em 10s comeca o teste G (pode fechar o app).");
        refresh();
        sceKernelUsleep(10 * 1000 * 1000);
        avAttempt(d, refresh, "G) o MESMO MP4 pela leitura propria (callbacks), nome live.mp4", true, "live.mp4", NULL, true, &mp4);
    }
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
        using namespace theme;
        uiBackground(r);
        uiLogo(r, 48, 28, 52);
        uiText(r, 118, 30, "Digite o @ do streamer", TEXT, 32, true);
        uiText(r, 120, 68, "Canal da Kick que você quer assistir", MUTED, 18);
        // campo de texto
        r.roundRect(48, 120, 960, 84, 22, CARD);
        r.roundRect(48, 120, 960, 84, 22, 0x1053FC18);
        uiText(r, 78, 142, "@", GREEN, 40, true);
        std::string shown = text;
        uiText(r, 118, 142, shown, TEXT, 40, true);
        if (((int)(nowSec() * 2)) % 2 == 0) r.roundRect(118 + r.textWidth(shown, 40) + 2, 140, 4, 44, 2, GREEN);
        // teclas
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                bool on = (x == cx && y == cy);
                int px = 48 + x * 96, py = 236 + y * 100;
                std::string k = keyAt(x, y);
                bool special = k == "<" || k == "OK";
                if (on) r.roundRect(px - 4, py - 4, 94, 94, 20, GREEN_GLOW);
                r.roundRect(px, py, 86, 86, 16, on ? GREEN : (k == "OK" ? 0xFF1F3A1B : CARD2));
                std::string lab = k == "<" ? "Apagar" : k;
                uiCenter(r, px + 43, py + 43, lab, on ? BLACK : (k == "OK" ? GREEN : TEXT), special ? 20 : 32, true);
            }
        // painel de controles
        const int cx0 = 1060, cw = r.W - 48 - cx0;
        r.roundRect(cx0, 120, cw, 516, 22, 0xFF111623);
        uiText(r, cx0 + 32, 142, "Controles", GREEN, 24, true);
        static const char *ks[5] = {"D-pad", "X", "QUADRADO", "R1", "O"};
        static const char *ls[5] = {"mover pelas teclas", "digitar a letra", "apagar", "confirmar", "cancelar"};
        for (int i = 0; i < 5; i++) {
            int ry = 214 + i * 80;
            int kw = r.textWidth(ks[i], 20) + 28;
            r.roundRect(cx0 + 32, ry - 20, kw, 40, 12, KEY);
            uiCenter(r, cx0 + 32 + kw / 2, ry, ks[i], TEXT, 20, true);
            uiText(r, cx0 + 32 + kw + 18, ry - 11, ls[i], SOFT, 22);
        }
    }
};

static const uint32_t B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_LEFT = 0x0080, B_RIGHT = 0x0020, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

enum class Mode { Menu, Diag, Play, Kbd };

// Leitura do controle sem perder toques rapidos: o scePadRead guarda os estados recentes, entao um aperto curto
// feito enquanto o app estava ocupado (carregando, trocando de pedaco) ainda e visto depois.
struct PadIn {
    int h = -1;
    uint32_t held = 0, pending = 0;
    void poll() {
        if (h < 0) return;
        OrbisPadData buf[16]; uint32_t seen = 0;
        int n = scePadRead(h, buf, 16);
        for (int i = 0; i < n; i++) seen |= buf[i].buttons;
        OrbisPadData d; uint32_t cur = held;
        if (scePadReadState(h, &d) == 0) cur = d.buttons;
        seen |= cur;
        pending |= seen & ~held;
        held = cur;
    }
    uint32_t take() { uint32_t e = pending; pending = 0; return e; }
};

int main() {
    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
    sceAudioOutInit();
    int pad = scePadOpen(userId, 0, 0, NULL);
    PadIn pin; pin.h = pad;
    bool wantQuit = false;

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
    BootScreen boot;
    boot.init(bufs[0], W, H);                                       // so escrita: igual ao codigo original
    sceVideoOutSubmitFlip(vh, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    boot.line("[1] video aberto");

    traceInit();
    trace("video-ok");
    if (!g_prevStage.empty()) boot.line("da ultima vez o app parou em: " + g_prevStage, 0xFFFFB020u);
    Ps4Platform pf;
    App app(pf);
    pf.app = &app;
    UI ui(app);
    fillDemo(app);
    ui.tab = Tab::Kick;                       // abre direto na aba Kick (os canais M3U sao so demonstracao)
    if (!g_prevStage.empty()) app.say("Aviso: da ultima vez o app parou no passo: " + g_prevStage);
    trace("estado-criado");
    boot.line("[2] estado do app criado");


    std::vector<std::string> diag;
    Mode mode = Mode::Menu;
    Player player;
    Keyboard kbd;
    bool gotFrame = false;
    double lastWaitDraw = 0;
    uint32_t prev = 0;
    int heldFrames = 0, back = 1;
    bool dirty = true;

    // A memoria de video e "write-combined": ESCREVER e rapido, mas LER e lentissimo. O desenho novo (transparencia,
    // texto suave) le pixels, entao desenhamos tudo numa tela de rascunho na RAM normal e so copiamos para o video.
    // O rascunho fica em memoria "cacheada" do sistema (tipo 0), fora do heap do libc.
    uint32_t *shadow = NULL;
    std::vector<uint32_t> shadowFallback;
    {
        off_t phys2 = 0; void *m2 = NULL;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bufSize, 0x200000, 0, &phys2) >= 0 &&
            sceKernelMapDirectMemory(&m2, bufSize, 0x33, 0, phys2, 0x200000) >= 0) shadow = (uint32_t *)m2;
        if (!shadow) { shadowFallback.assign((size_t)W * H, 0); shadow = shadowFallback.data(); trace("rascunho-no-heap"); }
        for (size_t i = 0; i < (size_t)W * H; i++) shadow[i] = 0xFF090B10u;
    }
    trace("rascunho-ok");
    boot.line(shadow == shadowFallback.data() ? "[3] rascunho de tela: heap" : "[3] rascunho de tela: memoria direta");
    auto flip = [&]() {
        memcpy(bufs[back], shadow, (size_t)W * H * 4);          // copia sequencial (rapida)
        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back ^= 1;
    };
    auto presentMenu = [&]() { FbRenderer r(shadow, W, H); ui.render(r); flip(); };
    auto presentDiag = [&](bool running) { FbRenderer r(shadow, W, H); renderDiag(r, diag, running); flip(); };
    auto presentWait = [&]() { FbRenderer r(shadow, W, H); player.renderWaiting(r); flip(); };
    auto presentKbd = [&]() { FbRenderer r(shadow, W, H); kbd.render(r); flip(); };
    boot.line("[4] abrindo a interface...");
    sceKernelUsleep(400000);
    trace("abertura");

    {   // ---- abertura animada: logo sobe e aparece, titulo, barra de carregamento ----
        double ti = nowSec();
        for (;;) {
            double e = nowSec() - ti;
            if (e > 1.8) break;
            FbRenderer r(shadow, W, H);
            using namespace theme;
            uiBackground(r);
            float k = (float)(e / 0.7); if (k > 1.f) k = 1.f;
            float ease = 1.f - (1.f - k) * (1.f - k) * (1.f - k);
            int cx = W / 2, cy = H / 2 - 70 + (int)((1.f - ease) * 40);
            r.roundRect(cx - 100, cy - 100, 200, 200, 52, GREEN_GLOW);
            uiLogo(r, cx - 84, cy - 84, 168);
            uiCenter(r, cx, H / 2 + 80, "Kick IPTV", TEXT, 64, true);
            uiCenter(r, cx, H / 2 + 138, "Lives da Kick direto no seu PS4", MUTED, 24);
            float pg = (float)(e / 1.6); if (pg > 1.f) pg = 1.f;
            r.roundRect(cx - 220, H / 2 + 200, 440, 10, 5, CARD2);
            r.roundRect(cx - 220, H / 2 + 200, (int)(440 * pg) < 10 ? 10 : (int)(440 * pg), 10, 5, GREEN);
            int fade = (int)(255.f * (1.f - ease));                      // entra do preto
            if (fade > 0) r.fillRect(0, 0, W, H, ((unsigned)fade << 24) | 0x00090B10u);
            flip();
            sceKernelUsleep(8000);
        }
    }
    trace("intro-ok");

    for (;;) {
        pin.poll();
        uint32_t now = pin.held, pressed = pin.take();
        if (wantQuit || ((now | pressed) & B_OPTIONS)) break;
        (void)prev;

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
            if (pressed) player.hudUntil = nowSec() + 5;      // qualquer botao mostra o HUD por 5s
            if (pressed & B_TRIANGLE) player.strideMode = (player.strideMode + 1) % 4;
            if (player.h) {
                // Sem HUD o video e desenhado DIRETO no quadro de video (so escrita, sem a copia de 8 MB).
                // Com HUD (precisa ler pixels para a transparencia) passa pelo rascunho, como antes.
                bool hud = player.hudActive();
                FbRenderer r(hud ? shadow : (uint32_t *)bufs[back], W, H);
                if (player.poll(r)) {
                    gotFrame = true;
                    if (hud) { player.overlay(r); flip(); }
                    else { sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back ^= 1; }
                }
                else if (!gotFrame && nowSec() - lastWaitDraw > 0.12) { lastWaitDraw = nowSec(); presentWait(); }
            } else if (nowSec() - lastWaitDraw > 0.12) { lastWaitDraw = nowSec(); presentWait(); }
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
            player.presentCb = presentWait;
            player.cancelCb = [&]() -> bool {        // O cancela o carregamento na hora; OPTIONS fecha o app
                pin.poll();
                if (pin.pending & B_CIRCLE) { pin.pending &= ~B_CIRCLE; return true; }
                if ((pin.held | pin.pending) & B_OPTIONS) { wantQuit = true; return true; }
                return false;
            };
            trace("player-start");
            player.start(url, title);
            if (player.cancelled) { player.cancelled = false; mode = Mode::Menu; dirty = true; trace("player-cancelado"); continue; }
            trace(player.failed ? "player-falhou" : "player-aberto");
            presentWait();
            continue;
        }
        if (dirty) { presentMenu(); dirty = false; static bool once = false; if (!once) { once = true; trace("menu-ok"); } }
        sceKernelUsleep(16000);
    }
    // ---- encerramento limpo: nenhuma thread pode estar viva ao sair (std::thread ativa no fim = "terminate" = erro no PS4) ----
    trace("saindo");
    if (player.h || player.h2 || mode == Mode::Play) player.stop();
    Player::waitTeardown(10.0);
    if (g_tearing.load() > 0) { trace("fim-forcado"); _Exit(0); }   // algo travou (rede): sai sem rodar destrutores
    g_feed.join();
    blit::pool().shutdown();
    trace("fim");
    return 0;
}
