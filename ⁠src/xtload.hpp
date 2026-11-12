#pragma once
// Conexao com um painel Xtream (player_api.php): confere o login e busca SO os nomes das categorias (3 requisicoes pequenas).
// Os itens de cada categoria sao baixados depois, quando voce abre a categoria (App::ensureCategory).
// Listas de provedores grandes (200 mil+ itens) nao cabem na memoria do PS4 como M3U; assim nao ha limite.
// Independente de plataforma (a rede entra como funcao): testavel no PC.
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "json.hpp"
#include "m3u.hpp"
#include "xtream.hpp"

using XtGet = std::function<bool(const std::string &url, std::string &body)>;

// 1 = conectou (out preenchido) | 0 = nao e um painel Xtream (usa a lista M3U) | 2 = o painel recusou o login (msg explica)
inline int xtreamConnect(const XtGet &get, const std::string &url, XtreamAcct &acct, Playlist &out, std::string &msg) {
    msg.clear();
    if (!xtreamParse(url, acct)) return 0;
    std::string body; Json j;
    if (!get(xtreamApi(acct), body) || !jsonParse(body, j)) return 0;
    const Json *ui = j.get("user_info");
    if (!ui) return 0;
    if (ui->num("auth", 0) != 1) { msg = "Falha: o painel recusou o usuario/senha (conta expirada?)"; return 2; }
    static const char *actions[3] = {"get_live_categories", "get_vod_categories", "get_series_categories"};
    Playlist p; p.xtream = true;
    std::vector<Category> *dst[3] = {&p.channelCats, &p.movieCats, &p.seriesCats};
    size_t total = 0;
    for (int i = 0; i < 3; i++) {
        std::string cb;
        if (!get(xtreamApi(acct, actions[i]), cb)) continue;
        jsonForEach(cb, [&](const Json &c) {
            std::string id = c.str("category_id");
            if (id.empty()) return;
            Category cat; cat.id = id; cat.lazy = true; cat.loaded = false;
            cat.name = cleanCategory(c.str("category_name", "Categoria " + id));
            dst[i]->push_back(std::move(cat));
        });
        total += dst[i]->size();
    }
    if (total == 0) return 0;                   // painel sem categorias: tenta a lista M3U
    out = std::move(p);
    return 1;
}

// Conecta numa thread propria (o app abre e responde na hora). A thread so faz rede e monta o resultado.
class XtLoader {
public:
    struct Result { int code = 0; XtreamAcct acct; std::shared_ptr<Playlist> list; std::string msg; };
    XtLoader() = default;
    XtLoader(const XtLoader &) = delete;
    XtLoader &operator=(const XtLoader &) = delete;
    ~XtLoader() { cancel(0.5); }

    void start(const std::string &url, XtGet get) {
        cancel(0.5);
        sh_ = std::make_shared<Shared>();
        taken_ = false;
        auto sh = sh_;
        th_ = std::thread([sh, url, get] {
            Result r; r.list = std::make_shared<Playlist>();
            try { r.code = xtreamConnect(get, url, r.acct, *r.list, r.msg); } catch (...) { r.code = 0; }
            { std::lock_guard<std::mutex> l(sh->m); sh->res = std::move(r); }
            sh->finished = true;
        });
    }
    bool cancel(double waitSec = 3.0) {
        if (!sh_) return true;
        auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(waitSec);
        while (!sh_->finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (th_.joinable()) { if (sh_->finished) th_.join(); else th_.detach(); }
        return sh_->finished.load();
    }
    bool running() const { return sh_ && !sh_->finished && !taken_; }
    bool stillAlive() const { return sh_ && !sh_->finished; }
    // Thread principal: devolve true UMA vez, quando terminou.
    bool take(Result &out) {
        if (!sh_ || taken_ || !sh_->finished) return false;
        taken_ = true;
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> l(sh_->m);
        out = std::move(sh_->res);
        return true;
    }

private:
    struct Shared { std::atomic<bool> finished{false}; std::mutex m; Result res; };
    std::shared_ptr<Shared> sh_;
    std::thread th_;
    bool taken_ = false;
};
