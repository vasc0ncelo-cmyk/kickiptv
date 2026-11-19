// App minimo de TESTE: grava /data/kickiptv_hello.txt e fica 60 s aberto (tela preta). Serve para saber se o
// empacotamento/toolchain gera um app que ABRE no console, independente do codigo do Kick IPTV.
#include <orbis/libkernel.h>
#include <string.h>

int main() {
    int fd = sceKernelOpen("/data/kickiptv_hello.txt", 0x0001 | 0x0200 | 0x0400, 0777);
    if (fd >= 0) {
        const char *m = "hello-main\n";
        sceKernelWrite(fd, m, strlen(m));
        sceKernelClose(fd);
    }
    for (int i = 0; i < 60; i++) sceKernelSleep(1);
    return 0;
}
