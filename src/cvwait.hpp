#pragma once
// Espera com tempo limite em condition_variable SEM lancar excecao.
// No PS4 o std::condition_variable::wait_for/wait_until do libc++ as vezes lanca std::system_error
// ("condition_variable timed_wait failed: Operation timed out") quando o tempo acaba. Excecao dentro de uma thread = std::terminate = abort():
// era isso que fechava o app do nada (canais, menu...). Aqui a excecao e engolida e vale como "tempo esgotado".
// Ao voltar (com ou sem excecao) o mutex do 'lk' continua travado, como no wait_for normal.
#include <chrono>
#include <condition_variable>
#include <mutex>

template <class Lock, class Rep, class Period>
inline std::cv_status cvWaitFor(std::condition_variable &cv, Lock &lk, const std::chrono::duration<Rep, Period> &d) {
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
