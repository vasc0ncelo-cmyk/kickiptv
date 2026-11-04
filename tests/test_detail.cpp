// Pagina de detalhes: dados do nome, navegacao (serie e filme), texto proporcional e desenho sem sair do buffer.
#include "../src/fbrender.hpp"
#include <cassert>
#include <iostream>
struct Fake : Platform {
    std::string played; std::vector<std::string> epUrls; int epIdx = -1;
    bool httpGet(const std::string &, std::string &) override { return false; }
    void play(const std::string &u, const std::string &) override { played = u; }
    void setEpisodeList(const std::vector<std::string> &u, const std::vector<std::string> &, int i) override { epUrls = u; epIdx = i; }
    void requestText(const std::string &) override {}
};
int main() {
    // --- nome -> titulo / ano / qualidade ---
    { std::string y, q;
      assert(detailCleanTitle("Stranger Things (2016) [HD]", &y, &q) == "Stranger Things" && y == "2016" && q == "HD");
      assert(detailCleanTitle("Blade Runner 2049", &y, &q) == "Blade Runner 2049" && y.empty());      // ano solto faz parte do titulo
      assert(detailCleanTitle("Filme Legal 4K", &y, &q) == "Filme Legal" && q == "4K");
      assert(detailCleanTitle("(2020)", &y, &q) == "(2020)");                                          // nunca fica vazio
      assert(detailMinutes(44) == "44min" && detailMinutes(130) == "2h 10min" && detailMinutes(120) == "2h" && detailMinutes(0).empty());
      assert(detailEpisodeLabel("Dark", "Dark S01E02 O Segredo", 2, 1) == "02. O Segredo");
      assert(detailEpisodeLabel("Dark", "Dark S01E10", 10, 1) == "10. Epis\xC3\xB3" "dio 10"); }
    // --- texto proporcional ---
    { assert(sansWidth("", 20) == 0 && sansWidth("WWWW", 20, SansBold) > sansWidth("iiii", 20, SansBold) * 2);   // proporcional
      assert(sansWidth("Stranger", 40, SansBold) > sansWidth("Stranger", 40, SansRegular));                    // negrito e mais largo
      std::string f = sansFit("Um titulo muito comprido para caber", 20, SansRegular, 150);
      assert(sansWidth(f, 20, SansRegular) <= 150 && f.size() > 3 && f.compare(f.size() - 3, 3, "\xE2\x80\xA6") == 0);
      auto ls = sansWrap("um dois tres quatro cinco seis sete oito nove dez onze doze", 20, SansRegular, 200, 2);
      assert(ls.size() == 2 && sansWidth(ls[0], 20) <= 200 && sansWidth(ls[1], 20) <= 200 && ls[1].compare(ls[1].size() - 3, 3, "\xE2\x80\xA6") == 0);
      assert(sansIndex(0xE7) >= 0 && sansIndex(0x2026) >= 0 && sansIndex(0x4E2D) < 0); }
    // --- navegacao ---
    std::string m3u = "#EXTM3U\n";
    for (int s = 1; s <= 2; s++) for (int e = 1; e <= 7; e++) {
        m3u += "#EXTINF:-1 tvg-logo=\"http://x/l" + std::to_string(s * 10 + e) + ".jpg\" group-title=\"S\xC3\x89RIES | Drama\",Dark S0" + std::to_string(s) + "E0" + std::to_string(e) + " Ep" + std::to_string(e) + "\nhttp://h/series/" + std::to_string(s * 10 + e) + ".mp4\n";
    }
    m3u += "#EXTINF:-1 group-title=\"FILMES | A\xC3\xA7\xC3\xA3o\",Duro de Matar (1988) [HD]\nhttp://h/movie/1.mp4\n#EXTINF:-1 group-title=\"FILMES | A\xC3\xA7\xC3\xA3o\",Sem Link\n\n";
    Fake f; App app(f); app.list = buildPlaylist(m3u); UI ui(app);
    assert(app.list.seriesTree.size() == 1 && app.list.movies.size() >= 1);
    DetailMeta given; given.rating = "9.0";
    int lookups = 0; ui.metaLookup = [&](const std::string &t, bool series, DetailMeta &m) { lookups++; assert(t == "Dark" && series); m.rating = given.rating; return true; };
    ui.openSeriesDetail("Dark", "Drama");
    assert(ui.screen == Screen::Detail && ui.detailSeries && ui.detailTitle == "Dark" && ui.detailSeasons.size() == 2 && lookups == 1);
    assert(ui.detailMeta.rating == "9.0" && ui.detailMeta.genres.size() == 1 && ui.detailMeta.genres[0] == "Drama");
    assert(ui.detailFocus == 2 && ui.detailEpCur == 0);                                   // abre como na imagem: foco no 1o episodio
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/11.mp4" && f.epIdx == 0 && f.epUrls.size() == 7);
    for (int i = 0; i < 4; i++) ui.handle(Input::Right);
    assert(ui.detailEpCur == 4 && ui.detailEpScroll == 0);
    ui.handle(Input::Right); assert(ui.detailEpCur == 5 && ui.detailEpScroll == 1);     // rola o carrossel (5 visiveis)
    ui.handle(Input::Right); ui.handle(Input::Right); assert(ui.detailEpCur == 6 && ui.detailEpScroll == 2);   // para no ultimo
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/17.mp4" && f.epIdx == 6);
    ui.handle(Input::Up); assert(ui.detailFocus == 1);
    ui.handle(Input::Right); assert(ui.detailSeasonIdx == 1 && ui.detailEpCur == 0 && ui.detailEpScroll == 0);   // temporada 2 volta ao 1o episodio
    ui.handle(Input::Right); assert(ui.detailSeasonIdx == 1);                              // sem dar a volta
    ui.handle(Input::Down); ui.handle(Input::Confirm); assert(f.played == "http://h/series/21.mp4");
    ui.handle(Input::PrevTab); assert(ui.detailSeasonIdx == 0);                            // L1 troca de temporada de qualquer foco
    ui.handle(Input::Up); ui.handle(Input::Up); assert(ui.detailFocus == 0);
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/11.mp4");               // "Assistir Agora" = 1o episodio da temporada
    ui.handle(Input::Back); assert(ui.screen == Screen::List);
    // filme
    f.played.clear(); ui.metaLookup = nullptr;
    ui.openMovieDetail(app.list.movies[0], "A\xC3\xA7\xC3\xA3o");
    assert(ui.screen == Screen::Detail && !ui.detailSeries && ui.detailTitle == "Duro de Matar" && ui.detailMeta.year == "1988" && ui.detailMeta.quality == "HD");
    ui.handle(Input::Down); ui.handle(Input::Right); assert(f.played.empty());              // nada alem de Assistir
    ui.handle(Input::Confirm); assert(f.played == "http://h/movie/1.mp4");
    ui.handle(Input::Back); assert(ui.screen == Screen::List);
    if (app.list.movies.size() > 1) { ui.openMovieDetail(app.list.movies[1], ""); f.played.clear(); ui.handle(Input::Confirm); assert(f.played.empty()); }   // sem link: nao toca
    // --- desenho: nao escreve fora do buffer, em 1920x1080 e em outras larguras ---
    for (int W : {1920, 1672, 1280}) {
        int H = W * 9 / 16;
        std::vector<uint32_t> g((size_t)W * H + 8192, 0xAABBCCDDu);
        FbRenderer r(g.data(), W, H);
        ui.timeText = "08:49"; ui.dateText = "22 Out 2026";
        ui.openSeriesDetail("Dark", "Drama"); ui.render(r);
        ui.openMovieDetail(app.list.movies[0], ""); ui.render(r);
        for (size_t i = (size_t)W * H; i < g.size(); i++) assert(g[i] == 0xAABBCCDDu);
    }
    // --- capa do FILME: a pagina de detalhes tem que PEDIR o poster (antes so series pediam; filme ficava sem capa) ---
    {
        std::string m2 = "#EXTM3U\n#EXTINF:-1 tvg-logo=\"http://img/poster_filme.jpg\" group-title=\"FILMES | A\xC3\xA7\xC3\xA3o\",Matrix (1999)\nhttp://h/movie/9.mp4\n";
        Fake f2; App app2(f2); app2.list = buildPlaylist(m2); UI ui2(app2);
        std::vector<std::string> gotFront, gotBack; int calls = 0;
        ui2.coverBatch = [&](const std::vector<std::string> &fr, const std::vector<std::string> &bk) { calls++; gotFront = fr; gotBack = bk; };
        assert(app2.list.movies.size() == 1 && app2.list.movies[0].logo == "http://img/poster_filme.jpg");
        ui2.openMovieDetail(app2.list.movies[0], "");
        const int W = 1920, H = 1080;
        std::vector<uint32_t> g((size_t)W * H, 0);
        FbRenderer r(g.data(), W, H);
        ui2.render(r);
        assert(calls == 1 && gotFront.size() == 1 && gotBack.empty());
        std::string url; int cw = 0, ch = 0;
        assert(coverSplitVariant(gotFront[0], url, cw, ch) && url == "http://img/poster_filme.jpg" && cw > 100 && ch > 100);   // o mesmo tamanho que o desenho usa
        ui2.render(r); assert(calls == 1);                                      // mesmo quadro: nao repete o pedido
        // desenho: se a capa estiver pronta no cache, ela e usada (nao aparece o texto "FILME")
        int gets = 0; std::string askedKey;
        ui2.coverGet = [&](const std::string &k, const uint32_t *&px, int &w, int &h) {
            gets++; askedKey = k; static std::vector<uint32_t> pix(8 * 8, 0xFF336699u); px = pix.data(); w = 8; h = 8; return true; };
        ui2.render(r);
        assert(gets >= 1 && askedKey == gotFront[0]);                           // desenho e pedido usam a MESMA chave
    }
    // --- ao voltar, as capas da lista voltam a ser pedidas ---
    { ui.openMovieDetail(app.list.movies[0], ""); ui.lastBatchKey = "x"; ui.handle(Input::Back); assert(ui.lastBatchKey.empty()); }
    std::cout << "OK\n";
}
