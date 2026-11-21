// Diagnostico de queda: descobre QUEM chama abort() (o log do app so mostrava "CRASH sig=6", sem dizer de onde vem).
// Grava em /data/kickiptv_crash.log o endereco de quem chamou abort()/assert/stack-check e alguns enderecos da pilha de chamadas.
// Os enderecos sao resolvidos depois com o arquivo mpv-symbols.txt que o build publica na Release (nm do app.elf).
// So usa open/write (nada de malloc/stdio): pode rodar dentro do abort().
#include <exception>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

namespace {
volatile int g_inAbort = 0;
int g_fd = -1;
void put(const char *s) { size_t n = strlen(s); if (g_fd >= 0) write(g_fd, s, n); }
void hex(const char *label, unsigned long v) {
    char b[64]; int p = 0;
    while (label[p] && p < 20) { b[p] = label[p]; p++; }
    b[p++] = '='; b[p++] = '0'; b[p++] = 'x';
    for (int i = 60; i >= 0; i -= 4) b[p++] = "0123456789abcdef"[(v >> i) & 15];
    b[p++] = ' '; b[p] = 0; put(b);
}
void report(const char *what, void *ra, void *fp0) {
    if (g_inAbort) return;
    g_inAbort = 1;
    g_fd = open("/data/kickiptv_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    put("[kickiptv] QUEM-ABORTOU "); put(what); put(" ");
    hex("ret", (unsigned long)ra);
    hex("ref", (unsigned long)&report);            // endereco desta funcao: com o mpv-symbols.txt da a base de carga do app
    put("\n[kickiptv] PILHA ");
    void **fp = (void **)fp0;
    for (int i = 0; i < 14 && fp; i++) {
        if (((uintptr_t)fp & 7) || (uintptr_t)fp < 0x10000) break;
        void *ra2 = fp[1]; void **nx = (void **)fp[0];
        hex("", (unsigned long)ra2);
        if (nx <= fp || (uintptr_t)nx - (uintptr_t)fp > (1u << 20)) break;
        fp = nx;
    }
    put("\n");
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_inAbort = 0;
}
void onTerminate() {
    g_fd = open("/data/kickiptv_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    put("[kickiptv] TERMINATE (excecao C++ sem tratamento ou thread ainda ativa): ");
    try { std::exception_ptr e = std::current_exception(); if (e) std::rethrow_exception(e); put("sem excecao"); }
    catch (const std::exception &ex) { put(ex.what()); }
    catch (...) { put("excecao desconhecida"); }
    put("\n");
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    __builtin_trap();
}
struct Init { Init() { std::set_terminate(onTerminate); } } g_init;
}

extern "C" {
void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
    report("abort", __builtin_return_address(0), __builtin_frame_address(0));
    __real_abort();
}
void __real___assert_fail(const char *, const char *, int, const char *) __attribute__((weak, noreturn));
void __wrap___assert_fail(const char *expr, const char *file, int line, const char *fn) {
    report("assert", __builtin_return_address(0), __builtin_frame_address(0));
    g_fd = open("/data/kickiptv_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    put("[kickiptv] ASSERT "); put(expr ? expr : "?"); put(" @ "); put(file ? file : "?"); put(" func="); put(fn ? fn : "?"); put("\n");
    hex("line", (unsigned long)line); put("\n");
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    if (__real___assert_fail) __real___assert_fail(expr, file, line, fn);
    __real_abort();
}
void __real___stack_chk_fail(void) __attribute__((weak, noreturn));
void __wrap___stack_chk_fail(void) {
    report("stack-chk", __builtin_return_address(0), __builtin_frame_address(0));
    if (__real___stack_chk_fail) __real___stack_chk_fail();
    __real_abort();
}
}
