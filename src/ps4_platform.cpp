// TESTE 1 - tela de cor solida + leitura do controle (OpenOrbis).
// Objetivo: provar que o PKG instala, abre e fica na tela sem CE-34878-0.
//   X = troca a cor | O = sai do app
// ATENCAO: escrito sem acesso ao SDK; se o build reclamar de algum nome,
// confira em $OO_PS4_TOOLCHAIN/include/orbis/ (VideoOut.h, Pad.h, UserService.h).
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <stdint.h>
#include "app.hpp"

// Camada de plataforma (ainda vazia; entra nos testes 2 e 3).
struct Ps4Platform : Platform {
    bool httpGet(const std::string &, std::string &) override { return false; }
    void play(const std::string &, const std::string &) override {}
    void requestText(const std::string &) override {}
};

static const int W = 1920, H = 1080;
static const uint32_t COLORS[] = {0xFF0000FF /*azul*/, 0xFF00AA00 /*verde*/, 0xFFCC0000 /*vermelho*/, 0xFF000000 /*preto*/};
// formato B8G8R8A8: 0xAARRGGBB na memoria little-endian

int main() {
    Ps4Platform pf;
    App app(pf);
    (void)app;

    // --- Controle ---
    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
    int pad = scePadOpen(userId, 0, 0, NULL);

    // --- Video: 1 framebuffer linear 1920x1080 ---
    int vh = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, NULL);
    if (vh < 0) return 1;

    const size_t fbSize = 0x800000; // 8 MB (>= 1920*1080*4), multiplo de 2 MB
    off_t phys = 0;
    void *fb = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), fbSize, 0x200000, 3, &phys) < 0) return 2;
    if (sceKernelMapDirectMemory(&fb, fbSize, 0x33 /*CPU+GPU RW*/, 0, phys, 0x200000) < 0) return 3;

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000,
                                  ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    void *addrs[1] = {fb};
    if (sceVideoOutRegisterBuffers(vh, 0, addrs, 1, &attr) < 0) return 4;

    int ci = 0;
    bool prevX = false;
    for (;;) {
        uint32_t *px = (uint32_t *)fb;
        uint32_t col = COLORS[ci];
        for (int i = 0; i < W * H; i++) px[i] = col;
        sceVideoOutSubmitFlip(vh, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);

        OrbisPadData d;
        if (pad >= 0 && scePadReadState(pad, &d) == 0) {
            bool x = d.buttons & ORBIS_PAD_BUTTON_CROSS;
            if (x && !prevX) ci = (ci + 1) % 4;
            prevX = x;
            if (d.buttons & ORBIS_PAD_BUTTON_CIRCLE) break;
        }
        sceKernelUsleep(33000);
    }
    return 0;
}
