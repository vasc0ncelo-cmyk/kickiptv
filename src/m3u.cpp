#include "m3u.hpp"
#include "urlutil.hpp"
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

// Minusculas e sem acento (UTF-8 comum do portugues/espanhol): "SÉRIES" -> "series", "Lançamentos" -> "lancamentos".
// Sem isso o grupo "SÉRIES | NETFLIX" nunca casava com "serie" e tudo caia na aba errada.
static std::string fold(const std::string &s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[++i];
            unsigned char u = d >= 0xA0 ? (unsigned char)(d - 0x20) : d;      // minuscula -> maiuscula
            char r = '?';
            if (u >= 0x80 && u <= 0x85) r = 'a';
            else if (u == 0x87) r = 'c';
            else if (u >= 0x88 && u <= 0x8B) r = 'e';
            else if (u >= 0x8C && u <= 0x8F) r = 'i';
            else if (u == 0x91) r = 'n';
            else if (u >= 0x92 && u <= 0x96) r = 'o';
            else if (u >= 0x99 && u <= 0x9C) r = 'u';
            o += r;
        } else o += (char)std::tolower(c);
    }
    return o;
}
// palavra inteira (evita "live" dentro de "deliver", "tv" dentro de "gtv"...)
static bool hasWord(const std::string &h, const char *w) {
    size_t n = strlen(w);
    for (size_t p = h.find(w); p != std::string::npos; p = h.find(w, p + 1)) {
        bool l = p == 0 || !std::isalnum((unsigned char)h[p - 1]);
        bool r = p + n >= h.size() || !std::isalnum((unsigned char)h[p + n]);
        if (l && r) return true;
    }
    return false;
}

// Procura "S01E02" (tambem "S01 E02", "S01.E02", "T01E02") no nome.
bool parseEpisode(const std::string &name, std::string &series, int &season, int &episode) {
    // 1) S01E02 / T01E02 / S1.E2 / S01 E02
    for (size_t i = 0; i + 3 < name.size(); i++) {
        char c0 = (char)std::toupper((unsigned char)name[i]);
        if ((c0 != 'S' && c0 != 'T') || !std::isdigit((unsigned char)name[i + 1])) continue;
        if (i > 0 && std::isalnum((unsigned char)name[i - 1])) continue;
        size_t j = i + 1; int s = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (s < 100000) s = s * 10 + (name[j] - '0'); j++; }
        if (j < name.size() && (name[j] == ' ' || name[j] == '.' || name[j] == '_' || name[j] == '-')) j++;
        if (j >= name.size() || std::toupper((unsigned char)name[j]) != 'E') continue;
        j++;
        if (j >= name.size() || !std::isdigit((unsigned char)name[j])) continue;
        int e = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (e < 100000) e = e * 10 + (name[j] - '0'); j++; }
        series = trim(name.substr(0, i));
        while (!series.empty() && (series.back() == '-' || series.back() == ':' || series.back() == '|' || series.back() == ' ')) series.pop_back();
        series = trim(series);
        if (s < 1) s = 1;
        if (e < 1) e = 1;
        season = s; episode = e;
        return !series.empty();
    }
    // 2) 1x02 / 12x05
    for (size_t i = 0; i + 3 < name.size(); i++) {
        if (!std::isdigit((unsigned char)name[i])) continue;
        if (i > 0 && std::isalnum((unsigned char)name[i - 1])) continue;
        size_t j = i; int s = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (s < 1000) s = s * 10 + (name[j] - '0'); j++; }
        if (j >= name.size() || (name[j] != 'x' && name[j] != 'X')) continue;
        j++;
        if (j >= name.size() || !std::isdigit((unsigned char)name[j])) continue;
        int e = 0;
        while (j < name.size() && std::isdigit((unsigned char)name[j])) { if (e < 100000) e = e * 10 + (name[j] - '0'); j++; }
        if (s < 1 || e < 1) continue;
        series = trim(name.substr(0, i));
        while (!series.empty() && (series.back() == '-' || series.back() == ':' || series.back() == '|')) series.pop_back();
        series = trim(series);
        season = s; episode = e;
        return !series.empty();
    }
    return false;
}

