#include "m3u.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>

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
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (s < 100000) s = s * 10 + (name[j] - '0'); j++; }
        if (j >= name.size() || std::toupper((unsigned char)name[j]) != 'E') continue;
        j++;
        if (j >= name.size() || !std::isdigit((unsigned char)name[j])) continue;
        int e = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (e < 100000) e = e * 10 + (name[j] - '0'); j++; }
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

// Limites de seguranca: uma linha gigante (lixo/arquivo que nao e M3U) nao pode estourar a memoria.
static const size_t kMaxLine = 8192;
static const size_t kMaxEntries = 200000;

PlaylistBuilder::PlaylistBuilder() {}

void PlaylistBuilder::feed(const char *d, size_t n) {
    size_t i = 0;
    while (i < n) {
        const char *nl = (const char *)memchr(d + i, '\n', n - i);
        size_t end = nl ? (size_t)(nl - d) : n;
        if (!skipRest_) {
            size_t room = kMaxLine > cur_.size() ? kMaxLine - cur_.size() : 0;
            size_t take = std::min(end - i, room);
            cur_.append(d + i, take);
            if (take < end - i) skipRest_ = true;          // linha longa demais: ignora o resto dela
        }
        if (nl) {
            line(cur_);
            cur_.clear();
            skipRest_ = false;
            i = end + 1;
        } else i = n;
    }
}

void PlaylistBuilder::finish() {
    if (!cur_.empty()) { line(cur_); cur_.clear(); }
    for (auto &s : p_.seriesTree)
        for (auto &t : s.second)
            std::stable_sort(t.second.begin(), t.second.end(), [](const Entry &a, const Entry &b) { return a.episode < b.episode; });
}

Playlist PlaylistBuilder::take() { return std::move(p_); }

void PlaylistBuilder::line(std::string &raw) {
    if (first_) {                                           // BOM UTF-8 no inicio do arquivo
        first_ = false;
        if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) raw.erase(0, 3);
    }
    std::string l = trim(raw);
    if (l.empty() || l[0] == '#') {
        if (l.compare(0, 7, "#EXTINF") == 0) extinf_ = l;
        return;                                             // #EXTM3U, #EXTVLCOPT, #EXTGRP etc.
    }
    if (count_ >= kMaxEntries) { extinf_.clear(); return; }
    Entry e;
    e.url = l;
    if (!extinf_.empty()) {
        // o nome vem depois da ultima virgula FORA das aspas (nomes de grupo/canal podem ter virgula)
        size_t q = extinf_.rfind('"');
        size_t c = extinf_.find(',', q == std::string::npos ? 0 : q);
        if (c == std::string::npos) c = extinf_.rfind(',');
        e.name = c == std::string::npos ? "" : trim(extinf_.substr(c + 1));
        e.group = attr(extinf_, "group-title");
        if (e.name.empty()) e.name = attr(extinf_, "tvg-name");
    }
    if (e.name.empty()) e.name = e.url;
    e.kind = classify(e);
    extinf_.clear();
    count_++;

    if (e.kind == Kind::Movie) {
        e.group.clear(); p_.movies.push_back(std::move(e));
    } else if (e.kind == Kind::Series) {
        std::string s; int se = 1, ep = 0;
        if (parseEpisode(e.name, s, se, ep)) { e.season = se; e.episode = ep; }
        else s = e.group.empty() ? e.name : e.group;
        e.group.clear();
        p_.seriesEpisodes++;
        int season = e.season;
        p_.seriesTree[s][season].push_back(std::move(e));
    } else {
        e.group.clear(); p_.channels.push_back(std::move(e));
    }
}

std::vector<Entry> parseM3U(const std::string &text) {   // mantido para testes: devolve tudo numa lista unica
    std::vector<Entry> out;
    PlaylistBuilder b; b.feed(text.data(), text.size()); b.finish();
    Playlist p = b.take();
    for (auto &e : p.channels) out.push_back(e);
    for (auto &e : p.movies) out.push_back(e);
    for (auto &s : p.seriesTree) for (auto &t : s.second) for (auto &e : t.second) out.push_back(e);
    return out;
}

Playlist buildPlaylist(const std::string &text) {
    PlaylistBuilder b; b.feed(text.data(), text.size()); b.finish();
    return b.take();
}
