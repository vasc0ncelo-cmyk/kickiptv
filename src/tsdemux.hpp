#pragma once
// Demuxer MPEG-TS em fluxo (para o player proprio): recebe bytes em QUALQUER tamanho de pedaco (segmento HLS inteiro, ou o fluxo continuo de
// um canal) e entrega, em ordem, os pacotes elementares prontos para os decoders:
//   - Video H.264: um "access unit" em Annex-B por pacote, com PTS/DTS (relogio de 90 kHz) e a marca de quadro-chave. Os quadros antes do
//     primeiro quadro-chave sao descartados e o SPS/PPS e repetido na frente de todo quadro-chave que nao o traga.
//   - Audio AAC (ADTS): um quadro ADTS por pacote, com PTS (o 2o quadro em diante de cada PES ganha o PTS calculado pela taxa de amostragem).
// Nao decodifica nada. Independente de plataforma (testavel no PC), sem excecoes, sem thread_local, sem stdio.
#include <stdint.h>
#include <string.h>
#include <functional>
#include <string>
#include <vector>
#include "tsmux.hpp"          // so para o parseSps (largura/altura do SPS)

namespace tsdemux {

enum StreamType : int {
    kNone = 0,
    kH264 = 0x1B, kHevc = 0x24, kMpeg2Video = 0x02,
    kAacAdts = 0x0F, kAacLatm = 0x11, kMp3a = 0x03, kMp3b = 0x04, kAc3 = 0x81, kEac3 = 0x87, kPrivate = 0x06,
};

struct Packet {
    bool video = true;
    std::vector<uint8_t> data;
    int64_t pts = -1, dts = -1;      // 90 kHz, 33 bits (sem desembrulhar). -1 = o fluxo nao trouxe
    bool key = false;                // video: quadro-chave (IDR)
    bool disc = false;               // o relogio saltou aqui (live reiniciou / emenda): quem consome deve reiniciar a sincronia
};

struct Info {
    int vPid = -1, aPid = -1;
    int vType = 0, aType = 0;
    int width = 0, height = 0;                      // do SPS
    int sampleRate = 0, channels = 0, aacObjectType = 0;   // do cabecalho ADTS (aacObjectType: 2 = LC)
    bool videoSupported() const { return vType == kH264; }
    bool audioSupported() const { return aType == kAacAdts; }
    bool ready() const { return vPid >= 0 && (aPid < 0 || aType != 0); }
};

struct Stats {
    uint64_t packets = 0, syncLost = 0, ccErrors = 0, vFrames = 0, aFrames = 0, droppedBeforeKey = 0, badAdts = 0, discontinuities = 0;
};

class Demuxer {
public:
    std::function<void(Packet &&)> onPacket;
    // quanto o PTS pode saltar (para tras ou para frente, em segundos) antes de contar como descontinuidade
    double discontinuitySecs = 2.5;

    const Info &info() const { return info_; }
    const Stats &stats() const { return st_; }
    // Texto para a tela quando o canal usa um codec que o player proprio ainda nao decodifica ("" = tudo certo)
    std::string unsupportedReason() const {
        if (info_.vType != 0 && info_.vType != kH264) return info_.vType == kHevc ? "video H.265/HEVC" : "video em formato nao suportado";
        if (info_.aType != 0 && info_.aType != kAacAdts) {
            switch (info_.aType) {
                case kAc3: return "audio AC-3 (Dolby Digital)";
                case kEac3: return "audio E-AC-3";
                case kMp3a: case kMp3b: return "audio MP3/MPEG";
                case kAacLatm: return "audio AAC-LATM";
                default: return "audio em formato nao suportado";
            }
        }
        return "";
    }

