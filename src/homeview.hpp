#pragma once
// Inicio (incluido no fim de ui.hpp): carrossel, atalhos, lista IPTV e rodape de comandos / apoiar.
// Layout em 1920x1080 (escala pela largura). Todas as animacoes sao por tempo (animT), nunca por quadro.

inline void homePlay(Renderer &r, int cx, int cy, int s, unsigned c) {      // triangulo de "play"
    for (int i = 0; i < s; i++) { int hh = (int)((float)(s - i) / s * s * 0.5f + 0.5f); r.fillRect(cx - s / 2 + i, cy - hh, 1, hh * 2, c); }
}
inline float homeK(const Renderer &r) { return (float)r.width() / 1920.f; }


// tamanhos (variantes) das imagens: pedidas e desenhadas com os MESMOS numeros, senao a capa nao e achada no cache
static constexpr int kHeroBdW = 880, kHeroBdH = 340;      // backdrop (paisagem)
static constexpr int kHeroPkW = 264, kHeroPkH = 288;      // capa das cartas laterais
static constexpr int kHeroPsW = 178, kHeroPsH = 267;      // poster nitido (quando nao ha backdrop)

// texto sem acento/maiuscula/pontuacao, para comparar titulos do painel com os do TMDB
inline std::string homeNorm(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char b = (unsigned char)s[++i]; if (b < 0xA0) b += 0x20;
            char m = 0;
            if (b >= 0xA0 && b <= 0xA5) m = 'a'; else if (b == 0xA7) m = 'c'; else if (b >= 0xA8 && b <= 0xAB) m = 'e'; else if (b >= 0xAC && b <= 0xAF) m = 'i';
            else if (b == 0xB1) m = 'n'; else if (b >= 0xB2 && b <= 0xB6) m = 'o'; else if (b >= 0xB9 && b <= 0xBC) m = 'u';
            if (m) o += m; continue;
        }
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += (char)c;
    }
    return o;
}

// ---- Banner do Inicio: SO filmes recentes e populares (TMDB) que existem no catalogo, com imagem larga e sinopse ----
// Fluxo: o TMDB entrega os ~60 filmes recentes mais populares (App::trendingGet). Cruzamos com os nomes do catalogo (titulo igual e ano
// parecido) e o que casar vira o "conjunto" (ate 30, todos com imagem larga). O banner mostra 5 por vez:
//  - o 1o conjunto e escolhido assim que ha 5 e FICA FIXO (o catalogo que continua chegando nao troca nada);
//  - ao sair do Inicio (catalogo ja carregado) o proximo conjunto de 5 e preparado (logos e imagens baixam), e ao voltar ele aparece;
//  - nunca repete filmes ate passar por todos do conjunto.
inline void UI::heroMatch() {
    const Playlist &p = app.list;
    std::vector<App::Trend> tr;
    if (!app.trendingGet(tr)) return;
    heroTrendFail_ = tr.empty();
    if (tr.empty() || p.movies.empty()) return;
    if (heroIdxN_ != p.movies.size()) {                    // indice nome normalizado -> filme (refeito so quando o catalogo muda)
        heroIdx_.clear(); heroIdxN_ = p.movies.size();
        for (uint32_t i = 0; i < p.movies.size(); i++) {
            const Entry &m = p.movies[i];
            if (m.url.empty() || m.logo.size() < 8) continue;
            std::string y, q, n = homeNorm(detailCleanTitle(m.name, &y, &q));
            if (q == "CAM" || q == "TS" || q == "HDCAM") continue;
            if (!n.empty() && !heroIdx_.count(n)) heroIdx_[n] = i;
        }
    }
    heroMatchN_ = p.movies.size();
    std::vector<HeroItem> got;
    for (const auto &t : tr) {
        if (got.size() >= 30) break;
        auto it = heroIdx_.find(homeNorm(t.title));
        if (it == heroIdx_.end()) it = heroIdx_.find(homeNorm(t.orig));
        if (it == heroIdx_.end()) continue;
        const Entry &m = p.movies[it->second];
        std::string py; detailCleanTitle(m.name, &py);
        if (py.size() == 4 && t.year.size() == 4 && std::abs(atoi(py.c_str()) - atoi(t.year.c_str())) > 1) continue;      // outro filme com o mesmo nome
        const std::string cat = movieCategoryOf(it->second);
        HeroItem h; h.kind = 1; h.idx = it->second; h.title = m.name; h.sub = cat; h.cat = cat; h.logo = m.logo;
        h.key = "t:" + t.id; h.backdrop = t.backdrop; h.plot = t.overview; h.infoDone = true;
        got.push_back(h);
    }
    heroPool_ = got;
}

