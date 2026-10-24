#include "../src/keyboard.hpp"
#include "../src/clock.hpp"
#include <cassert>
#include <iostream>
#include <map>

struct Fake : Platform {
    std::string played, asked;
    std::map<std::string, std::string> store;
    std::string lastUrl;
    bool httpGet(const std::string &url, std::string &b) override {
        lastUrl = url;
        if (url.find("kick.com") != std::string::npos) { b = "{\"playback_url\":\"https:\\/\\/k\\/a.m3u8\",\"livestream\":{\"id\":1}}"; return true; }
        b = "#EXTM3U\n#EXTINF:-1 group-title=\"A\",Canal 1\nhttp://h/1\n"
            "#EXTINF:-1 group-title=\"Filmes\",Filme 1\nhttp://h/movie/2\n"
            "#EXTINF:-1 group-title=\"Series\",Dark S02E01\nhttp://h/series/3\n"
            "#EXTINF:-1 group-title=\"Series\",Dark S01E02\nhttp://h/series/4\n"
            "#EXTINF:-1 group-title=\"Series\",Dark S01E01\nhttp://h/series/5\n";
        return true;
    }
    void play(const std::string &u, const std::string &) override { played = u; }
    void requestText(const std::string &p) override { asked = p; }
    std::string loadSetting(const std::string &k) override { return store[k]; }
    void saveSetting(const std::string &k, const std::string &v) override { store[k] = v; }
};
struct Rec : Renderer {
    std::vector<std::string> texts;
    int width() const override { return 1920; }
    int height() const override { return 1080; }
    void fillRect(int, int, int, int, unsigned) override {}
    void drawText(int, int, const std::string &t, unsigned, int) override { texts.push_back(t); }
    bool has(const std::string &s) { for (auto &t : texts) if (t == s) return true; return false; }
};