    void feed(const uint8_t *d, size_t n) {
        if (!n) return;
        pend_.insert(pend_.end(), d, d + n);
        size_t i = 0;
        while (pend_.size() - i >= 188) {
            if (!synced_) {                                                // procura o alinhamento: 0x47 repetido 3 vezes, de 188 em 188
                if (pend_[i] != 0x47) { i++; continue; }
                if (pend_.size() - i < 377) break;                         // falta dado para confirmar
                if (pend_[i + 188] != 0x47 || pend_[i + 376] != 0x47) { i++; continue; }
                synced_ = true;
            } else if (pend_[i] != 0x47) { synced_ = false; st_.syncLost++; i++; continue; }
            packet(pend_.data() + i);
            i += 188;
        }
        pend_.erase(pend_.begin(), pend_.begin() + (std::ptrdiff_t)i);
    }

    // Fim do fluxo / do segmento: entrega o que ainda estava montando. NAO apaga o estado (PAT/PMT, SPS/PPS, relogio):
    // o proximo segmento HLS continua de onde este parou.
    void flush() {
        // sobras ainda sem alinhamento confirmado (trecho curto, menos de 3 pacotes): aceita 0x47 seguido de 0x47 ou do fim
        size_t i = 0;
        while (!synced_ && pend_.size() - i >= 188) {
            if (pend_[i] == 0x47 && (pend_.size() - i < 376 || pend_[i + 188] == 0x47)) { packet(pend_.data() + i); i += 188; }
            else i++;
        }
        pend_.erase(pend_.begin(), pend_.begin() + (std::ptrdiff_t)i);
        finishPes(vp_, true); finishPes(ap_, false);
    }

    // Recomeca do zero (troca de canal / reabertura). Mantem o callback.
    void reset() { auto cb = std::move(onPacket); double ds = discontinuitySecs; Demuxer fresh; *this = std::move(fresh); onPacket = std::move(cb); discontinuitySecs = ds; }

private:
    struct Pes {
        std::vector<uint8_t> buf;
        bool open = false;
        int64_t pts = -1, dts = -1;
        int lastCc = -1;
        bool pendingDisc = false;
    };

    // ---------- pacote TS ----------
    void packet(const uint8_t *p) {
        st_.packets++;
        bool pusi = (p[1] & 0x40) != 0;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int afc = (p[3] >> 4) & 3, cc = p[3] & 15;
        if (p[1] & 0x80) return;                                           // transport_error_indicator
        size_t o = 4;
        bool discInd = false;
        if (afc & 2) {
            size_t al = p[4];
            if (al > 183) return;
            if (al > 0 && (p[5] & 0x80)) discInd = true;
            o = 5 + al;
        }
        if (!(afc & 1) || o >= 188) return;                                // sem payload
        const uint8_t *pl = p + o; size_t n = 188 - o;

        if (pid == 0) { section(pat_, pl, n, pusi, 0); return; }
        if (pid == pmtPid_) { section(pmt_, pl, n, pusi, 1); return; }
        Pes *s = nullptr; bool video = false;
        if (pid == info_.vPid) { s = &vp_; video = true; }
        else if (pid == info_.aPid) s = &ap_;
        else return;

        if (s->lastCc >= 0 && !discInd) {
            int expect = (s->lastCc + 1) & 15;
            if (cc != expect && cc != s->lastCc) st_.ccErrors++;           // cc repetido = pacote duplicado (permitido)
        }
        if (s->lastCc == cc && s->lastCc >= 0 && !pusi) return;            // duplicado: ignora
        s->lastCc = cc;
        if (discInd) s->pendingDisc = true;
        if (pusi) {
            finishPes(*s, video);
            startPes(*s, pl, n);
        } else if (s->open) {
            if (s->buf.size() + n > kMaxPes) { s->buf.clear(); s->open = false; return; }   // lixo: nunca acumula sem fim
            s->buf.insert(s->buf.end(), pl, pl + n);
        }
    }

    static const size_t kMaxPes = 4u << 20;

