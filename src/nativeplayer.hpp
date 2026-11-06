#pragma once
// Player nativo streaming: NAO carrega o MP4 inteiro na RAM.
// Indice (moov) via Range + 1 sample por frame via Range.
#include "hwplayer.hpp"
#include "mp4annexb.hpp"
#include "mp4stream.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace nativeplayer {

inline bool boot() { return hwplayer::Player::tryLoadApi(); }
inline bool available() { return hwplayer::Player::hwAvailable(); }

using FetchRange = std::function<bool(uint64_t from, uint64_t to, std::string &body)>;

inline std::string scanCodecs(const uint8_t *p, size_t n) {
    // procura atoms de codec nos primeiros/ultimos bytes baixados
    std::string found;
    auto has = [&](const char *tag) {
        for (size_t i = 0; i + 4 <= n; i++)
            if (p[i]==tag[0]&&p[i+1]==tag[1]&&p[i+2]==tag[2]&&p[i+3]==tag[3]) return true;
        return false;
    };
    if (has("avc1") || has("avcC") || has("AVC1")) found += "H.264 ";
    if (has("hvc1") || has("hev1") || has("hvcC") || has("HVC1")) found += "HEVC ";
    if (has("mp4a") || has("MP4A")) found += "AAC ";
    if (has("vp09") || has("vp08")) found += "VP9 ";
    if (has("av01")) found += "AV1 ";
    if (n >= 4 && p[0] == 0x47) found += "MPEG-TS ";
    if (n >= 4 && p[0]==0x1A && p[1]==0x45 && p[2]==0xDF && p[3]==0xA3) found += "MKV ";
    if (has("ftyp")) found += "ftyp ";
    if (has("moov")) found += "moov ";
    if (has("mdat")) found += "mdat ";
    if (found.empty()) found = "desconhecido";
    return found;
}

// Sessao de reproducao nativa: indice (moov) lido com poucas requisicoes Range + 1 sample por vez.
// IMPORTANTE: o indice vem do mp4s::Demuxer (mp4stream.hpp). O mp4ab::parse antigo procurava o avcC no lugar errado do avc1,
// misturava as trilhas de audio e video e nao entendia stsc: falhava com QUALQUER MP4 real ("MP4 sem trilha avc1/avcC").
class Session {
public:
    bool openUrl(const std::string &url, uint64_t totalHint, FetchRange fetch) {
        close();
        fetch_ = std::move(fetch);
        url_ = url;
        if (!boot()) { err_ = "sceVideodec indisponivel neste firmware"; return false; }
        if (!fetch_) { err_ = "sem fetch"; return false; }
        total_ = totalHint;
        if (total_ < 1024) { err_ = "tamanho do arquivo desconhecido"; return false; }

        // Primeiros bytes: so para dar uma mensagem clara quando nao e MP4.
        std::string head;
        if (!fetch_(0, 63, head) || head.size() < 16) { err_ = "falha ao ler inicio do arquivo"; return false; }
        if ((uint8_t)head[0] == 0x47) { err_ = "arquivo e MPEG-TS (nao MP4). Nativo ainda so MP4 H.264"; return false; }
        if ((uint8_t)head[0] == 0x1A && (uint8_t)head[1] == 0x45 && (uint8_t)head[2] == 0xDF && (uint8_t)head[3] == 0xA3) {
            err_ = "arquivo e MKV. Nativo ainda so MP4 H.264";
            return false;
        }

        // Leitura por Range para o demuxer. Funciona com o moov no comeco OU no fim (anda pelas caixas lendo so cabecalhos).
        rd_ = [this](uint8_t *dst, uint64_t pos, uint32_t len, int) -> int {
            if (!fetch_ || len == 0) return 0;
            std::string b;
            if (!fetch_(pos, pos + len - 1, b) || b.empty()) return 0;
            size_t n = std::min<size_t>(b.size(), len);
            memcpy(dst, b.data(), n);
            return (int)n;
        };
        std::string perr;
        if (!dm_.open(rd_, total_, perr)) {
            err_ = perr + " | " + scanCodecs((const uint8_t *)head.data(), head.size());
            return false;
        }
        width_ = dm_.width();
        height_ = dm_.height();
        sampleIdx_ = 0;

        if (!hw_.open(width_ ? (int)width_ : 1920, height_ ? (int)height_ : 1080)) {
            err_ = hw_.lastError().empty() ? "decoder open falhou" : hw_.lastError();
            return false;
        }
        open_ = true;      // o SPS/PPS vai junto com cada sample de sincronismo (Demuxer::readAnnexB)
        return true;
    }

    bool openFile(const std::string &path) {
        close();
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { err_ = "nao abriu: " + path; return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fclose(f);
        if (sz < 64) { err_ = "arquivo pequeno"; return false; }
        auto fetchDisk = [path](uint64_t from, uint64_t to, std::string &body) -> bool {
            FILE *fp = fopen(path.c_str(), "rb");
            if (!fp) return false;
            if (fseek(fp, (long)from, SEEK_SET) != 0) { fclose(fp); return false; }
            size_t n = (size_t)(to - from + 1);
            body.assign(n, '\0');
            size_t got = fread(&body[0], 1, n, fp);
            fclose(fp);
            body.resize(got);
            return got > 0;
        };
        return openUrl("file://" + path, (uint64_t)sz, fetchDisk);
    }

    bool openMemory(const uint8_t *p, size_t n) {
        if (n > 32u * 1024u * 1024u) { err_ = "buffer grande — use openUrl"; return false; }
        const char *tmp = "/data/kickiptv_native_mem.bin";
        FILE *fp = fopen(tmp, "wb");
        if (!fp) { err_ = "tmp"; return false; }
        fwrite(p, 1, n, fp);
        fclose(fp);
        return openFile(tmp);
    }

    bool pump(hwplayer::Frame &out) {
        out = hwplayer::Frame{};
        if (!open_ || sampleIdx_ >= dm_.count()) return false;
        std::vector<uint8_t> au;
        int r = dm_.readAnnexB(sampleIdx_, au, 15000);
        if (r == 0) { err_ = "Range sample falhou"; return false; }      // ainda nao chegou: tenta o mesmo de novo
        sampleIdx_++;
        if (r < 0) return false;                                          // sample invalido: pula
        return hw_.feedAu(au.data(), au.size(), out);
    }

    void close() {
        hw_.close();
        open_ = false;
        dm_ = mp4s::Demuxer();
        rd_ = nullptr;
        fetch_ = nullptr;
        err_.clear();
        sampleIdx_ = 0;
    }

    bool isOpen() const { return open_; }
    const std::string &lastError() const { return err_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t sampleCount() const { return dm_.count(); }
    size_t sampleIndex() const { return sampleIdx_; }

private:
    bool open_ = false;
    std::string err_, url_;
    FetchRange fetch_;
    hwplayer::Player hw_;
    mp4s::Demuxer dm_;
    mp4s::ReadAt rd_;
    size_t sampleIdx_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint64_t total_ = 0;
};

} // namespace nativeplayer
