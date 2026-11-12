#pragma once
// Canal ao vivo "cru" (MPEG-TS continuo: http://painel/usuario/senha/123 ou .../123.ts) -> pedacos de ~2 s.
// O restante do app (conversao TS->MP4 e reproducao em pedacos) foi feito para segmentos HLS: cada segmento comeca
// com PAT+PMT e num quadro-chave (IDR). Este cortador faz o mesmo com o fluxo continuo:
//   - guarda a ultima PAT/PMT e escreve no comeco de cada pedaco;
//   - so corta num quadro-chave (senao o inicio do pedaco ficaria sem imagem ate o proximo IDR).
// Independente de plataforma (testavel no PC).
#include <stdint.h>
#include <functional>
#include <string>

struct TsChunker {
    std::function<void(std::string &&)> emit;      // chamado com cada pedaco pronto
    double target = 2.0;                          // duracao alvo do pedaco, em segundos de relogio
    size_t maxBytes = 8u << 20;                   // sem quadro-chave por tempo demais: corta assim mesmo
    size_t maxPending = 4u << 20;                 // fluxo sem nenhum pacote TS valido (nao e TS): para de acumular
    bool notTs = false;                           // true = lixo (pagina HTML, lista m3u8...) em vez de MPEG-TS
    int vPid = -1;

    void feed(const char *d, size_t n, double now) {
        if (notTs) return;
        pend_.append(d, n);
        size_t i = 0;
        while (pend_.size() - i >= 188) {
            const uint8_t *p = (const uint8_t *)pend_.data() + i;
            bool ok = p[0] == 0x47;
            if (ok && !synced_) {                                            // confirma o alinhamento olhando 3 pacotes
                if (pend_.size() - i < 377) break;                           // falta dado para confirmar
                ok = p[188] == 0x47 && p[376] == 0x47;
            }
            if (!ok) {
                synced_ = false; i++;
                if (++skipped_ > maxPending) { notTs = true; pend_.clear(); return; }
                continue;
            }
            synced_ = true; skipped_ = 0;
            packet(p, now);
            i += 188;
        }
        pend_.erase(0, i);
    }

    // Termina o pedaco em andamento (fim do fluxo). Nao emite pedaco pela metade se ainda nao havia quadro-chave.
    void flush() { if (haveKey_ && cur_.size() > psi().size()) { emit(std::move(cur_)); } cur_.clear(); haveKey_ = false; }

private:
    std::string pend_, cur_, pat_, pmt_;
    bool synced_ = false, haveKey_ = false;
    size_t skipped_ = 0;
    int pmtPid_ = -1;
    double chunkT0_ = 0;

    std::string psi() const { return pat_ + pmt_; }

    // O pacote comeca um quadro de video com IDR (ou SPS, que vem logo antes do IDR)?
    bool keyStart(const uint8_t *p) const {
        int afc = (p[3] >> 4) & 3; size_t o = 4;
        if (afc & 2) o += 1 + p[4];
        if (!(afc & 1) || o + 9 >= 188) return false;
        const uint8_t *pl = p + o; size_t n = 188 - o;
        if (pl[0] || pl[1] || pl[2] != 1) return false;                      // inicio de PES
        size_t k = 9 + pl[8];
        for (; k + 4 < n; k++) {
            if (pl[k] == 0 && pl[k + 1] == 0 && (pl[k + 2] == 1 || (pl[k + 2] == 0 && pl[k + 3] == 1))) {
                size_t h = k + (pl[k + 2] == 1 ? 3 : 4);
                if (h >= n) return false;
                int t = pl[h] & 0x1F;
                if (t == 7 || t == 5) return true;                           // SPS ou IDR
                if (t != 9 && t != 6) return false;                          // AUD/SEI podem vir antes; qualquer outra coisa: quadro normal
                k = h;
            }
        }
        return false;
    }

    void packet(const uint8_t *p, double now) {
        if (p[1] & 0x80) { if (haveKey_) cur_.append((const char *)p, 188); return; }
        bool pusi = p[1] & 0x40; int pid = ((p[1] & 0x1F) << 8) | p[2];
        int afc = (p[3] >> 4) & 3; size_t o = 4;
        if (afc & 2) o += 1 + p[4];
        const uint8_t *pl = p + o; size_t pn = o < 188 ? 188 - o : 0;
        bool isPsi = false;
        if ((afc & 1) && pn > 12 && pusi) {
            if (pid == 0) {                                                  // PAT
                size_t ptr = pl[0];
                if (1 + ptr + 12 <= pn) {
                    const uint8_t *s = pl + 1 + ptr; size_t sl = ((s[1] & 0x0F) << 8) | s[2];
                    for (size_t k = 8; k + 4 <= 3 + sl - 4 && 1 + ptr + k + 4 <= pn; k += 4)
                        if (((s[k] << 8) | s[k + 1]) != 0) { pmtPid_ = ((s[k + 2] & 0x1F) << 8) | s[k + 3]; break; }
                    pat_.assign((const char *)p, 188); isPsi = true;
                }
            } else if (pid == pmtPid_) {                                     // PMT: acha o PID do video H.264
                size_t ptr = pl[0];
                if (1 + ptr + 12 <= pn) {
                    const uint8_t *s = pl + 1 + ptr; size_t sl = ((s[1] & 0x0F) << 8) | s[2];
                    size_t pil = ((s[10] & 0x0F) << 8) | s[11], k = 12 + pil, end = 3 + sl - 4;
                    while (k + 5 <= end && 1 + ptr + k + 5 <= pn) {
                        int st = s[k], ep = ((s[k + 1] & 0x1F) << 8) | s[k + 2]; size_t il = ((s[k + 3] & 0x0F) << 8) | s[k + 4];
                        if (st == 0x1B) { vPid = ep; break; }
                        k += 5 + il;
                    }
                    pmt_.assign((const char *)p, 188); isPsi = true;
                }
            }
        }
        if (isPsi) { if (haveKey_) cur_.append((const char *)p, 188); return; }   // PSI dentro do pedaco: mantem (e ja esta guardado para o proximo)
        if (pat_.empty() || pmt_.empty() || vPid < 0) return;                    // ainda nao sabemos o formato: descarta
        if (pid == vPid && pusi && keyStart(p)) {
            bool due = !haveKey_ || now - chunkT0_ >= target;
            if (haveKey_ && due) emit(std::move(cur_));
            if (due) { cur_ = psi(); chunkT0_ = now; haveKey_ = true; }
        }
        if (!haveKey_) return;                                                   // antes do primeiro quadro-chave: nada serve
        cur_.append((const char *)p, 188);
        if (cur_.size() > maxBytes) { emit(std::move(cur_)); cur_ = psi(); haveKey_ = false; chunkT0_ = now; }   // sem IDR: recomeca no proximo
    }
};