// Decide a aba. Ordem: 1) caminho da URL (paineis Xtream: /movie/ /series/ /live/ e decisivo)
// 2) grupo (canais primeiro: "CANAIS | FILMES" e "24H SERIES" sao canais ao vivo, nao filmes/series)
// 3) "S01E02" no nome  4) extensao de arquivo de video  5) padrao: canal.
// titulo comparavel: minusculas, sem ano/qualidade/pontuacao extra
static std::string normTitle(const std::string &name) {
    std::string s = fold(name);
    // remove (2019) [2020] {2021}
    std::string out; out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '(' || s[i] == '[' || s[i] == '{') {
            char close = s[i] == '(' ? ')' : (s[i] == '[' ? ']' : '}');
            size_t j = s.find(close, i);
            if (j != std::string::npos) { i = j + 1; continue; }
        }
        char c = s[i++];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        // ignora espacos e pontuacao para juntar "O Auto da Compadecida" variantes
    }
    // remove sufixos comuns de qualidade
    const char *suf[] = {"1080p","720p","480p","2160p","4k","fhd","hd","cam","legendado","dublado","dualaudio","bluray","webdl","webrip","hdtv"};
    for (const char *x : suf) {
        size_t pos = out.find(x);
        if (pos != std::string::npos) out.erase(pos, std::strlen(x));
    }
    return out;
}

static Kind classify(const Entry &e) {
    std::string u = lower(e.url), g = fold(e.group), n = fold(e.name);
    // 1) caminho da URL (Xtream) — decisivo
    if (has(u, "/series/")) return Kind::Series;
    if (has(u, "/movie/") || has(u, "/movies/") || has(u, "/vod/")) return Kind::Movie;
    if (has(u, "/live/") || has(u, "/streaming/")) return Kind::Channel;

    // 2) grupo de CANAIS (mesmo se o nome parecer filme/serie — "24H SERIES" e canal ao vivo)
    if (hasWord(g, "canais") || hasWord(g, "canal") || hasWord(g, "channels") || hasWord(g, "live") ||
        has(g, "tv aberta") || has(g, "tv fechada") || has(g, "ao vivo") || has(g, "24h") || has(g, "24 h") ||
        has(g, "esporte") || has(g, "sport") || has(g, "noticia") || has(g, "abertos") || has(g, "radio") ||
        has(g, "ppv") || has(g, "premiere") || has(g, "infantil tv") || has(g, "globo") || has(g, "sbt") ||
        has(g, "record") || has(g, "band") || has(g, "espn") || has(g, "fox sports")) return Kind::Channel;
    if (n.compare(0, 3, "24h") == 0 || n.compare(0, 4, "24 h") == 0) return Kind::Channel;

    std::string ext = urlExtension(u), s; int a, b;
    bool vodExt = ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "wmv" || ext == "flv" ||
                  ext == "mpg" || ext == "mpeg" || ext == "m4v" || ext == "webm";
    // 3) series por grupo ou SxxExx em VOD
    if (has(g, "serie") || has(g, "temporada") || has(g, "season") || has(g, "novela") || has(g, "anime") ||
        has(g, "dorama") || hasWord(g, "doramas") || has(g, "episod")) return Kind::Series;
    if (parseEpisode(e.name, s, a, b) && (vodExt || has(g, "filme") || has(g, "movie") || has(g, "vod"))) return Kind::Series;
    // 4) filmes por grupo
    if (has(g, "filme") || has(g, "movie") || has(g, "cine") || has(g, "vod") || has(g, "lancamento") ||
        has(g, "legendado") || has(g, "dublado") || has(g, "film") || has(g, "cinema")) return Kind::Movie;
    // 5) extensao de video sem grupo de canal = filme; .ts / sem extensao = canal
    if (vodExt) return Kind::Movie;
    if (ext == "ts" || ext == "m3u8" || ext.empty()) return Kind::Channel;
    return Kind::Channel;
}

