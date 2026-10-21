#pragma once
// Logica do app independente de plataforma (testavel no PC).
#include "kick.hpp"
#include "m3u.hpp"
#include <string>

enum class Tab { Kick = 0, Channels = 1, Movies = 2, Series = 3 };

// A camada PS4 (src/ps4_platform.cpp) implementa isto.
struct Platform {
    virtual ~Platform() {}
    virtual bool httpGet(const std::string &url, std::string &body) = 0;
    virtual void play(const std::string &url, const std::string &title) = 0;
    virtual void requestText(const std::string &prompt) = 0; // abre o teclado (IME); depois chame UI::onText
    virtual void command(const std::string &) {}             // comandos de diagnostico (ex.: "avdiag")
};

struct App {
    Platform &pf;
    Playlist list;
    std::string lastStatus = "Pronto";
    explicit App(Platform &p) : pf(p) {}

    void say(const std::string &m) { lastStatus = m; }

    bool loadM3U(const std::string &url) {
        std::string body;
        if (!pf.httpGet(url, body)) { say("Falha ao baixar a lista M3U"); return false; }
        list = buildPlaylist(body);
        say("Canais: " + std::to_string(list.channels.size()) + " | Filmes: " + std::to_string(list.movies.size()) +
            " | Series: " + std::to_string(list.seriesTree.size()));
        return true;
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
