#pragma once
// Teclado na tela (D-pad ou analogico esquerdo (L3) move, X ou clique no L3 digita, QUADRADO ou R3 apaga,
// L1 ou a tecla "abc" alterna MAIUSCULAS (so a proxima letra -> travado -> desligado),
// TRIANGULO insere "https://", L2 limpa, R1 confirma, O cancela).
// Serve para o @ do streamer e para o endereco da lista M3U.
#include "ui.hpp"
#include <string>

struct Keyboard {
    static const int COLS = 10, ROWS = 5;
    std::string text, title, subtitle;
    int cx = 0, cy = 0, maxLen = 25;
    bool shift = false, lock = false, isUrl = false;   // shift = maiusculas ligadas; lock = travadas (senao vale so para a proxima letra)

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
        text = initial; cx = cy = 0; shift = false; lock = false; isUrl = url;
        maxLen = url ? 500 : 25;
        title = url ? "Endereço da lista M3U" : "Digite o @ do streamer";
        subtitle = url ? "Cole o link da sua lista (deixe vazio e confirme para remover)" : "Canal da Kick que você quer assistir";
    }
    void move(int dx, int dy) { cx = (cx + dx + COLS) % COLS; cy = (cy + dy + ROWS) % ROWS; }
    void backspace() { if (!text.empty()) text.pop_back(); }
    // ciclo: desligado -> proxima letra maiuscula -> maiusculas travadas -> desligado
    void toggleShift() {
        if (!shift) { shift = true; lock = false; }
        else if (!lock) lock = true;
        else { shift = false; lock = false; }
    }
    static bool isLetter(const std::string &s) { return s.size() == 1 && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')); }
    void clear() { text.clear(); }
    void insert(const std::string &s) { if ((int)(text.size() + s.size()) <= maxLen) text += s; }
    // retorna true se confirmou (OK)
    bool press() {
        std::string k = keyAt(cx, cy);
        if (k == "OK") return true;
        if (k == "<") backspace();
        else if (k == "Aa") toggleShift();
        else if ((int)text.size() < maxLen) {
            text += k;
            if (shift && !lock && isLetter(k)) shift = false;       // modo "so a proxima letra"
        }
        return false;
    }
    // Onde fica o campo de texto e o cursor (usado por render e por drawCaret).
    struct Field { int fx, fy, fw, fh, px, caretX; std::string shown; };
    Field field(const Renderer &r) const {
        Field f;
        f.fx = 73; f.fy = 232; f.fw = r.width() - 146; f.fh = 84;
        f.px = f.fx + 30;
        if (!isUrl) f.px += 34;
        int maxChars = (f.fw - (f.px - f.fx) - 60) / (36 / 2);
        f.shown = text;                                          // mostra o final se for longo
        if ((int)f.shown.size() > maxChars) f.shown = "..." + f.shown.substr(f.shown.size() - (maxChars - 3));
        f.caretX = f.px + r.textWidth(f.shown, 36) + 3;
        return f;
    }
    // So o cursor piscando: apaga ou desenha um retangulo pequeno (nao precisa redesenhar o teclado inteiro).
    void drawCaret(Renderer &r, bool blink) const {
        using namespace theme;
        Field f = field(r);
        r.fillRect(f.caretX, f.fy + 14, 6, 56, 0xFF050805);       // apaga (cor do fundo do campo)
        if (blink) r.roundRect(f.caretX, f.fy + 18, 4, 48, 2, GREEN);
    }
    void render(Renderer &r, bool blink, const std::string &hhmm, const std::string &date) const {
        using namespace theme;
        uiHeader(r, hhmm, date);
        uiKMark(r, 73, 150, 40, 41, GREEN);
        uiHeavy(r, 130, 148, title, TEXT, 36);
        uiText(r, 132, 192, subtitle, MUTED, 22);
        // campo de texto
        Field f = field(r);
        const int fx = f.fx, fy = f.fy, fw = f.fw, fh = f.fh;
        uiGlow(r, fx, fy, fw, fh, 22, GREEN, 8, 90, false);
        uiFrame(r, fx, fy, fw, fh, 22, 3, GREEN, 0xFF050805);
        if (!isUrl) uiHeavy(r, fx + 30, fy + 22, "@", GREEN, 40);
        uiText(r, f.px, fy + 22, f.shown, TEXT, 36, true);
        if (blink) r.roundRect(f.caretX, fy + 18, 4, 48, 2, GREEN);
        // teclas
        const int kx = 73, ky = 350, ks = 80, kp = 92;
        for (int y = 0; y < ROWS; y++)
            for (int x = 0; x < COLS; x++) {
                bool on = (x == cx && y == cy);
                int px2 = kx + x * kp, py = ky + y * kp;
                std::string k = keyAt(x, y);
                bool special = k == "<" || k == "OK" || k == "Aa";
                if (on) uiGlow(r, px2, py, ks, ks, 16, GREEN, 8, 130, false);
                unsigned bg = on ? GREEN : (k == "OK" ? 0xFF14310B : (special ? CARD_LINE : CARD2));
                r.roundRect(px2, py, ks, ks, 16, bg);
                std::string lab = k == "<" ? "Apagar" : (k == "Aa" ? (lock ? "ABC" : shift ? "Abc" : "abc") : k);
                unsigned fg = on ? BLACK : (k == "OK" ? GREEN : (k == "Aa" && shift ? GREEN : TEXT));
                uiCenterHeavy(r, px2 + ks / 2, py + ks / 2, lab, fg, special ? 22 : 34);
            }
        // painel de controles
        const int cx0 = kx + COLS * kp + 24, cw = r.width() - 73 - cx0;
        uiFrame(r, cx0, ky, cw, 5 * kp - 12, 22, 2, CARD_LINE, 0xFF0B0B0B);
        uiText(r, cx0 + 30, ky + 22, "Controles", GREEN, 26, true);
        static const char *ks2[7] = {"D-pad / L3", "X / clique L3", "QUADRADO / R3", "L1", "TRIÂNGULO", "L2", "R1"};
        static const char *ls[7] = {"mover (segure p/ correr)", "digitar", "apagar", "maiúsculas", "https://", "limpar tudo", "confirmar"};
        for (int i = 0; i < 7; i++) {
            int ry = ky + 84 + i * 53;
            int kw = r.textWidth(ks2[i], 20) + 28;
            r.roundRect(cx0 + 30, ry - 20, kw, 40, 12, KEY);
            uiCenter(r, cx0 + 30 + kw / 2, ry, ks2[i], TEXT, 20, true);
            uiText(r, cx0 + 30 + kw + 18, ry - 11, ls[i], SOFT, 22);
        }
        uiHints(r, r.width() - 73, 1043, {{"O", "Cancelar"}});
    }
};
