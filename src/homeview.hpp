#pragma once
// Inicio (incluido no fim de ui.hpp): carrossel, atalhos, lista IPTV e rodape de comandos / apoiar.
// Layout em 1920x1080 (escala pela largura). Todas as animacoes sao por tempo (animT), nunca por quadro.

inline void homePlay(Renderer &r, int cx, int cy, int s, unsigned c) {      // triangulo de "play"
    for (int i = 0; i < s; i++) { int hh = (int)((float)(s - i) / s * s * 0.5f + 0.5f); r.fillRect(cx - s / 2 + i, cy - hh, 1, hh * 2, c); }
}
inline float homeK(const Renderer &r) { return (float)r.width() / 1920.f; }

inline void UI::heroRebuild() {
    const Playlist &p = app.list;
    std::string sig = std::to_string(p.movies.size()) + "|" + std::to_string(p.seriesTree.size()) + "|" + std::to_string(p.movieCats.size()) + "|" + std::to_string(p.seriesCats.size());
    if (sig == heroSig && !hero.empty()) return;
    heroSig = sig;
    hero.clear();
    HeroItem k; k.kind = 0; k.title = "KICK TV"; k.sub = "Assista suas lives favoritas da Kick em tempo real!";
    hero.push_back(k);
    // filmes e series intercalados, uma capa por categoria
    std::vector<HeroItem> mv, sr;
    for (const auto &c : p.movieCats) {
        if (mv.size() >= 2) break;
        for (uint32_t i : c.items) if (i < p.movies.size() && !p.movies[i].logo.empty()) {
            HeroItem h; h.kind = 1; h.idx = i; h.title = p.movies[i].name; h.sub = c.name; h.logo = p.movies[i].logo; h.cat = c.name; mv.push_back(h); break;
        }
    }
    for (const auto &c : p.seriesCats) {
        if (sr.size() >= 2) break;
        for (const auto &n : c.series) { std::string lg = seriesLogo(n); if (lg.empty()) continue;
            HeroItem h; h.kind = 2; h.title = n; h.sub = c.name; h.logo = lg; h.cat = c.name; sr.push_back(h); break; }
    }
    for (size_t i = 0; i < 2; i++) { if (i < mv.size()) hero.push_back(mv[i]); if (i < sr.size()) hero.push_back(sr[i]); }
    if (heroCur >= (int)hero.size()) heroCur = 0;
    heroFrom = heroCur;
}

inline void UI::heroMove(int d) {
    heroRebuild();
    int n = (int)hero.size(); if (n < 2) return;
    heroFrom = heroCur; heroCur = ((heroCur + d) % n + n) % n;
    heroT0 = animT; heroAuto = animT;
}

inline void UI::heroTick(double t) {
    if (screen != Screen::Home) return;
    heroRebuild();
    if (heroAuto <= 0) heroAuto = t;
    if (homeRow != 0 && t - heroAuto > 7.0) { int n = (int)hero.size(); if (n > 1) { heroFrom = heroCur; heroCur = (heroCur + 1) % n; heroT0 = t; } heroAuto = t; }
}

inline void UI::heroOpen() {
    heroRebuild();
    if (hero.empty()) return;
    const HeroItem &h = hero[heroCur];
    if (h.kind == 0) openTab(Tab::Kick);
    else if (h.kind == 1 && h.idx < app.list.movies.size()) openMovieDetail(app.list.movies[h.idx], h.cat);
    else if (h.kind == 2) openSeriesDetail(h.title, h.cat);
}

