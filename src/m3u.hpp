#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <vector>

enum class Kind { Channel, Movie, Series };

struct Entry {
    std::string name, url, group;   // (group so existe durante o parse; a Playlist final nao guarda, para poupar memoria)
    Kind kind = Kind::Channel;
    int season = 0, episode = 0;    // so para series
};

struct Playlist {
    std::vector<Entry> channels, movies;
    size_t seriesEpisodes = 0;   // total de episodios (os episodios em si ficam em seriesTree)
    bool truncated = false;      // a lista tinha mais itens que o limite de memoria (os ultimos ficaram de fora)
    // serie -> temporada -> episodios
    std::map<std::string, std::map<int, std::vector<Entry>>> seriesTree;
    bool empty() const { return channels.empty() && movies.empty() && seriesTree.empty(); }
};

// Le a lista M3U em PEDACOS (feed pode ser chamado com qualquer tamanho de bloco, inclusive cortando uma linha no meio).
// Nao precisa guardar o arquivo inteiro na memoria: listas de IPTV com 10-15 mil itens tem varios MB.
class PlaylistBuilder {
public:
    PlaylistBuilder();
    void feed(const char *data, size_t n);
    void finish();                 // processa a ultima linha e ordena os episodios
    Playlist take();               // entrega o resultado (use depois de finish)
    size_t count() const { return count_; }   // itens lidos ate agora

private:
    void line(std::string &l);
    std::string cur_, extinf_;
    bool skipRest_ = false, first_ = true;
    Playlist p_;
    size_t count_ = 0;
};

// Versoes "tudo de uma vez" (testes e compatibilidade).
std::vector<Entry> parseM3U(const std::string &text);
Playlist buildPlaylist(const std::string &text);
bool parseEpisode(const std::string &name, std::string &series, int &season, int &episode);
