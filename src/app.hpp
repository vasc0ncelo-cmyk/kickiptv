#pragma once
// Logica do app independente de plataforma (testavel no PC).
#include "kick.hpp"
#include "m3u.hpp"
#include "xtload.hpp"
#include "xtmeta.hpp"
#include <algorithm>
#include <unordered_map>
#include <functional>
#include <new>
#include <cstdio>
#include <string>
#include <vector>

enum class Tab { Kick = 0, Channels = 1, Movies = 2, Series = 3 };

// A camada PS4 (src/ps4_platform.cpp) implementa isto.
struct Platform {
    virtual ~Platform() {}
    virtual bool httpGet(const std::string &url, std::string &body) = 0;
    // Download grande (lista M3U): entrega em pedacos para 'sink' (false = cancelar) em vez de montar tudo na memoria.
    // true = baixou tudo. A versao padrao usa httpGet (testes / plataformas simples).
    virtual bool httpStream(const std::string &url, const std::function<bool(const char *, size_t)> &sink) {
        std::string body;
        if (!httpGet(url, body)) return false;
        return sink(body.data(), body.size());
    }
    virtual bool wasCancelled() { return false; }   // o ultimo httpStream foi cancelado pelo usuario?
    virtual void play(const std::string &url, const std::string &title) = 0;
    // Opcional: lista de episodios da temporada (para L1/R1 pular no player). Padrao: limpa.
    virtual void setEpisodeList(const std::vector<std::string> & /*urls*/,
                                const std::vector<std::string> & /*titles*/, int /*index*/) {}
    virtual void requestText(const std::string &prompt) = 0; // abre o teclado (IME); depois chame UI::onText
    // Consulta a API da Kick (o Cloudflare da Kick recusa clientes que "parecem robo": a plataforma pode tentar outros cabecalhos).
    virtual bool httpGetKick(const std::string &url, std::string &body) { return httpGet(url, body); }
    virtual int lastHttpStatus() { return 0; }                                       // status HTTP da ultima consulta (0 = sem resposta)
    virtual std::string loadSetting(const std::string &) { return ""; }              // dados guardados (ex.: "m3u", "kick")
    virtual void saveSetting(const std::string &, const std::string &) {}
    // Copia local da lista M3U (para nao baixar tudo a cada abertura do app).
    // cacheLoad: entrega em pedacos a copia salva DESTA url; false = nao existe / url diferente / cancelado / erro.
    virtual bool cacheLoad(const std::string & /*url*/, const std::function<bool(const char *, size_t)> & /*sink*/) { return false; }
    virtual void cacheBegin() {}                                   // vai comecar um download: comece a gravar
    virtual void cacheWrite(const char * /*d*/, size_t /*n*/) {}   // pedaco baixado, em ordem
    virtual void cacheEnd(const std::string & /*url*/, bool /*complete*/) {}   // complete=true: a copia vira a lista salva (senao descarta e mantem a antiga)
    virtual void cacheClear() {}                                   // apaga a copia salva
    virtual void busy(const std::string & /*msg*/) {}              // vai fazer uma consulta de rede que bloqueia: desenhe "carregando" agora
};

// Painel Xtream ativo: so as categorias estao na memoria; os itens vem da API quando a categoria abre.
struct XtState { bool active = false; XtreamAcct acct; };

struct App {
    Platform &pf;
    Playlist list;
    std::string lastStatus = "Pronto";
    XtState xt;
    XtreamAcct metaAcct;                                   // conta do painel para sinopse/nota (tambem com lista M3U comum)
    std::unordered_map<std::string, DetailMeta> metaCache; // "m:<id>" filmes, "s:<id>" series
    explicit App(Platform &p) : pf(p) {}

    void say(const std::string &m) { lastStatus = m; }