// "FILMES | Acao", "Series - Netflix", "CANAIS: Esportes" -> "Acao", "Netflix", "Esportes" (so tira prefixos que dizem o tipo).
std::string cleanCategory(const std::string &group) {
    std::string g = trim(group);
    static const char *types[] = {"filmes", "filme", "series", "serie", "canais", "canal", "movies", "movie", "vod", "live", "tv", "novelas", "animes", "ao vivo"};
    for (size_t i = 0; i < g.size(); i++) {
        size_t skip = 0;
        if (g[i] == '|' || g[i] == ':' || g[i] == '>' || g[i] == '/') skip = 1;
        else if (g[i] == '-' && i > 0 && g[i - 1] == ' ' && i + 1 < g.size() && g[i + 1] == ' ') skip = 1;
        if (!skip) continue;
        std::string left = trim(fold(g.substr(0, i))), right = trim(g.substr(i + skip));
        bool isType = false;
        for (const char *t : types) if (left == t) { isType = true; break; }
        if (isType && !right.empty()) { g = right; break; }
        if (!isType) break;     // o primeiro separador decide
    }
    return g.empty() ? "Sem categoria" : g;
}

int PlaylistBuilder::catFor(std::vector<Category> &cats, std::unordered_map<std::string, int> &idx, const std::string &group) {
    std::string name = cleanCategory(group);
    auto it = idx.find(name);
    if (it != idx.end()) return it->second;
    cats.push_back(Category()); cats.back().name = name;
    return idx[name] = (int)cats.size() - 1;
}

// Limites de seguranca: uma linha gigante (lixo/arquivo que nao e M3U) nao pode estourar a memoria.
static const size_t kMaxLine = 8192;
// (sem limite de itens: a lista carrega inteira. Se a memoria acabar, App::loadM3U trata o bad_alloc e aproveita o que ja foi lido.)

PlaylistBuilder::PlaylistBuilder() {}

