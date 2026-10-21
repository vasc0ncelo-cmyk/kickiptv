// TESTE 2 - UI na tela (abas + lista) navegavel com o controle, dados de exemplo (sem rede).
//   Direcionais: cima/baixo | L1/R1: trocar aba | L2/R2: pagina | X: confirmar | O: voltar | OPTIONS: sair
// Precisa tambem do arquivo src/font8x16.hpp (fonte embutida).
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <stdint.h>
#include <string>
#include "ui.hpp"
#include "font8x16.hpp"

// ---------- Plataforma (ainda sem rede/video; so mostra mensagens na barra de status) ----------
struct Ps4Platform : Platform {
    App *app = nullptr;
    bool httpGet(const std::string &, std::string &) override { return false; }
    void play(const std::string &, const std::string &title) override {
        if (app) app->say("Tocaria: " + title + "  (player entra no Teste 3)");
    }
    void requestText(const std::string &) override {
        if (app) app->say("Teclado na tela entra no Teste 3");
    }
};

// ---------- Renderer em cima do framebuffer (BGRA, 0xAARRGGBB) ----------
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
    // size = altura do texto em pixels (fonte base 8x16, ampliada por vizinho mais proximo)
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

// ---------- Dados de exemplo para testar a navegacao ----------
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
    for (int e = 1; e <= 4; e++)
        m += "#EXTINF:-1 group-title=\"Series\",Lost S01E0" + std::to_string(e) + "\nhttp://exemplo/series/l" + std::to_string(e) + ".mp4\n";
    app.list = buildPlaylist(m);
    app.say("TESTE 2 - dados de exemplo | OPTIONS sai");
}

// Mascaras de botao do PS4 (valores padrao do scePad)
static const uint32_t B_OPTIONS = 0x0008, B_UP = 0x0010, B_DOWN = 0x0040, B_L2 = 0x0100, B_R2 = 0x0200,
                      B_L1 = 0x0400, B_R1 = 0x0800, B_CIRCLE = 0x2000, B_CROSS = 0x4000;

int main() {
    Ps4Platform pf;
    App app(pf);
    pf.app = &app;
    UI ui(app);
    fillDemo(app);

    // --- Controle ---
    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
    int pad = scePadOpen(userId, 0, 0, NULL);

    // --- Video: 2 framebuffers lineares 1920x1080 ---
    const int W = 1920, H = 1080;
    int vh = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, NULL);
    if (vh < 0) return 1;
    const size_t bufSize = 0x800000;       // 8 MB cada (>= 1920*1080*4)
    const size_t total = bufSize * 2;
    off_t phys = 0;
    void *mem = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &phys) < 0) return 2;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, 0x200000) < 0) return 3;
    void *bufs[2] = {mem, (char *)mem + bufSize};

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    if (sceVideoOutRegisterBuffers(vh, 0, bufs, 2, &attr) < 0) return 4;

    // --- Loop: so redesenha quando algo muda ---
    uint32_t prev = 0;
    int heldFrames = 0;
    bool dirty = true;
    int back = 0;
    for (;;) {
        OrbisPadData d;
        uint32_t now = 0;
        if (pad >= 0 && scePadReadState(pad, &d) == 0) now = d.buttons;
        if (now & B_OPTIONS) break;

        uint32_t pressed = now & ~prev;
        // repeticao ao segurar cima/baixo
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
        prev = now;

        if (dirty) {
            FbRenderer r(bufs[back], W, H);
            ui.render(r);
            sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
            back ^= 1;
            dirty = false;
        }
        sceKernelUsleep(16000);
    }
    return 0;
}
