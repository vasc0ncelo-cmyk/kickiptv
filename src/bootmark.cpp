// Marcadores de inicializacao: gravam /data/kickiptv_boot.txt ANTES do main().
// Servem para descobrir ONDE o app morre quando ele fecha na hora (CE-34878-0) sem escrever nada no log normal:
//   - arquivo nao existe          -> o sistema recusou o app ao carregar (SELF, modulos .prx, bibliotecas)
//   - so a linha "ctor-cedo"      -> caiu nos construtores globais (mpv/ffmpeg/SDL) ou na libc
//   - "ctor-cedo" + "ctor-tarde"  -> caiu dentro do main()
// Usa so chamadas do kernel (sem stdio/malloc), entao funciona antes da libc terminar de iniciar.
#include <orbis/libkernel.h>
#include <string.h>

static void bootMark(const char *s, bool truncate) {
    int flags = 0x0001 | 0x0200 | (truncate ? 0x0400 : 0x0008);   // O_WRONLY | O_CREAT | (O_TRUNC ou O_APPEND)
    int fd = sceKernelOpen("/data/kickiptv_boot.txt", flags, 0777);
    if (fd < 0) return;
    sceKernelWrite(fd, s, strlen(s));
    sceKernelClose(fd);
}

__attribute__((constructor(101))) static void bootEarly() { bootMark("ctor-cedo\n", true); }
__attribute__((constructor(65000))) static void bootLate() { bootMark("ctor-tarde\n", false); }
