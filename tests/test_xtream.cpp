// Painel Xtream: categorias na conexao, itens so quando a categoria abre.
#include "../src/ui.hpp"
#include <cassert>
#include <iostream>

struct Rec : Renderer {
    std::vector<std::string> texts;
    int width() const override { return 1920; }
    int height() const override { return 1080; }
    void fillRect(int, int, int, int, unsigned) override {}
    void drawText(int, int, const std::string &t, unsigned, int) override { texts.push_back(t); }
    bool has(const std::string &s) { for (auto &t : texts) if (t == s) return true; return false; }
};

struct XtFake : Platform {
    std::vector<std::string> calls; bool authOk = true, api = true; int bigItems = 0;
    std::string played; std::map<std::string, std::string> store;
    int count(const std::string &needle) const { int n = 0; for (auto &c : calls) if (c.find(needle) != std::string::npos) n++; return n; }
    bool httpGet(const std::string &url, std::string &body) override {
        calls.push_back(url);
        if (url.find("player_api.php") == std::string::npos) { body = "#EXTM3U\n#EXTINF:-1 group-title=\"Abertos\",Globo\nhttp://h/live/1.ts\n"; return true; }
        if (!api) { body = "<html>404</html>"; return true; }
        if (url.find("action=") == std::string::npos) { body = authOk ? "{\"user_info\":{\"auth\":1,\"status\":\"Active\"},\"server_info\":{}}" : "{\"user_info\":{\"auth\":0}}"; return true; }
        if (url.find("get_live_categories") != std::string::npos) { body = "[{\"category_id\":\"1\",\"category_name\":\"CANAIS | Abertos\"},{\"category_id\":\"2\",\"category_name\":\"Esportes\"}]"; return true; }
        if (url.find("get_vod_categories") != std::string::npos) { body = "[{\"category_id\":\"10\",\"category_name\":\"FILMES | A\\u00e7\\u00e3o\"},{\"category_id\":\"11\",\"category_name\":\"Drama\"},{\"category_id\":\"12\",\"category_name\":\"Enorme\"}]"; return true; }
        if (url.find("get_series_categories") != std::string::npos) { body = "[{\"category_id\":\"20\",\"category_name\":\"Drama\"}]"; return true; }
        if (url.find("get_live_streams&category_id=2") != std::string::npos) { body = "[{\"name\":\"SporTV\",\"stream_id\":501,\"stream_icon\":\"http://i/s.png\"},{\"name\":\"ESPN\",\"stream_id\":\"502\"}]"; return true; }
        if (url.find("get_vod_streams&category_id=10") != std::string::npos) { body = "[{\"name\":\"Duro de Matar\",\"stream_id\":7,\"container_extension\":\"mkv\",\"stream_icon\":\"http://i/dm.jpg\"},{\"name\":\"Sem ext\",\"stream_id\":8}]"; return true; }
        if (url.find("get_vod_streams&category_id=11") != std::string::npos) { body = "[{\"name\":\"Titanic\",\"stream_id\":9,\"container_extension\":\"mp4\"}]"; return true; }
        if (url.find("get_vod_streams&category_id=12") != std::string::npos) {
            body = "[";
            for (int i = 0; i < bigItems; i++) { if (i) body += ","; body += "{\"name\":\"Filme " + std::to_string(i) + "\",\"stream_id\":" + std::to_string(100000 + i) + ",\"container_extension\":\"mp4\"}"; }
            body += "]"; return true;
        }
        if (url.find("get_series&category_id=20") != std::string::npos) { body = "[{\"name\":\"Zeta\",\"series_id\":98,\"cover\":\"http://i/z.jpg\"},{\"name\":\"4Ever\",\"series_id\":99,\"cover\":\"http://i/4.jpg\"},{\"name\":\"Vazia\",\"series_id\":97}]"; return true; }
        if (url.find("get_vod_info&vod_id=7") != std::string::npos) { body = "{\"info\":{\"plot\":\"Um policial salva o predio.\",\"genre\":\"Acao, Suspense\",\"rating\":\"7.9\",\"releasedate\":\"1988-07-15\",\"duration_secs\":7920},\"movie_data\":{}}"; return true; }
        if (url.find("get_series_info&series_id=99") != std::string::npos) {
            body = "{\"info\":{\"cover\":\"http://i/4.jpg\",\"plot\":\"Sinopse da serie\",\"genre\":\"Drama\",\"rating\":\"8.4\",\"episode_run_time\":\"45\"},\"episodes\":{\"2\":[{\"id\":\"302\",\"episode_num\":1,\"title\":\"X\",\"container_extension\":\"mp4\"}],"
                   "\"1\":[{\"id\":\"202\",\"episode_num\":2,\"title\":\"B\",\"container_extension\":\"mp4\"},{\"id\":\"201\",\"episode_num\":1,\"title\":\"A\",\"container_extension\":\"mp4\",\"info\":{\"movie_image\":\"http://i/e1.jpg\",\"duration_secs\":2700}}]}}";
            return true;
        }
        if (url.find("get_series_info&series_id=97") != std::string::npos) { body = "{\"info\":{},\"episodes\":[]}"; return true; }
        body = "[]"; return true;
    }
    void play(const std::string &url, const std::string &) override { played = url; }
    void requestText(const std::string &) override {}
    std::string loadSetting(const std::string &k) override { return store[k]; }
    void saveSetting(const std::string &k, const std::string &v) override { store[k] = v; }
};

