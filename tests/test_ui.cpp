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
    ui.handle(Input::Confirm); assert(ui.screen == Screen::List && ui.tab == Tab::Channels);
    ui.handle(Input::Confirm); assert(f.played == "http://h/1");
    ui.handle(Input::Back); assert(ui.screen == Screen::Home);
    // --- Filmes ---
    ui.handle(Input::Left); ui.handle(Input::Left); ui.handle(Input::Confirm); assert(ui.tab == Tab::Movies);
    ui.handle(Input::Confirm); assert(f.played == "http://h/movie/2");
    // --- Series: serie -> temporada 1 -> ep 1 (ordenado) ---
    ui.handle(Input::NextTab); assert(ui.tab == Tab::Series);
    ui.handle(Input::Confirm);          // Dark
    ui.handle(Input::Confirm);          // Temporada 1
    { Rec r; ui.render(r); assert(r.has("Ep 1 - Dark S01E01") && r.has("Ep 2 - Dark S01E02")); }
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/5");
    ui.handle(Input::Back); ui.handle(Input::Down); ui.handle(Input::Confirm);   // Temporada 2
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/3");
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
    // --- lista grande: navegar nao pode depender do tamanho da lista ---
    {
        Fake f4; App a4(f4); UI u4(a4);
        for (int i = 0; i < 50000; i++) { Entry e; e.name = "Canal " + std::to_string(i); a4.list.channels.push_back(e); }
        u4.tab = Tab::Channels; u4.screen = Screen::List;
        u4.handle(Input::PageDown); assert(u4.cursor[1] == UI::kRows);
        for (int i = 0; i < 5000; i++) u4.handle(Input::Down);
        assert(u4.cursor[1] == UI::kRows + 5000 && u4.scroll[1] == u4.cursor[1] - UI::kRows + 1);
        Rec r; u4.render(r); assert(r.has("Canal " + std::to_string(u4.cursor[1])));
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