// Escolhe o proximo conjunto de 5 (os que ainda nao passaram) e pede as logos. swap: ja pode aparecer assim que estiver pronto.
inline void UI::heroPrepare(bool swap) {
    if (heroPool_.size() < 3) return;
    std::vector<HeroItem> set;
    for (int pass = 0; pass < 2 && set.size() < 5; pass++) {
        for (const auto &h : heroPool_) {
            if (set.size() >= 5) break;
            bool dup = false; for (const auto &x : set) if (x.key == h.key) dup = true;
            if (dup || heroShown_.count(h.key)) continue;
            set.push_back(h);
        }
        if (set.size() < 5 && pass == 0) heroShown_.clear();            // passou por todos: recomeca (sem repetir dentro do conjunto)
    }
    if (set.size() < 3) return;
    for (const auto &h : set) { heroShown_.insert(h.key); app.heroInfoRequest(h.key, h.key.substr(2)); }
    heroPend_ = set; heroPendT_ = animT; heroWantSwap_ = swap || hero.empty();
    heroInfoVer_++;                                                      // pede as imagens do conjunto ja
}

inline void UI::heroRebuild() {
    if (app.tmdbKey.empty() || !app.heroInfoEnabled) return;
    const Playlist &p = app.list;
    if (heroFirstT_ < 0 && !p.movies.empty()) heroFirstT_ = animT;
    if (!heroAsked_) { heroAsked_ = true; app.trendingRequest(); }
    if (heroLocked_ || !heroPend_.empty() || p.movies.empty()) return;
    if (animT - heroMatchT_ < 2.5) return;                              // procura de novo a cada 2,5 s enquanto o catalogo chega
    heroMatchT_ = animT;
    if (heroIdxN_ != p.movies.size()) heroMatch(); else if (heroPool_.empty()) heroMatch();
    if (heroPool_.size() >= 5 || (!app.prefetching() && heroPool_.size() >= 3)) heroPrepare(true);
}

inline void UI::heroApplyInfo() {
    if (!heroPend_.empty()) {                                           // conjunto novo: espera as logos (no maximo 4 s)
        bool all = true;
        for (auto &h : heroPend_) {
            if (h.logoDone) continue;
            App::HeroInfo hi;
            if (app.heroInfoGet(h.key, hi)) { h.logoDone = true; h.tlogo = hi.logo; } else all = false;
        }
        if ((all || animT - heroPendT_ > 4.0) && (hero.empty() || heroWantSwap_)) {
            hero = heroPend_; heroPend_.clear(); heroCur = 0; heroFrom = 0; heroAuto = animT; heroWantSwap_ = false;
            heroLocked_ = true; heroSig = std::to_string(++heroSetNo_); heroInfoVer_++;
        }
    }
    for (auto &h : hero) {                                              // logo que chegou depois
        if (h.logoDone || h.key.empty()) continue;
        App::HeroInfo hi;
        if (!app.heroInfoGet(h.key, hi)) continue;
        h.logoDone = true; h.tlogo = hi.logo; heroInfoVer_++;
    }
}

inline void UI::heroOnLeaveHome() {
    if (!heroLocked_ || app.prefetching() || !heroPend_.empty()) return;   // carregando a lista: nada muda
    heroMatch();                                                         // catalogo completo: conjunto maior
    heroPrepare(false);
}
inline void UI::heroOnEnterHome(double t) {
    heroCur = 0; heroFrom = 0; heroAuto = t;
    if (!heroPend_.empty() && !app.prefetching()) heroWantSwap_ = true;
}