int main() {
    const std::string url = "http://180381.sbs/get.php?username=Dirce0273&password=Sar5438&type=m3u_plus&output=ts";
    // --- conexao: so categorias (nao baixa nenhuma lista M3U gigante nem nenhum item) ---
    XtFake x; App a(x); UI ui(a);
    assert(a.loadM3U(url) && a.xt.active && a.list.xtream && !a.list.empty());
    assert(a.lastStatus == "Painel IPTV: 2 categorias de canais | 3 de filmes | 1 de series");
    assert(x.calls.size() == 4 && x.calls[0] == "http://180381.sbs/player_api.php?username=Dirce0273&password=Sar5438");
    assert(x.count("get.php") == 0 && x.count("_streams") == 0 && x.count("get_series&") == 0);
    assert(a.list.movieCats[0].name == "A\xC3\xA7\xC3\xA3o" && a.list.channelCats[0].name == "Abertos" && a.list.movieCats[0].unloaded());
    assert(a.list.channels.empty() && a.list.movies.empty());

    // --- UI: abrir categoria de FILMES baixa so aquela ---
    ui.screen = Screen::List; ui.tab = Tab::Movies;
    { Rec r; ui.render(r); assert(r.has("Aperte X para abrir a categoria")); }
    ui.handle(Input::Confirm);                                                       // categoria "Acao"
    assert(ui.inItems && ui.itemCount() == 2 && ui.itemLabel(0) == "Duro de Matar" && x.count("get_vod_streams") == 1);
    assert(a.list.movies[0].url == "http://180381.sbs/movie/Dirce0273/Sar5438/7.mkv" && a.list.movies[0].logo == "http://i/dm.jpg");
    assert(a.list.movies[1].url == "http://180381.sbs/movie/Dirce0273/Sar5438/8.mp4");
    ui.handle(Input::Confirm);                                                       // abre a pagina de detalhes do filme
    assert(ui.screen == Screen::Detail && ui.detailUrl == "http://180381.sbs/movie/Dirce0273/Sar5438/7.mkv" && x.count("get_vod_info&vod_id=7") == 1);
    assert(ui.detailMeta.plot == "Um policial salva o predio." && ui.detailMeta.rating == "7.9" && ui.detailMeta.runtimeMin == 132 && ui.detailMeta.year == "1988");
    assert(ui.detailMeta.genres.size() == 2 && ui.detailMeta.genres[0] == "Acao" && ui.detailMeta.genres[1] == "Suspense");
    ui.handle(Input::Back); assert(ui.screen == Screen::List);
    ui.handle(Input::Confirm); assert(x.count("get_vod_info") == 1);                // reabrir: metadados em cache
    ui.handle(Input::Back); ui.screen = Screen::List; ui.inItems = true;
    ui.handle(Input::Back); assert(!ui.inItems);                                     // volta para as categorias
    ui.handle(Input::Confirm); assert(x.count("get_vod_streams") == 1);             // reabrir: nao baixa de novo
    ui.handle(Input::Back); ui.handle(Input::Down); ui.handle(Input::Confirm);       // categoria "Drama"
    assert(ui.itemCount() == 1 && ui.itemLabel(0) == "Titanic" && x.count("get_vod_streams") == 2);
    { Rec r; ui.render(r); assert(r.has("Titanic")); }

    // --- CANAIS ---
    ui.tab = Tab::Channels; ui.inItems = false; ui.catCur[1] = 1; ui.handle(Input::Confirm);
    assert(ui.itemCount() == 2 && a.list.channels[0].url == "http://180381.sbs/live/Dirce0273/Sar5438/501.ts" && a.list.channels[1].url.find("/502.ts") != std::string::npos);
    ui.handle(Input::Confirm); assert(x.played.find("/live/") != std::string::npos);

    // --- SERIES: serie -> (baixa episodios so agora) -> temporadas -> episodios ordenados ---
    ui.tab = Tab::Series; ui.inItems = false; ui.level = 0; ui.catCur[3] = 0;
    ui.handle(Input::Confirm);                                                       // categoria "Drama"
    assert(ui.itemCount() == 3 && ui.itemLabel(0) == "4Ever" && ui.itemLabel(2) == "Zeta" && x.count("get_series_info") == 0);   // ordenadas
    assert(ui.itemLogo(0) == "http://i/4.jpg");                                      // capa vem da lista (antes de abrir a serie)
    ui.handle(Input::Confirm);                                                       // abre "4Ever": baixa os episodios e mostra os detalhes
    assert(x.count("get_series_info") == 1 && ui.screen == Screen::Detail && ui.detailSeasons.size() == 2);
    assert(ui.detailMeta.plot == "Sinopse da serie" && ui.detailMeta.rating == "8.4" && ui.detailMeta.runtimeMin == 45 && ui.detailMeta.genres[0] == "Drama");
    { auto eps = ui.detailEpisodes(); assert(eps.size() == 2 && eps[0].episode == 1 && eps[0].mins == 45 && eps[0].logo == "http://i/e1.jpg" && eps[1].logo == "http://i/4.jpg" && eps[1].name == "B"); }
    ui.handle(Input::Confirm); assert(x.played == "http://180381.sbs/series/Dirce0273/Sar5438/201.mp4");   // foco no 1o episodio
    ui.handle(Input::Back); assert(ui.screen == Screen::List);
    ui.handle(Input::Confirm); assert(x.count("get_series_info") == 1);              // mesma serie: nao baixa de novo
    ui.handle(Input::Back); ui.screen = Screen::List; ui.inItems = true;
    ui.handle(Input::Right); ui.handle(Input::Confirm);                               // "Vazia" (a grade anda com as setas): sem episodios -> fica na lista, avisa
    assert(ui.level == 0 && a.lastStatus.find("sem episodios") != std::string::npos);

    // --- memoria: passou de ~100 mil itens carregados -> libera as categorias antigas ao abrir outra ---
    { XtFake y; y.bigItems = 105000; App b(y); UI u(b); assert(b.loadM3U(url));
      u.screen = Screen::List; u.tab = Tab::Movies; u.catCur[2] = 2; u.handle(Input::Confirm);          // "Enorme": 105 mil filmes
      assert(u.itemCount() == 105000 && b.list.movies.size() == 105000);
      u.handle(Input::Back); u.catCur[2] = 0; u.handle(Input::Confirm);                                  // abre outra categoria
      assert(b.list.movies.size() == 2 && u.itemLabel(0) == "Duro de Matar" && b.list.movieCats[2].unloaded());   // a enorme foi descartada
      u.handle(Input::Back); u.catCur[2] = 2; u.handle(Input::Confirm); assert(u.itemCount() == 105000);     // e volta a carregar ao reabrir
    }

    // --- painel recusa o login: avisa, nao baixa a lista gigante ---
    { XtFake y; y.authOk = false; App b(y); assert(!b.loadM3U(url) && b.lastStatus.find("recusou") != std::string::npos && !b.list.xtream && y.count("get.php") == 0); }
    // --- painel sem API / link M3U comum: usa a lista M3U ---
    { XtFake y; y.api = false; App b(y); assert(b.loadM3U(url) && !b.xt.active && b.list.channels.size() == 1); }
    { XtFake y; App b(y); assert(b.loadM3U("http://h/lista.m3u") && !b.xt.active && b.list.channels.size() == 1 && y.count("player_api") == 0); }
    // --- remover a lista tambem limpa o modo Xtream ---
    ui.setM3U(""); assert(!a.xt.active && a.list.empty());

    // --- conexao em segundo plano (o app abre e responde na hora) ---
    {
        XtFake y; XtLoader ld;
        ld.start(url, [&](const std::string &u, std::string &b) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); return y.httpGet(u, b); });
        XtLoader::Result r; int spins = 0;
        while (!ld.take(r)) { assert(spins++ < 2000); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        assert(r.code == 1 && r.list && r.list->xtream && r.list->movieCats.size() == 3 && r.acct.user == "Dirce0273");
        assert(!ld.take(r));                                                                             // so entrega uma vez
        App b(y); b.useXtream(r.acct, std::move(*r.list)); assert(b.xt.active && b.ensureCategory(Tab::Movies, 1) && b.list.movies.size() == 1);
        XtFake z; z.authOk = false; ld.start(url, [&](const std::string &u, std::string &b) { return z.httpGet(u, b); });
        while (!ld.take(r)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(r.code == 2 && !r.msg.empty());
        ld.start("http://h/lista.m3u", [&](const std::string &u, std::string &b) { return y.httpGet(u, b); });
        while (!ld.take(r)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        assert(r.code == 0);
    }
    // --- lista M3U comum com usuario/senha e links /movie/: sinopse tambem funciona (id vem da URL) ---
    { XtFake y; y.api = false; App b(y); assert(b.loadM3U(url) && !b.xt.active);
      y.api = true;
      Entry e; e.url = "http://180381.sbs/movie/Dirce0273/Sar5438/7.mkv"; DetailMeta m;
      assert(b.movieMeta(e, m) && m.plot == "Um policial salva o predio." && m.runtimeMin == 132);
      Entry f; f.url = "http://h/qualquer.mp4"; DetailMeta m2; assert(!b.movieMeta(f, m2) && m2.plot.empty()); }
    assert(xtMovieIdFromUrl("http://h/movie/u/p/123.mp4") == "123" && xtMovieIdFromUrl("http://h/live/u/p/123.ts").empty());
    { Json j; jsonParse("{\"duration\":\"01:57:00\",\"rating_5based\":3.5}", j); DetailMeta m; xtFillMeta(j, m); assert(m.runtimeMin == 117 && m.rating == "7.0"); }
    std::cout << "OK\n";
}
