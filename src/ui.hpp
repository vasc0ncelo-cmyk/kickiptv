#pragma once
// Navegacao e desenho das abas. Independente de plataforma: o PS4 so implementa Renderer + leitura do pad.
#include "app.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

enum class Input { Up, Down, PrevTab, NextTab, Confirm, Back, PageUp, PageDown };

struct Renderer {
    virtual ~Renderer() {}
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void fillRect(int x, int y, int w, int h, unsigned argb) = 0;
    virtual void drawText(int x, int y, const std::string &text, unsigned argb, int size) = 0;
    // extras visuais (o PS4 implementa de verdade; nos testes caem no retangulo simples)
    virtual void roundRect(int x, int y, int w, int h, int /*raio*/, unsigned argb) { fillRect(x, y, w, h, argb); }
    virtual void gradient(int x, int y, int w, int h, unsigned top, unsigned /*bottom*/) { fillRect(x, y, w, h, top); }
    virtual int textWidth(const std::string &t, int size) const {
        int n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) n++;
        return (int)(n * (int)(8 * size / 16.0f + 0.5f));
    }
};

// ---------- Tema (cores ARGB) ----------
namespace theme {
constexpr unsigned BG1 = 0xFF090B10, BG2 = 0xFF151A28;
constexpr unsigned CARD = 0xFF161B27, CARD2 = 0xFF1C2333, SEL = 0xFF232C42, CHIP = 0xFF252D40, KEY = 0xFF2F384F;
constexpr unsigned GREEN = 0xFF53FC18, GREEN_GLOW = 0x3353FC18, BLACK = 0xFF000000;
constexpr unsigned TEXT = 0xFFF2F4F8, SOFT = 0xFFC9D0DF, MUTED = 0xFF8B94A8, DIM = 0xFF5B6478;
constexpr unsigned RED = 0xFFFF4D5A, AMBER = 0xFFFFB020;
}

// ---------- Helpers de desenho ----------
inline void uiText(Renderer &r, int x, int y, const std::string &t, unsigned c, int size, bool bold = false) {
    r.drawText(x, y, t, c, size);
    if (bold) r.drawText(x + 1, y, t, c, size);
}
// centraliza em (cx, cy)
inline void uiCenter(Renderer &r, int cx, int cy, const std::string &t, unsigned c, int size, bool bold = false) {
    uiText(r, cx - r.textWidth(t, size) / 2, cy - size / 2, t, c, size, bold);
}
// corta o texto com "..." para caber em maxW
inline std::string uiFit(const Renderer &r, std::string s, int size, int maxW) {
    if (r.textWidth(s, size) <= maxW) return s;
    while (!s.empty() && r.textWidth(s + "...", size) > maxW) {
        s.pop_back();
        while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && ((unsigned char)s.back() & 0x80) && ((unsigned char)s.back() & 0xC0) == 0xC0) s.pop_back();
    }
    return s + "...";
}
// Dicas de botoes (ex.: [X] Selecionar) alinhadas a direita em rightX. Retorna o x onde comecam.
inline int uiHints(Renderer &r, int rightX, int cy, const std::vector<std::pair<std::string, std::string>> &h) {
    int x = rightX;
    for (int i = (int)h.size() - 1; i >= 0; i--) {
        int lw = r.textWidth(h[i].second, 20), kw = r.textWidth(h[i].first, 18) + 24;
        x -= lw; uiText(r, x, cy - 10, h[i].second, theme::MUTED, 20);
        x -= 10 + kw; r.roundRect(x, cy - 17, kw, 34, 11, theme::KEY);
        uiCenter(r, x + kw / 2, cy, h[i].first, theme::TEXT, 18, true);
        x -= 30;
    }
    return x + 30;
}
inline void uiBackground(Renderer &r) { r.gradient(0, 0, r.width(), r.height(), theme::BG1, theme::BG2); }
inline void uiLogo(Renderer &r, int x, int y, int size) {   // quadrado verde com "K"
    r.roundRect(x, y, size, size, size / 4, theme::GREEN);
    uiCenter(r, x + size / 2, y + size / 2, "K", theme::BLACK, size * 3 / 5, true);
}

