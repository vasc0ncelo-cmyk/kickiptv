// Politica de buffer da live: tamanho do buffer inicial e quando preparar o proximo pedaco.
#include "../src/livebuf.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace livebuf;
int main() {
    // buffer inicial: ~12 s (HLS) / ~8 s (canal), entre 3 e 8 segmentos, nunca mais do que a playlist tem
    assert(startSegs(2.0, kStartSecsHls) == 6);          // Kick, segmentos de 2 s: 6 segmentos = 12 s
    assert(startSegs(4.0, kStartSecsHls) == 3);
    assert(startSegs(6.0, kStartSecsHls) == 3);          // segmento longo: o minimo de antes (3)
    assert(startSegs(10.0, kStartSecsHls) == 3);
    assert(startSegs(1.0, kStartSecsHls) == 8);          // segmento curto: limitado a 8
    assert(startSegs(2.0, kStartSecsRaw) == 4);          // canal MPEG-TS: pedacos de 2 s -> 4 = 8 s
    assert(startSegs(2.0, kStartSecsHls, 4) == 4);       // a playlist so tem 4 segmentos
    assert(startSegs(2.0, kStartSecsHls, 1) == 1 && startSegs(2.0, kStartSecsHls, 0) == 1);
    assert(startSegs(0.0, kStartSecsHls) == 6 && startSegs(NAN, kStartSecsHls) == 6 && startSegs(-3, kStartSecsHls) == 6);   // duracao invalida: assume 2 s
    assert(startSegs(500.0, kStartSecsHls) == 3);
    // quando preparar o proximo pedaco
    assert(!shouldPrep(0, 12, 11, 300) && !shouldPrep(1, 12, 11, 300));       // precisa de pelo menos 2 segmentos novos
    assert(!shouldPrep(3, 12.0, 4.0, 300));                                   // pedaco atual com 8 s sobrando: espera e junta mais
    assert(shouldPrep(3, 12.0, 9.0, 300));                                    // sobram 3.0 s <= folga (2.5 + 0.9 = 3.4 s): prepara agora
    assert(!shouldPrep(3, 12.0, 8.0, 300));                                   // sobram 4.0 s > 3.4 s: ainda espera
    assert(shouldPrep(2, 12.0, 12.5, 300));                                   // o pedaco atual ja passou do fim
    assert(shouldPrep(2, 12.0, 8.0, 2000));                                   // conversao lenta (2 s): folga de 8.5 s -> comeca antes
    assert(!shouldPrep(2, 12.0, 0.0, 300) && !shouldPrep(7, 12.0, 0.0, 300)); // o pedaco atual ainda nao mostrou o 1o quadro: nao prepara
    assert(shouldPrep(kMaxMerge, 12.0, 0.0, 300) && shouldPrep(kMaxMerge + 3, 12.0, 1.0, 300));   // segmentos demais: prepara mesmo assim
    assert(shouldPrep(2, 0.0, 0.0, 0) && shouldPrep(5, 0.0, 3.0, 0));         // sem a duracao do pedaco atual: como antes (assim que ha 2)
    // ---- ajuste automatico depois de congelamentos ----
    assert(extraStartSecs(0) == 0 && extraStartSecs(-2) == 0);
    assert(extraStartSecs(1) == 4 && extraStartSecs(2) == 8 && extraStartSecs(3) == 12 && extraStartSecs(9) == 12);   // no maximo +12 s
    assert(startSegs(2.0, kStartSecsHls + extraStartSecs(3), 1000, kExtraMaxSegs) == 12);                            // 24 s de buffer = 12 segmentos de 2 s
    assert(startSegs(2.0, kStartSecsHls + extraStartSecs(3), 1000) == 8);                                            // sem o limite novo: 8 (como antes)
    assert(startSegs(2.0, kStartSecsHls + extraStartSecs(3), 7, kExtraMaxSegs) == 7);                                // nunca mais do que a playlist tem
    assert(capHeight(720, 0) == 720 && capHeight(720, 1) == 720 && capHeight(720, 2) == 480 && capHeight(720, 3) == 480);
    assert(capHeight(720, 4) == 360 && capHeight(1080, 9) == 360 && capHeight(360, 2) == 360 && capHeight(240, 5) == 240);
    { double ts[4] = {100.0, 0.0, 380.0, 399.0};                                  // 0 = vazio
      assert(recentStalls(ts, 4, 400.0) == 2);                                    // o de 100 s ja tem 300 s: esquecido
      assert(recentStalls(ts, 4, 150.0) == 1);                                    // os de 380/399 ainda estao no futuro: nao contam
      assert(recentStalls(ts, 4, 700.0) == 0 && recentStalls(ts, 0, 700.0) == 0); }
    assert(segDeadlineSecs(2.0) == 8.0 && segDeadlineSecs(4.0) == 16.0 && segDeadlineSecs(10.0) == 20.0);
    assert(segDeadlineSecs(0.0) == 8.0 && segDeadlineSecs(NAN) == 8.0 && segDeadlineSecs(-1) == 8.0);
    assert(backoffMs(0) == 400 && backoffMs(1) == 400 && backoffMs(2) == 800 && backoffMs(4) == 1600 && backoffMs(5) == 2000 && backoffMs(40) == 2000);
    std::cout << "OK\n";
}
