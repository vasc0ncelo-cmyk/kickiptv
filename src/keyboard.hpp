#pragma once
// Teclado na tela (D-pad move, X digita, QUADRADO apaga, TRIANGULO insere "https://", L2 limpa, R1 confirma, O cancela).
// Serve para o @ do streamer e para o endereco da lista M3U.
#include "ui.hpp"
#include <string>

struct Keyboard {
    static const int COLS = 10, ROWS = 5;
    std::string text, title, subtitle;
    int cx = 0, cy = 0, maxLen = 25;
    bool shift = false, isUrl = false;

    std::string keyAt(int x, int y) const {
        static const char *k[ROWS * COLS] = {
            "1","2","3","4","5","6","7","8","9","0",
            "q","w","e","r","t","y","u","i","o","p",
            "a","s","d","f","g","h","j","k","l",":",
            "z","x","c","v","b","n","m",".","/","-",
            "Aa","_","?","=","&","%","@","+","<","OK"};
        std::string s = k[y * COLS + x];
        if (shift && s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32);
        return s;
    }
    void open(const std::string &initial, bool url) {
        text = initial; cx = cy = 0; shift = false; isUrl = url;
        maxLen = url ? 500 : 25;
        title = url ? "Endereço da lista M3U" : "Digite o @ do streamer";
        subtitle = url ? "Cole o link da sua lista (deixe vazio e confirme para remover)" : "Canal da Kick que você quer assistir";
    }
    void move(int dx, int dy) { cx = (cx + dx + COLS) % COLS; cy = (cy + dy + ROWS) % ROWS; }
    void backspace() { if (!text.empty()) text.pop_back(); }
    void clear() { text.clear(); }
    void insert(const std::string &s) { if ((int)(text.size() + s.size()) <= maxLen) text += s; }
    // retorna true se confirmou (OK)
    bool press() {
        std::string k = keyAt(cx, cy);
        if (k == "OK") return true;
        if (k == "<") backspace();
        else if (k == "Aa") shift = !shift;
        else if ((int)text.size() < maxLen) text += k;
        return false;
    }
    void render(Renderer &r, bool blink, const std::string &hhmm, const std::string &date) const {
        using namespace theme;
        uiHeader(r, hhmm, date);
        uiKMark(r, 73, 150, 40, 41, GREEN);
        uiHeavy(r, 130, 148, title, TEXT, 36);
        uiText(r, 132, 192, subtitle, MUTED, 22);
        // campo de texto (mostra o final se for longo)
        const int fx = 73, fy = 232, fw = r.width() - 146, fh = 84;
        uiGlow(r, fx, fy, fw, fh, 22, GREEN, 8, 90);
        uiFrame(r, fx, fy, fw, fh, 22, 3, GREEN, 0xFF050805);
        int px = fx + 30;
        if (!isUrl) { uiHeavy(r, px, fy + 22, "@", GREEN, 40); px += 34; }
        int maxChars = (fw - (px - fx) - 60) / (36 / 2);
        std::string shown = text;
        if ((int)shown.size() > maxChars) shown = "..." + shown.substr(shown.size() - (maxChars - 3));
        uiText(r, px, fy + 22, shown, TEXT, 36, true);
        if (blink) r.roundRect(px + r.textWidth(shown, 36) + 3, fy + 18, 4, 48, 2, GREEN);
        // teclas
        const int kx = 73, ky = 350, ks = 80, kp = 92;
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                bool on = (x == cx && y == cy);
                int px2 = kx + x * kp, py = ky + y * kp;
                std::string k = keyAt(x, y);
                bool special = k == "<" || k == "OK" || k == "Aa";
                if (on) uiGlow(r, px2, py, ks, ks, 16, GREEN, 8, 130);
                unsigned bg = on ? GREEN : (k == "OK" ? 0xFF14310B : (special ? CARD_LINE : CARD2));
                r.roundRect(px2, py, ks, ks, 16, bg);
                std::string lab = k == "<" ? "Apagar" : k;
                unsigned fg = on ? BLACK : (k == "OK" ? GREEN : (k == "Aa" && shift ? GREEN : TEXT));
                uiCenterHeavy(r, px2 + ks / 2, py + ks / 2, lab, fg, special ? 22 : 34);
            }
        // painel de controles
        const int cx0 = kx + COLS * kp + 24, cw = r.width() - 73 - cx0;
        uiFrame(r, cx0, ky, cw, 5 * kp - 12, 22, 2, CARD_LINE, 0xFF0B0B0B);
        uiText(r, cx0 + 30, ky + 22, "Controles", GREEN, 26, true);
        static const char *ks2[6] = {"D-pad", "X", "QUADRADO", "TRIÂNGULO", "L2", "R1"};
        static const char *ls[6] = {"mover", "digitar", "apagar", "https://", "limpar tudo", "confirmar"};
        for (int i = 0; i < 6; i++) {
            int ry = ky + 90 + i * 62;
            int kw = r.textWidth(ks2[i], 20) + 28;
            r.roundRect(cx0 + 30, ry - 20, kw, 40, 12, KEY);
            uiCenter(r, cx0 + 30 + kw / 2, ry, ks2[i], TEXT, 20, true);
            uiText(r, cx0 + 30 + kw + 18, ry - 11, ls[i], SOFT, 22);
        }
        uiHints(r, r.width() - 73, 1043, {{"O", "Cancelar"}});
    }
};