// Pede as imagens do carrossel primeiro (backdrops/capas) e as das abas depois; so quando algo mudou.
inline void UI::requestHomeCovers() {
    if (!coverBatch) return;
    std::string key = homeCoverKey() + "#" + heroSig + "#" + std::to_string(heroInfoVer_) + "#" + std::to_string(heroPend_.size());
    if (key == lastHeroKey_) return;
    lastHeroKey_ = key;
    std::vector<std::string> front;
    for (const auto &h : hero) if (!h.backdrop.empty()) front.push_back(coverVariantKey(h.backdrop, kHeroBdW, kHeroBdH));
    for (const auto &h : hero) if (!h.tlogo.empty()) front.push_back(coverVariantKey(h.tlogo, kCoverLogoW, kCoverLogoH));
    for (const auto &h : hero) if (!h.logo.empty()) front.push_back(coverVariantKey(h.logo, kHeroPkW, kHeroPkH));
    std::vector<std::string> back;
    for (const auto &h : heroPend_) {                                   // conjunto seguinte: baixa enquanto o atual passa
        if (!h.backdrop.empty()) back.push_back(coverVariantKey(h.backdrop, kHeroBdW, kHeroBdH));
        if (!h.tlogo.empty()) back.push_back(coverVariantKey(h.tlogo, kCoverLogoW, kCoverLogoH));
        if (!h.logo.empty()) back.push_back(coverVariantKey(h.logo, kHeroPkW, kHeroPkH));
    }
    for (const auto &u : homeCoverList()) back.push_back(u);
    coverBatch(front, back);
}

inline void UI::heroMove(int d) {
    int n = (int)hero.size(); if (n < 2) return;
    heroFrom = heroCur; heroCur = ((heroCur + d) % n + n) % n;
    heroT0 = animT; heroAuto = animT;
}

inline void UI::heroTick(double t) {
    if (screen != Screen::Home) return;
    heroRebuild();
    heroApplyInfo();
    if (heroAuto <= 0) heroAuto = t;
    if (t - heroAuto > 2.0) {
        int n = (int)hero.size();
        if (n > 1) { heroFrom = heroCur; heroCur = (heroCur + 1) % n; heroT0 = t; }
        heroAuto = t;
    }
}

inline void UI::heroOpen() {
    heroRebuild();
    if (hero.empty()) { if (listLocked()) requestReload(); else requestM3U(); return; }
    const HeroItem &h = hero[heroCur];
    if (h.kind == 1 && h.idx < app.list.movies.size()) openMovieDetail(app.list.movies[h.idx], h.cat);
    else if (h.kind == 2) openSeriesDetail(h.title, h.cat);
    if (screen == Screen::Detail) detailBackHome = true;      // o detalhe aberto pelo banner volta para o Inicio
}

// particulas e brilho dos riscos de neon, bem sutis (atras do conteudo)
inline void UI::drawAmbient(Renderer &r) {
    const int W = r.width();
    const float t = (float)animT;
    // brilho que "respira" nos riscos de neon dos cantos (parado quando kUiBreathing = false)
    int a = kUiBreathing ? (int)(14 + 10 * uiSin(t * 1.3f)) : 14;
    // tracos de neon nos cantos (como na referencia)
    const unsigned G = 0xFF53FC18;
    uiBand(r, 100, 150, 0, 260, uiAlpha(G, 190), nullptr);   uiBand(r, 175, 195, 0, 260, uiAlpha(G, 130), nullptr);   uiBand(r, 230, 420, 0, 260, uiAlpha(G, 22), nullptr);
    uiBand(r, (float)W + 90, (float)W + 120, 0, 260, uiAlpha(G, 190), nullptr);   uiBand(r, (float)W + 145, (float)W + 157, 0, 260, uiAlpha(G, 130), nullptr);
    uiBand(r, 640, 690, 520, 1080, uiAlpha(G, 150), nullptr);   uiBand(r, 715, 730, 520, 1080, uiAlpha(G, 90), nullptr);
    uiBand(r, (float)W + 640, (float)W + 700, 560, 1080, uiAlpha(G, 90), nullptr);
    uiBand(r, 160, 178, 0, 300, uiAlpha(0xFF53FC18, a), nullptr);
    uiBand(r, (float)W + 40 + 65, (float)W + 40 + 83, 0, 300, uiAlpha(0xFF53FC18, a), nullptr);
    // pontinhos verdes subindo devagar (redesenho continuo: so com kUiBreathing)
    if (kUiBreathing) for (int i = 0; i < 14; i++) {
        float ph = uiFrac(t * (0.018f + 0.004f * (i % 4)) + i * 0.137f);
        int x = (int)((uiFrac(i * 0.6180339f) * 0.96f + 0.02f) * W + 12 * uiSin(t * 0.6f + i));
        int y = (int)(r.height() * (1.f - ph));
        int al = (int)(60 * uiSin(ph * 3.14159f));
        if (al > 0) r.fillRect(x, y, 3, 3, uiAlpha(0xFF53FC18, al));
    }
}

