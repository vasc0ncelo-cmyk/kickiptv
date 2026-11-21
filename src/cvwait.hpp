#pragma once
// Espera com tempo limite em condition_variable que NAO passa pelo libc++ do sistema.
// No PS4, std::condition_variable::wait_for/wait_until (que vivem DENTRO da biblioteca C++ do sistema) lancam std::system_error
// ("condition_variable timed_wait failed: Operation timed out") quando o tempo acaba, e a excecao nao consegue ser tratada
// (nem com try/catch no chamador): vira std::terminate -> abort() e o app fecha do nada (canais, menu...).
// Aqui a espera chama direto o pthread_cond_timedwait (o mesmo que o mpv/FFmpeg usam) e IGNORA o codigo de retorno: quem chama
// ja repete o teste da condicao, entao "acordou", "tempo esgotado" e "acordo falso" dao no mesmo.
// Ao voltar o mutex do 'lk' continua travado, como no wait_for normal.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <time.h>

// declarada com outro nome (asm label) para nao conflitar com a declaracao do pthread.h do toolchain; "weak": se o simbolo nao existir
// o link NAO quebra e cai no wait_for antigo.
extern "C" int kickCondTimedWait(void *cond, void *mutex, const struct timespec *abstime) __asm__("pthread_cond_timedwait") __attribute__((weak));

template <class Lock, class Rep, class Period>
inline std::cv_status cvWaitFor(std::condition_variable &cv, Lock &lk, const std::chrono::duration<Rep, Period> &d) {
    if (kickCondTimedWait) {
        long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
        if (ns < 0) ns = 0;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long long nsec = (long long)ts.tv_nsec + ns % 1000000000LL;
        ts.tv_sec += (time_t)(ns / 1000000000LL) + (time_t)(nsec / 1000000000LL);
        ts.tv_nsec = (long)(nsec % 1000000000LL);                 // sempre < 1e9 (tv_nsec invalido tambem da erro)
        int rc = kickCondTimedWait(cv.native_handle(), lk.mutex()->native_handle(), &ts);
        return rc == 0 ? std::cv_status::no_timeout : std::cv_status::timeout;
    }
    try { return cv.wait_for(lk, d); }
    catch (...) { return std::cv_status::timeout; }
}

template <class Lock, class Rep, class Period, class Pred>
inline bool cvWaitFor(std::condition_variable &cv, Lock &lk, const std::chrono::duration<Rep, Period> &d, Pred pred) {
    const auto end = std::chrono::steady_clock::now() + d;
    while (!pred()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) return pred();
        cvWaitFor(cv, lk, end - now);
    }
    return true;
}
