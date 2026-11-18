// Prioridade baixa para threads de segundo plano (rede, capas, catalogo): assim elas nunca tiram CPU da thread que desenha a tela e le o controle.
// No PS4 a prioridade vai de 256 (maior) a 767 (menor); o padrao e 700. As funcoes sao "fracas": se o sistema nao as exportar, nada acontece.
#pragma once
#if defined(__FreeBSD__)
extern "C" {
// nomes proprios + "asm label": nao conflitam com <orbis/libkernel.h> (toolchain pacbrew/GMCA)
__attribute__((weak)) void *kk_PthreadSelf(void) __asm__("scePthreadSelf");
__attribute__((weak)) int kk_PthreadSetprio(void *thread, int prio) __asm__("scePthreadSetprio");
}
#endif
inline void bgPrio() {
#if defined(__FreeBSD__)
    if (kk_PthreadSelf && kk_PthreadSetprio) kk_PthreadSetprio(kk_PthreadSelf(), 767);
#endif
}
