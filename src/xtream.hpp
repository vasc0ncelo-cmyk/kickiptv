#pragma once
// Paineis IPTV "Xtream Codes": alem da lista M3U gigante (get.php), eles tem uma API (player_api.php) que entrega as
// categorias e, so quando voce abre uma categoria, os itens dela. Para provedores com 200 mil+ itens e a unica forma viavel no PS4.
#include <string>

struct XtreamAcct {
    std::string base, user, pass;   // base = http://host:porta
    bool valid() const { return !base.empty() && !user.empty() && !pass.empty(); }
};

inline std::string xtQueryParam(const std::string &url, const std::string &key) {
    size_t q = url.find('?');
    if (q == std::string::npos) return "";
    size_t p = q + 1;
    while (p < url.size()) {
        size_t amp = url.find('&', p);
        size_t end = amp == std::string::npos ? url.size() : amp;
        if (url.compare(p, key.size() + 1, key + "=") == 0) return url.substr(p + key.size() + 1, end - p - key.size() - 1);
        p = end + 1;
    }
    return "";
}

// Aceita ...get.php?username=U&password=P&type=m3u_plus  (e variantes com playlist.php / panel_api.php)
inline bool xtreamParse(const std::string &url, XtreamAcct &a) {
    a = XtreamAcct();
    size_t se = url.find("://");
    if (se == std::string::npos) return false;
    size_t he = url.find('/', se + 3);
    std::string base = he == std::string::npos ? url : url.substr(0, he);
    size_t qm = base.find('?');
    if (qm != std::string::npos) base = base.substr(0, qm);
    a.base = base; a.user = xtQueryParam(url, "username"); a.pass = xtQueryParam(url, "password");
    return a.valid();
}

inline std::string xtreamApi(const XtreamAcct &a, const std::string &action = "", const std::string &extra = "") {
    std::string u = a.base + "/player_api.php?username=" + a.user + "&password=" + a.pass;
    if (!action.empty()) u += "&action=" + action;
    if (!extra.empty()) u += "&" + extra;
    return u;
}
// Canal ao vivo: MPEG-TS direto (igual ao que o painel entrega no get.php?output=ts). O player do app comeca a tocar em ~2 s
// e, se o painel nao entregar TS, tenta o HLS (.m3u8) equivalente sozinho.
inline std::string xtreamLiveUrl(const XtreamAcct &a, const std::string &id) { return a.base + "/live/" + a.user + "/" + a.pass + "/" + id + ".ts"; }
inline std::string xtreamMovieUrl(const XtreamAcct &a, const std::string &id, const std::string &ext) {
    return a.base + "/movie/" + a.user + "/" + a.pass + "/" + id + "." + (ext.empty() ? "mp4" : ext);
}
inline std::string xtreamEpisodeUrl(const XtreamAcct &a, const std::string &id, const std::string &ext) {
    return a.base + "/series/" + a.user + "/" + a.pass + "/" + id + "." + (ext.empty() ? "mp4" : ext);
}