void PlaylistBuilder::feed(const char *d, size_t n) {
    if (full_) return;                                     // orcamento de memoria cheio: ignora o resto (nem varre os bytes)
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
    for (auto &c : p_.seriesCats) std::sort(c.series.begin(), c.series.end());

    // Categorias A-Z (rapido). Itens dentro de cada categoria ficam na ordem do M3U
    // (ordenar 40k+ nomes em cada categoria deixava a abertura muito lenta).
    auto byCatName = [](const Category &a, const Category &b) { return fold(a.name) < fold(b.name); };
    std::sort(p_.movieCats.begin(), p_.movieCats.end(), byCatName);
    std::sort(p_.seriesCats.begin(), p_.seriesCats.end(), byCatName);
    std::sort(p_.channelCats.begin(), p_.channelCats.end(), byCatName);

    for (auto &c : p_.movieCats) if (c.name == "Todos") c.name = "Geral";
    for (auto &c : p_.seriesCats) if (c.name == "Todos") c.name = "Geral";
    for (auto &c : p_.channelCats) if (c.name == "Todos") c.name = "Geral";

    // "Todos" = lista completa; ordena SO uma vez (nao cada categoria)
    auto byNameMovie = [&](uint32_t a, uint32_t b) {
        return fold(p_.movies[a].name) < fold(p_.movies[b].name);
    };
    auto byNameChan = [&](uint32_t a, uint32_t b) {
        return fold(p_.channels[a].name) < fold(p_.channels[b].name);
    };
    // "Todos" SEM ordenar 40k nomes (isso sozinho demorava dezenas de segundos no PS4)
    if (!p_.movies.empty()) {
        Category all;
        all.name = "Todos";
        all.items.resize(p_.movies.size());
        for (uint32_t i = 0; i < (uint32_t)p_.movies.size(); i++) all.items[i] = i;
        p_.movieCats.insert(p_.movieCats.begin(), std::move(all));
    }
    if (!p_.seriesTree.empty()) {
        Category all;
        all.name = "Todos";
        for (const auto &kv : p_.seriesTree) all.series.push_back(kv.first);
        p_.seriesCats.insert(p_.seriesCats.begin(), std::move(all));
    }
    if (!p_.channels.empty()) {
        Category all;
        all.name = "Todos";
        all.items.resize(p_.channels.size());
        for (uint32_t i = 0; i < (uint32_t)p_.channels.size(); i++) all.items[i] = i;
        p_.channelCats.insert(p_.channelCats.begin(), std::move(all));
    }
    (void)byNameMovie; (void)byNameChan;
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
    if (full_) { extinf_.clear(); return; }
    if (l.find("://") == std::string::npos) { extinf_.clear(); return; }   // nao e um link (pagina de erro/HTML, lixo): nao vira item
    Entry e;
    e.url = l;
    if (!extinf_.empty()) {
        // o nome vem depois da ultima virgula FORA das aspas (nomes de grupo/canal podem ter virgula)
        size_t q = extinf_.rfind('"');
        size_t c = extinf_.find(',', q == std::string::npos ? 0 : q);
        if (c == std::string::npos) c = extinf_.rfind(',');
        e.name = c == std::string::npos ? "" : trim(extinf_.substr(c + 1));
        e.group = attr(extinf_, "group-title");
        e.logo = attr(extinf_, "tvg-logo");
        if (e.logo.empty()) e.logo = attr(extinf_, "logo");
        if (e.name.empty()) e.name = attr(extinf_, "tvg-name");
    }
    if (e.name.empty()) e.name = e.url;
    {   // orcamento de memoria: struct + 3 alocacoes de texto + indices
        size_t est = sizeof(Entry) + 3 * 32 + e.name.size() + e.url.size() + e.logo.size() + 16;
        if (memBytes_ + est > maxBytes()) { full_ = true; p_.truncated = true; extinf_.clear(); return; }
        memBytes_ += est;
    }
    // duplicata por URL OU mesmo titulo normalizado (lista IPTV repete o filme 2-3x)
    if (!e.url.empty() && !seenUrl_.insert(e.url).second) {
        extinf_.clear();
        return;
    }
    {
        std::string nt = normTitle(e.name);
        if (nt.size() >= 4 && !seenName_.insert(nt).second) {
            extinf_.clear();
            return;
        }
    }
    e.kind = classify(e);
    extinf_.clear();
    count_++;

    if (e.kind == Kind::Movie) {
        p_.movieCats[catFor(p_.movieCats, movieIdx_, e.group)].items.push_back((uint32_t)p_.movies.size());
        e.group.clear(); p_.movies.push_back(std::move(e));
    } else if (e.kind == Kind::Series) {
        std::string s; int se = 1, ep = 0;
        if (parseEpisode(e.name, s, se, ep)) { e.season = se; e.episode = ep; }
        else {
            s = e.group.empty() ? e.name : e.group;
            if (e.season < 1) e.season = 1;
        }
        std::string key = normTitle(s);
        if (key.size() < 3) key = s;
        std::string treeName = s.empty() ? key : s;
        for (auto &kv : p_.seriesTree) {
            if (normTitle(kv.first) == key) { treeName = kv.first; break; }
        }
        bool isNew = p_.seriesTree.find(treeName) == p_.seriesTree.end();
        if (isNew) {
            p_.seriesCats[catFor(p_.seriesCats, seriesIdx_, e.group)].series.push_back(treeName);
        }
        e.group.clear();
        p_.seriesEpisodes++;
        int season = e.season < 1 ? 1 : e.season;
        e.season = season;
        p_.seriesTree[treeName][season].push_back(std::move(e));
    } else {
        p_.channelCats[catFor(p_.channelCats, chanIdx_, e.group)].items.push_back((uint32_t)p_.channels.size());
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
