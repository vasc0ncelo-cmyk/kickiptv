#pragma once
// Demux MP4 muito simples: 1 trilha avc1 + avcC → stream Annex-B (start codes).
// Serve para muitos VOD progressivos. Nao cobre fragmentado (fMP4/HLS CMAF), HEVC, nem multi-trilha complexa.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <utility>

namespace mp4ab {

inline uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t be64(const uint8_t *p) {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

struct Sample {
    size_t off = 0;
    size_t size = 0;
    uint64_t pts = 0;
    bool sync = false;
};

struct Track {
    std::vector<uint8_t> extradata; // avcC
    std::vector<Sample> samples;
    uint32_t timescale = 1;
    uint32_t width = 0, height = 0;
};

inline void appendStartCode(std::vector<uint8_t> &o) {
    o.push_back(0); o.push_back(0); o.push_back(0); o.push_back(1);
}

// avcC → SPS/PPS em Annex-B
inline bool avcCToAnnexB(const std::vector<uint8_t> &avcC, std::vector<uint8_t> &out, int &nalLenSize) {
    out.clear();
    if (avcC.size() < 7) return false;
    nalLenSize = (avcC[4] & 3) + 1;
    size_t i = 5;
    int nSps = avcC[i++] & 0x1f;
    for (int s = 0; s < nSps; s++) {
        if (i + 2 > avcC.size()) return false;
        int n = (avcC[i] << 8) | avcC[i + 1]; i += 2;
        if (i + n > avcC.size()) return false;
        appendStartCode(out);
        out.insert(out.end(), avcC.begin() + i, avcC.begin() + i + n);
        i += n;
    }
    if (i >= avcC.size()) return true;
    int nPps = avcC[i++];
    for (int s = 0; s < nPps; s++) {
        if (i + 2 > avcC.size()) return false;
        int n = (avcC[i] << 8) | avcC[i + 1]; i += 2;
        if (i + n > avcC.size()) return false;
        appendStartCode(out);
        out.insert(out.end(), avcC.begin() + i, avcC.begin() + i + n);
        i += n;
    }
    return true;
}

// length-prefixed NALs → Annex-B
inline bool sampleToAnnexB(const uint8_t *data, size_t size, int nalLenSize, std::vector<uint8_t> &out) {
    out.clear();
    size_t i = 0;
    while (i + nalLenSize <= size) {
        uint32_t n = 0;
        for (int k = 0; k < nalLenSize; k++) n = (n << 8) | data[i + k];
        i += nalLenSize;
        if (n == 0 || i + n > size) break;
        appendStartCode(out);
        out.insert(out.end(), data + i, data + i + n);
        i += n;
    }
    return !out.empty();
}

struct Box {
    uint32_t type = 0;
    size_t start = 0; // offset of type field parent content
    size_t data = 0;
    size_t end = 0;
};

inline bool walk(const uint8_t *buf, size_t size, size_t from, size_t to,
                 const std::function<bool(uint32_t type, size_t data, size_t end)> &fn) {
    size_t p = from;
    while (p + 8 <= to && p + 8 <= size) {
        uint32_t sz = be32(buf + p);
        uint32_t ty = be32(buf + p + 4);
        size_t hdr = 8;
        if (sz == 1) {
            if (p + 16 > size) break;
            sz = (uint32_t)be64(buf + p + 8); // truncate ok for <4GB
            hdr = 16;
        } else if (sz == 0) sz = (uint32_t)(to - p);
        if (sz < hdr || p + sz > size) break;
        size_t data = p + hdr;
        size_t end = p + sz;
        if (!fn(ty, data, end)) return false;
        p = end;
    }
    return true;
}

inline uint32_t fourcc(const char *s) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

// Parseador incompleto mas util: pega stsd avcC + stsz + stco/co64 + stss + stts (pts simples)
inline bool parse(const uint8_t *buf, size_t size, Track &tr, std::string &err) {
    tr = Track();
    const uint32_t T_moov = fourcc("moov");
    const uint32_t T_trak = fourcc("trak");
    const uint32_t T_mdia = fourcc("mdia");
    const uint32_t T_minf = fourcc("minf");
    const uint32_t T_stbl = fourcc("stbl");
    const uint32_t T_stsd = fourcc("stsd");
    const uint32_t T_stsz = fourcc("stsz");
    const uint32_t T_stco = fourcc("stco");
    const uint32_t T_co64 = fourcc("co64");
    const uint32_t T_stss = fourcc("stss");
    const uint32_t T_stts = fourcc("stts");
    const uint32_t T_mdhd = fourcc("mdhd");
    const uint32_t T_avc1 = fourcc("avc1");
    const uint32_t T_avcC = fourcc("avcC");

    std::vector<uint32_t> sizes;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> syncs;
    std::vector<std::pair<uint32_t,uint32_t>> stts; // count, delta
    bool foundAvc = false;

    auto parseStsd = [&](size_t data, size_t end) {
        if (data + 8 > end) return;
        size_t p = data + 8; // version+flags+entry_count skip roughly
        // stsd: 4 ver/flags + 4 count
        if (data + 8 > end) return;
        uint32_t count = be32(buf + data + 4);
        p = data + 8;
        for (uint32_t e = 0; e < count && p + 8 <= end; e++) {
            uint32_t esz = be32(buf + p);
            uint32_t ety = be32(buf + p + 4);
            if (esz < 8 || p + esz > end) break;
            if (ety == T_avc1 && esz > 86) {
                // visual sample entry: skip 78 bytes from start of box to extensions roughly
                // avc1 structure: size(4)+type(4)+reserved(6)+data_ref(2)+pre_defined+... width at +32 from type?
                // From start of avc1 box: +32 width, +34 height (after 8 hdr? ISO: after 8 byte box hdr + 6 res + 2 + 16 + 2+2...)
                // Safer: search avcC child
                walk(buf, size, p + 8, p + esz, [&](uint32_t ty, size_t d, size_t en) {
                    if (ty == T_avcC) {
                        tr.extradata.assign(buf + d, buf + en);
                        foundAvc = true;
                    }
                    return true;
                });
                // width/height often at offset 32/34 from box start (including header)
                if (p + 40 <= end) {
                    tr.width = (buf[p + 32] << 8) | buf[p + 33];
                    tr.height = (buf[p + 34] << 8) | buf[p + 35];
                }
            }
            p += esz;
        }
    };

    bool ok = walk(buf, size, 0, size, [&](uint32_t ty, size_t data, size_t end) {
        if (ty != T_moov) return true;
        walk(buf, size, data, end, [&](uint32_t t2, size_t d2, size_t e2) {
            if (t2 != T_trak) return true;
            walk(buf, size, d2, e2, [&](uint32_t t3, size_t d3, size_t e3) {
                if (t3 != T_mdia) return true;
                walk(buf, size, d3, e3, [&](uint32_t t4, size_t d4, size_t e4) {
                    if (t4 == T_mdhd && e4 - d4 >= 20) {
                        uint8_t ver = buf[d4];
                        if (ver == 0 && e4 - d4 >= 20) tr.timescale = be32(buf + d4 + 12);
                        else if (ver == 1 && e4 - d4 >= 32) tr.timescale = be32(buf + d4 + 20);
                        if (tr.timescale == 0) tr.timescale = 1;
                    }
                    if (t4 != T_minf) return true;
                    walk(buf, size, d4, e4, [&](uint32_t t5, size_t d5, size_t e5) {
                        if (t5 != T_stbl) return true;
                        walk(buf, size, d5, e5, [&](uint32_t t6, size_t d6, size_t e6) {
                            if (t6 == T_stsd) parseStsd(d6, e6);
                            if (t6 == T_stsz && e6 - d6 >= 12) {
                                uint32_t fixed = be32(buf + d6 + 4);
                                uint32_t cnt = be32(buf + d6 + 8);
                                sizes.clear();
                                if (fixed != 0) {
                                    sizes.resize(cnt, fixed);
                                } else {
                                    if (d6 + 12 + cnt * 4 > e6) return true;
                                    for (uint32_t i = 0; i < cnt; i++) sizes.push_back(be32(buf + d6 + 12 + i * 4));
                                }
                            }
                            if (t6 == T_stco && e6 - d6 >= 8) {
                                uint32_t cnt = be32(buf + d6 + 4);
                                offsets.clear();
                                if (d6 + 8 + cnt * 4 > e6) return true;
                                for (uint32_t i = 0; i < cnt; i++) offsets.push_back(be32(buf + d6 + 8 + i * 4));
                            }
                            if (t6 == T_co64 && e6 - d6 >= 8) {
                                uint32_t cnt = be32(buf + d6 + 4);
                                offsets.clear();
                                if (d6 + 8 + cnt * 8 > e6) return true;
                                for (uint32_t i = 0; i < cnt; i++) offsets.push_back(be64(buf + d6 + 8 + i * 8));
                            }
                            if (t6 == T_stss && e6 - d6 >= 8) {
                                uint32_t cnt = be32(buf + d6 + 4);
                                syncs.clear();
                                if (d6 + 8 + cnt * 4 > e6) return true;
                                for (uint32_t i = 0; i < cnt; i++) syncs.push_back(be32(buf + d6 + 8 + i * 4));
                            }
                            if (t6 == T_stts && e6 - d6 >= 8) {
                                uint32_t cnt = be32(buf + d6 + 4);
                                stts.clear();
                                if (d6 + 8 + cnt * 8 > e6) return true;
                                for (uint32_t i = 0; i < cnt; i++) {
                                    uint32_t c = be32(buf + d6 + 8 + i * 8);
                                    uint32_t dlt = be32(buf + d6 + 12 + i * 8);
                                    stts.push_back({c, dlt});
                                }
                            }
                            return true;
                        });
                        return true;
                    });
                    return true;
                });
                return true;
            });
            return true;
        });
        return true;
    });

    if (!foundAvc || tr.extradata.empty()) { err = "MP4 sem trilha avc1/avcC"; return false; }
    if (sizes.empty() || offsets.empty()) { err = "MP4 sem stsz/stco"; return false; }
    // stco costuma ter 1 offset por chunk; stsz por sample — se counts diferem, assume 1 sample/chunk
    size_t n = sizes.size();
    if (offsets.size() != n) {
        // fallback: se 1 chunk, nao suportamos bem
        if (offsets.size() == 1 && n > 1) {
            // cumulative sizes from single chunk start
            uint64_t off = offsets[0];
            tr.samples.resize(n);
            uint64_t pts = 0;
            size_t stts_i = 0, stts_left = stts.empty() ? 0 : stts[0].first;
            uint32_t delta = stts.empty() ? 3000 : stts[0].second;
            for (size_t i = 0; i < n; i++) {
                tr.samples[i].off = (size_t)off;
                tr.samples[i].size = sizes[i];
                tr.samples[i].pts = pts;
                off += sizes[i];
                if (!stts.empty()) {
                    pts += delta;
                    if (stts_left) stts_left--;
                    if (stts_left == 0 && stts_i + 1 < stts.size()) {
                        stts_i++;
                        stts_left = stts[stts_i].first;
                        delta = stts[stts_i].second;
                    }
                } else pts += 3000;
            }
        } else {
            err = "stco/stsz incompatível (MP4 complexo)";
            return false;
        }
    } else {
        tr.samples.resize(n);
        uint64_t pts = 0;
        size_t stts_i = 0, stts_left = stts.empty() ? 0 : stts[0].first;
        uint32_t delta = stts.empty() ? 3000 : stts[0].second;
        for (size_t i = 0; i < n; i++) {
            tr.samples[i].off = (size_t)offsets[i];
            tr.samples[i].size = sizes[i];
            tr.samples[i].pts = pts;
            if (!stts.empty()) {
                pts += delta;
                if (stts_left) stts_left--;
                if (stts_left == 0 && stts_i + 1 < stts.size()) {
                    stts_i++;
                    stts_left = stts[stts_i].first;
                    delta = stts[stts_i].second;
                }
            } else pts += 3000;
        }
    }
    for (uint32_t s : syncs) {
        if (s >= 1 && s <= tr.samples.size()) tr.samples[s - 1].sync = true;
    }
    return true;
}

} // namespace mp4ab
