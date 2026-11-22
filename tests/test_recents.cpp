// "Assistido recentemente": historico em disco (WatchProgress) + categoria no topo de Filmes/Series + clicar retoma no minuto salvo.
#include "../src/fbrender.hpp"
#include "../src/WatchProgress.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>

struct Fake : Platform {
    std::string played, playedAtUrl; long long playedAtSec = -1;
    WatchRec begun; int begunCount = 0;
    std::vector<std::string> epUrls; int epIdx = -1;
    std::vector<WatchRec> history;
    bool httpGet(const std::string &, std::string &) override { return false; }
    void play(const std::string &u, const std::string &) override { played = u; playedAtSec = 0; }
    void playAt(const std::string &u, const std::string &, long long s) override { playedAtUrl = u; playedAtSec = s; }
    void setEpisodeList(const std::vector<std::string> &u, const std::vector<std::string> &, int i) override { epUrls = u; epIdx = i; }
    void watchBegin(const WatchRec &m) override { begun = m; begunCount++; }
    void watchList(std::vector<WatchRec> &out) override { out = history; }
    void requestText(const std::string &) override {}
};

int main() {
    using kick::WatchProgress; using kick::ProgressMeta;
    // ---------- historico em disco ----------
    const char *path = "/tmp/test_recents_progress.json";
    remove(path);
    {
        WatchProgress &wp = WatchProgress::Instance();
        wp.Init(path);
        ProgressMeta mv; mv.title = "Duro de Matar \"1988\""; mv.logo = "http://x/l.jpg";
        ProgressMeta ep; ep.title = "Dark - 03. O Segredo"; ep.series = "Dark"; ep.logo = "http://x/d.jpg"; ep.season = 1; ep.episode = 3; ep.isEpisode = true;
        wp.Update("http://h/movie/1.mp4", 600, 7000, true, &mv);
        wp.Update("http://h/series/13.mp4", 1200, 3000, true, &ep);
        wp.Update("http://h/movie/2.mp4", 5, 7000, true, &mv);        // < 15 s: nao entra
        auto l = wp.List();
        assert(l.size() == 2);
        assert(l[0].id == "http://h/series/13.mp4" || l[1].id == "http://h/series/13.mp4");
        // reabrir o arquivo: tudo volta (inclusive aspas no titulo e os dados do episodio)
        wp.Init(path);
        l = wp.List();
        assert(l.size() == 2);
        for (auto &e : l) {
            if (e.isEpisode) assert(e.series == "Dark" && e.season == 1 && e.episode == 3 && e.positionSec == 1200 && e.durationSec == 3000 && e.title == "Dark - 03. O Segredo");
            else assert(e.title == "Duro de Matar \"1988\"" && e.positionSec == 600 && e.logo == "http://x/l.jpg");
        }
        wp.Update("http://h/movie/1.mp4", 6990, 7000, true, &mv);      // chegou ao final: sai da lista
        assert(wp.List().size() == 1);
    }

    // ---------- interface ----------
    std::string m3u = "#EXTM3U\n";
    for (int s = 1; s <= 2; s++) for (int e = 1; e <= 4; e++)
        m3u += "#EXTINF:-1 group-title=\"S\xC3\x89RIES | Drama\",Dark S0" + std::to_string(s) + "E0" + std::to_string(e) + " Ep" + std::to_string(e) + "\nhttp://h/series/" + std::to_string(s * 10 + e) + ".mp4\n";
    m3u += "#EXTINF:-1 group-title=\"FILMES | A\xC3\xA7\xC3\xA3o\",Duro de Matar (1988) [HD]\nhttp://h/movie/1.mp4\n"
           "#EXTINF:-1 group-title=\"FILMES | A\xC3\xA7\xC3\xA3o\",Rambo\nhttp://h/movie/3.mp4\n";
    Fake f; App app(f); app.list = buildPlaylist(m3u); UI ui(app);
    auto openTab = [&](Tab t) { ui.tab = t; ui.screen = Screen::List; ui.level = 0; ui.refreshRecents(); };      // o que UI::openTab faz
    const size_t movieCatsBefore = app.list.movieCats.size(), seriesCatsBefore = app.list.seriesCats.size(), moviesBefore = app.list.movies.size();

    auto rec = [](const char *id, const char *title, long long pos, long long dur, bool ep = false, const char *series = "", int se = 0, int e = 0) {
        WatchRec r; r.id = id; r.title = title; r.pos = pos; r.dur = dur; r.isEpisode = ep; r.series = series; r.season = se; r.episode = e; r.logo = "http://x/c.jpg"; return r;
    };
    // sem historico: nada muda
    openTab(Tab::Movies);
    assert(app.list.movieCats.size() == movieCatsBefore && !app.list.movieCats[0].recent);

    f.history = { rec("http://h/movie/3.mp4", "Rambo", 1500, 6000),                                       // filme que ESTA na lista
                  rec("http://h/movie/99.mp4", "Filme Avulso", 300, 5000),                                // filme fora da lista (painel so baixa a categoria aberta)
                  rec("http://h/series/13.mp4", "Dark - 03. Ep3", 900, 2700, true, "Dark", 1, 3),
                  rec("http://h/series/11.mp4", "Dark - 01. Ep1", 100, 2700, true, "Dark", 1, 1) };      // mesma serie, mais antigo: so o mais recente aparece
    openTab(Tab::Movies);
    assert(app.list.movieCats.size() == movieCatsBefore + 1 && app.list.movieCats[0].recent && app.list.movieCats[0].name == "Assistido recentemente");
    assert(app.list.movieCats[0].items.size() == 2 && ui.itemCount() == 2);
    assert(ui.itemLabel(0) == "Rambo" && ui.itemLabel(1) == "Filme Avulso");
    assert(app.list.movies.size() == moviesBefore + 1);                                                   // so o avulso foi acrescentado
    ui.refreshRecents(); ui.refreshRecents();                                                             // repetir nao duplica
    assert(app.list.movieCats.size() == movieCatsBefore + 1 && app.list.movies.size() == moviesBefore + 1);
    assert(ui.movieCategoryOf(app.list.movieCats[0].items[0]) != "Assistido recentemente");               // o genero do detalhe nunca vira "Assistido recentemente"

    // desenha (com o selo "parou aqui") sem sair do buffer
    for (int W : {1920, 1280}) {
        int H = W * 9 / 16;
        std::vector<uint32_t> g((size_t)W * H + 8192, 0xAABBCCDDu);
        FbRenderer r(g.data(), W, H);
        ui.render(r);
        for (size_t i = (size_t)W * H; i < g.size(); i++) assert(g[i] == 0xAABBCCDDu);
    }

    // clicar: abre os detalhes com 2 botoes ("Continuar de 25:00" / "Assistir do inicio")
    ui.playRecent(0);
    assert(ui.screen == Screen::Detail && !ui.detailSeries && ui.detailResumeSec == 1500 && ui.detailBtn == 0 && ui.detailFocus == 0);
    assert(f.playedAtUrl.empty() && f.begunCount == 0);                                                   // ainda nao tocou: espera o botao
    ui.handle(Input::Confirm);                                                                            // "Continuar": no minuto salvo
    assert(f.begunCount == 1 && f.begun.id == "http://h/movie/3.mp4" && f.playedAtUrl == "http://h/movie/3.mp4" && f.playedAtSec == 1500);
    ui.handle(Input::Right); assert(ui.detailBtn == 1);                                                   // "Assistir do inicio"
    ui.handle(Input::Left); assert(ui.detailBtn == 0);
    ui.handle(Input::Right); f.played.clear(); f.playedAtUrl.clear(); ui.handle(Input::Confirm);
    assert(f.played == "http://h/movie/3.mp4" && f.playedAtUrl.empty() && f.playedAtSec == 0);            // do inicio: sem ponto
    // desenha os 2 botoes sem sair do buffer
    for (int W : {1920, 1672}) {
        int H = W * 9 / 16;
        std::vector<uint32_t> g((size_t)W * H + 8192, 0xAABBCCDDu);
        FbRenderer r(g.data(), W, H);
        ui.render(r);
        for (size_t i = (size_t)W * H; i < g.size(); i++) assert(g[i] == 0xAABBCCDDu);
    }
    ui.handle(Input::Back); assert(ui.screen == Screen::List);
    ui.playRecent(1);                                                                                     // filme fora da lista tambem abre os detalhes
    assert(ui.screen == Screen::Detail && ui.detailResumeSec == 300 && ui.detailUrl == "http://h/movie/99.mp4");
    ui.handle(Input::Confirm); assert(f.playedAtUrl == "http://h/movie/99.mp4" && f.playedAtSec == 300);
    ui.handle(Input::Back);

    // series
    openTab(Tab::Series);
    assert(app.list.seriesCats.size() == seriesCatsBefore + 1 && app.list.seriesCats[0].recent);
    assert(ui.itemCount() == 1 && ui.itemLabel(0) == "Dark");
    ui.playRecent(0);
    assert(ui.screen == Screen::Detail && ui.detailSeries && ui.detailResumeSec == 900 && ui.detailFocus == 0 && ui.detailEpCur == 2 && ui.curSeason == 1);
    assert(f.begunCount == 3);                                                                             // ainda nao tocou
    ui.handle(Input::Confirm);                                                                            // Continuar
    assert(f.playedAtUrl == "http://h/series/13.mp4" && f.playedAtSec == 900);                            // episodio certo, no minuto certo
    assert(f.epIdx == 2 && f.epUrls.size() == 4);                                                         // com a lista da temporada (proximo/anterior)
    assert(f.begun.isEpisode && f.begun.series == "Dark" && f.begun.season == 1 && f.begun.episode == 3);
    ui.handle(Input::Right); assert(ui.detailBtn == 1);
    f.played.clear(); f.playedAtUrl.clear(); ui.handle(Input::Confirm);                                   // do inicio do MESMO episodio
    assert(f.played == "http://h/series/13.mp4" && f.playedAtUrl.empty() && f.epIdx == 2);
    {   // desenha a serie com os 2 botoes
        const int W = 1920, H = 1080; std::vector<uint32_t> g((size_t)W * H, 0); FbRenderer r(g.data(), W, H); ui.render(r);
    }

    // jogar normalmente (sem retomar) tambem registra o historico
    int before = f.begunCount;
    ui.openSeriesDetail("Dark", "Drama");
    ui.handle(Input::Confirm);
    assert(f.begunCount == before + 1 && f.begun.isEpisode && f.played == "http://h/series/11.mp4" && f.playedAtSec == 0);

    // historico esvaziou: a categoria some e a selecao nao anda
    f.history.clear(); openTab(Tab::Movies);
    assert(app.list.movieCats.size() == movieCatsBefore && !app.list.movieCats[0].recent);
    remove(path);
    std::cout << "OK\n";
}
