// Testa as maiusculas do teclado na tela: "so a proxima letra" -> travado -> desligado.
#include <assert.h>
#include <stdio.h>
#include "../src/keyboard.hpp"

static void goTo(Keyboard &k, const std::string &key) {
    for (int y = 0; y < Keyboard::ROWS; y++)
        for (int x = 0; x < Keyboard::COLS; x++)
            if (k.keyAt(x, y) == key || (key.size() == 1 && k.keyAt(x, y).size() == 1 && tolower(k.keyAt(x, y)[0]) == tolower(key[0]))) { k.cx = x; k.cy = y; return; }
    assert(false);
}
int main() {
    Keyboard k; k.open("", true);
    goTo(k, "a"); k.press();                          // sem shift: minuscula
    assert(k.text == "a");
    k.toggleShift();                                  // L1: proxima letra maiuscula
    assert(k.shift && !k.lock);
    goTo(k, "b"); k.press();
    assert(k.text == "aB" && !k.shift);               // so uma letra: volta ao normal
    goTo(k, "c"); k.press();
    assert(k.text == "aBc");
    k.toggleShift(); k.toggleShift();                 // travado
    assert(k.shift && k.lock);
    goTo(k, "d"); k.press(); goTo(k, "e"); k.press();
    assert(k.text == "aBcDE" && k.shift);             // continua maiusculo
    goTo(k, "1"); k.press();                          // numero nao muda o estado
    assert(k.text == "aBcDE1" && k.shift && k.lock);
    k.toggleShift();                                  // desliga
    assert(!k.shift && !k.lock);
    goTo(k, "f"); k.press();
    assert(k.text == "aBcDE1f");
    goTo(k, "Aa"); k.press();                         // a tecla do teclado faz o mesmo ciclo
    assert(k.shift && !k.lock);
    goTo(k, "Aa"); k.press(); goTo(k, "Aa"); k.press();
    assert(!k.shift);
    k.open("x", false);                               // reabrir zera o estado
    assert(!k.shift && !k.lock);
    printf("OK\n");
    return 0;
}
