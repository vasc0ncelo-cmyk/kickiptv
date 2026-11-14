#pragma once
// API OFICIAL da Kick (https://docs.kick.com) para listar as lives que estao no ar, mais favoritos e historico de streamers.
//  - Token de app (OAuth "client credentials"): POST https://id.kick.com/oauth/token  -> dura ~1 h, sem refresh (pede outro).
//  - Lives: GET https://api.kick.com/public/v1/livestreams?limit=..&sort=viewer_count  (header Authorization: Bearer ...)
//  - A API oficial NAO entrega o endereco do video (HLS): para assistir continua valendo kick.hpp / App::watchKick.
//  - Independente de plataforma: a rede entra por 'Transport' (testavel no PC). Leitura de JSON tolerante a campos que faltam.
#include <stdint.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "json.hpp"
#include "kick.hpp"

namespace kickapi {

struct Live {
    std::string slug, title, category, language, thumb;
    long long viewers = 0;
    bool live = true;
};

// Metodo, endereco, cabecalho Authorization ("" = nenhum), corpo (POST) -> resposta. Retorna true se falou com o servidor; 'status' = HTTP.
using Transport = std::function<bool(const char *method, const std::string &url, const std::string &authHeader,
                                     const std::string &body, std::string &out, int &status)>;

inline std::string urlEncode(const std::string &s) {
    static const char *hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
    }
    return o;
}

inline std::string tokenBody(const std::string &id, const std::string &secret) {
    return "grant_type=client_credentials&client_id=" + urlEncode(id) + "&client_secret=" + urlEncode(secret);
}

inline std::string livesUrl(int limit = 50, const std::string &language = "", long long categoryId = 0) {
    std::string u = "https://api.kick.com/public/v1/livestreams?limit=" + std::to_string(std::max(1, std::min(100, limit))) + "&sort=viewer_count";
    if (!language.empty()) u += "&language=" + urlEncode(language);
    if (categoryId > 0) u += "&category_id=" + std::to_string(categoryId);
    return u;
}

// "access_token":"...","expires_in":3600
inline bool parseToken(const std::string &body, std::string &token, long long &expiresIn) {
    Json j;
    if (!jsonParse(body, j) || !j.isObj()) return false;
    token = j.str("access_token");
    expiresIn = j.num("expires_in", 3600);
    return !token.empty();
}

// {"data":[{"slug":..,"stream_title":..,"viewer_count":..,"category":{"name":..},"language":..,"thumbnail":..}], "message":"OK"}
// Tolerante: aceita array direto, "channel_slug"/"slug", "title"/"stream_title", "viewers"/"viewer_count"; pula itens sem slug valido.
inline std::vector<Live> parseLives(const std::string &body) {
    std::vector<Live> out;
    Json j;
    if (!jsonParse(body, j)) return out;
    const Json *arr = nullptr;
    if (j.isArr()) arr = &j;
    else if (const Json *d = j.get("data")) { if (d->isArr()) arr = d; else if (const Json *dd = d->get("data")) if (dd->isArr()) arr = dd; }
    if (!arr) return out;
    for (const Json &e : arr->arr) {
        if (!e.isObj()) continue;
        Live l;
        l.slug = kickSlug(e.str("slug", e.str("channel_slug")));
        if (l.slug.empty()) continue;
        l.title = e.str("stream_title", e.str("title"));
        l.viewers = e.num("viewer_count", e.num("viewers"));
        l.language = e.str("language");
        l.thumb = e.str("thumbnail");
        if (const Json *t = e.get("thumbnail")) if (t->isObj()) l.thumb = t->str("url", t->str("src"));
        if (const Json *c = e.get("category")) { if (c->isObj()) l.category = c->str("name"); else if (c->type == Json::Str) l.category = c->s; }
        out.push_back(l);
    }
    return out;
}

// Canal pelo endpoint nao oficial (mesmo que kick.hpp usa): esta ao vivo? quantos assistem? titulo/categoria?
inline Live parseChannelStatus(const std::string &slug, const std::string &body) {
    Live l; l.slug = slug;
    l.live = kickIsLive(body);
    if (!l.live) { l.viewers = 0; return l; }
    size_t ls = jsonValuePos(body, "livestream");
    std::string sub = ls == std::string::npos ? body : body.substr(ls);
    size_t v = jsonValuePos(sub, "viewer_count");
    if (v != std::string::npos) l.viewers = atoll(sub.c_str() + v);
    l.title = jsonString(sub, "session_title");
    return l;
}