// particulas e brilho dos riscos de neon, bem sutis (atras do conteudo)
inline void UI::drawAmbient(Renderer &r) {
    const int W = r.width();
    const float t = (float)animT;
    // brilho que "respira" nos riscos de neon dos cantos
    int a = (int)(14 + 10 * uiSin(t * 1.3f));
    uiBand(r, 160, 178, 0, 300, uiAlpha(0xFF53FC18, a), nullptr);
    uiBand(r, (float)W + 40 + 65, (float)W + 40 + 83, 0, 300, uiAlpha(0xFF53FC18, a), nullptr);
    // pontinhos verdes subindo devagar
    for (int i = 0; i < 14; i++) {
        float ph = uiFrac(t * (0.018f + 0.004f * (i % 4)) + i * 0.137f);
        int x = (int)((uiFrac(i * 0.6180339f) * 0.96f + 0.02f) * W + 12 * uiSin(t * 0.6f + i));
        int y = (int)(r.height() * (1.f - ph));
        int al = (int)(60 * uiSin(ph * 3.14159f));
        if (al > 0) r.fillRect(x, y, 3, 3, uiAlpha(0xFF53FC18, al));
    }
}

inline void UI::heroArt(Renderer &r, const std::string &logo, int x, int y, int w, int h, int rt, int rb) {
    const uint32_t *px = nullptr; int cw = 0, ch = 0;
    std::string key = coverVariantKey(logo, w, h);
    if (!key.empty() && coverGet && coverGet(key, px, cw, ch) && px && cw > 0 && ch > 0) r.blitImageRound(x, y, w, h, px, cw, ch, rt, rb);
    else r.roundRect(x, y, w, h, rt, 0xFF161616);
}

inline void UI::drawPeek(Renderer &r, int slide, int x, int y, int w, int h, float dimA) {
    using namespace theme;
    r.roundRect(x - 2, y - 2, w + 4, h + 4, 26, 0xFF1E1E1E);
    r.roundRect(x, y, w, h, 24, 0xFF0C0C0C);
    if (slide >= 0 && slide < (int)hero.size()) {
        const HeroItem &it = hero[slide];
        if (it.kind == 0) { uiKMark(r, x + w / 2 - 34, y + h / 2 - 34, 68, 69, uiAlpha(GREEN, 120)); }
        else heroArt(r, it.logo, x + 6, y + 6, w - 12, h - 12, 20, 20);
    }
    r.roundRect(x, y, w, h, 24, ((unsigned)(int)(dimA * 255) << 24));
}

