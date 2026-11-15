// Inicio: backdrop do painel Xtream e tamanho da imagem pedida ao TMDB.
#include "../src/ui.hpp"
#include <cassert>
#include <iostream>
#include <map>
#include <thread>

struct Rec : Renderer {
    int width() const override { return 1920; }
    int height() const override { return 1080; }
    void fillRect(int, int, int, int, unsigned) override {}
    void drawText(int, int, const std::string &, unsigned, int) override {}
};
struct TmdbFake : Platform {
    bool httpGet(const std::string &url, std::string &body) override {
        if (url.find("discover/movie") != std::string::npos && url.find("page=1") != std::string::npos) {
            body = "{\"results\":[";
            const char *t[] = {"Filme Um", "Filme Dois", "Filme Tres", "Filme Quatro", "Filme Cinco", "Filme Seis", "Filme Sete", "Duro de Matar", "Sem Fundo"};
            for (int i = 0; i < 9; i++) {
                if (i) body += ",";
                body += "{\"id\":" + std::to_string(100 + i) + ",\"title\":\"" + t[i] + "\",\"release_date\":\"" + (i == 7 ? "1988-07-15" : "2026-03-01") + "\",\"backdrop_path\":" + (i == 8 ? std::string("null") : "\"/b" + std::to_string(i) + ".jpg\"") + ",\"overview\":\"Sinopse " + std::to_string(i) + "\"}";
            }
            body += "]}"; return true;
        }
        if (url.find("/images?") != std::string::npos) { body = "{\"logos\":[{\"file_path\":\"/l1.png\",\"iso_639_1\":\"en\",\"vote_average\":5},{\"file_path\":\"/l2.png\",\"iso_639_1\":\"pt\",\"vote_average\":1},{\"file_path\":\"/l3.svg\",\"iso_639_1\":\"pt\",\"vote_average\":9}]}"; return true; }
        body = "{\"results\":[]}"; return url.find("discover") != std::string::npos;
    }
    void play(const std::string &, const std::string &) override {}
    void requestText(const std::string &) override {}
};
struct HFake : Platform {
    std::map<std::string, std::string> st;
    bool httpGet(const std::string &, std::string &b) override { b = ""; return true; }
    void play(const std::string &, const std::string &) override {}
    void requestText(const std::string &) override {}
    std::string loadSetting(const std::string &k) override { return st[k]; }
    void saveSetting(const std::string &k, const std::string &v) override { st[k] = v; }
};
int main() {
    { Json j; jsonParse("{\"backdrop_path\":[\"http://i/b1.jpg\",\"http://i/b2.jpg\"]}", j); assert(xtBackdrop(j) == "http://i/b1.jpg"); }
    { Json j; jsonParse("{\"backdrop_path\":\"https://i/b.jpg\"}", j); assert(xtBackdrop(j) == "https://i/b.jpg"); }
    { Json j; jsonParse("{\"backdrop_path\":[\"\",\"nao-url\"]}", j); assert(xtBackdrop(j).empty()); }
    { Json j; jsonParse("{\"plot\":\"x\",\"backdrop_path\":[\"http://i/b1.jpg\"]}", j); DetailMeta m; xtFillMeta(j, m); assert(m.backdrop == "http://i/b1.jpg" && m.plot == "x"); }
    std::string u = "https://image.tmdb.org/t/p/original/abc.jpg";
    assert(coverFetchUrl(coverVariantKey(u, 880, 340)) == "https://image.tmdb.org/t/p/w780/abc.jpg");
    assert(coverFetchUrl(coverVariantKey(u, 264, 288)) == "https://image.tmdb.org/t/p/w500/abc.jpg");
    assert(coverFetchUrl(coverVariantKey("https://image.tmdb.org/t/p/w500/logo.png", kCoverLogoW, kCoverLogoH)) == "https://image.tmdb.org/t/p/w500/logo.png");
    assert(coverFetchUrl(coverVariantKey("https://image.tmdb.org/t/p/w300/logo.png", kCoverLogoW, kCoverLogoH)) == "https://image.tmdb.org/t/p/w500/logo.png");
    assert(homeNorm("Missão: Impossível — Acerto") == "missaoimpossivelacerto" && homeNorm("AÇÃO 2") == "acao2");
    // banner: so filmes recentes e populares do TMDB que existem no catalogo; fixo enquanto carrega; troca ao voltar ao Inicio; passa sozinho a cada 2 s
    {
        TmdbFake f; App app(f); app.heroInfoEnabled = true; app.tmdbKey = "k"; UI ui(app);
        std::string m = "#EXTM3U\n";
        const char *nm[] = {"Filme Um (2026)", "Filme Dois (2026)", "Filme Tres (2026)", "Filme Quatro (2026)", "Filme Cinco (2026)", "Filme Seis (2026)", "Filme Sete [L] (2026)", "Duro de Matar (2013)", "Lixo Qualquer (2026)"};
        for (int i = 0; i < 9; i++) m += std::string("#EXTINF:-1 tvg-logo=\"http://img/p") + std::to_string(i) + ".jpg\" group-title=\"Filmes\"," + nm[i] + "\nhttp://h/movie/u/p/" + std::to_string(i) + ".mp4\n";
        app.list = buildPlaylist(m);
        Rec r; double t = 1.0;
        auto spin = [&](int n) { for (int i = 0; i < n; i++) { t += 0.1; ui.tick(t); ui.render(r); std::this_thread::sleep_for(std::chrono::milliseconds(40)); } };
        for (int i = 0; i < 200 && !(ui.heroLocked_ && ui.hero.size() == 5); i++) spin(1);
        assert(ui.heroLocked_ && ui.hero.size() == 5);
        for (const auto &h : ui.hero) assert(h.kind == 1 && h.title.find("Filme ") == 0 && h.title.find("[L]") == std::string::npos && !h.backdrop.empty() && !h.plot.empty());
        for (int i = 0; i < 60 && !ui.hero[0].logoDone; i++) spin(1);
        assert(ui.hero[0].tlogo == "https://image.tmdb.org/t/p/w500/l2.png");                         // logo: pt vence, .svg ignorado
        std::string first = ui.hero[0].title;
        // catalogo chegando: nada muda
        app.list = buildPlaylist(m + "#EXTINF:-1 tvg-logo=\"http://img/z.jpg\" group-title=\"Filmes\",Mais Um (2026)\nhttp://h/movie/u/p/99.mp4\n");
        spin(40); assert(ui.hero.size() == 5 && ui.hero[0].title == first && ui.hero[0].tlogo.size());
        // banner fica parado
        ui.heroAuto = t; int c0 = ui.heroCur; t += 2.5; ui.tick(t); assert(ui.heroCur == c0);      // troca automatica desligada (sem animacao)
        // sai do Inicio e volta: outros filmes (o conjunto novo e preparado ao sair)
        std::vector<std::string> before; for (const auto &h : ui.hero) before.push_back(h.title);
        ui.homeRow = 1; ui.homeCol = 1; ui.handle(Input::Confirm); assert(ui.screen == Screen::List);
        spin(3); for (int i = 0; i < 80 && ui.heroPend_.empty() == false && !ui.heroPend_.back().logoDone; i++) spin(1);
        ui.handle(Input::Back); spin(3); assert(ui.screen == Screen::Home);
        for (int i = 0; i < 80 && ui.hero[0].title == before[0]; i++) spin(1);
        assert(ui.hero.size() == 5 && ui.hero[0].title != before[0]);
        for (const auto &h : ui.hero) assert(h.title.find("Filme ") == 0 || h.title.find("Mais") == 0);
        app.stopHeroInfo(1.0);
    }
    std::cout << "hero ok\n";
}
