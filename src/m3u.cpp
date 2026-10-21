#include "m3u.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static std::string attr(const std::string &line, const std::string &key) {
    std::string k = key + "=\"";
    size_t p = line.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = line.find('"', p);
    return e == std::string::npos ? "" : line.substr(p, e - p);
}
static bool has(const std::string &h, const char *n) { return h.find(n) != std::string::npos; }

// Procura "S01E02" (case-insensitive) no nome.
bool parseEpisode(const std::string &name, std::string &series, int &season, int &episode) {
    for (size_t i = 0; i + 3 < name.size(); i++) {
        if (std::toupper((unsigned char)name[i]) != 'S' || !std::isdigit((unsigned char)name[i + 1])) continue;
        size_t j = i + 1; int s = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) s = s * 10 + (name[j++] - '0');
        if (j >= name.size() || std::toupper((unsigned char)name[j]) != 'E') continue;
        j++;
        if (j >= name.size() || !std::isdigit((unsigned char)name[j])) continue;
        int e = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) e = e * 10 + (name[j++] - '0');
        series = trim(name.substr(0, i));
        while (!series.empty() && (series.back() == '-' || series.back() == ':')) series.pop_back();
        series = trim(series);
        season = s; episode = e;
        return !series.empty();
    }
    return false;
}

static Kind classify(const Entry &e) {
    std::string u = lower(e.url), g = lower(e.group);
    std::string s; int a, b;
    if (has(u, "/series/") || has(g, "serie") || has(g, "temporada") || parseEpisode(e.name, s, a, b)) return Kind::Series;
    if (has(u, "/movie/") || has(g, "filme") || has(g, "movie") || has(g, "vod") || has(g, "cine")) return Kind::Movie;
    return Kind::Channel;
}

std::vector<Entry> parseM3U(const std::string &text) {
    std::vector<Entry> out;
    std::istringstream in(text);
    std::string line, extinf;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line == "#EXTM3U") continue;
        if (line.rfind("#EXTINF", 0) == 0) { extinf = line; continue; }
        if (line[0] == '#') continue;
        Entry e;
        e.url = line;
        if (!extinf.empty()) {
            size_t c = extinf.rfind(',');
            e.name = c == std::string::npos ? "" : trim(extinf.substr(c + 1));
            e.group = attr(extinf, "group-title");
            e.logo = attr(extinf, "tvg-logo");
            e.tvgId = attr(extinf, "tvg-id");
            if (e.name.empty()) e.name = attr(extinf, "tvg-name");
        }
        if (e.name.empty()) e.name = e.url;
        e.kind = classify(e);
        out.push_back(e);
        extinf.clear();
    }
    return out;
}

Playlist buildPlaylist(const std::string &text) {
    Playlist p;
    for (auto &e : parseM3U(text)) {
        if (e.kind == Kind::Movie) p.movies.push_back(e);
        else if (e.kind == Kind::Series) {
            std::string s; int se = 1, ep = 0;
            if (parseEpisode(e.name, s, se, ep)) { e.seriesName = s; e.season = se; e.episode = ep; }
            else { e.seriesName = e.group.empty() ? e.name : e.group; }
            p.seriesTree[e.seriesName][e.season].push_back(e);
            p.series.push_back(e);
        } else p.channels.push_back(e);
    }
    for (auto &s : p.seriesTree)
        for (auto &t : s.second)
            std::sort(t.second.begin(), t.second.end(), [](const Entry &a, const Entry &b) { return a.episode < b.episode; });
    return p;
}