struct UI {
    App &app;
    Tab tab = Tab::Channels;
    int cursor[4] = {0, 0, 0, 0};
    int scroll[4] = {0, 0, 0, 0};
    int level = 0;           // so Series: 0 series, 1 temporadas, 2 episodios
    std::string curSeries;
    int curSeason = 0;
    std::string kickLast;    // ultimo @ pesquisado
    static const int kRows = 10;

    explicit UI(App &a) : app(a) {}

    // Itens da aba Kick (a ordem muda se ja existe um "ultimo streamer")
    enum { KA_TYPE = 0, KA_LAST = 1, KA_TEST_HTTPS = 2, KA_TEST_HTTP = 3, KA_AVDIAG = 4 };
    std::vector<int> kickActions() const {
        std::vector<int> a;
        a.push_back(KA_TYPE);
        if (!kickLast.empty()) a.push_back(KA_LAST);
        a.push_back(KA_TEST_HTTPS);
        a.push_back(KA_TEST_HTTP);
        a.push_back(KA_AVDIAG);
        return a;
    }
    std::string kickLabel(int a) const {
        switch (a) {
        case KA_TYPE: return "Digitar @streamer";
        case KA_LAST: return "Assistir @" + kickLast;
        case KA_AVDIAG: return "Diagnóstico do AvPlayer";
        case KA_TEST_HTTPS: return "Teste do player (HTTPS)";
        default: return "Teste do player (HTTP)";
        }
    }
    std::string kickSub(int a) const {
        switch (a) {
        case KA_TYPE: return "Abre o teclado na tela e procura o canal ao vivo";
        case KA_LAST: return "Abrir de novo o último canal pesquisado";
        case KA_AVDIAG: return "Roda vários testes e mostra o resultado na tela";
        case KA_TEST_HTTPS: return "Vídeo HLS público para validar o player";
        default: return "Vídeo HLS público (sem criptografia)";
        }
    }
    static std::string kickIcon(int a) {
        switch (a) { case KA_TYPE: return "@"; case KA_LAST: return ">"; case KA_AVDIAG: return "D"; default: return "T"; }
    }

    std::vector<std::string> labels() const {
        std::vector<std::string> v;
        const Playlist &p = app.list;
        switch (tab) {
        case Tab::Kick:
            for (int a : kickActions()) v.push_back(kickLabel(a));
            break;
        case Tab::Channels: for (auto &e : p.channels) v.push_back(e.name); break;
        case Tab::Movies: for (auto &e : p.movies) v.push_back(e.name); break;
        case Tab::Series:
            if (level == 0) { for (auto &s : p.seriesTree) v.push_back(s.first); }
            else if (level == 1) {
                auto it = p.seriesTree.find(curSeries);
                if (it != p.seriesTree.end()) for (auto &t : it->second) v.push_back("Temporada " + std::to_string(t.first));
            } else {
                auto it = p.seriesTree.find(curSeries);
                if (it != p.seriesTree.end()) {
                    auto st = it->second.find(curSeason);
                    if (st != it->second.end())
                        for (auto &e : st->second) v.push_back(e.episode ? "Ep " + std::to_string(e.episode) + " - " + e.name : e.name);
                }
            }
            break;
        }
        return v;
    }

    void onText(const std::string &t) { // chamado pelo IME ao confirmar
        kickLast = kickSlug(t);
        if (kickLast.empty() && !t.empty()) { app.say("@ invalido"); return; }
        if (!kickLast.empty()) app.watchKick(kickLast);
    }

    void handle(Input in) {
        int t = (int)tab, n = (int)labels().size();
        switch (in) {
        case Input::PrevTab: setTab((t + 3) % 4); return;
        case Input::NextTab: setTab((t + 1) % 4); return;
        case Input::Up: cursor[t] = std::max(0, cursor[t] - 1); break;
        case Input::Down: cursor[t] = std::min(std::max(0, n - 1), cursor[t] + 1); break;
        case Input::PageUp: cursor[t] = std::max(0, cursor[t] - kRows); break;
        case Input::PageDown: cursor[t] = std::min(std::max(0, n - 1), cursor[t] + kRows); break;
        case Input::Back:
            if (tab == Tab::Series && level > 0) { level--; cursor[t] = 0; scroll[t] = 0; }
            return;
        case Input::Confirm: confirm(n); return;
        }
        if (cursor[t] < scroll[t]) scroll[t] = cursor[t];
        if (cursor[t] >= scroll[t] + kRows) scroll[t] = cursor[t] - kRows + 1;
    }

