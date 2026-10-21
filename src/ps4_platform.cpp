// TESTE 3a - REDE (HTTP) + diagnostico na tela. Mantem o menu do Teste 2.
//   QUADRADO = roda o teste de rede e mostra o resultado na tela | O = voltar ao menu | OPTIONS = sair
// Os modulos de rede do PS4 sao carregados em tempo de execucao (sem precisar de -lSceHttp no link).
// Precisa do src/font8x16.hpp (o mesmo do Teste 2).
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "ui.hpp"
#include "font8x16.hpp"

// ---------- Cliente HTTP (libSceNet + libSceSsl + libSceHttp carregadas via dlsym) ----------
struct NetClient {
    std::vector<std::string> *log = nullptr;
    bool ready = false, failed = false;
    int httpCtx = -1;

    int (*fNetInit)() = nullptr;
    int (*fNetPoolCreate)(const char *, int, int) = nullptr;
    int (*fSslInit)(size_t) = nullptr;
    int (*fHttpInit)(int, int, size_t) = nullptr;
    int (*fCreateTemplate)(int, const char *, int, int) = nullptr;
    int (*fCreateConn)(int, const char *, int) = nullptr;
    int (*fCreateReq)(int, int, const char *, unsigned long long) = nullptr;
    int (*fSend)(int, const void *, size_t) = nullptr;
    int (*fStatus)(int, int *) = nullptr;
    int (*fRead)(int, void *, size_t) = nullptr;
    int (*fDelReq)(int) = nullptr;
    int (*fDelConn)(int) = nullptr;
    int (*fDelTmpl)(int) = nullptr;
    // opcionais
    int (*fDisableOpt)(int, unsigned) = nullptr;
    int (*fAutoRedirect)(int, int) = nullptr;
    int (*fAddHeader)(int, const char *, const char *, int) = nullptr;
    int (*fTmoConnect)(int, unsigned) = nullptr;
    int (*fTmoSend)(int, unsigned) = nullptr;
    int (*fTmoRecv)(int, unsigned) = nullptr;
    int (*fTmoResolve)(int, unsigned) = nullptr;

