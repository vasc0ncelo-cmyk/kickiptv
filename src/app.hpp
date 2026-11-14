#pragma once
#include <ctime>
// Logica do app independente de plataforma (testavel no PC).
#include "kick.hpp"
#include "kickapi.hpp"
#include "kickkey.hpp"
#include "m3u.hpp"
#include "xtload.hpp"
#include "xtmeta.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <functional>
#include <new>
#include <cstdio>
#include <string>
#include <vector>

enum class Tab { Kick = 0, Channels = 1, Movies = 2, Series = 3 };

// Motivo legivel para uma falha de rede (para a tela mostrar POR QUE a lista nao baixou, em vez de so "falhou").
// st = status HTTP da ultima consulta (0 = nem chegou a ter resposta).
inline std::string httpReason(int st) {
    if (st <= 0) return "sem resposta do servidor (internet/DNS do PS4 ou provedor fora do ar)";
    if (st == 401 || st == 403) return "o provedor recusou o acesso (HTTP " + std::to_string(st) + "): conta bloqueada/expirada ou este IP nao e aceito";
    if (st == 404) return "link nao encontrado no provedor (HTTP 404)";
    if (st == 429 || st == 458 || st == 884) return "limite de conexoes do provedor (HTTP " + std::to_string(st) + "): a conta ja esta em uso em outro lugar";
    if (st >= 500) return "o provedor esta com problema (HTTP " + std::to_string(st) + ")";
    return "HTTP " + std::to_string(st);
}
// Inicio da resposta, limpo para caber numa linha da tela (so ASCII imprimivel). Vazio se nao havia texto.
inline std::string bodyPreview(const std::string &b, size_t maxLen = 90) {
    std::string o;
    for (unsigned char c : b) {
        if (o.size() >= maxLen) break;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c < 32 || c > 126) continue;
        if (c == ' ' && (o.empty() || o.back() == ' ')) continue;
        o += (char)c;
    }
    return o;
}

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
    // API oficial da Kick (POST do token + GET com "Authorization: Bearer"). Padrao: sem suporte.
    virtual bool httpApi(const char * /*method*/, const std::string & /*url*/, const std::string & /*authHeader*/, const std::string & /*body*/,
                         std::string & /*out*/, int & /*status*/) { return false; }
    virtual std::string lastBodyHead() { return ""; }                                // primeiros bytes da ultima lista baixada (para mostrar o que o provedor respondeu)
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
    bool prefetch = false;                                 // painel Xtream: baixa as categorias todas em segundo plano ao conectar
    kickapi::Client kickClient;                            // token da API oficial da Kick (em cache)
    explicit App(Platform &p) : pf(p) { kickClient.setCredentials(kickClientId(), kickClientSecret()); }
    ~App() { stopPrefetch(0.3); stopHeroInfo(0.3); }
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    void say(const std::string &m) { lastStatus = m; }

    static std::string fmtMB(size_t bytes) {
        char b[32]; snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0); return b;
    }

    // useCache=true (abrir o app): usa a copia salva no console, se houver (sem baixar nada).
    // useCache=false (usuario confirmou o link): baixa de novo e atualiza a copia salva.
    // Conexao com painel Xtream: guarda so as categorias (ver xtload.hpp).
    void useXtream(const XtreamAcct &a, Playlist &&p) {
        xt.active = true; xt.acct = a; metaAcct = a;
        stopPrefetch(0.5);
        list = std::move(p);
        startPrefetch();
        say("Painel IPTV: " + std::to_string(list.channelCats.size()) + " categorias de canais | " + std::to_string(list.movieCats.size()) +
            " de filmes | " + std::to_string(list.seriesCats.size()) + " de series");
    }
    void noteAccount(const std::string &url) { XtreamAcct a; if (xtreamParse(url, a)) metaAcct = a; else metaAcct = XtreamAcct(); }
    void clearXtream() { stopPrefetch(0.5); xt = XtState(); metaCache.clear(); }

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
            if (total == 0) { say("Falha ao baixar a lista M3U: " + httpReason(pf.lastHttpStatus())); return false; }
            // a conexao caiu no meio: aproveita o que ja chegou
            list = b.take();
            say("Aviso: lista incompleta (conexao caiu) - " + summary());
            return true;
        }
        if (total == 0) {
            std::string pv = bodyPreview(pf.lastBodyHead());
            say("Falha: o link nao devolveu uma lista M3U valida" + (pv.empty() ? std::string() : " - o provedor respondeu: \"" + pv + "\""));
            return false;
        }
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
        stopPrefetch(0.3);                                  // indices mudam: o catalogo em segundo plano para
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
        if (!applyCategoryBody(t, c, body)) { say("Falha ao carregar a categoria (resposta invalida do painel)"); return false; }
        say(c.name + ": " + std::to_string(c.size()) + " itens");
        return true;
    }

    // Le o JSON da categoria e preenche a lista (so a thread principal mexe em 'list'). false = resposta que nao e uma lista.
    bool applyCategoryBody(Tab t, Category &c, const std::string &body) {
        size_t f = body.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
        if (f == std::string::npos || body[f] != '[') return false;
        std::vector<uint32_t> items; std::vector<std::string> names;
        try {
            jsonForEach(body, [&](const Json &it) {
                std::string name = it.str("name");
                if (name.empty()) return;
                if (t == Tab::Series) {
                    std::string id = it.str("series_id");
                    if (id.empty() || list.seriesInfo.count(name)) return;
                    list.seriesInfo[name] = SeriesRef{id, it.str("cover"), xtBackdrop(it), dm::trimA(it.str("plot"))};
                    names.push_back(name);
                    return;
                }
                std::string id = it.str("stream_id");
                if (id.empty()) return;
                Entry e; e.name = name; e.logo = it.str("stream_icon");
                if (t == Tab::Channels) { e.kind = Kind::Channel; e.url = xtreamLiveUrl(xt.acct, id); list.channels.push_back(std::move(e)); items.push_back((uint32_t)list.channels.size() - 1); }
                else {
                    e.kind = Kind::Movie; e.url = xtreamMovieUrl(xt.acct, id, it.str("container_extension"));
                    e.added = (int)it.num("added");
                    { double rt = atof(it.str("rating").c_str()); if (rt > 0 && rt <= 10) e.rate10 = (int)(rt * 10 + 0.5); }
                    list.movies.push_back(std::move(e)); items.push_back((uint32_t)list.movies.size() - 1); }
            });
        } catch (const std::bad_alloc &) { say("Memoria insuficiente para esta categoria"); }
        std::sort(names.begin(), names.end());
        c.items = std::move(items); c.series = std::move(names); c.loaded = true;
        return true;
    }

    // ---------- Xtream: carrega o catalogo todo em segundo plano ----------
    // Uma thread so baixa o JSON de cada categoria (rede); a thread principal e que le e preenche a lista (prefetchTick), um por vez e
    // so no menu (nunca durante o video). Passou do limite de memoria: para e o resto abre por categoria, como antes.
    static constexpr size_t kXtPrefetchCap = 80000;
    struct PfItem { Tab t; int idx; std::string body; bool ok; };
    struct PfShared { std::atomic<bool> stop{false}, finished{false}; std::mutex m; std::deque<PfItem> q; size_t total = 0, done = 0; };
    std::shared_ptr<PfShared> pfs_;
    std::thread pfTh_;

    void startPrefetch() {
        if (!prefetch || !xt.active) return;
        struct Job { Tab t; int idx; std::string id; };
        std::vector<Job> jobs;
        size_t n[3] = {list.channelCats.size(), list.movieCats.size(), list.seriesCats.size()};
        size_t mx = std::max(n[0], std::max(n[1], n[2]));
        for (size_t i = 0; i < mx; i++) {                      // intercala as abas: a 1a categoria de cada uma chega logo
            if (i < n[1]) jobs.push_back({Tab::Movies, (int)i, list.movieCats[i].id});
            if (i < n[2]) jobs.push_back({Tab::Series, (int)i, list.seriesCats[i].id});
            if (i < n[0]) jobs.push_back({Tab::Channels, (int)i, list.channelCats[i].id});
        }
        if (jobs.empty()) return;
        auto sh = std::make_shared<PfShared>(); sh->total = jobs.size();
        pfs_ = sh;
        XtreamAcct acct = xt.acct; Platform *plat = &pf;
        pfTh_ = std::thread([sh, jobs, acct, plat] {
            for (const Job &j : jobs) {
                while (!sh->stop) {                                // a thread principal e mais lenta que a rede: espera ela esvaziar a fila
                    { std::lock_guard<std::mutex> l(sh->m); if (sh->q.size() < 3) break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (sh->stop) break;
                const char *action = j.t == Tab::Channels ? "get_live_streams" : j.t == Tab::Movies ? "get_vod_streams" : "get_series";
                PfItem it{j.t, j.idx, std::string(), false};
                try { it.ok = plat->httpGet(xtreamApi(acct, action, "category_id=" + j.id), it.body); } catch (...) { it.ok = false; }
                std::lock_guard<std::mutex> l(sh->m);
                sh->q.push_back(std::move(it));
            }
            sh->finished = true;
        });
    }
    void stopPrefetch(double waitSec = 0.3) {
        if (!pfs_) return;
        pfs_->stop = true;
        auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(waitSec);
        while (!pfs_->finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (pfTh_.joinable()) { if (pfs_->finished) pfTh_.join(); else pfTh_.detach(); }
        pfs_.reset();
    }
    bool prefetching() const { return pfs_ && !(pfs_->finished && pfs_->done >= pfs_->total); }
    size_t loadedCount() const { return list.channels.size() + list.movies.size() + list.seriesInfo.size(); }
    // Thread principal, uma vez por quadro no menu. true = algo novo na lista (redesenhar).
    bool prefetchTick() {
        if (!pfs_ || !xt.active) return false;
        PfItem it; bool got = false;
        { std::lock_guard<std::mutex> l(pfs_->m); if (!pfs_->q.empty()) { it = std::move(pfs_->q.front()); pfs_->q.pop_front(); got = true; } }
        if (!got) {
            if (pfs_->finished) {
                bool empty; { std::lock_guard<std::mutex> l(pfs_->m); empty = pfs_->q.empty(); }      // (o lock tem que soltar ANTES do reset abaixo)
                if (empty) { if (pfTh_.joinable()) pfTh_.join(); if (loadedCount() > 0) say("Catalogo carregado: " + summaryXt()); pfs_.reset(); return true; }
            }
            return false;
        }
        pfs_->done++;
        std::vector<Category> &cv = it.t == Tab::Channels ? list.channelCats : it.t == Tab::Movies ? list.movieCats : list.seriesCats;
        if (it.idx < 0 || it.idx >= (int)cv.size()) return false;
        if (loadedCount() >= kXtPrefetchCap) {                  // memoria: o resto continua por categoria
            pfs_->stop = true;
            if (pfTh_.joinable()) pfTh_.detach();
            say("Catalogo grande: " + summaryXt() + "  |  o resto abre por categoria");
            pfs_.reset();
            return true;
        }
        Category &c = cv[it.idx];
        if (!c.unloaded()) return false;                        // o usuario ja abriu esta
        if (!it.ok || !applyCategoryBody(it.t, c, it.body)) return false;   // falhou: continua lazy (abre ao apertar X)
        say("Carregando catalogo... " + std::to_string(pfs_->done) + "/" + std::to_string(pfs_->total) + " categorias");
        return true;
    }
    std::string summaryXt() const {
        return std::to_string(list.channels.size()) + " canais | " + std::to_string(list.movies.size()) + " filmes | " + std::to_string(list.seriesInfo.size()) + " series";
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
        if (!src.backdrop.empty()) dst.backdrop = src.backdrop;
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


    // ---------- Banner do Inicio: imagem larga (backdrop) e sinopse dos filmes ----------
    // A lista de filmes do painel so traz o poster. O backdrop e a sinopse vem de get_vod_info (1 consulta por filme do banner, no maximo
    // uns 3). Uma thread so faz essas consultas, uma de cada vez e com pausa, para nunca competir com o video nem com as capas.
    // (As series ja trazem backdrop/sinopse na propria lista: SeriesRef.)
    struct HeroInfo { std::string backdrop, plot, logo; };
    struct HeroReq { std::string key, id; };
    struct Trend { std::string id, title, orig, year, backdrop, overview; };      // filme em alta/recente do TMDB
    struct HeroShared {
        std::mutex m; std::deque<HeroReq> q; std::unordered_map<std::string, HeroInfo> info; std::unordered_map<std::string, bool> asked;
        std::vector<Trend> trend; bool trendDone = false;
        std::atomic<bool> stop{false}; bool running = false;
    };
    std::string tmdbKey;                                  // chave TMDB (logos e filmes em alta); vazia = sem
    bool heroInfoEnabled = false;                         // a plataforma liga; os testes ficam sem consultas extras
    std::shared_ptr<HeroShared> heroSh_ = std::make_shared<HeroShared>();
    std::thread heroTh_;

    bool heroInfoGet(const std::string &key, HeroInfo &out) {
        std::lock_guard<std::mutex> l(heroSh_->m);
        auto it = heroSh_->info.find(key);
        if (it == heroSh_->info.end()) return false;
        out = it->second; return true;
    }
    bool trendingGet(std::vector<Trend> &out) {
        std::lock_guard<std::mutex> l(heroSh_->m);
        if (!heroSh_->trendDone) return false;
        out = heroSh_->trend; return true;
    }
    void trendingRequest() { if (!tmdbKey.empty()) heroInfoRequest("T", "T"); }
    // logo do TMDB: devolve o endereco do melhor PNG (pt > en > sem idioma, depois a nota) ou vazio
    static std::string tmdbBestLogo(Platform *plat, const std::string &tk, bool tv, const std::string &tmdbId) {
        try {
            std::string b2; Json j2;
            std::string u = std::string("https://api.themoviedb.org/3/") + (tv ? "tv/" : "movie/") + tmdbId + "/images?api_key=" + tk + "&include_image_language=pt,en,null";
            if (plat->httpGet(u, b2) && jsonParse(b2, j2))
                if (const Json *ls = j2.get("logos"); ls && ls->isArr()) {
                    double best = -1; std::string bp;
                    for (const Json &e : ls->arr) {
                        std::string fp = e.str("file_path"); if (fp.size() < 5 || fp.compare(fp.size() - 4, 4, ".png") != 0) continue;
                        std::string lang = e.str("iso_639_1");
                        double sc = (lang == "pt" ? 20.0 : lang == "en" ? 10.0 : 0.0) + (e.get("vote_average") ? e.get("vote_average")->n : 0.0);
                        if (sc > best) { best = sc; bp = fp; }
                    }
                    if (!bp.empty()) return "https://image.tmdb.org/t/p/w500" + bp;
                }
        } catch (...) {}
        return "";
    }
    // Pede (uma vez so por chave). Chaves: "m:ID" filme do painel, "s:ID" serie do painel, "t:TMDBID" so a logo, "T" lista de filmes recentes em alta. Nao bloqueia.
    void heroInfoRequest(const std::string &key, const std::string &id) {
        if (!heroInfoEnabled || id.empty() || key.empty()) return;
        const XtreamAcct acct = metaAccount();
        if (!acct.valid() && key != "T" && key[0] != 't') return;
        auto sh = heroSh_;
        bool start = false;
        {
            std::lock_guard<std::mutex> l(sh->m);
            if (sh->asked.count(key)) return;
            sh->asked[key] = true; sh->q.push_back({key, id});
            if (!sh->running) { sh->running = true; start = true; }
        }
        if (!start) return;                                // a thread que ja esta rodando pega este pedido
        if (heroTh_.joinable()) heroTh_.join();            // a anterior ja terminou (running ficou falso)
        Platform *plat = &pf;
        const std::string tk = tmdbKey;
        heroTh_ = std::thread([sh, acct, plat, tk] {
            for (;;) {
                HeroReq r;
                {
                    std::lock_guard<std::mutex> l(sh->m);
                    if (sh->stop || sh->q.empty()) { sh->running = false; return; }
                    r = sh->q.front(); sh->q.pop_front();
                }
                if (r.key == "T") {                                        // filmes recentes mais populares (ate 3 paginas = 60)
                    std::vector<Trend> tr;
                    time_t t0 = time(nullptr) - 425 * 86400LL; struct tm g{}; gmtime_r(&t0, &g); char since[16]; strftime(since, sizeof since, "%Y-%m-%d", &g);
                    for (int pg = 1; pg <= 3 && !sh->stop && !tk.empty(); pg++) {
                        try {
                            std::string b, u = "https://api.themoviedb.org/3/discover/movie?api_key=" + tk + "&language=pt-BR&region=BR&include_adult=false&sort_by=popularity.desc&primary_release_date.gte=" + since + "&page=" + std::to_string(pg);
                            Json j;
                            if (plat->httpGet(u, b) && jsonParse(b, j))
                                if (const Json *rs = j.get("results"); rs && rs->isArr())
                                    for (const Json &e : rs->arr) {
                                        Trend x; x.id = e.str("id"); x.title = e.str("title"); x.orig = e.str("original_title"); x.overview = e.str("overview");
                                        std::string d = e.str("release_date"); x.year = d.size() >= 4 ? d.substr(0, 4) : "";
                                        std::string bd = e.str("backdrop_path"); if (!bd.empty()) x.backdrop = "https://image.tmdb.org/t/p/w780" + bd;
                                        if (!x.id.empty() && !x.backdrop.empty() && !x.title.empty()) tr.push_back(x);
                                    }
                        } catch (...) {}
                        std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    }
                    { std::lock_guard<std::mutex> l(sh->m); sh->trend = tr; sh->trendDone = true; }
                    continue;
                }
                HeroInfo hi; std::string body; Json j; std::string tmdbId;
                const bool isSeries = !r.key.empty() && r.key[0] == 's';
                if (r.key[0] == 't') tmdbId = r.id;                        // so a logo (o painel nao e consultado)
                else try {
                    if (plat->httpGet(xtreamApi(acct, isSeries ? "get_series_info" : "get_vod_info", std::string(isSeries ? "series_id=" : "vod_id=") + r.id), body) && jsonParse(body, j))
                        if (const Json *info = j.get("info")) {
                            DetailMeta d; xtFillMeta(*info, d); hi.backdrop = d.backdrop; hi.plot = d.plot;
                            tmdbId = info->str(isSeries ? "tmdb" : "tmdb_id"); if (tmdbId.empty()) tmdbId = info->str("tmdb_id");
                            if (tmdbId == "0") tmdbId.clear();
                        }
                } catch (...) {}
                if (!tk.empty() && !tmdbId.empty() && !sh->stop) hi.logo = tmdbBestLogo(plat, tk, isSeries, tmdbId);
                { std::lock_guard<std::mutex> l(sh->m); sh->info[r.key] = hi; }          // mesmo vazio: nao consulta de novo
                for (int i = 0; i < 15 && !sh->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    void stopHeroInfo(double waitSec = 0.3) {
        auto sh = heroSh_;
        sh->stop = true;
        auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(waitSec);
        for (;;) {
            { std::lock_guard<std::mutex> l(sh->m); if (!sh->running) break; }
            if (std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        bool done; { std::lock_guard<std::mutex> l(sh->m); done = !sh->running; }
        if (heroTh_.joinable()) { if (done) heroTh_.join(); else heroTh_.detach(); }
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

    // "Lives agora": as lives mais assistidas (API oficial). false + mensagem em 'err' se nao deu.
    bool kickLives(std::vector<kickapi::Live> &out, std::string &err) {
        pf.busy("Buscando lives na Kick...");
        return kickClient.lives([this](const char *m, const std::string &u, const std::string &a, const std::string &b, std::string &o, int &st) {
            return pf.httpApi(m, u, a, b, o, st);
        }, out, err, 60);
    }
    // Favoritos: ao vivo agora? (endpoint do canal, sem chave). Uma consulta por canal, no maximo 'cap'.
    void kickRefreshStatus(std::vector<kickapi::Live> &items, size_t cap = 12) {
        for (size_t i = 0; i < items.size() && i < cap; i++) {
            std::string body;
            pf.busy("Verificando @" + items[i].slug + " (" + std::to_string(i + 1) + "/" + std::to_string(std::min(cap, items.size())) + ")");
            bool got = false;
            for (auto &u : kickApiUrls(items[i].slug)) { body.clear(); if (pf.httpGetKick(u, body) && !body.empty()) { got = true; break; } }
            if (!got) continue;
            kickapi::Live l = kickapi::parseChannelStatus(items[i].slug, body);
            items[i].live = l.live; items[i].viewers = l.viewers; items[i].title = l.title;
        }
    }

    void watchKick(const std::string &input) {
        std::string slug = kickSlug(input);
        if (slug.empty()) { say("@ invalido"); return; }
        say("Abrindo @" + slug + "...");
        pf.busy("Abrindo @" + slug + "...");                    // desenha um quadro antes da consulta de rede (que bloqueia)
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
