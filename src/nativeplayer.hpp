#pragma once
// =============================================================================
// Player NATIVO — sem SceAvPlayer no pipeline de video
// =============================================================================
// Fluxo:
//   MP4 (avc1) ou .h264 Annex-B  →  demux (mp4annexb)  →  sceVideodec (hwplayer)
//   → frame NV12  →  blit (fbrender)
//
// O que JA funciona nesta base:
//   - Carregar libSceVideodec em runtime
//   - Decodificar H.264 Annex-B
//   - Converter MP4 progressivo simples (avc1) → Annex-B e decodificar
//
// Ainda NÃO (proximas fases):
//   - Audio (AAC/sceAudiodec)
//   - HLS/fMP4 ao vivo (Kick) — precisa juntar segmentos TS/fMP4 e alimentar o decoder
//   - Seek fino em arquivo enorme sem materializar Annex-B inteiro na RAM/disco
//
// VOD: so este player (AvPlayer desligado no filme/serie). Live/HLS ainda usa pipeline antigo.
// =============================================================================
#include "hwplayer.hpp"
#include "mp4annexb.hpp"
#include "mp4stream.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace nativeplayer {

inline bool boot() { return hwplayer::Player::tryLoadApi(); }
inline bool available() { return hwplayer::Player::hwAvailable(); }

class Session {
public:
    // Abre arquivo local .mp4 (avc1) ou .h264
    bool openFile(const std::string &path) {
        close();
        std::ifstream in(path, std::ios::binary);
        if (!in) { err_ = "nao abriu: " + path; return false; }
        in.seekg(0, std::ios::end);
        auto len = (std::streamoff)in.tellg();
        if (len <= 32 || len > 380ll * 1024 * 1024) { err_ = "tamanho invalido"; return false; }
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> data((size_t)len);
        in.read((char *)data.data(), len);
        if (!in) { err_ = "leitura falhou"; return false; }
        return openMemory(data.data(), data.size());
    }

    bool openMemory(const uint8_t *p, size_t n) {
        close();
        if (!p || n < 32) { err_ = "buffer vazio"; return false; }
        if (!boot()) { err_ = "sceVideodec indisponivel neste firmware"; return false; }

        const char *outPath = "/data/kickiptv_native.h264";

        // MP4?
        if (n >= 8 && p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p') {
            mp4ab::Track tr;
            if (!mp4ab::parse(p, n, tr, err_)) return false;
            std::vector<uint8_t> spsPps;
            int nalLen = 4;
            if (!mp4ab::avcCToAnnexB(tr.extradata, spsPps, nalLen)) {
                err_ = "avcC invalido";
                return false;
            }
            FILE *f = fopen(outPath, "wb");
            if (!f) { err_ = "nao gravou /data/kickiptv_native.h264"; return false; }
            fwrite(spsPps.data(), 1, spsPps.size(), f);
            std::vector<uint8_t> body;
            size_t written = 0;
            for (const auto &s : tr.samples) {
                if (s.off + s.size > n) continue;
                body.clear();
                if (!mp4ab::sampleToAnnexB(p + s.off, s.size, nalLen, body)) continue;
                fwrite(body.data(), 1, body.size(), f);
                written += body.size();
            }
            fclose(f);
            if (written < 64) { err_ = "nenhum sample avc1 extraido"; return false; }
            width_ = tr.width;
            height_ = tr.height;
            if (!hw_.openFile(outPath, width_ ? (int)width_ : 1920, height_ ? (int)height_ : 1080)) {
                err_ = hw_.lastError().empty() ? "decoder rejeitou o stream" : hw_.lastError();
                return false;
            }
            open_ = true;
            return true;
        }

        // Assume Annex-B
        FILE *f = fopen(outPath, "wb");
        if (!f) { err_ = "nao gravou annex-b"; return false; }
        fwrite(p, 1, n, f);
        fclose(f);
        if (!hw_.openFile(outPath)) {
            err_ = hw_.lastError().empty() ? "decoder rejeitou annex-b" : hw_.lastError();
            return false;
        }
        open_ = true;
        return true;
    }

