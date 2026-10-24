// Capas: decodificacao (poster preenche / logo fica inteiro) e fila por prioridade com varios downloads.
#include "../src/imgcache.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

static const unsigned char kPoster[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,60,0,0,0,90,8,2,0,0,0,107,51,162,98,0,0,1,156,73,68,65,84,120,218,213,194,7,139,8,0,0,134,225,55,73,146,36,73,146,36,73,111,146,36,73,146,164,75,146,179,247,62,238,204,187,179,157,125,103,239,189,247,222,155,36,73,146,36,73,146,36,73,146,36,93,146,36,73,23,191,227,123,122,248,39,113,169,177,86,92,254,90,59,46,127,172,19,151,223,214,141,203,47,235,197,229,167,245,227,242,195,6,113,249,110,195,184,124,179,81,92,170,109,28,151,175,54,137,203,23,155,198,229,179,205,226,242,201,230,113,249,104,139,184,124,176,101,92,222,219,42,46,239,108,29,151,183,182,137,203,27,141,203,107,219,198,229,149,237,226,242,210,246,113,121,97,135,184,60,183,99,92,158,217,41,46,79,237,28,151,39,118,137,203,99,187,198,229,145,221,226,242,208,238,113,121,96,143,184,220,183,103,92,238,89,16,151,187,246,138,203,29,123,199,229,182,125,226,114,203,190,113,185,97,191,184,92,115,64,92,174,56,40,46,151,28,18,151,11,14,139,203,57,71,196,229,140,163,226,114,202,49,113,57,225,184,184,28,115,66,92,142,56,49,46,135,44,142,203,1,39,199,101,159,83,227,178,199,233,113,217,101,105,92,118,88,30,151,109,206,138,203,22,231,196,101,147,243,226,178,193,5,113,89,231,194,184,172,113,113,92,86,185,52,46,43,92,30,151,74,171,226,178,204,149,113,89,226,234,184,44,114,109,92,42,92,31,151,249,110,140,203,92,55,199,101,182,91,227,50,211,237,113,41,115,103,92,102,184,59,46,211,220,27,151,41,238,143,75,137,7,227,50,201,195,113,41,242,104,92,198,123,60,46,99,61,25,151,209,158,142,203,72,207,198,101,184,231,227,50,212,139,113,25,236,229,184,12,244,106,92,250,123,61,46,133,222,140,251,31,122,144,80,17,175,118,3,141,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned char kLogo[] = {137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,40,0,0,0,40,8,6,0,0,0,140,254,184,109,0,0,0,46,73,68,65,84,120,218,237,206,65,17,0,0,8,195,176,249,55,13,34,198,241,74,106,160,9,0,252,152,227,12,26,52,104,208,160,65,131,6,13,26,52,104,16,0,90,11,7,144,29,14,145,241,66,68,0,0,0,0,73,69,78,68,174,66,96,130};

static bool waitFor(const std::function<bool()> &f, int ms = 3000) {
    for (int i = 0; i < ms / 5; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return f();
}

int main() {
    // --- poster 60x90 (2:3): preenche tudo, sem faixas do fundo ---
    {
        Cover c; std::string body((const char *)kPoster, sizeof kPoster);
        assert(imgDecodeToCover(body, c, ImgCache::kThumbW, ImgCache::kThumbH));
        assert(c.ready && c.w == ImgCache::kThumbW && c.h == ImgCache::kThumbH && (int)c.bgra.size() == c.w * c.h);
        uint32_t top = c.bgra[5 * c.w + c.w / 2], bot = c.bgra[(c.h - 6) * c.w + c.w / 2];
        assert((top >> 24) == 0xFF && ((top >> 16) & 255) > 200 && (bot & 255) > 100);           // topo avermelhado, base azulada
        assert(c.bgra[0] != 0x161C16FFu && ((c.bgra[0] >> 24) == 0xFF));
    }
    // --- logo quadrado com transparencia: fica inteiro no meio, com o fundo escuro em volta ---
    {
        Cover c; std::string body((const char *)kLogo, sizeof kLogo);
        assert(imgDecodeToCover(body, c, ImgCache::kThumbW, ImgCache::kThumbH));
        uint32_t mid = c.bgra[(c.h / 2) * c.w + c.w / 2], corner = c.bgra[0];
        assert(((mid >> 8) & 255) > 200 && (mid & 255) < 60);                                   // verde no centro
        assert(corner == 0xFF161C16u);                                                          // fundo escuro nas pontas
    }
    // --- lixo / arquivo curto: falha sem quebrar ---
    { Cover c; assert(!imgDecodeToCover("nao e imagem nenhuma, so texto qualquer aqui", c, 148, 222)); assert(!imgDecodeToCover("x", c, 148, 222)); }

    // --- fila: o que esta visivel baixa primeiro; baixa em paralelo; tudo fica pronto e avisa (version) ---
    {
        ImgCache ic;
        std::mutex m; std::vector<std::string> order; std::atomic<int> cur{0}, peak{0};
        ic.setFetch([&](const std::string &url, std::string &body) {
            int n = ++cur; int p = peak.load(); while (n > p && !peak.compare_exchange_weak(p, n)) {}
            { std::lock_guard<std::mutex> l(m); order.push_back(url); }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            cur--;
            if (url.find("bad") != std::string::npos) return false;
            body.assign((const char *)kPoster, sizeof kPoster);
            return true;
        });
        std::vector<std::string> front, back;
        for (int i = 0; i < 6; i++) front.push_back("http://img/f" + std::to_string(i) + ".png");
        for (int i = 0; i < 6; i++) back.push_back("http://img/b" + std::to_string(i) + ".png");
        back.push_back("http://img/bad.png");
        back.push_back("");                                                                    // url vazia: ignorada
        uint32_t v0 = ic.version();
        ic.setWanted(front, back);
        assert(waitFor([&] { for (auto &u : front) if (!ic.getPtr(u)) return false; return true; }));
        assert(waitFor([&] { for (auto &u : back) if (!u.empty() && u.find("bad") == std::string::npos && !ic.getPtr(u)) return false; return true; }));
        assert(ic.version() > v0);
        assert(peak.load() >= 2);                                                              // mais de um download ao mesmo tempo
        { std::lock_guard<std::mutex> l(m); for (int i = 0; i < 3; i++) assert(order[i].find("/f") != std::string::npos); }   // visiveis na frente
        assert(!ic.getPtr("http://img/bad.png"));                                              // falhou: nao tem capa (a tela mostra a inicial)
        Cover cp; assert(ic.get(front[0], cp) && cp.ready && cp.w == ImgCache::kThumbW);
        // trocar de pagina: o que ja esta pronto continua; so baixa o novo
        size_t before; { std::lock_guard<std::mutex> l(m); before = order.size(); }
        ic.setWanted({front[0], "http://img/novo.png"}, {});
        assert(waitFor([&] { return ic.getPtr("http://img/novo.png") != nullptr; }));
        { std::lock_guard<std::mutex> l(m); assert(order.size() == before + 1); }
        ic.shutdown();
    }
    puts("OK");
    return 0;
}
