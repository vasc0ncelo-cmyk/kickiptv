#pragma once
// Politica de buffer da transmissao ao vivo (Kick e canais), sem dependencia de plataforma (testavel no PC).
// Problema que isto resolve: a live comecava a tocar com ~2 s de conteudo (canal) ou 3 segmentos (Kick) e trocava de "pedaco" a cada
// poucos segundos; qualquer oscilacao da rede maior que isso esvaziava o buffer e travava SOM E VIDEO ate o vigia reabrir a live
// ("volta em outra parte"). Agora: comeca com mais conteudo guardado e junta mais segmentos em cada pedaco (menos trocas).
namespace livebuf {
constexpr double kStartSecsHls = 12.0;   // HLS (Kick): comeca a tocar uns 12 s atras do ao vivo (a playlist ja tem esse conteudo: nao demora mais para abrir)
constexpr double kStartSecsRaw = 8.0;    // canal MPEG-TS: espera juntar ~8 s antes de tocar (o fluxo chega em tempo real)
constexpr int kMinStartSegs = 3, kMaxStartSegs = 8;
constexpr int kWindowHls = 12, kWindowRaw = 16;   // quantos segmentos/pedacos ficam na memoria (antes 6 e 8)
constexpr int kMaxMerge = 8;             // prepara o proximo pedaco de qualquer jeito quando ja ha tantos segmentos novos

// Quantos segmentos (de 'segDur' s) formam o buffer inicial de 'wantSecs' s, limitado ao que ja existe ('available').
inline int startSegs(double segDur, double wantSecs, int available = 1000, int maxSegs = kMaxStartSegs) {
    if (!(segDur >= 0.5)) segDur = 2.0;                       // desconhecido (ou NaN): 2 s
    if (segDur > 12.0) segDur = 12.0;
    int n = (int)(wantSecs / segDur);
    if ((double)n * segDur < wantSecs) n++;                   // arredonda para cima
    if (n < kMinStartSegs) n = kMinStartSegs;
    if (n > maxSegs) n = maxSegs;
    if (n > available) n = available;
    if (n < 1) n = 1;
    return n;
}

// Hora de preparar (converter e gravar) o proximo pedaco? Em vez de preparar assim que ha 2 segmentos novos (pedacos curtos = muitas trocas),
// espera o pedaco atual chegar perto do fim e junta tudo o que chegou. curDur/el: duracao do pedaco atual e quanto ja tocou (s);
// convMs: tempo da ultima conversao (a folga cobre 3x isso).
inline bool shouldPrep(int fresh, double curDur, double el, double convMs) {
    if (fresh < 2) return false;
    if (fresh >= kMaxMerge) return true;
    if (curDur <= 0) return true;                             // sem a duracao do pedaco atual: como antes
    if (el <= 0) return false;                                // o pedaco atual ainda nao mostrou o 1o quadro
    return curDur - el <= 2.5 + convMs * 0.003;
}

// ---------- Ajuste automatico depois de congelamentos ----------
// Cada vez que o vigia precisa reabrir a live (imagem parada, rede caiu...), conta como um "congelamento". Os recentes (ultimos 5 min)
// fazem a proxima abertura ser mais conservadora: guarda mais conteudo antes de tocar e, se continuar, baixa a resolucao
// (menos dados para baixar e menos pixels para a CPU desenhar). Depois de 5 min sem congelar, volta ao normal.
constexpr double kStallWindowSecs = 300.0;
constexpr int kExtraSecsPerStall = 4, kMaxExtraSecs = 12, kExtraMaxSegs = 12;
inline double extraStartSecs(int stalls) {
    if (stalls <= 0) return 0.0;
    double e = (double)stalls * kExtraSecsPerStall;
    return e > kMaxExtraSecs ? (double)kMaxExtraSecs : e;
}
inline int capHeight(int maxH, int stalls) {                 // altura maxima da variante: 2 congelamentos -> 480p, 4 ou mais -> 360p
    if (stalls >= 4) return maxH < 360 ? maxH : 360;
    if (stalls >= 2) return maxH < 480 ? maxH : 480;
    return maxH;
}
// quantos congelamentos (instantes em 'times', relogio monotonico) aconteceram nos ultimos kStallWindowSecs
inline int recentStalls(const double *times, int n, double now) {
    int c = 0;
    for (int i = 0; i < n; i++) if (times[i] > 0 && now - times[i] >= 0 && now - times[i] < kStallWindowSecs) c++;
    return c;
}
// Tempo maximo para baixar UM segmento. O timeout de leitura do sceHttp vale por leitura, entao uma conexao que "pinga" bytes devagar
// nunca estourava e esvaziava o buffer; agora o segmento tem prazo total (4x a duracao dele, entre 8 e 20 s) e, estourando, tenta de novo.
inline double segDeadlineSecs(double segDur) {
    if (!(segDur > 0.5)) segDur = 2.0;
    double d = segDur * 4.0;
    return d < 8.0 ? 8.0 : (d > 20.0 ? 20.0 : d);
}
// Espera entre tentativas seguidas de baixar a playlist: cresce de 0,4 s ate 2 s (antes era sempre 0,4 s: martelava o servidor
// e refazia o handshake TLS, que no PS4 e lento, a cada falha).
inline int backoffMs(int errs) {
    int ms = 400 * (errs < 1 ? 1 : errs);
    return ms > 2000 ? 2000 : ms;
}
}  // namespace livebuf
