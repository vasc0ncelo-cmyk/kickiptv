#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

enum class Kind { Channel, Movie, Series };

struct Entry {
    std::string name, url, group;   // (group so existe durante o parse; a Playlist final nao guarda, para poupar memoria)
    std::string logo;               // tvg-logo
    Kind kind = Kind::Channel;
    int season = 0, episode = 0;    // so para series
};

// Categoria (group-title da lista). Filmes/canais: indices em Playlist::movies / channels. Series: nomes (chave de seriesTree).
struct Category {
    std::string name;
    std::vector<uint32_t> items;     // so canais e filmes
    std::vector<std::string> series; // so series (ordenadas)
    size_t size() const { return items.empty() ? series.size() : items.size(); }
};

struct Playlist {
    std::vector<Entry> channels, movies;
    std::vector<Category> channelCats, movieCats, seriesCats;   // na ordem em que aparecem na lista
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
    std::unordered_map<std::string, int> chanIdx_, movieIdx_, seriesIdx_;   // nome limpo da categoria -> posicao
    static int catFor(std::vector<Category> &cats, std::unordered_map<std::string, int> &idx, const std::string &group);
};

// Nome de categoria para mostrar: tira o prefixo do tipo ("FILMES | Acao" -> "Acao"); vazio vira "Sem categoria".
std::string cleanCategory(const std::string &group);

// Versoes "tudo de uma vez" (testes e compatibilidade).
std::vector<Entry> parseM3U(const std::string &text);
Playlist buildPlaylist(const std::string &text);
bool parseEpisode(const std::string &name, std::string &series, int &season, int &episode);
