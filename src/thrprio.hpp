// Prioridade baixa para threads de segundo plano (rede, capas, catalogo): assim elas nunca tiram CPU da thread que desenha a tela e le o controle.
// No PS4 a prioridade vai de 256 (maior) a 767 (menor); o padrao e 700. As funcoes sao "fracas": se o sistema nao as exportar, nada acontece.
#pragma once
#if defined(__FreeBSD__)
extern "C" {
__attribute__((weak)) void *scePthreadSelf(void);
__attribute__((weak)) int scePthreadSetprio(void *thread, int prio);
}
#endif
inline void bgPrio() {
#if defined(__FreeBSD__)
    if (scePthreadSelf && scePthreadSetprio) scePthreadSetprio(scePthreadSelf(), 767);
#endif
}
