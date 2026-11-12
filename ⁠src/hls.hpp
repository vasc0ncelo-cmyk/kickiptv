#pragma once
// Parser HLS minimo (independente de plataforma, testavel no PC).
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

struct HlsVariant { std::string uri; int bandwidth = 0, width = 0, height = 0; double fps = 0; };
struct HlsSegment { std::string uri; double dur = 0; unsigned long long seq = 0; };
struct HlsMedia {
    std::vector<HlsSegment> segs;
    double target = 2;
    bool endList = false, hasMap = false;
};

inline std::string hlsTrim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Resolve uma URI (absoluta, "//host", "/caminho" ou relativa) contra a URL base.
inline std::string hlsResolve(const std::string &base, const std::string &ref) {
    if (ref.compare(0, 7, "http://") == 0 || ref.compare(0, 8, "https://") == 0) return ref;
    size_t se = base.find("://");
    if (se == std::string::npos) return ref;
    std::string scheme = base.substr(0, se + 1);                 // "https:"
    if (ref.compare(0, 2, "//") == 0) return scheme + ref;
    size_t he = base.find('/', se + 3);
    std::string origin = he == std::string::npos ? base : base.substr(0, he);
    if (!ref.empty() && ref[0] == '/') return origin + ref;
    std::string b = base.substr(0, base.find_first_of("?#"));
    size_t ls = b.rfind('/');
    std::string dir = (ls == std::string::npos || ls < se + 3) ? b + "/" : b.substr(0, ls + 1);
    return dir + ref;
}

inline bool hlsIsMaster(const std::string &t) { return t.find("#EXT-X-STREAM-INF") != std::string::npos; }

inline std::string hlsAttr(const std::string &line, const std::string &key) {
    size_t p = 0;
    for (;;) {
        p = line.find(key + "=", p);
        if (p == std::string::npos) return "";
        if (p == 0 || line[p - 1] == ':' || line[p - 1] == ',') break;
        p += key.size();
    }
    p += key.size() + 1;
    if (p < line.size() && line[p] == '"') { size_t e = line.find('"', p + 1); return line.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1); }
    size_t e = line.find(',', p);
    return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

inline std::vector<HlsVariant> hlsParseMaster(const std::string &text) {
    std::vector<HlsVariant> v;
    std::istringstream in(text);
    std::string line; bool pending = false; HlsVariant cur;
    while (std::getline(in, line)) {
        line = hlsTrim(line);
        if (line.empty()) continue;
        if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0) {
            cur = HlsVariant();
            cur.bandwidth = atoi(hlsAttr(line, "BANDWIDTH").c_str());
            std::string r = hlsAttr(line, "RESOLUTION");
            size_t x = r.find('x');
            if (x != std::string::npos) { cur.width = atoi(r.c_str()); cur.height = atoi(r.c_str() + x + 1); }
            cur.fps = atof(hlsAttr(line, "FRAME-RATE").c_str());   // 0 = nao informado
            pending = true;
        } else if (line[0] != '#' && pending) { cur.uri = line; v.push_back(cur); pending = false; }
    }
    return v;
}

// Escolhe a melhor variante com altura <= maxHeight (ignora so-audio). Retorna -1 se vazio.
// maxFps > 0: prefere variantes com no maximo esse FPS (ex.: 30). O PS4 desenha o video por software (CPU),
// entao 60 fps custa o dobro de 30 fps. Se nenhuma variante cumprir o limite, ele e ignorado.
inline int hlsPickVariant(const std::vector<HlsVariant> &v, int maxHeight, int maxFps = 0) {
    bool anyRes = false;
    for (auto &x : v) if (x.height > 0) anyRes = true;
    for (int pass = 0; pass < 2; pass++) {
        bool useFps = pass == 0 && maxFps > 0;                    // 1a passada: com limite de fps; 2a: sem limite
        if (pass == 1 && maxFps <= 0) break;                      // sem limite, a 1a passada ja foi a completa
        int best = -1, lowest = -1;
        for (int i = 0; i < (int)v.size(); i++) {
            if (anyRes && v[i].height == 0) continue;             // so audio
            if (useFps && v[i].fps > maxFps + 0.5) continue;
            if (lowest < 0 || v[i].bandwidth < v[lowest].bandwidth) lowest = i;
            bool ok = anyRes ? v[i].height <= maxHeight : v[i].bandwidth <= 4500000;
            if (ok && (best < 0 || v[i].bandwidth > v[best].bandwidth)) best = i;
        }
        if (best >= 0) return best;
        if (!useFps && lowest >= 0) return lowest;                // nada cabe: a mais leve
    }
    return -1;
}

inline HlsMedia hlsParseMedia(const std::string &text) {
    HlsMedia m;
    std::istringstream in(text);
    std::string line; unsigned long long seq = 0; double dur = 0;
    while (std::getline(in, line)) {
        line = hlsTrim(line);
        if (line.empty()) continue;
        if (line.compare(0, 22, "#EXT-X-MEDIA-SEQUENCE:") == 0) seq = strtoull(line.c_str() + 22, nullptr, 10);
        else if (line.compare(0, 22, "#EXT-X-TARGETDURATION:") == 0) m.target = atof(line.c_str() + 22);
        else if (line.compare(0, 8, "#EXTINF:") == 0) dur = atof(line.c_str() + 8);
        else if (line.compare(0, 11, "#EXT-X-MAP:") == 0) m.hasMap = true;
        else if (line.compare(0, 14, "#EXT-X-ENDLIST") == 0) m.endList = true;
        else if (line[0] != '#') { HlsSegment s; s.uri = line; s.dur = dur; s.seq = seq++; m.segs.push_back(s); dur = 0; }
    }
    return m;
}