inline bool UI::heroArt(Renderer &r, const std::string &imgUrl, int vw, int vh, int x, int y, int w, int h, int rt, int rb) {
    const uint32_t *px = nullptr; int cw = 0, ch = 0;
    if (imgUrl.empty() || !coverGet) return false;
    std::string key = coverVariantKey(imgUrl, vw, vh);
    if (!key.empty() && coverGet(key, px, cw, ch) && px && cw > 0 && ch > 0) { r.blitImageRound(x, y, w, h, px, cw, ch, rt, rb); return true; }
    return false;
}

// seta grossa desenhada com linhas (a fonte nao tem os glifos)
inline void UI::heroChevron(Renderer &r, int cx, int cy, int dir, unsigned c) {
    const int th = 6;
    // dois tracos que se encontram na ponta
    int tipx = cx + dir * 8;
    for (int i = 0; i <= 16; i++) {
        int bx = tipx - dir * i;
        r.fillRect(bx - th / 2, cy - i - th / 2, th, th, c);
        r.fillRect(bx - th / 2, cy + i - th / 2, th, th, c);
    }
}

// carta lateral inclinada (perspectiva): a borda de fora e menor que a de dentro, coluna a coluna
inline void UI::drawPeek(Renderer &r, int slide, int x, int y, int w, int h, float dimA, int dir) {
    using namespace theme;
    const uint32_t *px = nullptr; int cw = 0, ch = 0;
    bool ok = false;
    if (slide >= 0 && slide < (int)hero.size() && coverGet) {
        std::string key = coverVariantKey(hero[slide].logo, kHeroPkW, kHeroPkH);
        ok = !key.empty() && coverGet(key, px, cw, ch) && px && cw > 0 && ch > 0;
    }
    // A carta inclinada e montada UMA vez (por capa) num bloco de pixels e depois so copiada: antes eram ~270 colunas x 2 cartas redesenhadas a cada quadro.
    const int di = dir < 0 ? 0 : 1;
    const std::string key = (ok ? hero[slide].logo : std::string("-")) + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string((int)(dimA * 100));
    if (peekKey_[di] != key || peekBuf_[di].size() != (size_t)w * h) {
        peekKey_[di] = key;
        std::vector<uint32_t> &B = peekBuf_[di];
        B.assign((size_t)w * h, 0u);
        const unsigned keep = (unsigned)((1.f - dimA) * 256.f);                 // escurece (dimA = quanto de preto)
        auto dark = [&](unsigned c) { return 0xFF000000u | (((((c >> 16) & 255) * keep) >> 8) << 16) | (((((c >> 8) & 255) * keep) >> 8) << 8) | ((((c) & 255) * keep) >> 8); };
        for (int c = 0; c < w; c++) {
            float t = (float)c / (float)(w - 1);                       // 0 = esquerda
            float inner = dir < 0 ? t : 1.f - t;                       // 1 = lado que encosta no cartao
            int dh = (int)(h * (0.84f + 0.16f * inner));
            int yy = (h - dh) / 2;
            int u = ok ? c * cw / w : 0;
            for (int j = 0; j < dh; j++) {
                unsigned col;
                if (c < 2 || c >= w - 2 || j < 2 || j >= dh - 2) col = 0xFF000000u | (((unsigned)(0x53 * 170 + 0x16 * 85) / 255) << 16) | (((unsigned)(0xFC * 170 + 0x16 * 85) / 255) << 8) | ((unsigned)(0x18 * 170 + 0x16 * 85) / 255);
                else col = ok ? px[(size_t)(j * ch / dh) * cw + u] : 0xFF161616u;
                B[(size_t)(yy + j) * w + c] = dark(col);
            }
        }
    }
    r.blitImageRound(x, y, w, h, peekBuf_[di].data(), w, h, 0, 0);
}

