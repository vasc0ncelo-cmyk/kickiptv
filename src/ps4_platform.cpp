// CAMADA PS4 (OpenOrbis) - PRECISA SER COMPLETADA E TESTADA NO CONSOLE.
// Nada aqui foi compilado contra o SDK. Confira os headers/assinaturas no seu OpenOrbis.
//
// httpGet : libSceNet + libSceSsl + libSceHttp (sceHttpCreateTemplate, sceHttpCreateConnectionWithURL,
//           sceHttpCreateRequestWithURL, sceHttpSendRequest, sceHttpReadData). Exige sceSslInit/sceHttpInit.
// play    : libSceAvPlayer (sceAvPlayerInit, sceAvPlayerAddSource, sceAvPlayerStart, sceAvPlayerGetVideoData)
//           e renderizar os frames via libSceVideoOut/GNM ou um framebuffer simples.
// input   : libScePad (scePadReadState) para D-pad/X/O e teclado na tela via libSceImeDialog.
// UI      : src/ui.hpp ja faz navegacao e layout. Aqui so falta: (1) Renderer com fillRect/drawText
//           sobre o framebuffer (fonte: ex. stb_truetype com uma TTF do sistema), (2) mapear o pad:
//           D-pad->Up/Down, L1/R1->PrevTab/NextTab, X->Confirm, O->Back, L2/R2->PageUp/PageDown.
#include "ui.hpp"

struct Ps4Platform : Platform {
    bool httpGet(const std::string &, std::string &) override { /* TODO */ return false; }
    void play(const std::string &, const std::string &) override { /* TODO */ }
    void requestText(const std::string &) override { /* TODO: libSceImeDialog; ao confirmar chamar ui.onText(texto) */ }
};

int main() {
    Ps4Platform pf;
    App app(pf);
    // TODO: init de modulos, ler a URL da M3U de /data/kickiptv/config.txt,
    // app.loadM3U(url); depois loop de input/render chamando app.watchKick(texto_do_IME)
    // ou pf.play(entry.url) ao escolher um item.
    return 0;
}
