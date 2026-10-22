#pragma once
// Hora e data para o canto da tela. Sem gmtime/localtime (o app evita funcoes extras da libc).
#include <string>

// Fuso horario fixo: Brasilia (UTC-3). Mude aqui se quiser outro fuso (em minutos).
constexpr int kTzOffsetMin = -180;

inline void clockStrings(long long unixSec, std::string &hhmm, std::string &date) {
    long long t = unixSec + (long long)kTzOffsetMin * 60;
    long long days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int hh = (int)(rem / 3600), mm = (int)((rem % 3600) / 60);
    // dias desde 1970-01-01 -> data civil (algoritmo de Howard Hinnant)
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    int d = (int)(doy - (153 * mp + 2) / 5 + 1);
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) y++;
    static const char *mes[12] = {"Jan", "Fev", "Mar", "Abr", "Mai", "Jun", "Jul", "Ago", "Set", "Out", "Nov", "Dez"};
    char b[32];
    b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':'; b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = 0;
    hhmm = b;
    date = std::to_string(d) + " " + mes[m - 1] + " " + std::to_string((int)y);
}
