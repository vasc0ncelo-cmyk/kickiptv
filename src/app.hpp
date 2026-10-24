#pragma once
// Logica do app independente de plataforma (testavel no PC).
#include "kick.hpp"
#include "m3u.hpp"
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
    virtual std::string loadSetting(const std::string &) { return ""; }              // dados guardados (ex.: "m3u", "kick")
    virtual void saveSetting(const std::string &, const std::string &) {}
    // Copia local da lista M3U (para nao baixar tudo a cada abertura do app).
    // cacheLoad: entrega em pedacos a copia salva DESTA url; false = nao existe / url diferente / cancelado / erro.
    virtual bool cacheLoad(const std::string & /*url*/, const std::function<bool(const char *, size_t)> & /*sink*/) { return false; }
    virtual void cacheBegin() {}                                   // vai comecar um download: comece a gravar
    virtual void cacheWrite(const char * /*d*/, size_t /*n*/) {}   // pedaco baixado, em ordem
    virtual void cacheEnd(const std::string & /*url*/, bool /*complete*/) {}   // complete=true: a copia vira a lista salva (senao descarta e mantem a antiga)
    virtual void cacheClear() {}                                   // apaga a copia salva
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

    // useCache=true (abrir o app): usa a copia salva no console, se houver (sem baixar nada).
    // useCache=false (usuario confirmou o link): baixa de novo e atualiza a copia salva.
    bool loadM3U(const std::string &url, bool useCache = false) {
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

    // aviso quando a lista passou do limite de memoria e so os primeiros itens foram carregados
    std::string truncNote() const { return list.truncated ? "  |  lista enorme: so os primeiros itens cabem na memoria" : ""; }

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
