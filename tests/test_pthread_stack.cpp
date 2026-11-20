// Compilar: gcc -c -DKICK_MIN_STACK_BYTES=16777216 ps4c/pthread_stack.c -o /tmp/ps.o && g++ -std=c++17 -pthread -o /tmp/tps tests/test_pthread_stack.cpp /tmp/ps.o -Wl,--wrap=pthread_create
#include <pthread.h>
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>
#include <unistd.h>
static size_t stackOf() { pthread_attr_t a; pthread_getattr_np(pthread_self(), &a); size_t s = 0; void *p; pthread_attr_getstack(&a, &p, &s); pthread_attr_destroy(&a); return s; }
static std::atomic<size_t> g_sz{0}; static std::atomic<int> g_det{-1};
static void *body(void *) { g_sz = stackOf(); pthread_attr_t a; pthread_getattr_np(pthread_self(), &a); int d; pthread_attr_getdetachstate(&a, &d); g_det = d; pthread_attr_destroy(&a); return nullptr; }
int main() {
    const size_t MIN = 16u << 20;
    { pthread_t t; assert(pthread_create(&t, nullptr, body, nullptr) == 0); pthread_join(t, nullptr); assert(g_sz >= MIN); }                 // attr NULL: ganha a pilha minima
    { std::thread th([] { g_sz = stackOf(); }); th.join(); assert(g_sz >= MIN); }                                                          // std::thread tambem
    { pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 256 * 1024); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
      g_sz = 0; g_det = -1; pthread_t t; assert(pthread_create(&t, &a, body, nullptr) == 0); for (int i = 0; i < 300 && g_det == -1; i++) usleep(10000);
      assert(g_sz >= MIN && g_det == PTHREAD_CREATE_DETACHED); pthread_attr_destroy(&a); }                                                 // atributo pequeno: aumenta e mantem detached
    { pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20); g_sz = 0; pthread_t t; assert(pthread_create(&t, &a, body, nullptr) == 0); pthread_join(t, nullptr); assert(g_sz >= (64u << 20) - 8192); pthread_attr_destroy(&a); }   // ja grande: intacto
    std::puts("test_pthread_stack OK");
}
