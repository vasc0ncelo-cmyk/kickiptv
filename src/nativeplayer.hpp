#pragma once
// Player nativo streaming: NAO carrega o MP4 inteiro na RAM.
// 1) Baixa so o indice (moov) via Range
// 2) A cada frame, baixa 1 sample via Range e manda pro sceVideodec
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

// fetch(from,to inclusive) -> body; return true se ok
using FetchRange = std::function<bool(uint64_t from, uint64_t to, std::string &body)>;

class Session {
public:
    // Abre URL HTTP: so indice na RAM; video sob demanda.
    bool openUrl(const std::string &url, uint64_t totalHint, FetchRange fetch) {
        close();
        fetch_ = std::move(fetch);
        url_ = url;
        if (!boot()) { err_ = "sceVideodec indisponivel neste firmware"; return false; }
        if (!fetch_) { err_ = "sem fetch"; return false; }

        uint64_t total = totalHint;
        std::string head;
        // 1) primeiros 8 MB (moov no inicio = faststart)
        uint64_t headTo = 8ull * 1024ull * 1024ull - 1;
        if (totalHint > 0 && headTo >= totalHint) headTo = totalHint - 1;
        if (!fetch_(0, headTo, head) || head.size() < 64) {
            err_ = "falha ao ler inicio do arquivo";
            return false;
        }
        if (total == 0) total = head.size(); // desconhecido

        mp4ab::Track tr;
        std::string perr;
        bool parsed = false;
        if (head.size() >= 8 && head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p') {
            if (mp4ab::parse((const uint8_t *)head.data(), head.size(), tr, perr) && !tr.samples.empty())
                parsed = true;
        }
        // 2) moov no fim: baixa ultimos 4 MB
        if (!parsed && totalHint > 8ull * 1024ull * 1024ull) {
            uint64_t tailFrom = totalHint > 4ull * 1024ull * 1024ull ? totalHint - 4ull * 1024ull * 1024ull : 0;
            std::string tail;
            if (fetch_(tailFrom, totalHint - 1, tail) && tail.size() > 64) {
                // procura 'moov' no tail e parseia a partir dai com buffer = tail
                // offsets stco sao absolutos no arquivo — parse so precisa do moov
                const uint8_t *p = (const uint8_t *)tail.data();
                size_t n = tail.size();
                for (size_t i = 0; i + 8 < n; i++) {
                    if (p[i+4]=='m'&&p[i+5]=='o'&&p[i+6]=='o'&&p[i+7]=='v') {
                        // box size before type
                        uint32_t sz = mp4ab::be32(p + i);
                        if (sz < 8) continue;
                        // monta buffer minimo: ftyp fake + moov
                        std::vector<uint8_t> buf;
                        // reparse: walk from i as top-level by temporarily using parse on
                        // a buffer that starts at ftyp of head + moov from tail
                        std::vector<uint8_t> combo;
                        combo.insert(combo.end(), head.begin(), head.begin() + std::min(head.size(), (size_t)32));
                        // force re-parse of full head+tail isn't valid for offsets in mdat
                        // Better: call parse on head which may include partial mdat; for moov-at-end
                        // parse entire tail looking for trak via parse() on a synthetic file:
                        // [ftyp][moov from tail] — sample offsets still absolute in real file. OK.
                        std::vector<uint8_t> syn;
                        // minimal ftyp
                        const uint8_t ftyp[] = {
                            0,0,0,24, 'f','t','y','p', 'i','s','o','m',0,0,2,0, 'i','s','o','m','m','p','4','1'
                        };
                        syn.insert(syn.end(), ftyp, ftyp+24);
                        if (i + sz <= n) {
                            syn.insert(syn.end(), p + i, p + i + sz);
                            tr = mp4ab::Track{};
                            if (mp4ab::parse(syn.data(), syn.size(), tr, perr) && !tr.samples.empty()) {
                                parsed = true;
                            }
                        }
                        break;
                    }
                }
            }
        }
        if (!parsed) {
            err_ = perr.empty() ? "MP4 sem indice avc1 (moov)" : perr;
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
        total_ = totalHint ? totalHint : total;

        int mw = width_ ? (int)width_ : 1920;
        int mh = height_ ? (int)height_ : 1080;
        if (!hw_.open(mw, mh)) {
            err_ = hw_.lastError().empty() ? "decoder open falhou" : hw_.lastError();
            return false;
        }
        // alimenta SPS/PPS
        hwplayer::Frame fr;
        hw_.feedAu(spsPps_.data(), spsPps_.size(), fr);
        open_ = true;
        return true;
    }

    // Legado: arquivo local — le sample a sample do disco (nao carrega tudo)
    bool openFile(const std::string &path) {
        close();
        FILE *f = fopen(path.c_str(), "rb");
        if (!f) { err_ = "nao abriu: " + path; return false; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 64) { fclose(f); err_ = "arquivo pequeno"; return false; }

        // le so 8 MB do inicio
        size_t headN = (size_t)std::min(sz, 8L * 1024 * 1024);
        std::string head(headN, '\0');
        if (fread(&head[0], 1, headN, f) != headN) { fclose(f); err_ = "leitura head"; return false; }

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
        fclose(f);
        return openUrl("file://" + path, (uint64_t)sz, fetchDisk);
    }

    bool openMemory(const uint8_t *p, size_t n) {
        // so para testes pequenos (< 32 MB)
        if (n > 32u * 1024u * 1024u) { err_ = "buffer grande demais — use openUrl"; return false; }
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
