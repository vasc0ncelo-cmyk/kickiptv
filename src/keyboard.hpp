#pragma once
// Teclado na tela melhorado (espaco, simbolos, layout amplo).
// D-pad/analogico move, X digita, QUADRADO apaga, L1 maiusculas, R1 confirma, O cancela.
#include "ui.hpp"
#include <string>

struct Keyboard {
    static const int COLS = 10, ROWS = 5;
    std::string text, title, subtitle;
    int cx = 0, cy = 0, maxLen = 80;
    bool shift = false, lock = false, isUrl = false;

    std::string keyAt(int x, int y) const {
        static const char *k[ROWS * COLS] = {
            "1","2","3","4","5","6","7","8","9","0",
            "q","w","e","r","t","y","u","i","o","p",
            "a","s","d","f","g","h","j","k","l","-",
            "z","x","c","v","b","n","m",".","/","@",
            "Aa","esp","esp","esp","esp",",","_","+","<","OK"};
        // "esp" ocupa 4 colunas na ultima linha (x=1..4)
        if (y == 4 && x >= 1 && x <= 4) return " ";
        std::string s = k[y * COLS + x];
        if (s == "esp") return " ";
        if (shift && s.size() == 1 && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 32);
        return s;
    }
    void open(const std::string &initial, bool url, int mode = 0) {
        text = initial; cx = cy = 0; shift = false; lock = false; isUrl = url || mode == 1;
        if (mode == 2) {
            maxLen = 80;
            title = "Buscar";
            subtitle = "Espaco = tecla ESP. Vazio + OK limpa a busca.";
        } else if (isUrl) {
            maxLen = 500;
            title = "Endereco da lista M3U";
            subtitle = "Use ESP para espaco. Vazio + OK remove a lista.";
        } else {
            maxLen = 40;
            title = "Digite o @ do streamer";
            subtitle = "Canal da Kick. ESP = espaco.";
        }
    }
    void move(int dx, int dy) {
        int nx = cx + dx, ny = cy + dy;
        if (ny < 0) ny = ROWS - 1;
        if (ny >= ROWS) ny = 0;
        if (nx < 0) nx = COLS - 1;
        if (nx >= COLS) nx = 0;
        // unifica as 4 teclas de espaco visualmente
        if (ny == 4 && nx >= 1 && nx <= 4) nx = 1;
        cx = nx; cy = ny;
    }
    void backspace() { if (!text.empty()) text.pop_back(); }
    void toggleShift() {
        if (!shift) { shift = true; lock = false; }
        else if (!lock) lock = true;
        else { shift = false; lock = false; }
    }
    static bool isLetter(const std::string &s) {
        return s.size() == 1 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'));
    }
    void clear() { text.clear(); }
    void insert(const std::string &s) {
        if ((int)(text.size() + s.size()) <= maxLen) text += s;
    }
    bool press() {
        std::string k = keyAt(cx, cy);
        if (k == "OK") return true;
        if (k == "<") { backspace(); return false; }
        if (k == "Aa") { toggleShift(); return false; }
        if ((int)text.size() < maxLen) {
            text += k;
            if (shift && !lock && isLetter(k)) shift = false;
        }
        return false;
    }
    struct Field { int fx, fy, fw, fh, px, caretX; std::string shown; };
    Field field(const Renderer &r) const {
        Field f;
        f.fx = 73; f.fy = 232; f.fw = r.width() - 146; f.fh = 84;
        f.px = f.fx + 30;
        if (!isUrl) f.px += 34;
        int maxChars = (f.fw - (f.px - f.fx) - 60) / (36 / 2);
        f.shown = text;
        if ((int)f.shown.size() > maxChars) f.shown = "..." + f.shown.substr(f.shown.size() - (maxChars - 3));
        f.caretX = f.px + r.textWidth(f.shown, 36) + 3;
        return f;
    }
    void drawCaret(Renderer &r, bool blink) const {
        using namespace theme;
        Field f = field(r);
        r.fillRect(f.caretX, f.fy + 14, 6, 56, 0xFF050805);
        if (blink) r.roundRect(f.caretX, f.fy + 18, 4, 48, 2, GREEN);
    }
    void render(Renderer &r, bool blink, const std::string &hhmm, const std::string &date) const {
        using namespace theme;
        uiHeader(r, hhmm, date);
        uiKMark(r, 73, 150, 40, 41, GREEN);
        uiHeavy(r, 130, 148, title, TEXT, 36);
        uiText(r, 132, 192, subtitle, MUTED, 22);
        Field f = field(r);
        uiGlow(r, f.fx, f.fy, f.fw, f.fh, 22, GREEN, 8, 90, false);
        uiFrame(r, f.fx, f.fy, f.fw, f.fh, 22, 3, GREEN, 0xFF050805);
        if (!isUrl) uiHeavy(r, f.fx + 30, f.fy + 22, "@", GREEN, 40);
        uiText(r, f.px, f.fy + 22, f.shown, TEXT, 36, true);
        if (blink) r.roundRect(f.caretX, f.fy + 18, 4, 48, 2, GREEN);

        const int kx = 73, ky = 350, ks = 80, kp = 92;
        // desenha teclas; espaco vira barra larga
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                if (y == 4 && x >= 2 && x <= 4) continue; // parte da barra de espaco
                bool on = (x == cx && y == cy) || (y == 4 && cy == 4 && cx >= 1 && cx <= 4 && x == 1);
                int px2 = kx + x * kp, py = ky + y * kp;
                int bw = ks, bh = ks;
                std::string k = keyAt(x, y);
                std::string label = k;
                if (y == 4 && x == 1) { // barra de espaco (4 colunas)
                    bw = ks + 3 * kp;
                    label = "espaco";
                    k = " ";
                } else if (k == " ") {
                    continue;
                } else if (k == "<") label = "apagar";
                else if (k == "Aa") label = shift ? (lock ? "ABC" : "Abc") : "abc";
                bool special = (k == "<" || k == "OK" || k == "Aa" || k == " ");
                if (on) uiGlow(r, px2, py, bw, bh, 16, GREEN, 8, 130, false);
                uiFrame(r, px2, py, bw, bh, 16, on ? 3 : 2, on ? GREEN : CARD_LINE, on ? SEL : (special ? KEY : CARD));
                int fs = (label.size() > 2) ? 18 : 28;
                uiCenterHeavy(r, px2 + bw / 2, py + bh / 2, label, on ? TEXT : SOFT, fs);
            }
        }
        uiHints(r, r.width() - 73, 1043, {
            {"D-pad", "Mover"}, {"X", "Digitar"}, {"□", "Apagar"}, {"L1", "Maiusculas"}, {"R1", "OK"}, {"O", "Cancelar"}
        });
    }
};
