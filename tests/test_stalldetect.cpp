// StallDetector: so acusa travamento quando o contador para de andar de verdade (nao quando o console suspende).
#include <cassert>
#include <cstdio>
#include "../src/stalldetect.hpp"

int main() {
    // 1) andando: nunca acusa
    { StallDetector d; double t = 100; uint64_t k = 0;
      for (int i = 0; i < 500; i++) { t += 1.0; k += 3; assert(d.step(k, t) == 0); } }
    // 2) parou: conta 1, 2, 3...; volta a andar: zera
    { StallDetector d; double t = 0; uint64_t k = 7;
      d.step(k, t);
      for (int i = 1; i <= 120; i++) { t += 1.0; assert(d.step(k, t) == i); }
      t += 1.0; k++; assert(d.step(k, t) == 0);
      t += 1.0; assert(d.step(k, t) == 1); }
    // 3) o console suspendeu (intervalo grande entre duas chamadas): nao conta e recomeca do zero
    { StallDetector d; double t = 10; uint64_t k = 1;
      d.step(k, t);
      for (int i = 0; i < 50; i++) { t += 1.0; d.step(k, t); }          // ja estava "parado" ha 50 s...
      t += 600.0;                                                       // ...e o console dormiu 10 min
      assert(d.step(k, t) == 0);                                        // volta de repouso: nao e travamento
      t += 1.0; assert(d.step(k, t) == 1);                              // so conta a partir dai
    }
    // 4) relogio andando para tras: nao acusa
    { StallDetector d; d.step(1, 100); assert(d.step(1, 50) == 0); }
    // 5) uma volta lenta (ate 5 s) ainda conta como parado, mas nao some com a contagem
    { StallDetector d; d.step(5, 0); assert(d.step(5, 4.0) == 1); assert(d.step(5, 8.0) == 2); }
    printf("OK\n");
    return 0;
}
