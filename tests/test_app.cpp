#include "../src/app.hpp"
#include <cassert>
#include <iostream>
#include <algorithm>

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

// Plataforma com cache em memoria: conta downloads para provar que abrir o app nao baixa de novo.
struct CacheFake : Fake {
    int downloads = 0;
    std::string saved, savedUrl, building;
    bool httpStream(const std::string &url, const std::function<bool(const char *, size_t)> &sink) override {
        downloads++;
        std::string body; httpGet(url, body);
        for (size_t i = 0; i < body.size(); i += 50) if (!sink(body.data() + i, std::min<size_t>(50, body.size() - i))) return false;
        return true;
    }
    bool cacheLoad(const std::string &url, const std::function<bool(const char *, size_t)> &sink) override {
        if (saved.empty() || url != savedUrl) return false;
        return sink(saved.data(), saved.size());
    }
    void cacheBegin() override { building.clear(); }
    void cacheWrite(const char *d, size_t n) override { building.append(d, n); }
    void cacheEnd(const std::string &url, bool complete) override { if (complete) { saved = building; savedUrl = url; } }
    void cacheClear() override { saved.clear(); savedUrl.clear(); }
};

int main() {
    {   // sem limite de itens: 350 mil entradas (acima do antigo teto de 300 mil)
        std::string t = "#EXTM3U\n";
        for (int i = 0; i < 350000; i++) t += "#EXTINF:-1 group-title=\"Abertos\",Canal " + std::to_string(i) + "\nhttp://h/live/" + std::to_string(i) + ".ts\n";
        Playlist p = buildPlaylist(t);
        assert(p.channels.size() == 350000 && !p.truncated);
    }
    {   // cache: 1a vez baixa e salva; abrir o app (useCache) nao baixa; confirmar o link baixa de novo; outro link baixa
        CacheFake f; App a(f);
        assert(a.loadM3U("http://lista", true) && f.downloads == 1 && !f.saved.empty());      // sem copia ainda: baixa
        App b(f);
        assert(b.loadM3U("http://lista", true) && f.downloads == 1);                          // usa a copia
        assert(b.list.channels.size() == 1 && b.list.movies.size() == 1 && b.list.seriesEpisodes == 2);
        assert(b.lastStatus.find("lista salva") != std::string::npos);
        assert(b.loadM3U("http://lista") && f.downloads == 2);                                // usuario confirmou: atualiza
        assert(b.loadM3U("http://outra", true) && f.downloads == 3);                          // link diferente: nao usa a copia
        f.cacheClear(); assert(b.loadM3U("http://outra", true) && f.downloads == 4);
    }
    Fake f; App a(f);
    assert(a.loadM3U("http://lista"));
    assert(a.list.channels.size() == 1 && a.list.movies.size() == 1 && a.list.seriesEpisodes == 2);
    assert(a.list.seriesTree["Dark"][1][0].episode == 1);
    a.watchKick("@Streamer");
    assert(f.playedUrl == "https://x.live/a.m3u8");
    f.playedUrl.clear(); a.watchKick("@offline");
    assert(f.playedUrl.empty() && a.lastStatus.find("offline") != std::string::npos);
    assert(jsonString("{\"playback_url\":\"https:\\/\\/h\\/a.m3u8?x=1\\u0026y=2\"}", "playback_url") == "https://h/a.m3u8?x=1&y=2");
    a.watchKick("@@@bad name!"); assert(a.lastStatus == "@ invalido");
    std::cout << "OK\n";
}