    void startPes(Pes &s, const uint8_t *pl, size_t n) {
        s.open = false; s.buf.clear(); s.pts = s.dts = -1;
        if (n < 9 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1) return;
        int flags = pl[7] >> 6;
        size_t hl = pl[8];
        if (9 + hl > n) {                                                   // cabecalho do PES cortado no pacote (raro): descarta
            return;
        }
        auto ts = [&](const uint8_t *q) -> int64_t {
            return ((int64_t)((q[0] >> 1) & 7) << 30) | ((int64_t)q[1] << 22) | ((int64_t)(q[2] >> 1) << 15) | ((int64_t)q[3] << 7) | (q[4] >> 1);
        };
        if ((flags & 2) && hl >= 5) s.pts = ts(pl + 9);
        if (flags == 3 && hl >= 10) s.dts = ts(pl + 14);
        if (s.dts < 0) s.dts = s.pts;
        s.buf.assign(pl + 9 + hl, pl + n);
        s.open = true;
    }

    // ---------- PSI (PAT / PMT) ----------
    struct Sect { std::vector<uint8_t> buf; bool open = false; };
    void section(Sect &sc, const uint8_t *pl, size_t n, bool pusi, int kind) {
        if (pusi) {
            size_t ptr = pl[0];
            if (1 + ptr > n) { sc.open = false; return; }
            pl += 1 + ptr; n -= 1 + ptr;
            sc.buf.clear(); sc.open = true;
        } else if (!sc.open) return;
        sc.buf.insert(sc.buf.end(), pl, pl + n);
        if (sc.buf.size() > 4096) { sc.open = false; sc.buf.clear(); return; }
        if (sc.buf.size() < 3) return;
        size_t len = (size_t)(((sc.buf[1] & 0x0F) << 8) | sc.buf[2]) + 3;
        if (len > 1024 + 3) { sc.open = false; return; }
        if (sc.buf.size() < len) return;
        sc.open = false;
        if (kind == 0) parsePat(sc.buf.data(), len); else parsePmt(sc.buf.data(), len);
    }
    void parsePat(const uint8_t *s, size_t len) {
        if (s[0] != 0x00 || len < 12) return;
        for (size_t i = 8; i + 4 <= len - 4; i += 4) {
            int prog = (s[i] << 8) | s[i + 1], pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
            if (prog != 0) { pmtPid_ = pid; return; }                       // primeiro programa
        }
    }
    void parsePmt(const uint8_t *s, size_t len) {
        if (s[0] != 0x02 || len < 16) return;
        size_t pil = (size_t)(((s[10] & 0x0F) << 8) | s[11]);
        size_t i = 12 + pil;
        int vPid = -1, aPid = -1, vType = 0, aType = 0;
        while (i + 5 <= len - 4) {
            int type = s[i], pid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
            size_t esl = (size_t)(((s[i + 3] & 0x0F) << 8) | s[i + 4]);
            bool isVideo = type == kH264 || type == kHevc || type == kMpeg2Video || type == 0x01;
            bool isAudio = type == kAacAdts || type == kAacLatm || type == kMp3a || type == kMp3b || type == kAc3 || type == kEac3;
            if (type == kPrivate) {                                          // AC-3 em stream privado: descritor 0x6A / E-AC-3: 0x7A
                for (size_t k = i + 5; k + 2 <= i + 5 + esl && k + 2 <= len - 4; k += 2 + s[k + 1]) {
                    if (s[k] == 0x6A) { type = kAc3; isAudio = true; }
                    else if (s[k] == 0x7A) { type = kEac3; isAudio = true; }
                }
            }
            if (isVideo && vPid < 0) { vPid = pid; vType = type; }
            else if (isAudio && aPid < 0) { aPid = pid; aType = type; }
            i += 5 + esl;
        }
        if (vPid != info_.vPid || aPid != info_.aPid || vType != info_.vType || aType != info_.aType) {
            bool pidsChanged = vPid != info_.vPid || aPid != info_.aPid;
            info_.vPid = vPid; info_.aPid = aPid; info_.vType = vType; info_.aType = aType;
            if (pidsChanged) { vp_ = Pes(); ap_ = Pes(); aCarry_.clear(); gotKey_ = false; }
        }
    }