inline void UI::drawHeroCard(Renderer &r, int slide, int x, int y, int w, int h, float, bool focus) {
    using namespace theme;
    if (slide < 0 || slide >= (int)hero.size()) return;
    const HeroItem &it = hero[slide];
    uiFrame(r, x, y, w, h, 28, 3, focus ? uiAlpha(GREEN, 255) : 0xFF2A2A2A, 0xFF0A0A0A);
    if (focus) r.glow(x, y, w, h, 28, GREEN, 26, pk(70), false);
    const int pad = 36;
    const bool kick = it.kind == 0;
    if (kick) {
        r.fillRect(x + 4, y + 4, w - 8, h - 8, 0xFF050805);
        uiBand(r, (float)x + w * 0.55f, (float)x + w * 0.62f, (float)y + 4, (float)y + h - 4, 0x1853FC18, nullptr);
        uiBand(r, (float)x + w * 0.66f, (float)x + w * 0.69f, (float)y + 4, (float)y + h - 4, 0x3053FC18, nullptr);
        uiKMark(r, x + w - 470, y + h / 2 - 150, 300, 300, 0x2253FC18);
        uiKMark(r, x + pad + 6, y + 70, 74, 75, GREEN);
        uiSansL(r, x + pad + 100, y + 108, "KICK TV", TEXT, 66.f, SansBold);
        auto ls = sansWrap(it.sub, 34.f, SansMedium, 900, 2);
        for (size_t i = 0; i < ls.size(); i++) uiSansL(r, x + pad + 6, y + 190 + (int)i * 46, ls[i], SOFT, 34.f, SansMedium);
    } else {
        const int ax = x + (int)(w * 0.42f), aw = x + w - ax - 4;
        heroArt(r, it.logo, ax, y + 4, aw, h - 8, 0, 0);                        // arte (capa esticada) escurecida
        r.fillRect(ax, y + 4, aw, h - 8, 0xB8000000);
        for (int i = 0; i < 18; i++) r.fillRect(ax + i * 12, y + 4, 12, h - 8, ((unsigned)((int)(255 * (1.f - (i + 1) / 18.f) * 0.95f)) << 24) | 0x0A0A0A);   // degrade na esquerda
        r.fillRect(x + 4, y + 4, ax - x - 4, h - 8, 0xFF0A0A0A);
        for (int i = 0; i < 18; i++) r.fillRect(ax + i * 12, y + 4, 12, h - 8, ((unsigned)((int)(240 * (1.f - (i + 1) / 18.f))) << 24) | 0x0A0A0A);
        const int ph = h - 70, pw = ph * 2 / 3, px0 = x + w - pw - 120, py0 = y + 35;   // poster nitido
        r.roundRect(px0 - 3, py0 - 3, pw + 6, ph + 6, 18, 0xFF303030);
        heroArt(r, it.logo, px0, py0, pw, ph, 14, 14);
        const char *chip = it.kind == 1 ? "FILME" : "SÉRIE";
        int cwid = sansWidth(chip, 22.f, SansBold) + 40;
        r.roundRect(x + pad + 6, y + 50, cwid, 44, 22, uiAlpha(GREEN, 40));
        uiSansC(r, x + pad + 6 + cwid / 2, y + 72, chip, GREEN, 22.f, SansBold);
        auto ls = sansWrap(detailCleanTitle(it.title), 56.f, SansBold, ax - x - pad - 40, 2);
        for (size_t i = 0; i < ls.size(); i++) uiSansL(r, x + pad + 6, y + 150 + (int)i * 66, ls[i], TEXT, 56.f, SansBold);
        uiSansL(r, x + pad + 6, y + 150 + (int)ls.size() * 66 + 10, sansFit(it.sub, 28.f, SansMedium, ax - x - pad - 40), SOFT, 28.f, SansMedium);
    }
    // botao
    const int bw = 346, bh = 61, bx = x + pad + 6, by = y + h - bh - 44;
    const bool on = homeRow == 0;
    r.roundRect(bx, by, bw, bh, 18, on ? GREEN : 0xFF1C1C1C);
    if (!on) r.roundRect(bx + 2, by + 2, bw - 4, bh - 4, 16, 0xFF101010);
    const unsigned tc = on ? BLACK : TEXT;
    homePlay(r, bx + 56, by + bh / 2, 24, tc);
    uiSansL(r, bx + 100, by + bh / 2, "Assistir Agora", tc, 28.f, SansBold);
}