inline std::string fmtViewers(long long n) {
    char b[32];
    if (n >= 10000) snprintf(b, sizeof b, "%.0fk", n / 1000.0);
    else if (n >= 1000) snprintf(b, sizeof b, "%.1fk", n / 1000.0);
    else snprintf(b, sizeof b, "%lld", n);
    return b;
}

// Cliente com token em cache. Erros: 'err' vazio quando ok.
class Client {
public:
    using Clock = std::function<double()>;       // segundos (monotonico); injetavel nos testes
    Client() { clock_ = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }; }
    void setCredentials(const std::string &id, const std::string &secret) { id_ = id; secret_ = secret; token_.clear(); }
    void setClock(Clock c) { clock_ = std::move(c); }
    bool configured() const { return !id_.empty() && !secret_.empty(); }

    bool lives(const Transport &tp, std::vector<Live> &out, std::string &err, int limit = 50, const std::string &lang = "") {
        out.clear(); err.clear();
        if (!configured()) { err = "sem chave da API da Kick"; return false; }
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!ensureToken(tp, err)) return false;
            std::string body; int st = 0;
            if (!tp("GET", livesUrl(limit, lang), "Bearer " + token_, "", body, st)) { err = "sem resposta da Kick"; return false; }
            if (st == 401) { token_.clear(); continue; }                       // token vencido/recusado: pede outro uma vez
            if (st == 429) { err = "Kick pediu para esperar (muitas consultas)"; return false; }
            if (st < 200 || st >= 300) { err = "Kick respondeu HTTP " + std::to_string(st); return false; }
            out = parseLives(body);
            if (out.empty() && body.find("\"data\"") == std::string::npos) { err = "resposta inesperada da Kick"; return false; }
            std::stable_sort(out.begin(), out.end(), [](const Live &a, const Live &b) { return a.viewers > b.viewers; });
            return true;
        }
        err = "a Kick recusou a chave (confira o ID e o segredo)";
        return false;
    }

private:
    bool ensureToken(const Transport &tp, std::string &err) {
        double now = clock_();
        if (!token_.empty() && now < expiresAt_) return true;
        std::string body; int st = 0;
        if (!tp("POST", "https://id.kick.com/oauth/token", "", tokenBody(id_, secret_), body, st)) { err = "sem resposta da Kick (token)"; return false; }
        if (st == 400 || st == 401 || st == 403) { err = "a Kick recusou a chave (confira o ID e o segredo)"; return false; }
        long long ex = 0;
        if (st < 200 || st >= 300 || !parseToken(body, token_, ex)) { token_.clear(); err = "Kick respondeu HTTP " + std::to_string(st) + " no token"; return false; }
        expiresAt_ = now + std::max<long long>(60, ex - 60);                  // renova 1 min antes de vencer
        return true;
    }
    std::string id_, secret_, token_;
    double expiresAt_ = 0;
    Clock clock_;
};

// Lista de slugs guardada como texto (um por linha): favoritos (sem repetir) e historico (mais recente primeiro, limitado).
inline std::vector<std::string> splitSlugs(const std::string &text) {
    std::vector<std::string> v; std::string cur;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            std::string s = kickSlug(cur); cur.clear();
            if (!s.empty() && std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
        } else cur += text[i];
    }
    return v;
}
inline std::string joinSlugs(const std::vector<std::string> &v) { std::string o; for (auto &s : v) { o += s; o += '\n'; } return o; }
inline bool contains(const std::vector<std::string> &v, const std::string &s) { return std::find(v.begin(), v.end(), s) != v.end(); }
// true = agora e favorito
inline bool toggle(std::vector<std::string> &v, const std::string &s) {
    auto it = std::find(v.begin(), v.end(), s);
    if (it != v.end()) { v.erase(it); return false; }
    v.insert(v.begin(), s); return true;
}
inline void remember(std::vector<std::string> &v, const std::string &s, size_t cap = 20) {
    auto it = std::find(v.begin(), v.end(), s);
    if (it != v.end()) v.erase(it);
    v.insert(v.begin(), s);
    if (v.size() > cap) v.resize(cap);
}

}  // namespace kickapi