inline void UI::drawHeroCard(Renderer &r, int slide, int x, int y, int w, int h, float, bool focus) {
    using namespace theme;
    r.glow(x, y, w, h, 28, GREEN, focus ? 30 : 20, focus ? pk(110) : pk(60), false);
    uiFrame(r, x, y, w, h, 28, 4, GREEN, 0xFF0A0A0A);
    const int pad = 46;
    const bool has = slide >= 0 && slide < (int)hero.size();
    const int ix = x + 4, iy = y + 4, iw = w - 8, ih = h - 8;
    std::string title, body;
    if (has) {
        const HeroItem &it = hero[slide];
        title = detailCleanTitle(it.title); body = it.plot.empty() ? it.sub : it.plot;
        const int ax = x + (int)(w * 0.42f), aw = x + w - 4 - ax;
        bool art = !it.backdrop.empty() && heroArt(r, it.backdrop, kHeroBdW, kHeroBdH, ax, iy, aw, ih, 0, 0);
        if (!art) {                                         // sem backdrop: faixas verdes + poster nitido
            r.fillRect(ix, iy, iw, ih, 0xFF070A07);
            const UiClip cl{ix, iy, iw, ih, 24};
            for (int b = 0; b < 4; b++) uiBand(r, (float)x + w * (0.50f + 0.07f * b), (float)x + w * (0.50f + 0.07f * b) + 26 + 8 * b, (float)iy, (float)iy + ih, uiAlpha(GREEN, 18 + 8 * b), &cl);
            const int ph = h - 70, pw = ph * 2 / 3, px0 = x + w - pw - 130, py0 = y + 35;
            r.roundRect(px0 - 3, py0 - 3, pw + 6, ph + 6, 18, 0xFF303030);
            if (!heroArt(r, it.logo, kHeroPsW, kHeroPsH, px0, py0, pw, ph, 14, 14)) r.roundRect(px0, py0, pw, ph, 14, 0xFF161616);
        } else {
            r.fillRect(ix, iy, ax - ix, ih, 0xFF0A0A0A);   // coluna esquerda do texto
            const int steps = 64, sw = (int)(aw * 0.60f) / steps + 1;
            for (int i = 0; i < steps; i++) { float t = (float)i / steps; r.fillRect(ax + i * sw, iy, sw, ih, ((unsigned)(int)(255.f * (1.f - t) * (1.f - t * 0.2f)) << 24) | 0x0A0A0A); }
            for (int i = 0; i < 14; i++) r.fillRect(ax, iy + ih - 14 + i, aw, 1, ((unsigned)(i * 9) << 24));   // sombra suave embaixo
        }
    } else {
        r.fillRect(ix, iy, iw, ih, 0xFF070A07);
        title = "Nenhuma lista carregada"; body = "Carregue a lista M3U para ver filmes e séries aqui.";
    }
    const int tw = (int)(w * 0.44f);
    bool logoOk = false;
    const int lgY = y + 60;                                  // logo do titulo (area 480x150, alinhada embaixo)
    if (has && !hero[slide].tlogo.empty()) logoOk = heroArt(r, hero[slide].tlogo, kCoverLogoW, kCoverLogoH, x + pad - 4, lgY, kCoverLogoW, kCoverLogoH, 0, 0);
    auto ls = sansWrap(logoOk ? std::string() : title, 62.f, SansBold, tw, 2);
    const int nl = logoOk ? 0 : (int)ls.size() < 1 ? 1 : (int)ls.size();
    const int ty0 = logoOk ? lgY + kCoverLogoH + 36 - 0 : y + 78 + (nl == 1 ? 24 : 0);
    for (size_t i = 0; i < ls.size(); i++) uiSansL(r, x + pad, ty0 + (int)i * 68, ls[i], TEXT, 62.f, SansBold);
    auto bl = sansWrap(body, 29.f, SansMedium, tw, logoOk ? 2 : 2);
    for (size_t i = 0; i < bl.size(); i++) uiSansL(r, x + pad, logoOk ? ty0 + (int)i * 36 : ty0 + nl * 68 + 8 + (int)i * 38, bl[i], TEXT, 29.f, SansMedium);
    // botao "Assistir Agora" (contorno verde; cheio quando o carrossel esta selecionado)
    const int bw = 350, bh = 62, bx = x + pad, by = y + h - bh - 38;
    const bool on = homeRow == 0;
    r.roundRect(bx, by, bw, bh, 20, GREEN);
    r.roundRect(bx + 3, by + 3, bw - 6, bh - 6, 17, on ? GREEN : 0xFF0B140A);
    const unsigned tc = on ? BLACK : GREEN;
    homePlay(r, bx + 56, by + bh / 2, 24, tc);
    uiSansL(r, bx + 100, by + bh / 2, has ? "Assistir Agora" : "Carregar lista", tc, 29.f, SansBold);
}

