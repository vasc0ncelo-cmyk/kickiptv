#pragma once
// Teclado: letras, botao "#+=" para simbolos (X em cima dele), espaco, shift.
#include "ui.hpp"
#include <string>

struct Keyboard {
    static const int COLS = 10, ROWS = 5;
    std::string text, title, subtitle;
    int cx = 0, cy = 0, maxLen = 80;
    bool shift = false, lock = false, isUrl = false;
    bool symbols = false;

    std::string keyAt(int x, int y) const {
        // Ultima linha: Aa | espaco x4 | #+= | . | < | OK
        static const char *letters[ROWS * COLS] = {
            "1","2","3","4","5","6","7","8","9","0",
            "q","w","e","r","t","y","u","i","o","p",
            "a","s","d","f","g","h","j","k","l","-",
            "z","x","c","v","b","n","m",".","/","@",
            "Aa","esp","esp","esp","esp","#+=",",","_","<","OK"};
        static const char *syms[ROWS * COLS] = {
            "!","?","#","$","%","&","*","(",")","=",
            "[","]","{","}","<",">","\\","|","~","^",
            "'","\"",";",":","`","´","°","§","€","£",
            "+","-","_","=","/","@","&","#","%","*",
            "ABC","esp","esp","esp","esp","ABC",",",".","<","OK"};
        if (y == 4 && x >= 1 && x <= 4) return " ";
        const char **k = symbols ? syms : letters;
        std::string s = k[y * COLS + x];
        if (s == "esp") return " ";
        if (s == "ABC") return "Aa";
        if (s == "#+=") return "#+=";
        if (!symbols && shift && s.size() == 1 && s[0] >= 'a' && s[0] <= 'z')
            s[0] = (char)(s[0] - 32);
        return s;
    }
    void open(const std::string &initial, bool url, int mode = 0) {
        text = initial; cx = cy = 0; shift = false; lock = false; symbols = false; isUrl = url || mode == 1;
        if (mode == 2) {
            maxLen = 80; title = "Buscar";
            subtitle = "X digita  |  #+ = simbolos  |  △ espaco  |  R1 OK";
        } else if (isUrl) {
            maxLen = 500; title = "Endereco da lista M3U";
            subtitle = "Selecione #+ = para simbolos  |  vazio+OK remove lista";
        } else {
            maxLen = 40; title = "Digite o @ do streamer";
            subtitle = "X no botao #+ = abre simbolos  |  L1 maiusculas";
        }
    }
    void move(int dx, int dy) {
        int nx = cx + dx, ny = cy + dy;
        if (ny < 0) ny = ROWS - 1;
        if (ny >= ROWS) ny = 0;
        if (nx < 0) nx = COLS - 1;
        if (nx >= COLS) nx = 0;
        if (ny == 4 && nx >= 1 && nx <= 4) nx = 1;
        cx = nx; cy = ny;
    }
    void backspace() { if (!text.empty()) text.pop_back(); }
    void toggleShift() {
        if (symbols) { symbols = false; return; }
        if (!shift) { shift = true; lock = false; }
        else if (!lock) lock = true;
        else { shift = false; lock = false; }
    }
    void toggleSymbols() {
        symbols = !symbols;
        shift = false; lock = false;
        if (cy == 4 && cx >= 1 && cx <= 4) cx = 1;
    }
    static bool isLetter(const std::string &s) {
        return s.size() == 1 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'));
    }
    void clear() { text.clear(); }
    void insert(const std::string &s) {
        for (char c : s) {
            if ((int)text.size() >= maxLen) break;
            text.push_back(c);
        }
    }
    bool press() {
        std::string k = keyAt(cx, cy);
        if (k == "OK") return true;
        if (k == "<") { backspace(); return false; }
        if (k == "Aa") {
            if (symbols) { symbols = false; return false; }
            toggleShift();
            return false;
        }
        if (k == "#+=") { toggleSymbols(); return false; }
        if (k == " ") { insert(" "); return false; }
        insert(k);
        if (shift && !lock && isLetter(k)) shift = false;
        return false;
    }

    void render(Renderer &r, bool blink, const std::string &timeText, const std::string &dateText) {
        using namespace theme;
        const int W = r.width(), H = r.height();
        uiBackground(r);
        uiHeavy(r, 48, 40, title, TEXT, 32);
        uiText(r, 48, 88, subtitle, MUTED, 20);
        if (!timeText.empty()) {
            int tw = r.textWidth(timeText, 22);
            uiText(r, W - 48 - tw, 40, timeText, SOFT, 22);
        }
        const int fx = 48, fy = 130, fw = W - 96, fh = 64;
        uiFrame(r, fx, fy, fw, fh, 14, 2, GREEN, CARD);
        std::string show = text;
        if (blink) show += "|";
        uiText(r, fx + 20, fy + 18, uiFit(r, show, 28, fw - 40), TEXT, 28);

        const int ks = 92, kp = 102, ox = (W - (COLS * kp - (kp - ks))) / 2, oy = 230;
        const int bh = 72;
        for (int y = 0; y < ROWS; y++) {
            for (int x = 0; x < COLS; x++) {
                if (y == 4 && x >= 2 && x <= 4) continue;
                int px2 = ox + x * kp, py = oy + y * (bh + 14);
                int bw = ks;
                std::string k = keyAt(x, y);
                std::string label = k;
                if (y == 4 && x == 1) {
                    bw = ks + 3 * kp;
                    label = "espaco";
                    k = " ";
                } else if (k == " ") continue;
                else if (k == "<") label = "apagar";
                else if (k == "Aa") label = symbols ? "ABC" : (shift ? (lock ? "ABC" : "Abc") : "abc");
                else if (k == "#+=") label = symbols ? "ABC" : "#+=";
                bool on = (cx == x && cy == y) || (y == 4 && x == 1 && cy == 4 && cx >= 1 && cx <= 4);
                bool special = (k == "<" || k == "OK" || k == "Aa" || k == " " || k == "#+=");
                unsigned edge = on ? GREEN : (k == "#+=" ? 0xFF3A8CFF : CARD_LINE);
                if (on) uiGlow(r, px2, py, bw, bh, 16, edge, 8, 130, false);
                uiFrame(r, px2, py, bw, bh, 16, on ? 3 : 2, edge, on ? SEL : (special ? KEY : CARD));
                int fs = (label.size() > 2) ? 18 : 28;
                uiCenterHeavy(r, px2 + bw / 2, py + bh / 2, label, on ? TEXT : SOFT, fs);
            }
        }
        uiText(r, 48, H - 48,
               symbols
                   ? "Modo SIMBOLOS  |  X no ABC volta letras  |  R1 confirmar  |  O cancelar"
                   : "X no #+ = abre simbolos  |  △ espaco  |  L1 Aa  |  R1 OK  |  O cancelar",
               DIM, 18);
    }
    void drawCaret(Renderer &r, bool on) {
        if (!on) return;
        using namespace theme;
        const int W = r.width();
        const int fx = 48, fy = 130;
        int tw = r.textWidth(text, 28);
        r.fillRect(fx + 20 + tw + 2, fy + 16, 3, 32, GREEN);
    }
};
