#pragma once
// Pagina de detalhes de filme / serie (incluida no fim de ui.hpp).
// Layout desenhado em coordenadas da imagem de referencia (1672x941) e escalado pela largura da tela (k = largura / 1672):
// no PS4 (1920x1080) k = 1,148; o preview do PC renderiza em 1672x941 para comparar pixel a pixel com a foto.
// Filme: so cabecalho + pagina de informacoes (sem temporadas / episodios).
#include <string>
#include <vector>

namespace dp {
// Cores medidas na imagem de referencia (o verde dela e mais "puro" que o #53FC18 do resto do app)
constexpr unsigned GREEN = 0xFF0FFB19, GREEN_SOFT = 0xFF8AD88A, BG = 0xFF010403;
constexpr unsigned TXT = 0xFFFFFFFF, TXT2 = 0xFFE4E4E4, TXT3 = 0xFFC8C8C8, GRAY_LINE = 0xFF4B4E4F, CHIP_LINE = 0xFF464747;
constexpr unsigned TAB_FILL = 0xFF0E0F0E, TAB_SEL_FILL = 0xFF032604, CARD_FILL = 0xFF020505, KEYCAP = 0xFF1C1F1F, STAR = 0xFFFFB81C;

inline void ring(Renderer &r, float cx, float cy, float R, float t, unsigned c) {   // anel de raio externo R e espessura t
    uiPaint(r, (int)(cx - R - 2), (int)(cy - R - 2), (int)(cx + R + 3), (int)(cy + R + 3), c, [&](float px, float py) {
        float dx = px - cx, dy = py - cy, d = uiSqrt(dx * dx + dy * dy);
        return uiClamp01(0.5f - (uiAbs(d - (R - t * 0.5f)) - t * 0.5f));
    });
}
inline void thickLine(Renderer &r, float x0, float y0, float x1, float y1, float t, unsigned c) {
    float dx = x1 - x0, dy = y1 - y0, len = uiSqrt(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    UiPt p[4] = {{x0 + nx, y0 + ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}, {x0 - nx, y0 - ny}};
    uiPoly(r, p, 4, c);
}
inline void triRight(Renderer &r, float x, float y, float w, float h, unsigned c) {   // triangulo "play" (x,y = canto superior esquerdo)
    UiPt p[3] = {{x, y}, {x, y + h}, {x + w, y + h * 0.5f}};
    uiPoly(r, p, 3, c);
}
inline void triLeft(Renderer &r, float x, float y, float w, float h, unsigned c) {
    UiPt p[3] = {{x + w, y}, {x + w, y + h}, {x, y + h * 0.5f}};
    uiPoly(r, p, 3, c);
}
inline void star(Renderer &r, float cx, float cy, float R, unsigned c) {
    UiPt p[10];
    static const float ox[5] = {0.f, 0.9511f, 0.5878f, -0.5878f, -0.9511f}, oy[5] = {-1.f, -0.3090f, 0.8090f, 0.8090f, -0.3090f};
    for (int i = 0; i < 5; i++) {
        p[2 * i] = {cx + ox[i] * R, cy + oy[i] * R};
        float a = (ox[i] + ox[(i + 1) % 5]) * 0.5f, b = (oy[i] + oy[(i + 1) % 5]) * 0.5f;       // ponto interno: meio da aresta, para dentro
        float m = uiSqrt(a * a + b * b); if (m < 0.001f) m = 1.f;
        p[2 * i + 1] = {cx + a / m * R * 0.45f, cy + b / m * R * 0.45f};
    }
    uiPoly(r, p, 10, c);
}
}  // namespace dp

// ---------------- Abrir ----------------
inline void UI::openMovieDetail(const Entry &e, const std::string &category) {
    detailSeries = false;
    std::string y, q;
    detailTitle = detailCleanTitle(e.name, &y, &q);
    detailLogo = e.logo; detailUrl = e.url; detailGroup = category;
    detailFocus = 0; detailEpCur = 0; detailEpScroll = 0; detailSeaScroll = 0; detailSeasonIdx = 0;
    detailSeasons.clear();
    detailMeta = DetailMeta();
    detailMeta.year = y; detailMeta.quality = q;
    if (!category.empty() && category != "Sem categoria" && category.size() <= 28) detailMeta.genres.push_back(category);
    if (metaLookup) metaLookup(detailTitle, false, detailMeta);
    lastBatchKey.clear();
    screen = Screen::Detail;
    app.say(e.url.empty() ? "Este filme nao tem link de video" : "Detalhes do filme");
}
inline void UI::openSeriesDetail(const std::string &name, const std::string &category) {
    detailSeries = true;
    std::string y, q;
    detailTitle = detailCleanTitle(name, &y, &q);
    detailLogo = seriesLogo(name); detailUrl.clear(); detailGroup = category;
    detailEpCur = 0; detailEpScroll = 0; detailSeaScroll = 0; detailSeasonIdx = 0;
    detailSeasons.clear();
    auto it = app.list.seriesTree.find(name);
    if (it != app.list.seriesTree.end()) for (const auto &kv : it->second) detailSeasons.push_back(kv.first);
    curSeries = name;
    if (!detailSeasons.empty()) curSeason = detailSeasons[0];
    detailMeta = DetailMeta();
    detailMeta.year = y; detailMeta.quality = q;
    if (!category.empty() && category != "Sem categoria" && category.size() <= 28) detailMeta.genres.push_back(category);
    if (metaLookup) metaLookup(detailTitle, true, detailMeta);
    detailFocus = detailEpisodes().empty() ? 0 : 2;          // como na imagem: foco no 1o episodio
    lastBatchKey.clear();
    screen = Screen::Detail;
    app.say("Detalhes da serie");
}

inline std::vector<Entry> UI::detailEpisodes() const {
    if (!detailSeries || detailSeasons.empty()) return {};
    int sn = detailSeasons[detailSeasonIdx < (int)detailSeasons.size() ? detailSeasonIdx : 0];
    auto sit = app.list.seriesTree.find(curSeries);
    if (sit == app.list.seriesTree.end()) return {};
    auto it = sit->second.find(sn);
    if (it == sit->second.end()) return {};
    return sortedEps(it->second);
}

inline void UI::playSeriesEpisode(int epIndex) {
    auto eps = detailEpisodes();
    if (epIndex < 0 || epIndex >= (int)eps.size()) return;
    if (eps[epIndex].url.empty()) { app.say("Episodio sem link"); return; }
    std::vector<std::string> urls, titles;
    for (size_t i = 0; i < eps.size(); i++) {
        urls.push_back(eps[i].url);
        titles.push_back(detailTitle + " - " + detailEpisodeLabel(curSeries, eps[i].name, eps[i].episode, (int)i + 1));
    }
    app.pf.play(urls[epIndex], titles[epIndex]);
    app.pf.setEpisodeList(urls, titles, epIndex);
}

// ---------------- Controle ----------------
inline void UI::handleDetail(Input in) {
    if (in == Input::Back) { screen = Screen::List; lastBatchKey.clear(); return; }
    if (!detailSeries) {
        if (in == Input::Confirm) {
            if (detailUrl.empty()) app.say("Sem link de video");
            else app.pf.play(detailUrl, detailTitle);
        }
        return;
    }
    const int nSea = (int)detailSeasons.size();
    int nEp = (int)detailEpisodes().size();
    auto setSeason = [&](int idx) {
        if (idx < 0) idx = 0;
        if (idx > nSea - 1) idx = nSea - 1;
        if (idx < 0 || idx == detailSeasonIdx) return;
        detailSeasonIdx = idx; detailEpCur = 0; detailEpScroll = 0;
        curSeason = detailSeasons[idx];
        if (idx < detailSeaScroll) detailSeaScroll = idx;
        if (idx >= detailSeaScroll + kDetTabs) detailSeaScroll = idx - kDetTabs + 1;
    };
    auto setEp = [&](int i) {
        if (nEp <= 0) return;
        if (i < 0) i = 0;
        if (i > nEp - 1) i = nEp - 1;
        detailEpCur = i;
        if (i < detailEpScroll) detailEpScroll = i;
        if (i >= detailEpScroll + kDetEps) detailEpScroll = i - kDetEps + 1;
    };
    switch (in) {
    case Input::Up:
        if (detailFocus == 2) detailFocus = nSea > 0 ? 1 : 0; else if (detailFocus == 1) detailFocus = 0;
        break;
    case Input::Down:
        if (detailFocus == 0) detailFocus = nSea > 0 ? 1 : (nEp > 0 ? 2 : 0);
        else if (detailFocus == 1 && nEp > 0) detailFocus = 2;
        break;
    case Input::Left:  if (detailFocus == 1) setSeason(detailSeasonIdx - 1); else if (detailFocus == 2) setEp(detailEpCur - 1); break;
    case Input::Right: if (detailFocus == 1) setSeason(detailSeasonIdx + 1); else if (detailFocus == 2) setEp(detailEpCur + 1); break;
    case Input::PrevTab: setSeason(detailSeasonIdx - 1); break;     // L1 / R1: troca de temporada de qualquer lugar
    case Input::NextTab: setSeason(detailSeasonIdx + 1); break;
    case Input::PageUp: if (detailFocus == 2) setEp(detailEpCur - kDetEps); break;
    case Input::PageDown: if (detailFocus == 2) setEp(detailEpCur + kDetEps); break;
    case Input::Confirm:
        if (detailFocus == 0) { if (nEp > 0) playSeriesEpisode(0); else app.say("Sem episodios"); }
        else if (detailFocus == 1) { if (nEp > 0) detailFocus = 2; }
        else playSeriesEpisode(detailEpCur);
        break;
    default: break;
    }
}

inline void UI::wantDetailCovers(const std::string &poster, const std::vector<std::string> &front, const std::vector<std::string> &back) {
    if (!coverBatch) return;
    std::string key = "D|" + poster + "|" + (front.empty() ? "" : front[0]) + "|" + std::to_string(front.size()) + "|" + std::to_string(back.size());
    if (key == lastBatchKey) return;
    lastBatchKey = key;
    std::vector<std::string> f;
    if (!poster.empty()) f.push_back(poster);
    for (const auto &u : front) if (!u.empty()) f.push_back(u);
    coverBatch(f, back);
}

// ---------------- Desenho ----------------
inline void UI::renderDetail(Renderer &r) {
    using namespace dp;
    const int W = r.width(), H = r.height();
    const float k = (float)W / 1672.f;
    auto S = [&](float v) { return (int)(v * k + (v < 0 ? -0.5f : 0.5f)); };
    auto F = [&](float v) { return v * k; };
    const auto eps = detailEpisodes();
    const int nSea = (int)detailSeasons.size(), nEp = (int)eps.size();

    // ---- fundo: preto + brilho verde suave a direita ----
    r.fillRect(0, 0, W, H, BG);
    {
        const float cx = F(1570), cy = F(390), rx = F(430), ry = F(240);
        int x0 = (int)(cx - rx), x1 = W, y0 = (int)(cy - ry), y1 = (int)(cy + ry);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (y1 > H) y1 = H;
        for (int y = y0; y < y1; y++) {
            float dy = (y + 0.5f - cy) / ry;
            for (int x = x0; x < x1; x++) {
                float dx = (x + 0.5f - cx) / rx, d2 = dx * dx + dy * dy;
                if (d2 >= 1.f) continue;
                float f = 1.f - d2;
                int a = (int)(f * f * 21.f);
                if (a > 0) r.pixel(x, y, ((unsigned)a << 24) | 0x0000FF70u);
            }
        }
    }
    // ---- cabecalho ----
    {
        const int lineY = S(84);
        UiClip none{0, 0, W, lineY + 1, 0};
        uiBand(r, F(1063), F(1120), 0, (float)lineY, 0xFF042507u, &none);
        uiBand(r, F(1158), F(1206), 0, (float)lineY, 0xFF053109u, &none);
        uiBand(r, F(1223), F(1294), 0, (float)lineY, 0xFF1CC729u, &none);
        r.fillRect(0, lineY, W, 1, 0xFF0F840Du);
        r.fillRect(0, lineY + 1, W, S(2) < 2 ? 2 : S(2) + 0, GREEN);
        r.fillRect(0, lineY + 1 + (S(2) < 2 ? 2 : S(2)) - 1, W, 1, 0xFF11CA17u);
        const int lh = S(47);
        int lw = uiWord(r, "KICK", S(53), S(29), lh, GREEN, 115);
        uiWord(r, "IPTV", S(53) + lw + S(26), S(29), lh, TXT, 106);
        uiIconWifi(r, S(1435), S(66.5f), S(48), GREEN);
        r.fillRect(S(1494), S(30), S(2) < 2 ? 2 : S(2), S(48), 0xFF6A6A6A);
        const int rx = S(1616);
        const int cs = (int)(F(34) + 0.5f), ds = (int)(F(16) + 0.5f);
        uiHeavy(r, rx - r.textWidth(timeText, cs) - S(3), S(25), timeText, TXT, cs);
        uiText(r, rx - r.textWidth(dateText, ds), S(63), dateText, TXT2, ds);
    }
    // ---- migalhas: <- Series | Titulo ----
    {
        const float t = F(3.2f);
        const float ay = F(122.0f);
        dp::thickLine(r, F(51.5f), ay, F(78), ay, t, TXT);
        dp::thickLine(r, F(51.5f), ay, F(64), ay - F(11.5f), t, TXT);
        dp::thickLine(r, F(51.5f), ay, F(64), ay + F(11.5f), t, TXT);
        sansDraw(r, S(105), S(131), detailSeries ? "S\xC3\xA9ries" : "Filmes", TXT, F(24.2f), SansBold);
        r.fillRect(S(207), S(105), S(2) < 2 ? 2 : S(2), S(36), 0xFF8A8A8A);
        sansDraw(r, S(238), S(131), sansFit(detailTitle, F(19.8f), SansMedium, W - S(238) - S(60)), TXT, F(19.8f), SansMedium);
    }
    // ---- poster ----
    {
        const int px = S(52), py = S(162), pw = S(337), ph = S(345), bw = S(2) < 2 ? 2 : S(2);
        r.roundRect(px, py, pw, ph, S(10), GRAY_LINE);
        r.roundRect(px + bw, py + bw, pw - 2 * bw, ph - 2 * bw, S(8), 0xFF101414);
        const int iw = pw - 2 * bw, ih = ph - 2 * bw;
        std::string key = coverVariantKey(detailLogo, iw, ih);
        bool drew = false;
        if (!key.empty() && coverGet) {
            const uint32_t *pxl = nullptr; int cw = 0, ch = 0;
            if (coverGet(key, pxl, cw, ch) && pxl && cw > 0 && ch > 0) { r.blitImageRound(px + bw, py + bw, iw, ih, pxl, cw, ch, S(8), S(8)); drew = true; }
        }
        if (!drew) {
            sansDraw(r, px + pw / 2 - sansWidth(detailSeries ? "S\xC3\x89RIE" : "FILME", F(26), SansBold) / 2, py + ph / 2, detailSeries ? "S\xC3\x89RIE" : "FILME", GREEN, F(26), SansBold);
            std::string t = sansFit(detailTitle, F(18), SansRegular, pw - S(40));
            sansDraw(r, px + (pw - sansWidth(t, F(18), SansRegular)) / 2, py + ph / 2 + S(34), t, TXT3, F(18), SansRegular);
        }
    }
    // ---- bloco de informacoes (x = 423) ----
    const int tx = S(423), maxTx = W - S(52);
    {
        sansDraw(r, tx - S(1), S(207), sansFit(detailTitle, F(37.5f), SansBold, maxTx - tx), TXT, F(37.5f), SansBold);
        // chips: ano, generos, qualidade
        int cx = tx; const int cy0 = S(228), ch = S(30);
        auto chip = [&](const std::string &t) {
            const float fs = F(16.7f);
            int w = sansWidth(t, fs, SansSemi) + (int)(F(25.4f) + 0.5f);
            if (cx + w > maxTx) return;
            r.roundRect(cx, cy0, w, ch, S(8), CHIP_LINE);
            r.roundRect(cx + 1, cy0 + 1, w - 2, ch - 2, S(8) > 1 ? S(8) - 1 : 0, 0xFF030706);
            sansDraw(r, cx + (int)(F(12.7f) + 0.5f), cy0 + S(21.3f), t, TXT, fs, SansSemi);
            cx += w + S(17);
        };
        if (!detailMeta.year.empty()) chip(detailMeta.year);
        for (const auto &g : detailMeta.genres) chip(g);
        if (!detailMeta.quality.empty()) {
            const float fs = F(20.5f);
            int w = sansWidth(detailMeta.quality, fs, SansBold) + S(14);
            if (w < S(46)) w = S(46);
            r.roundRect(cx, cy0, w, ch, S(8), GREEN);
            sansDraw(r, cx + (w - sansWidth(detailMeta.quality, fs, SansBold)) / 2, cy0 + S(22.2f), detailMeta.quality, 0xFF04200Au, fs, SansBold);
        }
        int y = S(296);
        // sinopse (ate 3 linhas)
        int blockBottom = S(228) + ch;
        if (!detailMeta.plot.empty()) {
            auto lines = sansWrap(detailMeta.plot, F(22.0f), SansRegular, S(612), 3);
            for (size_t i = 0; i < lines.size(); i++) sansDraw(r, tx, S(297) + (int)(i * F(29.7f) + 0.5f), lines[i], TXT2, F(22.0f), SansRegular);
            blockBottom = S(297) + (int)((lines.size() - 1) * F(29.7f)) + S(8);
        }
        (void)y;
        // linha de dados: nota | temporadas | duracao
        struct Item { int kind; std::string text; };
        std::vector<Item> items;
        if (!detailMeta.rating.empty()) items.push_back({0, detailMeta.rating + "/10"});
        if (detailSeries && nSea > 0) items.push_back({1, std::to_string(nSea) + (nSea == 1 ? " Temporada" : " Temporadas")});
        if (detailMeta.runtimeMin > 0) items.push_back({2, detailMinutes(detailMeta.runtimeMin) + (detailSeries ? " (eps)" : "")});
        const int rowCy = !detailMeta.plot.empty() ? S(400) : blockBottom + S(30);
        int ix = tx;
        for (size_t i = 0; i < items.size(); i++) {
            if (i > 0) { ix += S(28); r.fillRect(ix, rowCy - S(14), S(2) < 2 ? 2 : S(2), S(28), 0xFF8A8A8A); ix += S(31); }
            const float fs = F(20.4f);
            if (items[i].kind == 0) {
                dp::star(r, (float)ix + F(15.5f), (float)rowCy, F(15.5f), STAR);
                ix += S(39);
            } else if (items[i].kind == 1) {
                const float bx = (float)ix, bs = F(25), by = (float)rowCy - bs / 2;
                uiPaint(r, (int)bx - 1, (int)by - 1, (int)(bx + bs) + 2, (int)(by + bs) + 2, TXT, [&](float px, float py) {
                    float sd = uiSdBox(px - (bx + bs / 2), py - (by + bs / 2), bs / 2 - F(1), bs / 2 - F(1), F(4));
                    return uiClamp01(0.5f - (uiAbs(sd + F(1.1f)) - F(1.1f)));
                });
                dp::triRight(r, bx + bs * 0.38f, by + bs * 0.3f, bs * 0.3f, bs * 0.4f, TXT);
                ix += S(37);
            } else {
                const float cr = F(13), ccx = (float)ix + cr, ccy = (float)rowCy;
                dp::ring(r, ccx, ccy, cr, F(2.2f), TXT);
                dp::thickLine(r, ccx, ccy, ccx, ccy - cr * 0.55f, F(2.2f), TXT);
                dp::thickLine(r, ccx, ccy, ccx + cr * 0.42f, ccy + cr * 0.2f, F(2.2f), TXT);
                ix += S(35);
            }
            ix += sansDraw(r, ix, rowCy + S(8), items[i].text, TXT, fs, SansRegular);
        }
        if (!items.empty()) blockBottom = rowCy + S(16);
        // botao Assistir Agora
        const int bx = tx - S(2), by = S(442), bw = S(390), bh = S(67);
        const bool foc = detailFocus == 0;
        if (foc) uiGlow(r, bx, by, bw, bh, S(14), GREEN, S(10), 120, false);
        r.roundRect(bx, by, bw, bh, S(14), GREEN);
        r.roundRect(bx + S(3), by + S(3), bw - S(6), bh - S(6), S(11), foc ? GREEN : 0xFF020604);
        const unsigned bc = foc ? 0xFF031A06 : GREEN;
        dp::triRight(r, F(499), F(460), F(28), F(31), bc);
        sansDraw(r, S(552), S(484), "Assistir Agora", bc, F(25.3f), SansBold);
        if (!detailSeries) {
            // filme: sem temporadas / episodios
        }
    }
    // ---- temporadas e episodios (so serie) ----
    if (detailSeries && nSea > 0) {
        const int tabW = S(264), tabH = S(71), tabY = S(536), pitch = S(283.4f), x0 = S(53);
        // faixa discreta atras das abas
        r.fillRect(0, S(534), W, S(2), 0xFF0A0A0A);
        r.fillRect(0, S(604), W, S(2), 0xFF0A0A0A);
        for (int j = 0; j < kDetTabs && detailSeaScroll + j < nSea; j++) {
            int i = detailSeaScroll + j;
            const bool sel = i == detailSeasonIdx;
            const int x = x0 + j * pitch;
            int sn = detailSeasons[i] < 1 ? 1 : detailSeasons[i];
            int ne = 0;
            { auto sit = app.list.seriesTree.find(curSeries); if (sit != app.list.seriesTree.end()) { auto it = sit->second.find(detailSeasons[i]); if (it != sit->second.end()) ne = (int)it->second.size(); } }
            std::string t1 = "Temporada " + std::to_string(sn), t2 = std::to_string(ne) + (ne == 1 ? " epis\xC3\xB3" "dio" : " epis\xC3\xB3" "dios");
            int bx = x, by = tabY, bw = tabW, bh = tabH;
            if (sel) {
                bx -= S(5); by -= S(4); bw += S(10); bh += S(8);
                uiGlow(r, bx, by, bw, bh, S(12), GREEN, S(detailFocus == 1 ? 10 : 6), detailFocus == 1 ? 100 : 45, false);
                r.roundRect(bx, by, bw, bh, S(12), GREEN);
                r.roundRect(bx + S(3), by + S(3), bw - S(6), bh - S(6), S(9), TAB_SEL_FILL);
            } else {
                r.roundRect(bx, by, bw, bh, S(10), 0xFF282929);
                r.roundRect(bx + S(2), by + S(2), bw - S(4), bh - S(4), S(8), TAB_FILL);
            }
            const float f1 = F(21.3f), f2 = F(15.7f);
            sansDraw(r, x + (tabW - sansWidth(t1, f1, SansSemi)) / 2, S(569), t1, TXT, f1, SansSemi);
            sansDraw(r, x + (tabW - sansWidth(t2, f2, SansRegular)) / 2, S(593), t2, sel ? GREEN_SOFT : TXT2, f2, SansRegular);
        }
        // titulo da secao
        sansDraw(r, S(52), S(647), "Epis\xC3\xB3" "dios - Temporada " + std::to_string(detailSeasons[detailSeasonIdx] < 1 ? 1 : detailSeasons[detailSeasonIdx]), TXT, F(21.8f), SansBold);
        // cartoes de episodio
        const int cw = S(296), chh = S(163), cy = S(666), cp = S(319), cx0 = S(49);
        std::vector<std::string> front, back;
        const int imgW = cw - 2 * S(2), imgH = S(97);
        for (int i = detailEpScroll; i < detailEpScroll + kDetEps && i < nEp; i++) front.push_back(coverVariantKey(eps[i].logo, imgW, imgH));
        for (int i = detailEpScroll + kDetEps; i < detailEpScroll + 2 * kDetEps && i < nEp; i++) back.push_back(coverVariantKey(eps[i].logo, imgW, imgH));
        wantDetailCovers(coverVariantKey(detailLogo, S(337) - 2 * (S(2) < 2 ? 2 : S(2)), S(345) - 2 * (S(2) < 2 ? 2 : S(2))), front, back);
        for (int j = 0; j < kDetEps && detailEpScroll + j < nEp; j++) {
            int i = detailEpScroll + j;
            const bool sel = detailFocus == 2 && i == detailEpCur;
            const int x = cx0 + j * cp, y = cy, bw = S(2) < 2 ? 2 : S(2);
            if (sel) {
                uiGlow(r, x - S(3), y - S(2), cw + S(6), chh + S(4), S(13), GREEN, S(9), 70, false);
                r.roundRect(x - S(3), y - S(2), cw + S(6), chh + S(4), S(13), GREEN);
                r.roundRect(x + S(1), y + S(2), cw - S(2), chh - S(4), S(10), CARD_FILL);
            } else {
                r.roundRect(x, y, cw, chh, S(10), GRAY_LINE);
                r.roundRect(x + bw, y + bw, cw - 2 * bw, chh - 2 * bw, S(8), CARD_FILL);
            }
            const int ix = sel ? x + S(1) : x + bw, iy = sel ? y + S(2) : y + bw, iw = sel ? cw - S(2) : cw - 2 * bw, ih = S(98);
            bool drew = false;
            std::string key = coverVariantKey(eps[i].logo, imgW, imgH);
            if (!key.empty() && coverGet) {
                const uint32_t *pxl = nullptr; int pw = 0, ph = 0;
                if (coverGet(key, pxl, pw, ph) && pxl && pw > 0 && ph > 0) { r.blitImageRound(ix, iy, iw, ih, pxl, pw, ph, S(8), 0); drew = true; }
            }
            if (!drew) {
                r.roundRect(ix, iy, iw, ih, S(8), 0xFF0C1514);
                r.fillRect(ix, iy + ih - S(10), iw, S(10), 0xFF0C1514);
            }
            std::string label = detailEpisodeLabel(curSeries, eps[i].name, eps[i].episode, i + 1);
            sansDraw(r, x + S(17), S(789), sansFit(label, F(16.5f), SansSemi, cw - S(17) - S(62)), TXT, F(16.5f), SansSemi);
            int mins = i < (int)detailMeta.epMin.size() ? detailMeta.epMin[i] : 0;
            if (mins > 0) sansDraw(r, x + S(17), S(815), detailMinutes(mins), TXT2, F(14.8f), SansRegular);
            dp::ring(r, (float)x + F(268), F(798), F(18.2f), F(2.2f), GREEN);
            dp::triRight(r, (float)x + F(262), F(789.5f), F(14), F(17), GREEN);
        }
    }
    // ---- capa quando NAO ha temporadas/episodios (filme, ou serie sem temporadas) ----
    // O pedido de download da capa (wantDetailCovers) so rodava dentro do bloco de episodios acima: em FILMES ninguem mandava baixar
    // o poster do detalhe (ele so aparecia se por acaso ja estivesse no cache). Aqui pedimos so o poster, com a MESMA chave/tamanho
    // que o desenho do poster usa (poster = 337x345 menos a borda).
    if (!(detailSeries && nSea > 0)) {
        const int pbw = S(2) < 2 ? 2 : S(2);
        wantDetailCovers(coverVariantKey(detailLogo, S(337) - 2 * pbw, S(345) - 2 * pbw), {}, {});
    }
    // ---- rodape ----
    {
        r.fillRect(0, S(860), W, S(3), GREEN);
        const float fs = F(19.17f);
        const int ts = (int)(fs + 0.5f), cy = S(898);
        const std::string &st = app.lastStatus;
        unsigned dot = GREEN;
        if (st.find("Falha") != std::string::npos || st.find("offline") != std::string::npos || st.find("invalido") != std::string::npos) dot = 0xFFFF4D5A;
        r.roundRect(S(54), cy - S(7), S(14), S(14), S(7), dot);
        // dicas de botoes: iguais a imagem (teclas de controle remoto). Para os simbolos do PS4 troque por {"O","Voltar"},{"X","Selecionar"},{"OPT","Opções"}
        static const char *kKeys[3] = {"<", "OK", "MENU"};
        static const char *kLabels[3] = {"Voltar", "Selecionar", "Op\xC3\xA7\xC3\xB5" "es"};
        int x = W - S(45);
        for (int i = 2; i >= 0; i--) {
            int lw = r.textWidth(kLabels[i], ts);
            x -= lw; uiText(r, x, cy - ts / 2 - S(1), kLabels[i], TXT2, ts);
            std::string key = kKeys[i];
            int capW = key == "<" ? S(40) : key == "OK" ? S(38) : S(51);
            x -= S(13) + capW;
            r.roundRect(x, cy - S(18), capW, S(36), S(8), KEYCAP);
            if (key == "<") dp::triLeft(r, (float)x + capW / 2 - F(7), (float)cy - F(8), F(14), F(16), TXT);
            else sansDraw(r, x + (capW - sansWidth(key, F(15.5f), SansBold)) / 2, cy + S(5.5f), key, TXT, F(15.5f), SansBold);
            x -= S(39);
        }
        const int hx = x + S(39);
        r.drawText(S(82), cy - ts / 2 - S(1), uiFit(r, st, ts, hx - S(82) - S(30)), TXT2, ts);
    }
}