inline void UI::renderHome(Renderer &r) {
    using namespace theme;
    const int W = r.width();
    heroRebuild();
    wantHomeCovers();
    // pede as capas do carrossel (nitidas + versao grande para o fundo)
    if (coverBatch) {
        std::vector<std::string> front;
        for (const auto &h : hero) if (!h.logo.empty()) front.push_back(coverVariantKey(h.logo, 1358 * W / 1920 / 2, 337)), front.push_back(coverVariantKey(h.logo, 224, 336));
        std::string key = "HH|" + heroSig;
        if (key != lastHeroKey_) { lastHeroKey_ = key; coverBatch(front, {}); }
    }
    const float age = (float)(animT - fadeAt);
    auto intro = [&](float delay) { return uiEaseOut((age - delay) / 0.5f); };      // 0..1 por bloco (escalonado)

    // ---------- carrossel ----------
    {
        UiFx fx(r, 0, (int)((1.f - intro(0.0f)) * 40.f), intro(0.0f));
        Renderer &q = fx;
        const int cx = 283, cy = 169, cw = 1358, chh = 337;
        float tw = uiEaseInOut((float)(animT - heroT0) / 0.45f);                    // progresso da troca de slide
        int n = (int)hero.size();
        int dirm = 0;
        if (tw < 1.f && n > 1) dirm = ((heroCur - heroFrom + n) % n) == 1 ? 1 : -1;
        const int shift = (int)(tw * (cw + 40));
        auto S = [&](int k) { return ((heroCur + k) % n + n) % n; };
        if (n > 0) {
            // cartas laterais (peek)
            if (n > 1) {
                drawPeek(q, S(-1), 21 - (int)(dirm == -1 ? 0 : 0), cy + 22, 269, chh - 44, 0.45f);
                drawPeek(q, S(1), 1630, cy + 22, 269, chh - 44, 0.45f);
            }
            // carta central: a que sai desliza para o lado e a nova entra do outro
            if (dirm == 0) drawHeroCard(q, heroCur, cx, cy, cw, chh, 1.f, homeRow == 0);
            else {
                UiFx a(q, -dirm * shift, 0, 1.f - tw);
                drawHeroCard(a, heroFrom, cx, cy, cw, chh, 0.f, false);
                UiFx b(q, dirm * (cw + 40 - shift), 0, tw);
                drawHeroCard(b, heroCur, cx, cy, cw, chh, 1.f, homeRow == 0);
            }
            // setas e pontos
            const int ay = cy + chh / 2;
            auto chev = [&](int x, int dir) { r.roundRect(x - 26, ay - 26, 52, 52, 26, 0xB0000000); uiSansC(q, x, ay - 3, dir < 0 ? "‹" : "›", TEXT, 46.f, SansBold); };
            if (n > 1) { chev(222, -1); chev(1695, 1); }
            const int dots = n, gap = 26, dx0 = 960 - (dots - 1) * gap / 2, dy = cy + chh - 28;
            for (int i = 0; i < dots; i++) {
                float on = sm(950 + i, i == heroCur ? 1.f : 0.f, 14.f);
                int wd = 10 + (int)(on * 22);
                int xx = dx0 + i * gap - wd / 2 + (int)((on > 0.01f ? 0 : 0));
                q.roundRect(xx, dy - 5, wd, 10, 5, on > 0.5f ? GREEN : 0xFF5A5A5A);
            }
        }
    }

    // ---------- atalhos ----------
    {
        const int mx = 37, gap = 19, ty = 560, th = 251, tw4 = (W - 2 * mx - 3 * gap) / 4;
        static const char *names[4] = {"KICK TV", "FILMES", "SÉRIES", "CANAIS"};
        for (int i = 0; i < 4; i++) {
            const float in = intro(0.10f + 0.07f * i);
            const bool sel = homeRow == 1 && homeCol == i;
            const float f = sm(100 + i, sel ? 1.f : 0.f, 16.f);
            const int lift = (int)(f * 10.f);
            UiFx fx(r, 0, (int)((1.f - in) * 36.f) - lift, in);
            Renderer &q = fx;
            const int x = mx + i * (tw4 + gap), y = ty;
            if (f > 0.02f) q.glow(x, y, tw4, th, 30, GREEN, 28, (int)(f * pk(90)), false);
            unsigned line = f > 0.5f ? GREEN : 0xFF2A2A2A;
            uiFrame(q, x, y, tw4, th, 30, f > 0.5f ? 4 : 2, line, f > 0.5f ? SEL : 0xFF0D0D0D);
            const int icx = x + tw4 / 2, icy = y + 98;
            const unsigned ic = f > 0.5f ? GREEN : 0xFFEDEDED;
            const int isz = 100 + (int)(f * 8);
            if (i == 0) uiKMark(q, icx - isz * 41 / 80, icy - isz / 2, isz * 82 / 100, isz * 83 / 100, ic);
            else if (i == 1) { q.roundRect(icx - 60, icy - 44, 120, 88, 12, ic); q.roundRect(icx - 52, icy - 36, 104, 72, 8, f > 0.5f ? SEL : 0xFF0D0D0D); homePlay(q, icx - 10, icy, 36, ic); }
            else if (i == 2) { q.roundRect(icx - 60, icy - 38, 120, 78, 10, ic); q.roundRect(icx - 52, icy - 30, 104, 62, 6, f > 0.5f ? SEL : 0xFF0D0D0D); q.fillRect(icx - 30, icy + 52, 60, 6, ic); homePlay(q, icx - 8, icy - 2, 30, ic); }
            else { q.roundRect(icx - 60, icy - 36, 120, 76, 10, ic); q.roundRect(icx - 52, icy - 28, 104, 60, 6, f > 0.5f ? SEL : 0xFF0D0D0D); q.fillRect(icx - 22, icy + 52, 44, 6, ic); q.fillRect(icx - 8, icy + 40, 16, 12, ic); }
            uiSansC(q, icx, y + 205, names[i], f > 0.5f ? GREEN : TEXT, 48.f, SansBold);
            if (f > 0.02f) q.fillRect(icx - (int)(f * 60), y + th - 14, (int)(f * 120), 5, uiAlpha(GREEN, (int)(f * 255)));
        }
    }

    // ---------- LISTA IPTV ----------
    {
        const float in = intro(0.38f);
        UiFx fx(r, 0, (int)((1.f - in) * 30.f), in);
        Renderer &q = fx;
        const int mx = 37, y = 820, h = 120, w = W - 2 * mx;
        const bool sel = homeRow == 2;
        const float f = sm(120, sel ? 1.f : 0.f, 16.f);
        if (f > 0.02f) q.glow(mx, y, w, h, 26, GREEN, 22, (int)(f * pk(80)), false);
        uiFrame(q, mx, y, w, h, 26, f > 0.5f ? 4 : 2, f > 0.5f ? GREEN : 0xFF2A2A2A, f > 0.5f ? SEL : 0xFF0D0D0D);
        uiSansL(q, mx + 40, y + 40, listLocked() ? "LISTA IPTV" : "LISTA M3U", TEXT, 40.f, SansBold);
        uiSansL(q, mx + 40, y + 86, sansFit(listSummary(), 26.f, SansRegular, w - 380), SOFT, 26.f, SansRegular);
        const int pw = 250, ph = 58, px0 = mx + w - pw - 34, py0 = y + (h - ph) / 2;
        q.roundRect(px0, py0, pw, ph, 29, GREEN);
        const int xs = px0 + 34, ys = py0 + ph / 2;
        for (int d = -9; d <= 9; d++) { q.fillRect(xs + d - 1, ys + d - 1, 3, 3, BLACK); q.fillRect(xs + d - 1, ys - d - 1, 3, 3, BLACK); }
        uiSansL(q, px0 + 66, ys, listLocked() ? "ATUALIZAR" : "ADICIONAR", BLACK, 24.f, SansBold);
    }

    // ---------- comandos e apoiar ----------
    {
        const float in = intro(0.48f);
        UiFx fx(r, 0, (int)((1.f - in) * 24.f), in);
        Renderer &q = fx;
        const int mx = 37, y = 950, h = 62, gap = 19, total = W - 2 * mx;
        const bool don = hasDonate();
        const int lw = don ? 1180 * W / 1920 : total, rw = total - lw - gap;
        auto card = [&](int x, int w, bool sel, int id) {
            const float f = sm(id, sel ? 1.f : 0.f, 16.f);
            if (f > 0.02f) q.glow(x, y, w, h, 22, GREEN, 18, (int)(f * pk(80)), false);
            uiFrame(q, x, y, w, h, 22, f > 0.5f ? 3 : 2, f > 0.5f ? GREEN : 0xFF2A2A2A, f > 0.5f ? SEL : 0xFF0D0D0D);
        };
        card(mx, lw, homeRow == 3 && homeCol3 == 0, 130);
        uiSansL(q, mx + 32, y + h / 2, "COMANDOS E AJUDA", TEXT, 26.f, SansBold);
        uiSansL(q, mx + 32 + 471 * W / 1920, y + h / 2, "Botões, player, teclado, Kick TV", MUTED, 24.f, SansRegular);
        if (don) {
            const int x2 = mx + lw + gap;
            card(x2, rw, homeRow == 3 && homeCol3 == 1, 131);
            uiHeart(q, x2 + 40, y + h / 2, 26, GREEN);
            uiSansL(q, x2 + 72, y + h / 2, "APOIAR O APP", TEXT, 26.f, SansBold);
            uiSansL(q, x2 + 72 + sansWidth("APOIAR O APP", 26.f, SansBold) + 22, y + h / 2, "|  QR Code Pix", MUTED, 24.f, SansRegular);
        }
    }
}
