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
inline int startSegs(double segDur, double wantSecs, int available = 1000) {
    if (!(segDur >= 0.5)) segDur = 2.0;                       // desconhecido (ou NaN): 2 s
    if (segDur > 12.0) segDur = 12.0;
    int n = (int)(wantSecs / segDur);
    if ((double)n * segDur < wantSecs) n++;                   // arredonda para cima
    if (n < kMinStartSegs) n = kMinStartSegs;
    if (n > kMaxStartSegs) n = kMaxStartSegs;
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
}  // namespace livebuf
