#pragma once
// Endereco da MINIATURA de uma capa. As listas IPTV costumam apontar para o TMDB com o poster em tamanho enorme
// ("/t/p/original/..." ou "/w600_and_h900_bestv2/..." = 300 KB a 1 MB). A grade usa posters de 148x222 pixels, entao pedimos o tamanho "w154"
// (154x231, ~8-15 KB): baixa e decodifica 20-50x mais rapido. Qualquer outro servidor fica como esta.
#include <string>

inline std::string coverThumbUrl(const std::string &url) {
    static const char kKey[] = "image.tmdb.org/t/p/";
    size_t h = url.find(kKey);
    if (h == std::string::npos) return url;
    size_t s = h + sizeof(kKey) - 1;
    size_t e = url.find('/', s);
    if (e == std::string::npos || e == s) return url;
    std::string size = url.substr(s, e - s);
    if (size == "w154" || size == "w92") return url;             // ja e pequeno
    bool tmdbSize = size == "original";
    if (!tmdbSize && (size[0] == 'w' || size[0] == 'h')) {       // w185, w500, w600_and_h900_bestv2, h632...
        tmdbSize = true;
        for (char c : size) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) tmdbSize = false;
    }
    if (!tmdbSize) return url;
    return url.substr(0, s) + "w154" + url.substr(e);
}