    void render(Renderer &r) {
        using namespace theme;
        const int W = r.width(), H = r.height();
        uiBackground(r);

        // ---- Cabecalho: logo + titulo + abas em "pilulas" ----
        uiLogo(r, 48, 28, 64);
        uiText(r, 132, 26, "Kick IPTV", TEXT, 38, true);
        uiText(r, 134, 72, "Lives da Kick direto no seu PS4", MUTED, 20);
        static const char *names[4] = {"Kick", "Canais", "Filmes", "Séries"};
        const int pw = 160, ph = 50, pg = 6, total = 4 * pw + 3 * pg;
        const int tx = W - 48 - total - 6;
        r.roundRect(tx - 6, 34, total + 12, ph + 12, (ph + 12) / 2, CARD);
        for (int i = 0; i < 4; i++) {
            bool on = i == (int)tab;
            int px = tx + i * (pw + pg);
            if (on) { r.roundRect(px - 3, 37, pw + 6, ph + 6, 28, GREEN_GLOW); r.roundRect(px, 40, pw, ph, ph / 2, GREEN); }
            uiCenter(r, px + pw / 2, 40 + ph / 2, names[i], on ? BLACK : MUTED, 24, on);
        }

        // ---- Titulo da secao ----
        auto v = labels();
        int t = (int)tab, n = (int)v.size();
        std::string title;
        switch (tab) {
        case Tab::Kick: title = "Assistir na Kick"; break;
        case Tab::Channels: title = "Canais"; break;
        case Tab::Movies: title = "Filmes"; break;
        default:
            title = level == 0 ? "Séries" : level == 1 ? curSeries : curSeries + "  /  Temporada " + std::to_string(curSeason);
        }
        uiText(r, 52, 128, uiFit(r, title, 30, W - 400), TEXT, 30, true);
        if (tab != Tab::Kick) {
            std::string c = std::to_string(n) + (n == 1 ? " item" : " itens");
            uiText(r, W - 48 - r.textWidth(c, 20), 134, c, MUTED, 20);
        }

        // ---- Lista de cartoes ----
        const int x = 48, rw = W - 96 - 24, rh = 68, pitch = 78, y0 = 184;
        auto acts = kickActions();
        for (int row = 0; row < kRows; row++) {
            int idx = scroll[t] + row;
            if (idx >= n) break;
            int y = y0 + row * pitch;
            bool sel = idx == cursor[t];
            if (sel) r.roundRect(x - 5, y - 5, rw + 10, rh + 10, 23, GREEN_GLOW);
            r.roundRect(x, y, rw, rh, 18, sel ? SEL : CARD);
            if (sel) r.roundRect(x, y + 16, 6, rh - 32, 3, GREEN);
            // icone redondo com a inicial
            std::string ic = "#";
            if (tab == Tab::Kick) ic = kickIcon(acts[idx]);
            else for (unsigned char ch : v[idx]) if (std::isalnum(ch)) { ic = std::string(1, (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch)); break; }
            r.roundRect(x + 22, y + 12, 44, 44, 22, sel ? GREEN : CHIP);
            uiCenter(r, x + 44, y + 34, ic, sel ? BLACK : MUTED, 24, true);
            unsigned tc = sel ? TEXT : SOFT;
            int textMax = rw - 88 - 110;
            if (tab == Tab::Kick) {
                uiText(r, x + 88, y + 9, uiFit(r, v[idx], 26, textMax), tc, 26, true);
                uiText(r, x + 88, y + 40, uiFit(r, kickSub(acts[idx]), 18, textMax), MUTED, 18);
            } else {
                uiText(r, x + 88, y + (rh - 26) / 2, uiFit(r, v[idx], 26, textMax), tc, 26, sel);
            }
            if (sel) { r.roundRect(x + rw - 78, y + 16, 56, 36, 11, KEY); uiCenter(r, x + rw - 50, y + 34, "X", TEXT, 22, true); }
        }
        if (n == 0) uiCenter(r, W / 2, y0 + 120, "Nada por aqui ainda", MUTED, 28);

        // ---- Barra de rolagem ----
        if (n > kRows) {
            int th = kRows * pitch - 10;
            r.roundRect(W - 44, y0, 8, th, 4, CARD);
            int kh = std::max(48, th * kRows / n);
            int ky = y0 + (th - kh) * scroll[t] / std::max(1, n - kRows);
            r.roundRect(W - 44, ky, 8, kh, 4, MUTED);
        }

        // ---- Cartao de ajuda (aba Kick) ----
        if (tab == Tab::Kick) {
            int hy = y0 + n * pitch + 12;
            r.roundRect(x, hy, rw, 156, 18, 0xFF111623);
            uiText(r, x + 28, hy + 18, "Como usar", GREEN, 22, true);
            uiText(r, x + 28, hy + 54, "1. Escolha \"Digitar @streamer\" e informe o nome do canal (ex.: @dekin021).", MUTED, 20);
            uiText(r, x + 28, hy + 84, "2. O canal precisa estar ao vivo. Aperte O para voltar ao menu a qualquer momento.", MUTED, 20);
            uiText(r, x + 28, hy + 114, "3. Se o vídeo não abrir, rode o Diagnóstico e tire um print da tela.", MUTED, 20);
        }

        // ---- Rodape: status + dicas de botoes ----
        const int fy = H - 92, fh = 60;
        r.roundRect(48, fy, W - 96, fh, 20, 0xFF111623);
        int hintsX = uiHints(r, W - 48 - 24, fy + fh / 2, {{"L1/R1", "Abas"}, {"X", "Selecionar"}, {"O", "Voltar"}});
        const std::string &st = app.lastStatus;
        unsigned dot = AMBER;
        if (st.find("Falha") != std::string::npos || st.find("offline") != std::string::npos || st.find("invalido") != std::string::npos) dot = RED;
        else if (st.compare(0, 10, "Assistindo") == 0 || st == "Pronto") dot = GREEN;
        r.roundRect(48 + 26, fy + fh / 2 - 6, 12, 12, 6, dot);
        uiText(r, 48 + 52, fy + fh / 2 - 11, uiFit(r, st, 22, hintsX - (48 + 52) - 30), SOFT, 22);
    }

private:
    void setTab(int t) { tab = (Tab)t; level = 0; }
    void confirm(int n) {
        int t = (int)tab, c = cursor[t];
        const Playlist &p = app.list;
        if (tab == Tab::Kick) {
            auto acts = kickActions();
            if (c >= (int)acts.size()) return;
            switch (acts[c]) {
            case KA_LAST: app.watchKick(kickLast); break;
            case KA_AVDIAG: app.pf.command("avdiag"); break;
            case KA_TEST_HTTPS: app.say("Teste de player (https)"); app.pf.play(kTestHlsHttps(), "Teste HLS publico (https)"); break;
            case KA_TEST_HTTP: app.say("Teste de player (http)"); app.pf.play(kTestHlsHttp(), "Teste HLS publico (http)"); break;
            default: app.pf.requestText("@ do streamer"); break;
            }
            return;
        }
        if (c >= n) return;
        if (tab == Tab::Channels) app.pf.play(p.channels[c].url, p.channels[c].name);
        else if (tab == Tab::Movies) app.pf.play(p.movies[c].url, p.movies[c].name);
        else if (level == 0) { auto it = p.seriesTree.begin(); std::advance(it, c); curSeries = it->first; level = 1; cursor[t] = scroll[t] = 0; }
        else if (level == 1) { auto &m = p.seriesTree.at(curSeries); auto it = m.begin(); std::advance(it, c); curSeason = it->first; level = 2; cursor[t] = scroll[t] = 0; }
        else { const Entry &e = p.seriesTree.at(curSeries).at(curSeason)[c]; app.pf.play(e.url, e.name); }
    }
};
