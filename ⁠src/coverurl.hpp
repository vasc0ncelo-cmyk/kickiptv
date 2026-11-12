#pragma once
// Endereco da MINIATURA de uma capa. As listas IPTV costumam apontar para o TMDB com o poster em tamanho enorme
// ("/t/p/original/..." ou "/w600_and_h900_bestv2/..." = 300 KB a 1 MB). A grade usa posters de 148x222 pixels, entao pedimos o tamanho "w154"
// (154x231, ~8-15 KB): baixa e decodifica 20-50x mais rapido. Qualquer outro servidor fica como esta.
//
// VARIANTES: a pagina de detalhes precisa de imagens maiores (poster ~390 px, miniatura de episodio em paisagem). A chave da capa
// ganha o sufixo "#LARGURAxALTURA" (coverVariantKey); o cache decodifica nesse tamanho e coverFetchUrl pede ao TMDB uma imagem maior.
#include <string>

// Troca o tamanho no endereco do TMDB (".../t/p/<tamanho>/..."). Devolve vazio se nao for um endereco do TMDB com tamanho reconhecivel.
inline std::string coverTmdbResize(const std::string &url, const std::string &want, bool keepSmall) {
    static const char kKey[] = "image.tmdb.org/t/p/";
    size_t h = url.find(kKey);
    if (h == std::string::npos) return "";
    size_t s = h + sizeof(kKey) - 1;
    size_t e = url.find('/', s);
    if (e == std::string::npos || e == s) return "";
    std::string size = url.substr(s, e - s);
    if (keepSmall && (size == "w154" || size == "w92")) return url;     // ja e pequeno
    bool tmdbSize = size == "original";
    if (!tmdbSize && (size[0] == 'w' || size[0] == 'h')) {              // w185, w500, w600_and_h900_bestv2, h632...
        tmdbSize = true;
        for (char c : size) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) tmdbSize = false;
    }
    if (!tmdbSize) return "";
    return url.substr(0, s) + want + url.substr(e);
}

inline std::string coverThumbUrl(const std::string &url) {
    std::string r = coverTmdbResize(url, "w154", true);
    return r.empty() ? url : r;
}

// ---- variantes de tamanho ----
inline std::string coverVariantKey(const std::string &url, int w, int h) {
    if (url.empty() || w <= 0 || h <= 0) return url;
    return url + "#" + std::to_string(w) + "x" + std::to_string(h);
}
// "url#390x400" -> url, 390, 400. Sem sufixo valido: devolve false (url = a propria chave, w = h = 0).
inline bool coverSplitVariant(const std::string &key, std::string &url, int &w, int &h) {
    url = key; w = h = 0;
    size_t hash = key.rfind('#');
    if (hash == std::string::npos) return false;
    size_t x = key.find('x', hash + 1);
    if (x == std::string::npos || x == hash + 1 || x + 1 >= key.size()) return false;
    long long a = 0, b = 0;
    for (size_t i = hash + 1; i < x; i++) { if (key[i] < '0' || key[i] > '9') return false; a = a * 10 + (key[i] - '0'); if (a > 4096) return false; }
    for (size_t i = x + 1; i < key.size(); i++) { if (key[i] < '0' || key[i] > '9') return false; b = b * 10 + (key[i] - '0'); if (b > 4096) return false; }
    if (a < 8 || b < 8) return false;
    url = key.substr(0, hash); w = (int)a; h = (int)b;
    return true;
}
// O que baixar para uma chave de capa (com ou sem variante). TMDB: poster/quadrado -> w500, paisagem -> w300; outros servidores: o endereco como esta.
inline std::string coverFetchUrl(const std::string &key) {
    std::string url; int w, h;
    if (!coverSplitVariant(key, url, w, h)) return coverThumbUrl(key);
    std::string r = coverTmdbResize(url, w > h ? "w300" : "w500", false);
    return r.empty() ? url : r;
}
