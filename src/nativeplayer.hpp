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
// AvPlayer continua no app so como fallback ate essas fases fecharem.
// =============================================================================
#include "hwplayer.hpp"
#include "mp4annexb.hpp"
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
        if (len <= 32 || len > 400ll * 1024 * 1024) { err_ = "tamanho invalido"; return false; }
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

    bool pump(hwplayer::Frame &out) {
        if (!open_) return false;
        return hw_.pump(out);
    }

    void close() {
        hw_.close();
        open_ = false;
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
    uint32_t width_ = 0, height_ = 0;
};

} // namespace nativeplayer
