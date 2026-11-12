#pragma once
// Atualizacao SILENCIOSA da lista salva (regras puras, testaveis no PC; a rede e o disco entram pelo ps4_platform.cpp).
// Ideia: o app abre mostrando a copia salva (milissegundos) e, com o menu ja na tela e sem fazer nada, baixa a lista de novo so para o
// ARQUIVO de copia (sem montar uma segunda lista na memoria: o PS4 tem pouca RAM e duas listas grandes derrubavam o app).
// A copia nova so vale na proxima abertura. Regras: so se a copia tem mais de 6 h, so se a resposta parece uma lista de verdade
// e nao encolheu demais, e nunca enquanto toca video (muitos paineis IPTV permitem uma conexao por conta).
#include <cstdint>
#include <cstdio>
#include <string>

namespace listrefresh {
constexpr long kMinAgeSecs = 6 * 3600;        // so confere a rede se a copia salva tem mais que isso
constexpr double kDelaySecs = 15.0;           // espera depois de o menu aparecer (nao disputa rede/CPU com a abertura)
constexpr double kRetrySecs = 1800.0;         // se falhar, tenta de novo daqui a 30 min (com o app aberto)
constexpr long kValidClock = 1600000000L;     // relogio do console antes de 2020 = sem hora certa

// Esta na hora de conferir a rede? lastOk = quando a copia foi gravada pela ultima vez (0 = nao se sabe).
inline bool due(long now, long lastOk) {
    if (lastOk <= 0) return true;             // sem registro: confere uma vez (e passa a registrar)
    if (now < kValidClock) return false;      // sem hora confiavel: nao da para medir a idade, nao repete a cada abertura
    if (now < lastOk) return true;            // o relogio voltou atras
    return now - lastOk >= kMinAgeSecs;
}

// Resumo barato do conteudo (FNV-1a 64 bits): detecta "a lista e a mesma" sem guardar nada na memoria.
struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    void feed(const char *d, size_t n) { for (size_t i = 0; i < n; i++) { h ^= (unsigned char)d[i]; h *= 1099511628211ULL; } }
    std::string hex() const { char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)h); return b; }
};

// Os primeiros bytes parecem uma lista M3U? (ignora BOM e espacos). Paineis devolvem pagina de erro/JSON com HTTP 200.
inline bool looksLikeM3U(const std::string &head) {
    size_t i = 0;
    if (head.size() >= 3 && (unsigned char)head[0] == 0xEF && (unsigned char)head[1] == 0xBB && (unsigned char)head[2] == 0xBF) i = 3;
    while (i < head.size() && (head[i] == ' ' || head[i] == '\t' || head[i] == '\r' || head[i] == '\n')) i++;
    return head.compare(i, 7, "#EXTM3U") == 0;
}

// A copia nova substitui a antiga? So se parece lista, nao e minuscula e nao encolheu mais da metade (resposta ruim/truncada).
inline bool accept(bool headOk, uint64_t newBytes, uint64_t oldBytes) {
    if (!headOk || newBytes < 256) return false;
    if (oldBytes > 0 && newBytes * 2 < oldBytes) return false;
    return true;
}

// "resumo|bytes|url" guardado nas configuracoes: so vale para a MESMA url (outra lista = sem comparacao).
inline std::string packInfo(const std::string &sum, uint64_t bytes, const std::string &url) { return sum + "|" + std::to_string((unsigned long long)bytes) + "|" + url; }
inline bool unpackInfo(const std::string &v, const std::string &url, std::string &sum, uint64_t &bytes) {
    sum.clear(); bytes = 0;
    size_t a = v.find('|'); if (a == std::string::npos) return false;
    size_t b = v.find('|', a + 1); if (b == std::string::npos) return false;
    if (v.compare(b + 1, std::string::npos, url) != 0) return false;
    sum = v.substr(0, a);
    bytes = strtoull(v.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
    return true;
}
}  // namespace listrefresh