    // ---- STREAMING (VOD remoto) ------------------------------------------------------------
    // Nao baixa o filme: 'rd' le por posicao (httpfile::Reader::read no PS4). A RAM usada e so o
    // indice do MP4 (alguns MB) + 1 sample por vez, em vez de 2-3 copias do arquivo inteiro.
    // Fase 1 (so I/O, pode rodar em thread): le e interpreta o indice (moov).
    bool openStreamIndex(const mp4s::ReadAt &rd, uint64_t total, int openTimeoutMs = 60000) {
        close();
        rd_ = rd;
        if (!dm_.open(rd, total, err_, openTimeoutMs)) return false;
        width_ = dm_.width();
        height_ = dm_.height();
        return true;
    }
    // Fase 2 (thread principal): cria o decoder sceVideodec.
    bool openStreamDecoder() {
        if (!boot()) { err_ = "sceVideodec indisponivel neste firmware"; return false; }
        if (dm_.count() == 0) { err_ = "indice vazio"; return false; }
        if (!hw_.openStream(width_ ? (int)width_ : 1920, height_ ? (int)height_ : 1080)) {
            err_ = hw_.lastError().empty() ? "decoder rejeitou o stream" : hw_.lastError();
            return false;
        }
        next_ = 0; clockMs_ = 0; lastT_ = -1; stalls_ = 0; streaming_ = true; open_ = true;
        return true;
    }
    bool openStream(const mp4s::ReadAt &rd, uint64_t total, int openTimeoutMs = 60000) {
        return openStreamIndex(rd, total, openTimeoutMs) && openStreamDecoder();
    }

    // Chamar a cada volta do loop de video. 'nowSec' = relogio monotonico (s).
    // O relogio de reproducao so anda enquanto ha dados (buffering para o relogio) e nao pula apos pausa.
    // Retorna true quando ha um quadro novo em 'out'.
    bool pump(hwplayer::Frame &out, double nowSec) {
        out = hwplayer::Frame{};
        if (!open_ || !streaming_) return false;
        double dt = lastT_ < 0 ? 0 : nowSec - lastT_;
        lastT_ = nowSec;
        if (dt < 0 || dt > 0.5) dt = 0;                    // pausa/travada: nao "recuperar" o tempo perdido
        if (!buffering_) clockMs_ += dt * 1000.0;
        for (int k = 0; k < 4 && next_ < dm_.count(); k++) {
            const mp4s::Sample &s = dm_.at(next_);
            if ((double)s.dtsMs > clockMs_ + 40.0) return false;      // ainda nao e hora deste sample
            int r = dm_.readAnnexB(next_, au_, 30);                   // 30 ms: nunca prender o loop de video na rede
            if (r == 0) { buffering_ = true; stalls_++; return false; }
            buffering_ = false; stalls_ = 0;
            size_t i = next_++;
            if (r < 0) continue;                                      // sample corrompido: pula
            if (hw_.feedAu(au_.data(), au_.size(), (uint64_t)std::max<int64_t>(0, dm_.at(i).ptsMs),
                           (uint64_t)std::max<int64_t>(0, dm_.at(i).dtsMs), out) && out.valid)
                return true;
        }
        if (next_ >= dm_.count()) ended_ = true;
        return false;
    }

    bool isBuffering() const { return buffering_; }
    int stalledPumps() const { return stalls_; }              // quantas voltas seguidas sem dados
    bool ended() const { return ended_; }
    uint64_t durationMs() const { return streaming_ ? dm_.durationMs() : 0; }
    uint64_t positionMs() const { return (uint64_t)clockMs_; }

    // Seek: volta ao sync sample anterior a 'ms' e reinicia o relogio ali.
    void seekMs(int64_t ms) {
        if (!streaming_) return;
        next_ = dm_.syncBefore(ms < 0 ? 0 : ms);
        clockMs_ = (double)dm_.at(next_).dtsMs;
        buffering_ = false; ended_ = false;
    }

    bool pump(hwplayer::Frame &out) {
        if (!open_ || streaming_) return false;
        return hw_.pump(out);
    }

    void close() {
        hw_.close();
        dm_ = mp4s::Demuxer();
        rd_ = nullptr;
        au_.clear(); au_.shrink_to_fit();
        open_ = false; streaming_ = false; buffering_ = false; ended_ = false;
        next_ = 0; clockMs_ = 0; lastT_ = -1; stalls_ = 0;
        err_.clear();
    }

    bool isOpen() const { return open_; }
    const std::string &lastError() const { return err_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    bool open_ = false;
    std::string err_;
    hwplayer::Player hw_;
    mp4s::Demuxer dm_;
    mp4s::ReadAt rd_;
    std::vector<uint8_t> au_;
    size_t next_ = 0;
    double clockMs_ = 0, lastT_ = -1;
    int stalls_ = 0;
    bool streaming_ = false, buffering_ = false, ended_ = false;
    uint32_t width_ = 0, height_ = 0;
};

} // namespace nativeplayer
