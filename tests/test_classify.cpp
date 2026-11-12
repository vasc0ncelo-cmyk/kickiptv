// Classificacao em Filmes / Series / Canais (abas) com listas de painel reais: acentos, grupos misturados, URLs Xtream.
#include "../src/m3u.hpp"
#include <cassert>
#include <iostream>

static Playlist L(const std::string &body) { return buildPlaylist("#EXTM3U\n" + body); }
static std::string item(const std::string &group, const std::string &name, const std::string &url) {
    return "#EXTINF:-1 tvg-id=\"x\" tvg-logo=\"http://l/a.png\" group-title=\"" + group + "\"," + name + "\n" + url + "\n";
}

int main() {
    // exatamente o caso do print: serie de painel Xtream
    {
        Playlist p = L(item("SÉRIES | GLOBOPLAY", "4Ever [L] 2023 - S01E0002", "http://180381.sbs/series/Dirce0273/Sar5438/585332.mp4"));
        assert(p.channels.empty() && p.movies.empty() && p.seriesTree.size() == 1);
        auto &s = p.seriesTree.begin()->second;
        assert(p.seriesTree.begin()->first == "4Ever [L] 2023" && s.count(1) && s.at(1)[0].episode == 2);
    }
    // grupo em maiuscula com acento, URL sem /series/ (lista m3u simples): antes caia em "canais"
    { Playlist p = L(item("SÉRIES | NETFLIX", "La Casa S02E03", "http://h/u/p/55.mp4")); assert(p.seriesTree.size() == 1 && p.channels.empty()); }
    { Playlist p = L(item("Séries de TV", "Show S01E01", "http://h/55.mkv")); assert(p.seriesTree.size() == 1); }
    // canais ao vivo cujo grupo tem "FILMES"/"SERIES" no nome NAO sao filmes/series
    { Playlist p = L(item("CANAIS | FILMES E SÉRIES", "HBO HD", "http://h/u/p/77.ts")); assert(p.channels.size() == 1 && p.movies.empty() && p.seriesTree.empty()); }
    { Playlist p = L(item("24H FILMES", "24H Rambo", "http://h/u/p/78")); assert(p.channels.size() == 1 && p.movies.empty()); }
    { Playlist p = L(item("24H SERIES", "24H Simpsons", "http://h/u/p/79")); assert(p.channels.size() == 1 && p.seriesTree.empty()); }
    { Playlist p = L(item("Filmes", "Telecine Pipoca", "http://h/live/u/p/80.ts")); assert(p.channels.size() == 1); }   // /live/ manda mais que o grupo
    { Playlist p = L(item("ESPORTES | SERIE A", "Canal Serie A 1", "http://h/u/p/81.ts")); assert(p.channels.size() == 1); }
    // filmes
    { Playlist p = L(item("FILMES | AÇÃO", "Duro de Matar (1988)", "http://h/movie/u/p/10.mkv")); assert(p.movies.size() == 1); }
    { Playlist p = L(item("Lançamentos", "Avatar 2", "http://h/u/p/11.mp4")); assert(p.movies.size() == 1); }
    { Playlist p = L(item("", "Filme sem grupo", "http://h/arquivos/f.mkv?token=1")); assert(p.movies.size() == 1); }
    // serie dentro de grupo "Lançamentos" (mistura filmes e episodios)
    { Playlist p = L(item("Lançamentos", "The Boys S04E01", "http://h/u/p/12.mp4")); assert(p.seriesTree.size() == 1 && p.movies.empty()); }
    // variacoes de numeracao do episodio
    { Playlist p = L(item("Series", "Dark S01 E02", "http://h/series/u/p/1.mp4") + item("Series", "Dark T01E03", "http://h/series/u/p/2.mp4") +
                     item("Series", "Dark s1.e4", "http://h/series/u/p/3.mp4"));
      assert(p.seriesTree.size() == 1 && p.seriesTree["Dark"][1].size() == 3 && p.seriesTree["Dark"][1][2].episode == 4); }
    // canal comum
    { Playlist p = L(item("Abertos", "Globo SP HD", "http://h/u/p/1")); assert(p.channels.size() == 1); }
    { Playlist p = L(item("Canais | Variedades", "Multishow", "http://h/u/p/2.m3u8")); assert(p.channels.size() == 1); }
    // lista mista grande: cada item na aba certa
    {
        std::string t;
        for (int i = 0; i < 50; i++) t += item("CANAIS | ABERTOS", "Canal " + std::to_string(i), "http://h/live/u/p/" + std::to_string(i) + ".ts");
        for (int i = 0; i < 30; i++) t += item("FILMES | DRAMA", "Filme " + std::to_string(i), "http://h/movie/u/p/" + std::to_string(i) + ".mp4");
        for (int i = 0; i < 20; i++) t += item("SÉRIES | DRAMA", "Serie A S01E" + std::to_string(i + 1), "http://h/series/u/p/" + std::to_string(i) + ".mp4");
        Playlist p = L(t);
        assert(p.channels.size() == 50 && p.movies.size() == 30 && p.seriesEpisodes == 20 && p.seriesTree.size() == 1 && !p.truncated);
    }
    std::cout << "OK\n";
}