    static std::string fmtMB(size_t bytes) {
        char b[32]; snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0); return b;
    }

    // useCache=true (abrir o app): usa a copia salva no console, se houver (sem baixar nada).
    // useCache=false (usuario confirmou o link): baixa de novo e atualiza a copia salva.
    // Conexao com painel Xtream: guarda so as categorias (ver xtload.hpp).
    void useXtream(const XtreamAcct &a, Playlist &&p) {
        xt.active = true; xt.acct = a; metaAcct = a;
        list = std::move(p);
        say("Painel IPTV: " + std::to_string(list.channelCats.size()) + " categorias de canais | " + std::to_string(list.movieCats.size()) +
            " de filmes | " + std::to_string(list.seriesCats.size()) + " de series");
    }
    void noteAccount(const std::string &url) { XtreamAcct a; if (xtreamParse(url, a)) metaAcct = a; else metaAcct = XtreamAcct(); }
    void clearXtream() { xt = XtState(); metaCache.clear(); }

    bool loadM3U(const std::string &url, bool useCache = false) {
        noteAccount(url);
        {   // painel Xtream? (so quando o link tem usuario e senha) -> categorias pela API, sem baixar a lista gigante
            XtreamAcct acct; Playlist xp; std::string msg;
            say("Conectando ao painel IPTV...");
            pf.busy("Conectando ao painel IPTV...");
            int r = xtreamConnect([&](const std::string &u, std::string &b) { return pf.httpGet(u, b); }, url, acct, xp, msg);
            if (r == 1) { clearXtream(); useXtream(acct, std::move(xp)); return true; }
            if (r == 2) { say(msg); return false; }
        }
        clearXtream();
        if (useCache) {
            int r = loadCached(url);
            if (r == 1) return true;
            if (r == -1) return false;          // cancelado
        }
        PlaylistBuilder b;
        size_t bytes = 0;
        bool oom = false;
        say("Baixando lista M3U...  (O cancela)");
        pf.cacheBegin();
        bool ok = pf.httpStream(url, [&](const char *d, size_t n) {
            pf.cacheWrite(d, n);                 // a copia local recebe tudo, mesmo que o parse fique sem memoria
            try { b.feed(d, n); } catch (const std::bad_alloc &) { oom = true; return false; }
            bytes += n;
            say("Baixando lista M3U... " + fmtMB(bytes) + "  |  " + std::to_string(b.count()) + " itens  (O cancela)");
            return true;
        });
        try { b.finish(); } catch (...) {}
        size_t total = b.count();
        pf.cacheEnd(url, ok && total > 0);       // so vira a lista salva se baixou tudo
        if (oom) {
            if (total == 0) { say("Memoria insuficiente para carregar a lista"); return false; }
            list = b.take();
            say("Aviso: acabou a memoria, so parte da lista foi carregada - " + summary());
            return true;
        }
        if (!ok) {
            if (pf.wasCancelled()) { say("Download da lista cancelado"); return false; }
            if (total == 0) { say("Falha ao baixar a lista M3U"); return false; }
            // a conexao caiu no meio: aproveita o que ja chegou
            list = b.take();
            say("Aviso: lista incompleta (conexao caiu) - " + summary());
            return true;
        }
        if (total == 0) { say("Falha: o link nao devolveu uma lista M3U valida"); return false; }
        list = b.take();
        say(summary() + truncNote());
        return true;
    }

    // 1 = carregou da copia salva | 0 = nao tem copia valida (baixa) | -1 = o usuario cancelou
    int loadCached(const std::string &url) {
        PlaylistBuilder b;
        size_t bytes = 0;
        bool oom = false;
        say("Carregando lista salva...  (O cancela)");
        bool ok = pf.cacheLoad(url, [&](const char *d, size_t n) {
            try { b.feed(d, n); } catch (const std::bad_alloc &) { oom = true; return false; }
            bytes += n;
            say("Carregando lista salva... " + fmtMB(bytes) + "  |  " + std::to_string(b.count()) + " itens  (O cancela)");
            return true;
        });
        try { b.finish(); } catch (...) {}
        if (oom && b.count() > 0) {
            list = b.take();
            say("Aviso: acabou a memoria, so parte da lista foi carregada - " + summary());
            return 1;
        }
        if (!ok) { if (pf.wasCancelled()) { say("Carregamento cancelado"); return -1; } return 0; }
        if (b.count() == 0) return 0;
        list = b.take();
        say(summary() + "  |  lista salva" + truncNote());
        return 1;
    }

    // ---------- Xtream: itens por categoria ----------
    static constexpr size_t kXtDropAt = 100000;   // acima disso (itens carregados) as categorias antigas sao descartadas ao abrir outra

    void dropLoaded() {
        std::vector<Entry>().swap(list.channels); std::vector<Entry>().swap(list.movies);
        list.seriesTree.clear(); list.seriesInfo.clear(); list.seriesEpisodes = 0;
        for (auto *cv : {&list.channelCats, &list.movieCats, &list.seriesCats})
            for (auto &c : *cv) if (c.lazy && c.loaded) { std::vector<uint32_t>().swap(c.items); std::vector<std::string>().swap(c.series); c.loaded = false; }
    }

    // Baixa os itens da categoria (se ainda nao baixou). true = categoria pronta.
    bool ensureCategory(Tab t, int idx) {
        if (!xt.active || t == Tab::Kick) return true;
        std::vector<Category> &cv = t == Tab::Channels ? list.channelCats : t == Tab::Movies ? list.movieCats : list.seriesCats;
        if (idx < 0 || idx >= (int)cv.size()) return false;
        if (!cv[idx].unloaded()) return true;
        if (list.channels.size() + list.movies.size() + list.seriesInfo.size() > kXtDropAt) dropLoaded();
        Category &c = cv[idx];
        say("Carregando \"" + c.name + "\"...");
        pf.busy("Carregando \"" + c.name + "\"...");
        const char *action = t == Tab::Channels ? "get_live_streams" : t == Tab::Movies ? "get_vod_streams" : "get_series";
        std::string body;
        if (!pf.httpGet(xtreamApi(xt.acct, action, "category_id=" + c.id), body)) { say("Falha ao carregar a categoria (o painel nao respondeu)"); return false; }
        size_t f = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        if (f == std::string::npos || body[f] != '[') { say("Falha ao carregar a categoria (resposta invalida do painel)"); return false; }
        std::vector<uint32_t> items; std::vector<std::string> names;
        try {
            jsonForEach(body, [&](const Json &it) {
                std::string name = it.str("name");
                if (name.empty()) return;
                if (t == Tab::Series) {
                    std::string id = it.str("series_id");
                    if (id.empty() || list.seriesInfo.count(name)) return;
                    list.seriesInfo[name] = SeriesRef{id, it.str("cover")};
                    names.push_back(name);
                    return;
                }
                std::string id = it.str("stream_id");
                if (id.empty()) return;
                Entry e; e.name = name; e.logo = it.str("stream_icon");
                if (t == Tab::Channels) { e.kind = Kind::Channel; e.url = xtreamLiveUrl(xt.acct, id); list.channels.push_back(std::move(e)); items.push_back((uint32_t)list.channels.size() - 1); }
                else { e.kind = Kind::Movie; e.url = xtreamMovieUrl(xt.acct, id, it.str("container_extension")); list.movies.push_back(std::move(e)); items.push_back((uint32_t)list.movies.size() - 1); }
            });
        } catch (const std::bad_alloc &) { say("Memoria insuficiente para esta categoria"); }
        std::sort(names.begin(), names.end());
        c.items = std::move(items); c.series = std::move(names); c.loaded = true;
        say(c.name + ": " + std::to_string(c.size()) + " itens");
        return true;
    }

    // Episodios de uma serie do painel (get_series_info). true = tem episodios na arvore.
    bool ensureSeries(const std::string &name) {
        if (!xt.active) return true;
        auto it = list.seriesTree.find(name);
        if (it != list.seriesTree.end() && !it->second.empty()) return true;
        auto ri = list.seriesInfo.find(name);
        if (ri == list.seriesInfo.end()) { say("Serie sem identificacao no painel"); return false; }
        say("Carregando episodios...");
        pf.busy("Carregando episodios...");
        std::string body; Json j;
        if (!pf.httpGet(xtreamApi(xt.acct, "get_series_info", "series_id=" + ri->second.id), body)) { say("Falha ao carregar os episodios (o painel nao respondeu)"); return false; }
        if (!jsonParse(body, j) || !j.isObj()) { say("Falha ao carregar os episodios (resposta invalida do painel)"); return false; }
        const Json *info = j.get("info");
        std::string cover = info ? info->str("cover") : "";
        if (cover.empty()) cover = ri->second.cover; else if (ri->second.cover.empty()) ri->second.cover = cover;
        if (info && info->isObj()) { DetailMeta m; xtFillMeta(*info, m); metaCache["s:" + ri->second.id] = m; }
        std::map<int, std::vector<Entry>> seasons;
        auto addSeason = [&](int sn, const Json &arr) {
            if (!arr.isArr()) return;
            int pos = 0;
            for (const Json &ep : arr.arr) {
                pos++;
                std::string id = ep.str("id");
                if (id.empty()) continue;
                Entry e; e.kind = Kind::Series; e.season = sn;
                e.episode = (int)ep.num("episode_num", pos);
                e.name = ep.str("title"); if (e.name.empty()) e.name = "Episodio " + std::to_string(e.episode);
                e.url = xtreamEpisodeUrl(xt.acct, id, ep.str("container_extension"));
                e.logo = cover;
                if (const Json *ei = ep.get("info")) {
                    std::string mi = ei->str("movie_image"); if (!mi.empty()) e.logo = mi;
                    e.mins = xtDurationMin(*ei);
                }
                seasons[sn].push_back(std::move(e));
            }
        };
        const Json *eps = j.get("episodes");
        if (eps && eps->isObj()) for (const auto &kv : eps->obj) addSeason(atoi(kv.first.c_str()), kv.second);
        else if (eps && eps->isArr()) { int sn = 0; for (const Json &arr : eps->arr) addSeason(++sn, arr); }
        if (seasons.empty()) { say("A serie \"" + name + "\" esta sem episodios no painel"); return false; }
        size_t n = 0; for (auto &kv : seasons) n += kv.second.size();
        list.seriesEpisodes += n;
        list.seriesTree[name] = std::move(seasons);
        return true;
    }

    // ---------- Metadados do painel (sinopse, nota, generos, duracao) ----------
    static void applyMeta(const DetailMeta &src, DetailMeta &dst) {
        if (!src.plot.empty()) dst.plot = src.plot;
        if (!src.rating.empty()) dst.rating = src.rating;
        if (src.runtimeMin > 0) dst.runtimeMin = src.runtimeMin;
        if (!src.genres.empty()) dst.genres = src.genres;
        if (dst.year.empty()) dst.year = src.year;
    }
    const XtreamAcct &metaAccount() const { return xt.active ? xt.acct : metaAcct; }

    bool movieMeta(const Entry &e, DetailMeta &m) {
        std::string id = xtMovieIdFromUrl(e.url);
        const XtreamAcct &a = metaAccount();
        if (id.empty() || !a.valid()) return false;
        const std::string key = "m:" + id;
        auto c = metaCache.find(key);
        if (c == metaCache.end()) {
            pf.busy("Carregando detalhes...");
            std::string body; Json j;
            if (!pf.httpGet(xtreamApi(a, "get_vod_info", "vod_id=" + id), body) || !jsonParse(body, j)) return false;
            DetailMeta d;
            if (const Json *info = j.get("info")) xtFillMeta(*info, d);
            c = metaCache.emplace(key, d).first;
        }
        applyMeta(c->second, m);
        return true;
    }
    // Series (so painel Xtream): os dados vieram junto com os episodios (ensureSeries).
    bool seriesMeta(const std::string &name, DetailMeta &m) {
        auto ri = list.seriesInfo.find(name);
        if (ri == list.seriesInfo.end()) return false;
        auto c = metaCache.find("s:" + ri->second.id);
        if (c == metaCache.end()) return false;
        applyMeta(c->second, m);
        return true;
    }

    // aviso quando a lista passou do limite de memoria e so os primeiros itens foram carregados
    std::string truncNote() const { return list.truncated ? "  |  lista enorme: so os primeiros itens cabem na memoria" : ""; }

    std::string summary() const {
        return "Canais: " + std::to_string(list.channels.size()) + " | Filmes: " + std::to_string(list.movies.size()) +
               " | Series: " + std::to_string(list.seriesTree.size());
    }

    // Busca playback_url fresco (as URLs da CDN da Kick expiram em poucos minutos)
    // err: mensagem de erro; retorno vazio = falha
    std::string resolveKickHls(const std::string &input, std::string *err = nullptr) {
        std::string slug = kickSlug(input);
        if (slug.empty()) { if (err) *err = "@ invalido"; return ""; }
        std::string body;
        int st = 0; bool got = false;
        for (const std::string &u : kickApiUrls(slug)) {
            body.clear();
            if (pf.httpGetKick(u, body) && !body.empty()) { got = true; break; }
            st = pf.lastHttpStatus();
            if (st == 404) break;
        }
        if (!got) {
            if (err) {
                if (st == 404) *err = "canal nao existe";
                else if (st > 0) *err = "Kick HTTP " + std::to_string(st);
                else *err = "Kick sem resposta";
            }
            return "";
        }
        if (!kickIsLive(body)) { if (err) *err = "@" + slug + " offline"; return ""; }
        std::string hls = jsonString(body, "playback_url");
        if (hls.empty() && err) *err = "sem playback_url";
        return hls;
    }

    void watchKick(const std::string &input) {
        std::string slug = kickSlug(input);
        if (slug.empty()) { say("@ invalido"); return; }
        std::string err, hls = resolveKickHls(slug, &err);
        if (hls.empty()) {
            if (err.find("offline") != std::string::npos) say("@" + slug + " esta offline");
            else if (err.find("nao existe") != std::string::npos) say("Falha: o canal @" + slug + " nao existe na Kick");
            else if (!err.empty()) say("Falha ao consultar a Kick (" + err + ")");
            else say("Falha ao consultar a Kick");
            return;
        }
        say("Assistindo @" + slug);
        pf.play(hls, "@" + slug);
    }
};