int main() {
    Fake f; App a(f); UI ui(a);
    // --- relogio: 11 Out 2026 23:47 (UTC-3) ---
    { std::string t, d; clockStrings(1791773220LL, t, d); assert(t == "23:47" && d == "11 Out 2026"); }
    // --- Home: abre direto nos atalhos, KICK TV selecionado ---
    assert(ui.screen == Screen::Home && ui.homeRow == 1 && ui.homeCol == 0);
    { Rec r; ui.render(r); assert(r.has("KICK TV") && r.has("FILMES") && r.has("SÉRIES") && r.has("CANAIS") && r.has("LISTA M3U") && r.has("Acessar Agora")); }
    // --- Lista M3U: pede o teclado, baixa e guarda ---
    ui.handle(Input::Down); assert(ui.homeRow == 2);
    ui.handle(Input::Confirm); assert(f.asked == "Endereço da lista M3U" && ui.textKind == 2);
    ui.onText("servidor.com/lista.m3u");
    assert(f.lastUrl == "http://servidor.com/lista.m3u" && f.store["m3u"] == "http://servidor.com/lista.m3u");
    assert(a.list.channels.size() == 1 && a.list.movies.size() == 1);
    // reabrir o app: carrega a lista guardada
    { Fake f2; f2.store = f.store; App a2(f2); UI u2(a2); u2.loadSaved(); assert(u2.hasSavedList()); u2.loadSavedList(); assert(a2.list.channels.size() == 1); }
    // --- Canais ---
    ui.handle(Input::Up); ui.handle(Input::Right); ui.handle(Input::Right); ui.handle(Input::Right);   // CANAIS
    ui.handle(Input::Confirm); assert(ui.screen == Screen::List && ui.tab == Tab::Channels && !ui.inItems);
    { Rec r; ui.render(r); assert(r.has("CANAIS") && r.has("A")); }                      // barra de categorias mostra o grupo "A"
    ui.handle(Input::Confirm); assert(ui.inItems && f.played.empty());                   // 1o X abre a categoria
    ui.handle(Input::Confirm); assert(f.played == "http://h/1");                         // 2o X toca
    ui.handle(Input::Back); assert(ui.screen == Screen::List && !ui.inItems);            // O volta para as categorias
    ui.handle(Input::Back); assert(ui.screen == Screen::Home);
    // --- Filmes ---
    ui.handle(Input::Left); ui.handle(Input::Left); ui.handle(Input::Confirm); assert(ui.tab == Tab::Movies);
    ui.handle(Input::Right); assert(ui.inItems);                                         // seta direita tambem entra na categoria
    ui.handle(Input::Left); assert(!ui.inItems); ui.handle(Input::Right);
    ui.handle(Input::Confirm); assert(f.played == "http://h/movie/2");
    // --- Series: serie -> temporada 1 -> ep 1 (ordenado) ---
    ui.handle(Input::NextTab); assert(ui.tab == Tab::Series && !ui.inItems);
    ui.handle(Input::Confirm);          // abre a categoria "Series"
    ui.handle(Input::Confirm);          // Dark
    ui.handle(Input::Confirm);          // Temporada 1
    { Rec r; ui.render(r); assert(r.has("Ep 1 - Dark S01E01") && r.has("Ep 2 - Dark S01E02")); }
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/5");
    ui.handle(Input::Back); ui.handle(Input::Down); ui.handle(Input::Confirm);   // Temporada 2
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/3");
    ui.handle(Input::Back); ui.handle(Input::Back); assert(ui.level == 0 && ui.inItems);   // episodios -> temporadas -> series
    ui.handle(Input::Back); assert(!ui.inItems && ui.screen == Screen::List);              // series -> categorias
    // --- Kick TV ---
    ui.handle(Input::NextTab); assert(ui.tab == Tab::Channels);
    ui.handle(Input::NextTab); assert(ui.tab == Tab::Kick && ui.screen == Screen::Kick);
    ui.handle(Input::Confirm); assert(f.asked == "@ do streamer" && ui.textKind == 1);
    ui.onText("@Streamer"); assert(f.played == "https://k/a.m3u8" && a.lastStatus == "Assistindo @streamer" && f.store["kick"] == "streamer");
    // o ultimo canal aparece como segundo item
    { Rec r; ui.render(r); assert(r.has("Assistir @streamer")); }
    f.played.clear(); ui.handle(Input::Down); ui.handle(Input::Confirm); assert(f.played == "https://k/a.m3u8");
    // --- Remover lista: campo vazio ---
    ui.handle(Input::Back); ui.homeRow = 2; ui.handle(Input::Confirm); ui.onText("");
    assert(a.list.channels.empty() && f.store["m3u"].empty());
    // --- Teclado ---
    { Keyboard k; k.open("", true); assert(k.keyAt(0, 1) == "q");
      k.cx = 0; k.cy = 4; k.press(); assert(k.shift && k.keyAt(0, 1) == "Q");
      k.cx = 0; k.cy = 1; k.press(); assert(k.text == "Q");
      k.insert("https://"); assert(k.text == "Qhttps://"); k.backspace(); k.clear(); assert(k.text.empty());
      k.open("", false); assert(k.maxLen == 25); }
    // --- itemCount/itemLabel (sem copiar a lista toda) == labels() em todas as abas e niveis ---
    {
        Fake f3; App a3(f3); UI u3(a3); u3.setM3U("http://x/l.m3u");
        for (Tab t : {Tab::Kick, Tab::Channels, Tab::Movies, Tab::Series}) {
            u3.tab = t; u3.level = 0;
            auto v = u3.labels(); assert((int)v.size() == u3.itemCount());
            for (int i = 0; i < u3.itemCount(); i++) assert(u3.itemLabel(i) == v[i]);
        }
        u3.tab = Tab::Series; u3.level = 1; u3.curSeries = "Dark"; assert(u3.itemCount() == 2 && u3.itemLabel(0) == "Temporada 1");
        u3.level = 2; u3.curSeason = 1; assert(u3.itemCount() == 2 && u3.itemLabel(0) == "Ep 1 - Dark S01E01");
        assert(u3.itemLabel(99).empty() && u3.itemLabel(-1).empty());
    }
    // --- varias categorias por aba: cada uma mostra so os seus itens; trocar de categoria zera o cursor ---
    {
        struct F5 : Fake { bool httpGet(const std::string &, std::string &b) override {
            b = "#EXTM3U\n#EXTINF:-1 group-title=\"FILMES | Comédia\",Gente Grande\nhttp://h/movie/1.mp4\n"
                "#EXTINF:-1 group-title=\"FILMES | Ação\",Duro de Matar\nhttp://h/movie/2.mp4\n"
                "#EXTINF:-1 group-title=\"FILMES | Comédia\",Superbad\nhttp://h/movie/3.mp4\n"
                "#EXTINF:-1 group-title=\"CANAIS | Esportes\",ESPN\nhttp://h/live/4.ts\n"
                "#EXTINF:-1 group-title=\"CANAIS | Abertos\",Globo\nhttp://h/live/5.ts\n"
                "#EXTINF:-1 group-title=\"SÉRIES | Netflix\",Dark S01E01\nhttp://h/series/6.mp4\n"
                "#EXTINF:-1 group-title=\"SÉRIES | Netflix\",Dark S01E02\nhttp://h/series/7.mp4\n"
                "#EXTINF:-1 group-title=\"SÉRIES | Disney\",Andor S01E01\nhttp://h/series/8.mp4\n"
                "#EXTINF:-1 group-title=\"Todos\",Sem Url Pista\nhttp://h/movie/9.mp4\n";
            return true; } };
        F5 f5; App a5(f5); UI u5(a5); u5.setM3U("http://x/l.m3u");
        u5.screen = Screen::List; u5.tab = Tab::Movies;
        assert(u5.catCount() == 3 && u5.catLabel(0) == "Comédia" && u5.catLabel(1) == "Ação" && u5.catLabel(2) == "Todos");
        assert(u5.catSize(0) == 2 && u5.itemCount() == 2 && u5.itemLabel(1) == "Superbad");
        u5.handle(Input::Right); u5.handle(Input::Right); assert(u5.cursor[2] == 1);   // grade: direita anda 1 poster
        u5.handle(Input::Left); u5.handle(Input::Left); u5.handle(Input::Down);       // 1a coluna: esquerda volta as categorias                                   // categoria "Ação"
        assert(u5.catCur[2] == 1 && u5.cursor[2] == 0 && u5.itemCount() == 1 && u5.itemLabel(0) == "Duro de Matar");
        u5.handle(Input::Confirm); f5.played.clear(); u5.handle(Input::Confirm); assert(f5.played == "http://h/movie/2.mp4");
        { Rec r; u5.render(r); assert(r.has("FILMES") && r.has("Ação") && r.has("Comédia") && r.has("Duro de Matar") && !r.has("Superbad")); }
        u5.tab = Tab::Channels; u5.inItems = false;
        assert(u5.catCount() == 2 && u5.catLabel(0) == "Esportes" && u5.itemLabel(0) == "ESPN");
        u5.handle(Input::Down); assert(u5.itemLabel(0) == "Globo");
        u5.tab = Tab::Series; u5.inItems = false; u5.level = 0;
        assert(u5.catCount() == 2 && u5.catLabel(0) == "Netflix" && u5.catLabel(1) == "Disney");
        assert(u5.itemCount() == 1 && u5.itemLabel(0) == "Dark");
        u5.handle(Input::Down); assert(u5.itemLabel(0) == "Andor");
        for (Tab t : {Tab::Channels, Tab::Movies, Tab::Series}) { u5.tab = t; Rec r; u5.render(r); }   // renderiza sem quebrar
    }
    // --- lista grande: navegar nao pode depender do tamanho da lista ---
    {
        Fake f4; App a4(f4); UI u4(a4);
        { std::string m = "#EXTM3U\n"; for (int i = 0; i < 50000; i++) m += "#EXTINF:-1 group-title=\"Canais | Todos\",Canal " + std::to_string(i) + "\nhttp://h/live/" + std::to_string(i) + ".ts\n";
          a4.list = buildPlaylist(m); }
        u4.tab = Tab::Channels; u4.screen = Screen::List; u4.inItems = true;
        u4.handle(Input::PageDown); assert(u4.cursor[1] == UI::kCols * UI::kGridRows);
        for (int i = 0; i < 5000; i++) u4.handle(Input::Down);                       // grade: cada Down pula uma linha de 8 posters
        assert(u4.cursor[1] == UI::kCols * UI::kGridRows + 5000 * UI::kCols);
        assert(u4.scroll[1] == u4.cursor[1] / UI::kCols - UI::kGridRows + 1);
        Rec r; u4.render(r); assert(r.has("Canal " + std::to_string(u4.cursor[1])));
        // grade: Esquerda/Direita/Cima nas bordas nao saem da lista
        u4.cursor[1] = 0; u4.scroll[1] = 0;
        u4.handle(Input::Up); assert(u4.cursor[1] == 0);
        u4.handle(Input::Right); assert(u4.cursor[1] == 1);
        for (int i = 0; i < 20; i++) u4.handle(Input::Right);
        assert(u4.cursor[1] == UI::kCols - 1);                                       // para no fim da linha
    }
    // --- capas: a tela pede as da pagina visivel primeiro e as proximas depois ---
    {
        Fake fc; App ac(fc); UI uc(ac);
        std::string m = "#EXTM3U\n";
        for (int i = 0; i < 100; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/" + std::to_string(i) + ".jpg\" group-title=\"FILMES | Todos\",Filme " + std::to_string(i) + "\nhttp://h/m/" + std::to_string(i) + ".mp4\n";
        ac.list = buildPlaylist(m);
        std::vector<std::string> front, back; int calls = 0;
        uc.coverBatch = [&](const std::vector<std::string> &f, const std::vector<std::string> &b) { front = f; back = b; calls++; };
        std::vector<std::string> bgAll; int bgCalls = 0;
        uc.coverBackground = [&](std::vector<std::string> v) { bgAll = std::move(v); bgCalls++; };
        uc.screen = Screen::List; uc.tab = Tab::Movies;
        { Rec r; uc.render(r); }
        assert(bgCalls == 1 && (int)bgAll.size() == UI::kCols * UI::kGridRows && bgAll[0] == "http://img/0.jpg" && bgAll[23] == "http://img/23.jpg");   // 1a tela de cada categoria para o baixador de fundo
        { Rec r; uc.render(r); } assert(bgCalls == 1);                                                                     // so manda de novo se a lista mudar
        assert(calls == 1 && (int)front.size() == UI::kCols * UI::kGridRows && front[0] == "http://img/0.jpg" && front[23] == "http://img/23.jpg");
        assert((int)back.size() == UI::kCols * UI::kGridRows * 2 && back[0] == "http://img/24.jpg");   // 2 paginas adiante
        { Rec r; uc.render(r); } assert(calls == 1);                                  // nada mudou: nao repete o pedido
        uc.inItems = true; uc.handle(Input::PageDown); uc.handle(Input::PageDown); { Rec r; uc.render(r); }
        assert(calls >= 2 && front[0] == "http://img/32.jpg");   // cursor 48 -> primeira linha visivel = 4 (poster 32)
    }
    // --- lista enorme: o baixador de capas em segundo plano recebe so as primeiras (nao copia centenas de milhares de enderecos) ---
    {
        Fake fb; App ab(fb); UI ub(ab);
        std::string m = "#EXTM3U\n";
        for (int i = 0; i < 30000; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/m" + std::to_string(i) + ".jpg\" group-title=\"FILMES | Todos\",Filme " + std::to_string(i) + "\nhttp://h/m/" + std::to_string(i) + ".mp4\n";
        for (int i = 0; i < 30000; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/c" + std::to_string(i) + ".jpg\" group-title=\"Abertos\",Canal " + std::to_string(i) + "\nhttp://h/live/" + std::to_string(i) + ".ts\n";
        ab.list = buildPlaylist(m);
        std::vector<std::string> bg; int bgCalls = 0;
        ub.coverBackground = [&](std::vector<std::string> v) { bg = std::move(v); bgCalls++; };
        ub.screen = Screen::List; ub.tab = Tab::Movies;
        { Rec r; ub.render(r); }
        assert(bgCalls == 1 && bg.size() <= 3 * 14 * UI::kCols * UI::kGridRows && bg.size() >= 2 * UI::kCols * UI::kGridRows);   // so a 1a tela de cada categoria, nunca a lista toda
        assert(bg[0] == "http://img/m0.jpg");                                          // as primeiras de cada aba vem primeiro
    }
    // --- TRIANGULO: baixa as capas de TODAS as categorias; apertar de novo cancela ---
    {
        Fake fb2; App ab2(fb2); UI ub2(ab2);
        std::string m = "#EXTM3U\n";
        for (int i = 0; i < 30; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/f" + std::to_string(i) + ".jpg\" group-title=\"FILMES | " + std::string(i % 2 ? "Acao" : "Drama") + "\",Filme " + std::to_string(i) + "\nhttp://h/m/" + std::to_string(i) + ".mp4\n";
        for (int i = 0; i < 5; i++) m += "#EXTINF:-1 tvg-logo=\"http://img/c" + std::to_string(i) + ".jpg\" group-title=\"Abertos\",Canal " + std::to_string(i) + "\nhttp://h/live/" + std::to_string(i) + ".ts\n";
        ab2.list = buildPlaylist(m);
        std::vector<std::string> got; bool active = false; int stops = 0;
        ub2.coverBulk = [&](std::vector<std::string> v) { got = std::move(v); active = true; };
        ub2.coverBulkStop = [&] { active = false; stops++; };
        ub2.coverBulkActive = [&] { return active; };
        ub2.handle(Input::Extra);
        assert(got.size() == 35 && active && ab2.lastStatus.find("Baixando capas") == 0);
        ub2.handle(Input::Extra); assert(!active && stops == 1 && ab2.lastStatus.find("cancelado") != std::string::npos);
        UI ub3(ab2); got.clear(); ub3.coverBulk = [&](std::vector<std::string> v) { got = std::move(v); };
        Playlist keep = std::move(ab2.list); ab2.list = Playlist(); ub3.handle(Input::Extra);
        assert(got.empty() && ab2.lastStatus.find("Carregue uma lista") == 0);
    }
    // --- uiFit: corta com "..." sem estourar a largura e sem quebrar UTF-8 ---
    {
        Rec r; std::string longo(200, 'a');
        std::string o = uiFit(r, longo, 28, 500); assert(r.textWidth(o, 28) <= 500 && o.size() > 10 && o.compare(o.size() - 3, 3, "...") == 0);
        std::string u = uiFit(r, "çãõéíú çãõéíú çãõéíú çãõéíú çãõéíú", 28, 200);
        assert(r.textWidth(u, 28) <= 200);
        for (size_t i = 0; i < u.size();) { unsigned char c = u[i]; size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4; assert(i + n <= u.size()); i += n; }
        assert(uiFit(r, "curto", 28, 500) == "curto");
    }
    // --- Teclado: cursor piscando e posicao do campo ---
    {
        Keyboard k; k.open("abc", false);
        Rec r; auto fld = k.field(r);
        assert(fld.shown == "abc" && fld.caretX == fld.px + r.textWidth("abc", 36) + 3);
        k.drawCaret(r, true); k.drawCaret(r, false);                      // nao pode quebrar
        k.open(std::string(300, 'x'), true);
        auto f2 = k.field(r); assert(f2.shown.compare(0, 3, "...") == 0 && f2.caretX < r.width());
    }
    std::cout << "OK\n";
}