    // ---------- fim de um PES: vira pacote ----------
    void finishPes(Pes &s, bool video) {
        if (!s.open) return;
        s.open = false;
        if (s.buf.empty()) return;
        if (video) emitVideo(s); else emitAudio(s);
        s.buf.clear();
    }

    // NAL types presentes (varre os start codes)
    struct Nals { bool idr = false, sps = false, pps = false, vcl = false; };
    static Nals scan(const std::vector<uint8_t> &b, std::vector<uint8_t> *spsOut, std::vector<uint8_t> *ppsOut) {
        Nals r; size_t n = b.size();
        for (size_t i = 0; i + 3 < n;) {
            if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
                size_t s = i + 3, e = s;
                while (e + 2 < n && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || (b[e + 2] == 0 && e + 3 < n && b[e + 3] == 1)))) e++;
                if (e + 2 >= n) e = n;
                if (s < n) {
                    int t = b[s] & 0x1F;
                    if (t == 5) { r.idr = true; r.vcl = true; }
                    else if (t >= 1 && t <= 4) r.vcl = true;
                    else if (t == 7) { r.sps = true; if (spsOut) spsOut->assign(b.begin() + (std::ptrdiff_t)s, b.begin() + (std::ptrdiff_t)e); }
                    else if (t == 8) { r.pps = true; if (ppsOut) ppsOut->assign(b.begin() + (std::ptrdiff_t)s, b.begin() + (std::ptrdiff_t)e); }
                }
                i = e;
            } else i++;
        }
        return r;
    }

    // tira zeros finais de um NAL (o "trailing_zero_8bits" / inicio de um start code de 4 bytes)
    static void trimZeros(std::vector<uint8_t> &v) { while (!v.empty() && v.back() == 0) v.pop_back(); }

    bool jumped(int64_t prev, int64_t cur) const {
        if (prev < 0 || cur < 0) return false;
        int64_t d = cur - prev;
        if (d > (1LL << 32)) d -= (1LL << 33); else if (d < -(1LL << 32)) d += (1LL << 33);     // volta de 33 bits nao conta
        double sec = (double)d / 90000.0;
        return sec > discontinuitySecs * 4 || sec < -discontinuitySecs;
    }

    void emitVideo(Pes &s) {
        if (info_.vType != kH264 || !onPacket) return;
        std::vector<uint8_t> sps, pps;
        Nals nl = scan(s.buf, &sps, &pps);
        if (nl.sps && !sps.empty()) { trimZeros(sps); if (sps != sps_) { sps_ = sps; int w = 0, h = 0; if (tsmux::detail::parseSps(sps_, w, h)) { info_.width = w; info_.height = h; } } }
        if (nl.pps && !pps.empty()) { trimZeros(pps); pps_ = pps; }
        if (!nl.vcl) return;                                                // so SPS/PPS/SEI soltos: nao e quadro
        int64_t t = s.dts >= 0 ? s.dts : s.pts;
        if (s.pendingDisc || jumped(lastVDts_, t)) { st_.discontinuities++; gotKey_ = false; vDisc_ = true; s.pendingDisc = false; }
        if (!gotKey_) {                                                     // depois de uma emenda (ou no comeco) so entra a partir de um quadro-chave
            if (!nl.idr) { st_.droppedBeforeKey++; return; }
            gotKey_ = true;
        }
        Packet pk; pk.video = true; pk.key = nl.idr; pk.pts = s.pts; pk.dts = s.dts; pk.disc = vDisc_; vDisc_ = false;
        if (nl.idr && !(nl.sps && nl.pps) && !sps_.empty() && !pps_.empty()) {     // quadro-chave sem parametros: poe na frente
            static const uint8_t sc[4] = {0, 0, 0, 1};
            pk.data.insert(pk.data.end(), sc, sc + 4); pk.data.insert(pk.data.end(), sps_.begin(), sps_.end());
            pk.data.insert(pk.data.end(), sc, sc + 4); pk.data.insert(pk.data.end(), pps_.begin(), pps_.end());
        }
        pk.data.insert(pk.data.end(), s.buf.begin(), s.buf.end());
        lastVDts_ = t;
        st_.vFrames++;
        onPacket(std::move(pk));
    }

    static const int *rates() { static const int r[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350}; return r; }

    void emitAudio(Pes &s) {
        if (info_.aType != kAacAdts || !onPacket) return;
        std::vector<uint8_t> &c = aCarry_;
        size_t carryBefore = c.size();
        c.insert(c.end(), s.buf.begin(), s.buf.end());
        int64_t pts = s.pts;
        bool first = true;
        bool disc = s.pendingDisc || jumped(lastAPts_, s.pts);
        if (disc) { st_.discontinuities++; aBase_ = -1; }
        size_t i = 0;
        while (i + 7 <= c.size()) {
            if (!(c[i] == 0xFF && (c[i + 1] & 0xF6) == 0xF0)) { i++; st_.badAdts++; continue; }   // sincronismo ADTS (layer 0)
            int prot = c[i + 1] & 1, obj = ((c[i + 2] >> 6) & 3) + 1, sf = (c[i + 2] >> 2) & 15;
            int ch = ((c[i + 2] & 1) << 2) | (c[i + 3] >> 6);
            size_t fl = (size_t)(((c[i + 3] & 3) << 11) | (c[i + 4] << 3) | (c[i + 5] >> 5));
            if (sf > 12 || fl < (size_t)(prot ? 7 : 9)) { i++; st_.badAdts++; continue; }
            if (i + fl > c.size()) break;                                  // quadro incompleto: espera o proximo PES
            int sr = rates()[sf];
            Packet pk; pk.video = false; pk.disc = disc && first;
            pk.data.assign(c.begin() + (std::ptrdiff_t)i, c.begin() + (std::ptrdiff_t)(i + fl));
            // PTS: o primeiro quadro que COMECA neste PES usa o PTS do PES; os outros (e os que vieram de um PES anterior) seguem a taxa
            bool startsHere = i >= carryBefore;
            if (startsHere && first && pts >= 0) { aBase_ = pts; aCount_ = 0; first = false; }
            else if (startsHere && first) first = false;
            if (aBase_ >= 0) pk.pts = pk.dts = (aBase_ + (int64_t)aCount_ * 92160000 / sr) & ((1LL << 33) - 1);   // 1024 amostras por quadro, em ticks de 90 kHz
            aCount_++;
            if (ch > 0 && ch <= 7) { info_.channels = ch == 7 ? 8 : ch; info_.sampleRate = sr; info_.aacObjectType = obj; }
            i += fl;
            st_.aFrames++;
            if (pk.pts >= 0) lastAPts_ = pk.pts;
            onPacket(std::move(pk));
        }
        c.erase(c.begin(), c.begin() + (std::ptrdiff_t)i);
        if (c.size() > 16384) c.clear();                                   // sem sincronismo ha muito tempo: joga fora
        s.pendingDisc = false;
    }

    std::vector<uint8_t> pend_;
    bool synced_ = false;
    int pmtPid_ = -1;
    Sect pat_, pmt_;
    Info info_;
    Stats st_;
    Pes vp_, ap_;
    std::vector<uint8_t> sps_, pps_, aCarry_;
    bool gotKey_ = false, vDisc_ = false;
    int64_t lastVDts_ = -1, lastAPts_ = -1, aBase_ = -1;
    int64_t aCount_ = 0;
};

}  // namespace tsdemux
