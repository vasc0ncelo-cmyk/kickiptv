#pragma once
// Logica do app independente de plataforma (testavel no PC).
#include "kick.hpp"
#include "m3u.hpp"
#include <functional>
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
    virtual std::string loadSetting(const std::string &) { return ""; }              // dados guardados (ex.: "m3u", "kick")
    virtual void saveSetting(const std::string &, const std::string &) {}
};

struct App {
    Platform &pf;
    Playlist list;
    std::string lastStatus = "Pronto";
    explicit App(Platform &p) : pf(p) {}

    void say(const std::string &m) { lastStatus = m; }

    static std::string fmtMB(size_t bytes) {
        char b[32]; snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0); return b;
    }

    bool loadM3U(const std::string &url) {
        PlaylistBuilder b;
        size_t bytes = 0;
        say("Baixando lista M3U...  (O cancela)");
        bool ok = pf.httpStream(url, [&](const char *d, size_t n) {
            b.feed(d, n); bytes += n;
            say("Baixando lista M3U... " + fmtMB(bytes) + "  |  " + std::to_string(b.count()) + " itens  (O cancela)");
            return true;
        });
        b.finish();
        size_t total = b.count();
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
        say(list.truncated ? "Aviso: lista muito grande, so os primeiros itens foram carregados - " + summary() : summary());
        return true;
    }

    std::string summary() const {
        return "Canais: " + std::to_string(list.channels.size()) + " | Filmes: " + std::to_string(list.movies.size()) +
               " | Series: " + std::to_string(list.seriesTree.size());
    }

    void watchKick(const std::string &input) {
        std::string slug = kickSlug(input);
        if (slug.empty()) { say("@ invalido"); return; }
        std::string body;
        if (!pf.httpGet(kickApiUrl(slug), body)) { say("Falha ao consultar a Kick"); return; }
        if (!kickIsLive(body)) { say("@" + slug + " esta offline"); return; }
        std::string hls = jsonString(body, "playback_url");
        if (hls.empty()) { say("Stream nao encontrado"); return; }
        say("Assistindo @" + slug);
        pf.play(hls, "@" + slug);
    }
};
