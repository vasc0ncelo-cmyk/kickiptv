// Lista M3U grande, lida em pedacos de tamanhos quebrados (o download chega assim no PS4).
#include "../src/app.hpp"
#include <cassert>
#include <iostream>

static std::string bigList(int channels, int movies, int seriesCount, int eps) {
    std::string s = "\xEF\xBB\xBF#EXTM3U\r\n";
    for (int i = 0; i < channels; i++)
        s += "#EXTINF:-1 tvg-id=\"c" + std::to_string(i) + "\" tvg-logo=\"http://l/" + std::to_string(i) + ".png\" group-title=\"Canais, HD\",Canal " + std::to_string(i) + " FHD\r\nhttp://h/live/u/p/" + std::to_string(i) + ".ts\r\n";
    for (int i = 0; i < movies; i++)
        s += "#EXTINF:-1 group-title=\"Filmes | Acao\",Filme " + std::to_string(i) + " (2020)\nhttp://h/movie/u/p/" + std::to_string(i) + ".mp4\n";
    for (int i = 0; i < seriesCount; i++)
        for (int e = eps; e >= 1; e--) {   // de tras para frente: precisa reordenar
            char ep[16]; snprintf(ep, sizeof ep, "S01E%02d", e);
            s += "#EXTINF:-1 group-title=\"Series | Drama\",Serie " + std::to_string(i) + " " + ep + "\nhttp://h/series/u/p/" + std::to_string(i * 100 + e) + ".mp4\n";
        }
    return s;
}

int main() {
    std::string text = bigList(9000, 4000, 300, 10);           // ~16 mil itens, varios MB
    Playlist whole = buildPlaylist(text);
    assert(whole.channels.size() == 9000 && whole.movies.size() == 4000);
    assert(whole.seriesTree.size() == 300 && whole.seriesEpisodes == 3000);
    assert(whole.channels[0].name == "Canal 0 FHD");           // virgula dentro do group-title nao quebra o nome
    assert(whole.channels[0].url == "http://h/live/u/p/0.ts");  // BOM e \r\n tratados
    {   // o m3u.cpp nao ordena episodios (custava caro no PS4); a UI ordena ao abrir a temporada (Ui::sortedEps).
        // Aqui so confere que todos os 10 episodios entraram na temporada certa, em qualquer ordem.
        const auto &eps = whole.seriesTree["Serie 7"][1];
        assert(eps.size() == 10);
        int seen = 0;
        for (const auto &e : eps) seen |= 1 << e.episode;
        assert(seen == 0x7FE);                                  // bits 1..10
    }

    // mesma lista, em pedacos de tamanhos diferentes (cortando linhas no meio)
    for (size_t chunk : {1u, 7u, 100u, 4093u, 65536u}) {
        if (chunk == 1 && text.size() > 3000000) { /* ok: so e lento, ainda roda */ }
        PlaylistBuilder b;
        for (size_t i = 0; i < text.size(); i += chunk) b.feed(text.data() + i, std::min(chunk, text.size() - i));
        b.finish();
        Playlist p = b.take();
        assert(p.channels.size() == whole.channels.size() && p.movies.size() == whole.movies.size());
        assert(p.seriesTree.size() == whole.seriesTree.size() && p.seriesEpisodes == whole.seriesEpisodes);
        assert(p.channels[8999].url == whole.channels[8999].url);
    }

    // sem quebra de linha no final + linha gigante de lixo no meio
    {
        std::string t = "#EXTM3U\n#EXTINF:-1,A\nhttp://a\n" + std::string(100000, 'x') + "\n#EXTINF:-1,B\nhttp://b";
        Playlist p = buildPlaylist(t);
        assert(p.channels.size() == 3 || p.channels.size() == 2);   // lixo truncado vira "canal", mas nao estoura a memoria
        assert(p.channels.back().url == "http://b" && p.channels.back().name == "B");
    }
    // arquivo que nao e M3U (pagina de erro)
    { Playlist p = buildPlaylist("<html><body>Erro 403</body></html>\n"); assert(p.channels.size() <= 1); }

    // App: a lista chega em varios pedacos; cancelamento e queda de conexao
    struct Fake : Platform {
        std::string data; int mode = 0;   // 0 ok, 1 cai no meio, 2 cancelado
        bool cancelled = false;
        bool httpGet(const std::string &, std::string &) override { return false; }
        bool httpStream(const std::string &, const std::function<bool(const char *, size_t)> &sink) override {
            cancelled = false;
            size_t lim = mode == 1 ? data.size() / 2 : data.size();
            for (size_t i = 0; i < lim; i += 65536) {
                if (mode == 2 && i > 200000) { cancelled = true; return false; }
                if (!sink(data.data() + i, std::min<size_t>(65536, lim - i))) return false;
            }
            return mode != 1;
        }
        bool wasCancelled() override { return cancelled; }
        void play(const std::string &, const std::string &) override {}
        void requestText(const std::string &) override {}
    };
    Fake f; f.data = text;
    App a(f);
    assert(a.loadM3U("http://x") && a.list.channels.size() == 9000);
    assert(a.lastStatus.find("Canais: 9000") == 0);
    f.mode = 1; App a2(f);
    assert(a2.loadM3U("http://x") && !a2.list.empty() && a2.lastStatus.find("Aviso") == 0);     // aproveita o que chegou
    f.mode = 2; App a3(f);
    assert(!a3.loadM3U("http://x") && a3.list.empty() && a3.lastStatus.find("cancelado") != std::string::npos);
    // cancelar nao apaga a lista que ja estava carregada
    assert(a.list.channels.size() == 9000); f.mode = 2; assert(!a.loadM3U("http://x") && a.list.channels.size() == 9000);
    f.mode = 0; f.data = "nao e uma lista"; // texto sem URL de verdade vira 1 item? -> so conta se tiver linha
    f.data = "#EXTM3U\n"; App a4(f); assert(!a4.loadM3U("http://x") && a4.lastStatus.find("Falha") == 0);
    // categorias: prefixo do tipo some, o resto fica
    assert(cleanCategory("FILMES | Comédia") == "Comédia" && cleanCategory("Séries - Netflix") == "Netflix" && cleanCategory("CANAIS: Esportes") == "Esportes");
    assert(cleanCategory("Ação - Aventura") == "Ação - Aventura" && cleanCategory("") == "Sem categoria" && cleanCategory("Filmes") == "Filmes");
    {   // as 3 abas ficam separadas por categoria e a soma bate com o total
        Playlist p = buildPlaylist(text);
        size_t n = 0; for (auto &c : p.channelCats) if (c.name != "Todos") n += c.items.size(); assert(n == p.channels.size() && p.channelCats[0].name == "Todos");
        n = 0; for (auto &c : p.movieCats) if (c.name != "Todos") n += c.items.size(); assert(n == p.movies.size() && p.movieCats[0].name == "Todos");
        n = 0; for (auto &c : p.seriesCats) if (c.name != "Todos") n += c.series.size(); assert(n == p.seriesTree.size() && p.seriesCats[0].name == "Todos");
    }
    std::cout << "OK\n";
}
