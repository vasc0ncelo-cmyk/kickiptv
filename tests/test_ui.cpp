#include "../src/ui.hpp"
#include <cassert>
#include <iostream>

struct Fake : Platform {
    std::string played, asked;
    bool httpGet(const std::string &url, std::string &b) override {
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
};
struct Rec : Renderer {
    std::vector<std::string> texts; int rects = 0;
    int width() const override { return 1920; }
    int height() const override { return 1080; }
    void fillRect(int, int, int, int, unsigned) override { rects++; }
    void drawText(int, int, const std::string &t, unsigned, int) override { texts.push_back(t); }
    bool has(const std::string &s) { for (auto &t : texts) if (t == s) return true; return false; }
};

int main() {
    Fake f; App a(f); UI ui(a);
    assert(a.loadM3U("http://lista"));
    // Canais
    ui.handle(Input::Confirm); assert(f.played == "http://h/1");
    // Filmes
    ui.handle(Input::NextTab); ui.handle(Input::Confirm); assert(f.played == "http://h/movie/2");
    // Series: serie -> temporada 1 -> ep 1 (ordenado)
    ui.handle(Input::NextTab); ui.handle(Input::Confirm);          // Dark
    ui.handle(Input::Confirm);                                     // Temporada 1
    Rec r; ui.render(r); assert(r.has("Ep 1 - Dark S01E01") && r.has("Ep 2 - Dark S01E02"));
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/5");
    ui.handle(Input::Back); ui.handle(Input::Down); ui.handle(Input::Confirm); // Temporada 2
    ui.handle(Input::Confirm); assert(f.played == "http://h/series/3");
    // Kick
    ui.handle(Input::NextTab); assert(ui.tab == Tab::Kick);
    ui.handle(Input::Confirm); assert(f.asked == "@ do streamer");
    ui.onText("@Streamer"); assert(f.played == "https://k/a.m3u8" && a.lastStatus == "Assistindo @streamer");
    ui.handle(Input::PrevTab); assert(ui.tab == Tab::Series && ui.level == 0);
    std::cout << "OK\n";
}
