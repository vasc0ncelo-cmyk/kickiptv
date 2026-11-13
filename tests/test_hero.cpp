// Inicio: backdrop do painel Xtream e tamanho da imagem pedida ao TMDB.
#include "../src/ui.hpp"
#include <cassert>
#include <iostream>
#include <map>

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
    // banner: so filmes RECENTES (ano no nome), passa sozinho a cada 2 s e o detalhe volta para o Inicio
    {
        HFake f; App a(f); UI ui(a);
        std::string m = "#EXTM3U\n";
        for (int i = 0; i < 6; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/o" + std::to_string(i) + ".jpg\" group-title=\"Filmes\",Antigo " + std::to_string(i) + " (200" + std::to_string(i) + ")\nhttp://h/movie/u/p/" + std::to_string(i) + ".mp4\n";
        for (int i = 0; i < 6; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/n" + std::to_string(i) + ".jpg\" group-title=\"Filmes\",Novo " + std::to_string(i) + " (2026)\nhttp://h/movie/u/p/1" + std::to_string(i) + ".mp4\n";
        a.list = buildPlaylist(m);
        ui.tick(1.0); { struct R : Renderer { int width() const override { return 1920; } int height() const override { return 1080; } void fillRect(int, int, int, int, unsigned) override {} void drawText(int, int, const std::string &, unsigned, int) override {} } r; ui.render(r); }
        assert(ui.hero.size() == 5);
        for (const auto &h : ui.hero) assert(h.kind == 1 && h.title.find("Novo") == 0);    // nenhum filme antigo
        ui.tick(1.04); int c0 = ui.heroCur; ui.tick(3.2); assert(ui.heroCur != c0);       // passou sozinho (2 s)
        ui.homeRow = 0; ui.handle(Input::Confirm); assert(ui.screen == Screen::Detail);
        ui.handle(Input::Back); assert(ui.screen == Screen::Home);                         // volta para o Inicio, nao para a aba Filmes
        ui.tick(4.0); ui.tick(4.04); assert(ui.hero.size() == 5);                          // novo sorteio ao voltar ao Inicio
        ui.homeRow = 1; ui.homeCol = 1; ui.handle(Input::Confirm); assert(ui.screen == Screen::List);
    }
    std::cout << "hero ok\n";
}