inline void UI::renderHome(Renderer &r) {
    using namespace theme;
    const int W = r.width();
    heroRebuild();
    heroApplyInfo();
    requestHomeCovers();
    const float age = (float)(animT - fadeAt);
    auto intro = [&](float delay) { return kUiFancyTransitions ? uiEaseOut((age - delay) / 0.5f) : 1.f; };      // 0..1 por bloco (escalonado)

    // ---------- carrossel ----------
    {
        UiFx fx(r, 0, (int)((1.f - intro(0.0f)) * 40.f), intro(0.0f));
        Renderer &q = fx;
        const int cx = 283, cy = 168, cw = 1358, chh = 432;
        float tw = kUiFancyTransitions ? uiEaseInOut((float)(animT - heroT0) / 0.45f) : 1.f;      // progresso da troca de slide (sem animacao: troca seca)
        int n = (int)hero.size();
        int dirm = 0;
        if (tw < 1.f && n > 1) dirm = ((heroCur - heroFrom + n) % n) == 1 ? 1 : -1;
        const int shift = (int)(tw * (cw + 40));
        auto S = [&](int k) { return ((heroCur + k) % n + n) % n; };
        if (n > 1) {
            drawPeek(q, S(-1), 21, cy + 22, 269, chh - 44, 0.35f, -1);
            drawPeek(q, S(1), 1630, cy + 22, 269, chh - 44, 0.35f, 1);
        }
        if (dirm == 0 || n < 2) drawHeroCard(q, n ? heroCur : -1, cx, cy, cw, chh, 1.f, homeRow == 0);
        else {
            UiFx a(q, -dirm * shift, 0, 1.f - tw);
            drawHeroCard(a, heroFrom, cx, cy, cw, chh, 0.f, false);
            UiFx b(q, dirm * (cw + 40 - shift), 0, tw);
            drawHeroCard(b, heroCur, cx, cy, cw, chh, 1.f, homeRow == 0);
        }
        if (n > 1) {
            const int ay = cy + chh / 2;
            auto chev = [&](int x, int dir) { heroChevron(q, x, ay, dir, GREEN); };
            chev(222, -1); chev(1695, 1);
            const int gap = 26, dx0 = 960 - (n - 1) * gap / 2, dy = cy + chh - 26;
            for (int i = 0; i < n; i++) {
                float on = sm(950 + i, i == heroCur ? 1.f : 0.f, 14.f);
                int wd = 10 + (int)(on * 22);
                q.roundRect(dx0 + i * gap - wd / 2, dy - 5, wd, 10, 5, on > 0.5f ? GREEN : 0xFF5A5A5A);
            }
        }
    }

    // ---------- atalhos (icone branco, faixas verdes no canto, rotulo + sublinhado) ----------
    {
        const int mx = 69, gap = 31, ty = 626, th = 200, tw4 = (W - 2 * mx - 3 * gap) / 4;
        static const char *names[4] = {"KICK TV", "FILMES", "SÉRIES", "CANAIS"};
        for (int i = 0; i < 4; i++) {
            const float in = intro(0.10f + 0.07f * i);
            const bool sel = homeRow == 1 && homeCol == i;
            const float f = sm(100 + i, sel ? 1.f : 0.f, 16.f);
            const int lift = (int)(f * 8.f);
            UiFx fx(r, 0, (int)((1.f - in) * 36.f) - lift, in);
            Renderer &q = fx;
            const int x = mx + i * (tw4 + gap), y = ty;
            if (f > 0.02f) q.glow(x, y, tw4, th, 28, GREEN, 28, (int)(f * pk(120)), false);
            const bool hi = f > 0.5f;
            uiFrame(q, x, y, tw4, th, 28, hi ? 4 : 2, hi ? GREEN : 0xFF2C2C2C, hi ? 0xFF0C1A0A : 0xFF111111);
            const UiClip cl{x + 3, y + 3, tw4 - 6, th - 6, 25};
            const float kC = (float)(x + tw4 + y + th);                  // canto inferior direito
            uiBand(q, kC - 52, kC + 10, (float)y, (float)(y + th), uiAlpha(GREEN, 70), &cl);          // triangulo escuro no canto
            uiBand(q, kC - 122, kC - 64, (float)y, (float)(y + th), GREEN, &cl);                       // faixa larga
            uiBand(q, kC - 156, kC - 136, (float)y, (float)(y + th), uiAlpha(GREEN, 150), &cl);        // faixa fina
            const unsigned bg = hi ? 0xFF0C1A0A : 0xFF111111;
            const int icx = x + tw4 / 2, isz = 92 + (int)(f * 6), iy0 = y + 20;
            const unsigned W0 = 0xFFFFFFFF;
            if (i == 0) uiKMark(q, icx - isz * 41 / 100, iy0, isz * 82 / 100, isz * 83 / 100, GREEN);
            else if (i == 1) uiIconClapper(q, icx - isz / 2, iy0, isz, W0, bg);
            else if (i == 2) uiIconTvPlay(q, icx - isz / 2, iy0, isz, W0, bg);
            else uiIconTvRetro(q, icx - isz / 2, iy0, isz, W0, bg);
            uiSansC(q, icx, y + 146, names[i], TEXT, 40.f, SansBold);
            const int ul = tw4 * 55 / 100;
            q.fillRect(icx - ul / 2, y + 174, ul, 3, hi ? GREEN : uiAlpha(GREEN, 110));
        }
    }

    // ---------- LISTA IPTV ----------
    {
        const float in = intro(0.38f);
        UiFx fx(r, 0, (int)((1.f - in) * 30.f), in);
        Renderer &q = fx;
        const int mx = 69, y = 846, h = 92, w = W - 2 * mx;
        const bool sel = homeRow == 2;
        const float f = sm(120, sel ? 1.f : 0.f, 16.f);
        if (f > 0.02f) q.glow(mx, y, w, h, 26, GREEN, 22, (int)(f * pk(80)), false);
        uiFrame(q, mx, y, w, h, 26, f > 0.5f ? 4 : 2, f > 0.5f ? GREEN : 0xFF2A2A2A, f > 0.5f ? SEL : 0xFF0D0D0D);
        {   // icone de corrente (dois elos), divisor verde, titulo e resumo
            const int lx = mx + 36, ly = y + h / 2;
            uiFrame(q, lx, ly - 12, 40, 24, 12, 4, TEXT, f > 0.5f ? SEL : 0xFF0D0D0D);
            uiFrame(q, lx + 26, ly - 12, 40, 24, 12, 4, TEXT, f > 0.5f ? SEL : 0xFF0D0D0D);
            uiFrame(q, lx + 26, ly - 12, 14, 24, 8, 4, TEXT, f > 0.5f ? SEL : 0xFF0D0D0D);
            q.fillRect(mx + 118, y + 22, 3, h - 44, GREEN);
        }
        uiSansL(q, mx + 150, y + 34, listLocked() ? "LISTA IPTV" : "LISTA M3U", TEXT, 36.f, SansBold);
        uiSansL(q, mx + 150, y + 76, sansFit(listSummary(), 24.f, SansRegular, w - 520), SOFT, 24.f, SansRegular);
        const int pw = 250, ph = 56, px0 = mx + w - pw - 34, py0 = y + (h - ph) / 2;
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
        const int mx = 69, y = 956, h = 58, gap = 31, total = W - 2 * mx;
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
