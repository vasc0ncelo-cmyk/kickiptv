#pragma once
// Player nativo streaming: NAO carrega o MP4 inteiro na RAM.
// Indice (moov) via Range + 1 sample por frame via Range.
#include "hwplayer.hpp"
#include "mp4annexb.hpp"
#include <algorithm>
#include <cstdint>
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

inline bool tryParseBuf(const uint8_t *p, size_t n, mp4ab::Track &tr, std::string &err) {
    tr = mp4ab::Track{};
    err.clear();
    if (n < 16) { err = "buffer curto"; return false; }
    return mp4ab::parse(p, n, tr, err) && !tr.samples.empty() && !tr.extradata.empty();
}

class Session {
public:
    bool openUrl(const std::string &url, uint64_t totalHint, FetchRange fetch) {
        close();
        fetch_ = std::move(fetch);
        url_ = url;
        if (!boot()) { err_ = "sceVideodec indisponivel neste firmware"; return false; }
        if (!fetch_) { err_ = "sem fetch"; return false; }

        uint64_t total = totalHint;

        // --- HEAD: ate 16 MB (indice faststart costuma estar aqui) ---
        const uint64_t headMax = 16ull * 1024ull * 1024ull;
        uint64_t headTo = headMax - 1;
        if (total > 0 && headTo >= total) headTo = total - 1;
        std::string head;
        if (!fetch_(0, headTo, head) || head.size() < 32) {
            err_ = "falha ao ler inicio do arquivo";
            return false;
        }

        // TS puro?
        if ((uint8_t)head[0] == 0x47) {
            err_ = "arquivo e MPEG-TS (nao MP4). Nativo ainda so MP4 avc1";
            return false;
        }
        // MKV?
        if ((uint8_t)head[0]==0x1A && (uint8_t)head[1]==0x45 && (uint8_t)head[2]==0xDF && (uint8_t)head[3]==0xA3) {
            err_ = "arquivo e MKV. Nativo ainda so MP4 avc1";
            return false;
        }

        std::string scanH = scanCodecs((const uint8_t *)head.data(), head.size());
        mp4ab::Track tr;
        std::string perr;
        bool parsed = tryParseBuf((const uint8_t *)head.data(), head.size(), tr, perr);

        // --- TAIL: se nao achou, baixa ultimos 8 MB (moov no fim) ---
        std::string scanT;
        if (!parsed && total > headMax) {
            uint64_t tailSz = 8ull * 1024ull * 1024ull;
            uint64_t tailFrom = total > tailSz ? total - tailSz : 0;
            std::string tail;
            if (fetch_(tailFrom, total - 1, tail) && tail.size() > 64) {
                scanT = scanCodecs((const uint8_t *)tail.data(), tail.size());
                // procura atom moov no tail e monta ftyp minimo + moov
                const uint8_t *p = (const uint8_t *)tail.data();
                size_t n = tail.size();
                for (size_t i = 0; i + 8 < n; i++) {
                    if (p[i+4]=='m'&&p[i+5]=='o'&&p[i+6]=='o'&&p[i+7]=='v') {
                        uint32_t sz = mp4ab::be32(p + i);
                        if (sz < 8 || i + sz > n) {
                            // box grande demais p/ o tail — tenta parse no tail inteiro a partir de i
                            sz = (uint32_t)std::min((size_t)0x7fffffff, n - i);
                        }
                        std::vector<uint8_t> syn;
                        const uint8_t ftyp[] = {
                            0,0,0,24, 'f','t','y','p', 'i','s','o','m',0,0,2,0, 'i','s','o','m','m','p','4','1'
                        };
                        syn.insert(syn.end(), ftyp, ftyp+24);
                        syn.insert(syn.end(), p + i, p + i + std::min((size_t)sz, n - i));
                        if (tryParseBuf(syn.data(), syn.size(), tr, perr)) {
                            parsed = true;
                            break;
                        }
                    }
                }
                // ultima tentativa: parse no tail cru (alguns arquivos tem ftyp+moov so no fim — raro)
                if (!parsed) tryParseBuf((const uint8_t *)tail.data(), tail.size(), tr, perr), parsed = !tr.samples.empty() && !tr.extradata.empty();
            }
        }

        if (!parsed) {
            // diagnostico util
            std::string det = "head[" + scanH + "]";
            if (!scanT.empty()) det += " tail[" + scanT + "]";
            if (perr.size()) det += " | " + perr;
            // mensagens claras
            if (scanH.find("HEVC") != std::string::npos || scanT.find("HEVC") != std::string::npos)
                err_ = "video e HEVC (h.265), nao H.264. Decoder nativo so avc1 por enquanto. " + det;
            else if (scanH.find("moov") == std::string::npos && scanT.find("moov") == std::string::npos)
                err_ = "MP4 sem atom moov no inicio/fim (talvez fMP4/fragmentado). " + det;
            else
                err_ = "MP4 sem trilha avc1/avcC. " + det;
            return false;
        }

        int nalLen = 4;
        if (!mp4ab::avcCToAnnexB(tr.extradata, spsPps_, nalLen)) {
            err_ = "avcC invalido";
            return false;
        }
        nalLen_ = nalLen;
        samples_ = std::move(tr.samples);
        width_ = tr.width;
        height_ = tr.height;
        sampleIdx_ = 0;
        total_ = total;

        int mw = width_ ? (int)width_ : 1920;
        int mh = height_ ? (int)height_ : 1080;
        if (!hw_.open(mw, mh)) {
            err_ = hw_.lastError().empty() ? "decoder open falhou" : hw_.lastError();
            return false;
        }
        hwplayer::Frame fr;
        hw_.feedAu(spsPps_.data(), spsPps_.size(), fr);
        open_ = true;
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
        if (!open_ || sampleIdx_ >= samples_.size()) return false;
        const mp4ab::Sample &s = samples_[sampleIdx_];
        if (s.size == 0 || s.size > 4u * 1024u * 1024u) { sampleIdx_++; return false; }
        std::string raw;
        if (!fetch_((uint64_t)s.off, (uint64_t)s.off + s.size - 1, raw) || raw.size() < s.size) {
            err_ = "Range sample falhou";
            return false;
        }
        std::vector<uint8_t> au;
        if (!mp4ab::sampleToAnnexB((const uint8_t *)raw.data(), raw.size(), nalLen_, au)) {
            sampleIdx_++;
            return false;
        }
        raw.clear();
        sampleIdx_++;
        return hw_.feedAu(au.data(), au.size(), out);
    }

    void close() {
        hw_.close();
        open_ = false;
        samples_.clear();
        spsPps_.clear();
        fetch_ = nullptr;
        err_.clear();
        sampleIdx_ = 0;
    }

    bool isOpen() const { return open_; }
    const std::string &lastError() const { return err_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    size_t sampleCount() const { return samples_.size(); }
    size_t sampleIndex() const { return sampleIdx_; }

private:
    bool open_ = false;
    std::string err_, url_;
    FetchRange fetch_;
    hwplayer::Player hw_;
    std::vector<mp4ab::Sample> samples_;
    std::vector<uint8_t> spsPps_;
    int nalLen_ = 4;
    size_t sampleIdx_ = 0;
    uint32_t width_ = 0, height_ = 0;
    uint64_t total_ = 0;
};

} // namespace nativeplayer
