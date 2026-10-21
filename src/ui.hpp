#pragma once
// Navegacao e desenho das abas. Independente de plataforma: o PS4 so implementa Renderer + leitura do pad.
#include "app.hpp"
#include <algorithm>
#include <string>
#include <vector>

enum class Input { Up, Down, PrevTab, NextTab, Confirm, Back, PageUp, PageDown };

struct Renderer {
    virtual ~Renderer() {}
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void fillRect(int x, int y, int w, int h, unsigned argb) = 0;
    virtual void drawText(int x, int y, const std::string &text, unsigned argb, int size) = 0;
};

struct UI {
    App &app;
    Tab tab = Tab::Channels;
    int cursor[4] = {0, 0, 0, 0};
    int scroll[4] = {0, 0, 0, 0};
    int level = 0;           // so Series: 0 series, 1 temporadas, 2 episodios
    std::string curSeries;
    int curSeason = 0;
    std::string kickLast;    // ultimo @ pesquisado
    static const int kRows = 14;

    explicit UI(App &a) : app(a) {}

    std::vector<std::string> labels() const {
        std::vector<std::string> v;
        const Playlist &p = app.list;
        switch (tab) {
        case Tab::Kick:
            v.push_back("[X] Pesquisar @streamer");
            if (!kickLast.empty()) v.push_back("Ultimo: @" + kickLast + "  (X para assistir de novo)");
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
        const int W = r.width(), H = r.height();
        r.fillRect(0, 0, W, H, 0xFF101018);
        static const char *names[4] = {"Kick", "Canais", "Filmes", "Series"};
        for (int i = 0; i < 4; i++) {
            bool on = i == (int)tab;
            r.fillRect(40 + i * 220, 30, 200, 50, on ? 0xFF53FC18 : 0xFF2A2A38);
            r.drawText(60 + i * 220, 42, names[i], on ? 0xFF000000 : 0xFFFFFFFF, 28);
        }
        auto v = labels();
        int t = (int)tab;
        for (int row = 0; row < kRows; row++) {
            int idx = scroll[t] + row;
            if (idx >= (int)v.size()) break;
            int y = 110 + row * 44;
            if (idx == cursor[t]) r.fillRect(40, y - 4, W - 80, 40, 0xFF3A3A55);
            r.drawText(60, y, v[idx], 0xFFFFFFFF, 26);
        }
        r.drawText(40, H - 50, app.lastStatus, 0xFFAAAAAA, 22);
        r.drawText(W - 700, H - 50, "L1/R1 abas  X ok  O voltar", 0xFF777777, 22);
    }

private:
    void setTab(int t) { tab = (Tab)t; level = 0; }
    void confirm(int n) {
        int t = (int)tab, c = cursor[t];
        const Playlist &p = app.list;
        if (tab == Tab::Kick) {
            if (c == 1 && !kickLast.empty()) app.watchKick(kickLast);
            else app.pf.requestText("@ do streamer");
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
