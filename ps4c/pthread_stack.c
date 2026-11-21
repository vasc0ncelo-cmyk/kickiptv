/*
 * Pilha minima das threads (corrige o fechamento do app 1 s depois de comecar a live no player mpv).
 *
 * A musl do PS4 cria cada thread com pilha padrao de so 128 KiB. O mpv/FFmpeg criam threads dentro das proprias bibliotecas
 * (decodificacao H.264 por software em varias threads, demuxer, decoder do mpv, swscale...), e o codigo do codec usa bem mais
 * pilha que isso: a thread estoura a pilha logo nos primeiros quadros e o app fecha. O app antigo (AvPlayer, decodificacao pelo
 * hardware do PS4) nunca passava por esse codigo.
 *
 * Linkado com --wrap=pthread_create: TODA thread criada no executavel (mpv, FFmpeg, SDL, std::thread do app) passa por aqui
 * e ganha pelo menos KICK_MIN_STACK_BYTES de pilha. Pilha reservada so de memoria virtual: so paginas usadas ocupam RAM.
 * Quem ja pediu uma pilha maior, ou entregou a propria pilha, nao e alterado; o resto dos atributos (detached etc.) e mantido.
 */
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifndef KICK_MIN_STACK_BYTES
#define KICK_MIN_STACK_BYTES (1u * 1024u * 1024u)
#endif

int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    pthread_attr_t local;
    int haveLocal = 0;
    if (attr) {
        size_t sz = 0;
        if (pthread_attr_getstacksize(attr, &sz) == 0 && sz >= KICK_MIN_STACK_BYTES) return __real_pthread_create(t, attr, fn, arg);
        local = *attr;                                   /* copia: mantem detached, prioridade, etc. */
        haveLocal = 1;
    } else {
        if (pthread_attr_init(&local) != 0) return __real_pthread_create(t, NULL, fn, arg);
        haveLocal = 2;
    }
    int rc;
    if (pthread_attr_setstacksize(&local, KICK_MIN_STACK_BYTES) != 0) {
        rc = __real_pthread_create(t, attr, fn, arg);   /* nao deu para aumentar: segue como veio */
    } else {
        rc = __real_pthread_create(t, &local, fn, arg);
        if (rc == EAGAIN || rc == ENOMEM) rc = __real_pthread_create(t, attr, fn, arg);   /* sem memoria para a pilha grande: tenta a padrao */
    }
    if (haveLocal == 2) pthread_attr_destroy(&local);
    return rc;
}
