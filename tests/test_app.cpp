#include "../src/app.hpp"
#include <cassert>
#include <iostream>

struct Fake : Platform {
    std::string playedUrl;
    bool httpGet(const std::string &url, std::string &body) override {
        if (url.find("kick.com") != std::string::npos) {
            if (url.find("offline") != std::string::npos) body = "{\"livestream\":null}";
            else body = "{\"playback_url\":\"https:\\/\\/x.live\\/a.m3u8\",\"livestream\":{\"id\":1}}";
            return true;
        }
        body = "#EXTM3U\n"
               "#EXTINF:-1 tvg-id=\"g\" group-title=\"Abertos\",Globo HD\nhttp://h/live/1.ts\n"
               "#EXTINF:-1 group-title=\"Filmes Acao\",Duro de Matar\nhttp://h/movie/u/p/10.mp4\n"
               "#EXTINF:-1 group-title=\"Series\",Dark S01E02\nhttp://h/series/u/p/21.mp4\n"
               "#EXTINF:-1 group-title=\"Series\",Dark S01E01\nhttp://h/series/u/p/20.mp4\n";
        return true;
    }
    void play(const std::string &u, const std::string &) override { playedUrl = u; }
    void requestText(const std::string &) override {}
};

int main() {
    Fake f; App a(f);
    assert(a.loadM3U("http://lista"));
    assert(a.list.channels.size() == 1 && a.list.movies.size() == 1 && a.list.series.size() == 2);
    assert(a.list.seriesTree["Dark"][1][0].episode == 1);
    a.watchKick("@Streamer");
    assert(f.playedUrl == "https://x.live/a.m3u8");
    f.playedUrl.clear(); a.watchKick("@offline");
    assert(f.playedUrl.empty() && a.lastStatus.find("offline") != std::string::npos);
    a.watchKick("@@@bad name!"); assert(a.lastStatus == "@ invalido");
    std::cout << "OK\n";
}