    void L(const std::string &s) { if (log) log->push_back(s); }
    static std::string hex(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    template <class T> bool sym(int mod, const char *name, T &out, bool required = true) {
        void *p = nullptr;
        int rc = (mod > 0) ? sceKernelDlsym(mod, name, &p) : -1;
        if (rc < 0 || !p) { if (required) L(std::string("  FALTA: ") + name); return false; }
        out = (T)p;
        return true;
    }

    bool init() {
        if (ready) return true;
        if (failed) { L("(inicializacao ja falhou acima)"); return false; }
        failed = true;
        const char *paths[3] = {"/system/common/lib/libSceNet.sprx", "/system/common/lib/libSceSsl.sprx", "/system/common/lib/libSceHttp.sprx"};
        const char *shortn[3] = {"Net", "Ssl", "Http"};
        int h[3];
        for (int i = 0; i < 3; i++) {
            int res = 0;
            h[i] = sceKernelLoadStartModule(paths[i], 0, NULL, 0, NULL, &res);
            L(std::string("modulo ") + shortn[i] + ": handle=" + hex(h[i]));
        }
        bool ok = true;
        ok &= sym(h[0], "sceNetInit", fNetInit);
        ok &= sym(h[0], "sceNetPoolCreate", fNetPoolCreate);
        ok &= sym(h[1], "sceSslInit", fSslInit);
        ok &= sym(h[2], "sceHttpInit", fHttpInit);
        ok &= sym(h[2], "sceHttpCreateTemplate", fCreateTemplate);
        ok &= sym(h[2], "sceHttpCreateConnectionWithURL", fCreateConn);
        ok &= sym(h[2], "sceHttpCreateRequestWithURL", fCreateReq);
        ok &= sym(h[2], "sceHttpSendRequest", fSend);
        ok &= sym(h[2], "sceHttpGetStatusCode", fStatus);
        ok &= sym(h[2], "sceHttpReadData", fRead);
        ok &= sym(h[2], "sceHttpDeleteRequest", fDelReq);
        ok &= sym(h[2], "sceHttpDeleteConnection", fDelConn);
        ok &= sym(h[2], "sceHttpDeleteTemplate", fDelTmpl);
        sym(h[2], "sceHttpsDisableOption", fDisableOpt, false);
        sym(h[2], "sceHttpSetAutoRedirect", fAutoRedirect, false);
        sym(h[2], "sceHttpAddRequestHeader", fAddHeader, false);
        sym(h[2], "sceHttpSetConnectTimeOut", fTmoConnect, false);
        sym(h[2], "sceHttpSetSendTimeOut", fTmoSend, false);
        sym(h[2], "sceHttpSetRecvTimeOut", fTmoRecv, false);
        sym(h[2], "sceHttpSetResolveTimeOut", fTmoResolve, false);
        if (!ok) { L("funcoes obrigatorias ausentes -> abortando"); return false; }

        int rc = fNetInit();                       L("sceNetInit: " + hex(rc));
        int netMem = fNetPoolCreate("kickiptv", 64 * 1024, 0); L("sceNetPoolCreate: " + hex(netMem));
        int ssl = fSslInit(300 * 1024);            L("sceSslInit: " + hex(ssl));
        httpCtx = fHttpInit(netMem, ssl, 128 * 1024); L("sceHttpInit: " + hex(httpCtx));
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
        int tmpl = fCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        L("  CreateTemplate: " + hex(tmpl));
        if (tmpl < 0) return false;
        if (fDisableOpt) fDisableOpt(tmpl, 0x1F);   // sem verificacao de certificado (o PS4 nao tem os CAs carregados aqui)
        if (fAutoRedirect) fAutoRedirect(tmpl, 1);
        const unsigned T = 15 * 1000 * 1000;
        if (fTmoResolve) fTmoResolve(tmpl, T);
        if (fTmoConnect) fTmoConnect(tmpl, T);
        if (fTmoSend) fTmoSend(tmpl, T);
        if (fTmoRecv) fTmoRecv(tmpl, T);
        int conn = fCreateConn(tmpl, url.c_str(), 1);
        L("  CreateConnection: " + hex(conn));
        bool ok = false;
        if (conn >= 0) {
            int req = fCreateReq(conn, 0 /*GET*/, url.c_str(), 0);
            L("  CreateRequest: " + hex(req));
            if (req >= 0) {
                if (fAddHeader) {
                    fAddHeader(req, "Accept", "application/json, text/plain, */*", 0);
                    fAddHeader(req, "Accept-Language", "pt-BR,pt;q=0.9,en;q=0.8", 0);
                }
                int rc = fSend(req, NULL, 0);
                L("  SendRequest: " + hex(rc));
                if (rc >= 0) {
                    fStatus(req, &status);
                    char buf[4096];
                    for (;;) {
                        int n = fRead(req, buf, sizeof buf);
                        if (n <= 0) break;
                        body.append(buf, n);
                        if (body.size() > 256 * 1024) break;
                    }
                    ok = true;
                }
                fDelReq(req);
            }
            fDelConn(conn);
        }
        fDelTmpl(tmpl);
        return ok;
    }
};

static NetClient g_net;

// ---------- Plataforma ----------
struct Ps4Platform : Platform {
    App *app = nullptr;
    bool httpGet(const std::string &url, std::string &body) override {
        int st = 0;
        bool ok = g_net.get(url, body, st);
        return ok && st >= 200 && st < 300;
    }
    void play(const std::string &, const std::string &title) override {
        if (app) app->say("Tocaria: " + title + "  (player entra no Teste 3c)");
    }
    void requestText(const std::string &) override {
        if (app) app->say("Teclado na tela entra no Teste 3b");
    }
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
    app.say("TESTE 3a | QUADRADO = teste de rede | OPTIONS sai");
}

static const uint32_t B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_SQUARE = 0x8000, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

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
    bool diagMode = false;
    uint32_t prev = 0;
    int heldFrames = 0, back = 0;
    bool dirty = true;

    auto present = [&](bool running) {
        FbRenderer r(bufs[back], W, H);
        if (diagMode) renderDiag(r, diag, running); else ui.render(r);
        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
        back ^= 1;
    };

    for (;;) {
        OrbisPadData d;
        uint32_t now = 0;
        if (pad >= 0 && scePadReadState(pad, &d) == 0) now = d.buttons;
        if (now & B_OPTIONS) break;
        uint32_t pressed = now & ~prev;
        prev = now;

        if (diagMode) {
            if (pressed & B_CIRCLE) { diagMode = false; dirty = true; }
            if (pressed & B_SQUARE) { // rodar de novo
                diag.assign(1, "Executando...");
                present(true);
                runNetTest(diag);
                dirty = true;
            }
        } else {
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
            if (pressed & B_SQUARE) {
                diagMode = true;
                diag.assign(1, "Executando...");
                present(true);        // mostra "Executando" antes de bloquear na rede
                runNetTest(diag);
                dirty = true;
            }
        }
        if (dirty) { present(false); dirty = false; }
        sceKernelUsleep(16000);
    }
    return 0;
}
