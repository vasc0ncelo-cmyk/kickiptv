#pragma once
// Metadados que o PROPRIO PAINEL entrega (sinopse, nota, generos, duracao, ano): o painel ja consultou o TMDB e devolve tudo pronto
// em get_vod_info / get_series_info. Funcoes puras (testaveis no PC). Campo ausente = fica vazio (nada e inventado).
#include <cstdlib>
#include <string>
#include <vector>

#include "detailmeta.hpp"
#include "json.hpp"

// id do filme na URL do painel: ".../movie/user/pass/12345.mp4" -> "12345" ("" se a URL nao tem esse formato)
inline std::string xtMovieIdFromUrl(const std::string &url) {
    size_t m = url.find("/movie/");
    if (m == std::string::npos) return "";
    size_t s = url.find_last_of('/');
    if (s == std::string::npos || s < m + 7) return "";
    std::string seg = url.substr(s + 1);
    size_t q = seg.find_first_of("?#"); if (q != std::string::npos) seg.erase(q);
    size_t d = seg.find('.'); if (d != std::string::npos) seg.erase(d);
    if (seg.empty()) return "";
    for (char c : seg) if (c < '0' || c > '9') return "";
    return seg;
}

// Duracao em minutos: duration_secs, ou "01:57:00", ou runtime / episode_run_time em minutos.
inline int xtDurationMin(const Json &info) {
    long long secs = info.num("duration_secs", 0);
    if (secs > 0) return (int)((secs + 30) / 60);
    std::string d = info.str("duration");
    int p[3] = {0, 0, 0}, n = 0; bool digits = false;
    for (size_t i = 0; i <= d.size() && n < 3; i++) {
        char c = i < d.size() ? d[i] : ':';
        if (c >= '0' && c <= '9') { p[n] = p[n] * 10 + (c - '0'); digits = true; }
        else if (c == ':') { if (digits) n++; digits = false; }
        else { n = 0; break; }
    }
    if (n == 3) { int m = p[0] * 60 + p[1] + (p[2] >= 30 ? 1 : 0); if (m > 0) return m; }
    else if (n == 2) { if (p[0] > 0) return p[0] + (p[1] >= 30 ? 1 : 0); }
    long long r = info.num("runtime", 0); if (r > 0 && r < 1000) return (int)r;
    r = info.num("episode_run_time", 0); if (r > 0 && r < 1000) return (int)r;
    return 0;
}

// Preenche o que o painel trouxe. Generos do painel substituem o chip de categoria; ano da lista/nome so e trocado se estava vazio.
inline void xtFillMeta(const Json &info, DetailMeta &m) {
    if (!info.isObj()) return;
    std::string plot = dm::trimA(info.str("plot"));
    if (plot.empty()) plot = dm::trimA(info.str("description"));
    if (plot.empty()) plot = dm::trimA(info.str("overview"));
    if (!plot.empty()) m.plot = plot;
    std::string g = info.str("genre");
    if (!g.empty()) {
        std::vector<std::string> gs; std::string cur;
        for (size_t i = 0; i <= g.size(); i++) {
            char c = i < g.size() ? g[i] : ',';
            if (c == ',' || c == '/' || c == '|') { cur = dm::trimA(cur); if (!cur.empty() && cur.size() <= 24 && gs.size() < 4) gs.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!gs.empty()) m.genres = gs;
    }
    double r = 0;
    { std::string rs = info.str("rating"); if (!rs.empty()) r = atof(rs.c_str()); }
    if (r <= 0) { std::string r5 = info.str("rating_5based"); if (!r5.empty()) r = atof(r5.c_str()) * 2.0; }
    if (r > 10.0) r = 10.0;
    if (r > 0) { char b[16]; snprintf(b, sizeof b, "%.1f", r); m.rating = b; }
    std::string y = info.str("releasedate");
    if (y.empty()) y = info.str("release_date");
    if (y.empty()) y = info.str("year");
    if (y.size() >= 4 && dm::isYear(y.substr(0, 4)) && m.year.empty()) m.year = y.substr(0, 4);
    int mins = xtDurationMin(info);
    if (mins > 0) m.runtimeMin = mins;
}
