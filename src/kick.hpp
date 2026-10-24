#pragma once
#include <cctype>
#include <string>
#include <vector>

// Normaliza "@Streamer" -> "streamer". Retorna "" se invalido.
inline std::string kickSlug(std::string in) {
    std::string s;
    for (char c : in) {
        if (c == '@' || std::isspace((unsigned char)c)) continue;
        c = (char)std::tolower((unsigned char)c);
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-')) return "";
        s += c;
    }
    return s.size() > 25 ? "" : s;
}

// API NAO OFICIAL da Kick (pode mudar ou bloquear via Cloudflare).
inline std::string kickApiUrl(const std::string &slug) { return "https://kick.com/api/v2/channels/" + slug; }
// Enderecos para tentar, em ordem (o 2o e a versao antiga da mesma API: as vezes passa quando o outro e recusado).
inline std::vector<std::string> kickApiUrls(const std::string &slug) {
    return {"https://kick.com/api/v2/channels/" + slug, "https://kick.com/api/v1/channels/" + slug};
}

// Posicao do valor de uma chave JSON (logo depois dos ":" e dos espacos). npos se nao achou. Aceita {"a":1} e { "a" : 1 }.
inline size_t jsonValuePos(const std::string &json, const std::string &key) {
    std::string k = "\"" + key + "\"";
    for (size_t p = json.find(k); p != std::string::npos; p = json.find(k, p + 1)) {
        size_t q = p + k.size();
        while (q < json.size() && std::isspace((unsigned char)json[q])) q++;
        if (q >= json.size() || json[q] != ':') continue;
        q++;
        while (q < json.size() && std::isspace((unsigned char)json[q])) q++;
        return q;
    }
    return std::string::npos;
}

// Extrai o valor string de uma chave JSON (suficiente para "playback_url").
// Trata \/ e \uXXXX (ex.: \u0026 = &).
inline std::string jsonString(const std::string &json, const std::string &key) {
    size_t p = jsonValuePos(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '"') return "";
    p++;
    std::string out;
    for (; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            p++;
            char c = json[p];
            if (c == 'u' && p + 4 < json.size()) {
                unsigned v = 0; bool ok = true;
                for (int i = 1; i <= 4; i++) {
                    char h = json[p + i]; v <<= 4;
                    if (h >= '0' && h <= '9') v |= h - '0';
                    else if (h >= 'a' && h <= 'f') v |= h - 'a' + 10;
                    else if (h >= 'A' && h <= 'F') v |= h - 'A' + 10;
                    else ok = false;
                }
                if (ok) { out += v < 0x80 ? (char)v : '?'; p += 4; continue; }
            }
            if (c == '/') { out += '/'; continue; }
            if (c == 'n' || c == 'r' || c == 't') continue;
            out += c;
            continue;
        }
        out += json[p];
    }
    return out;
}

// Canal offline: "livestream":null
inline bool kickIsLive(const std::string &json) {
    size_t p = jsonValuePos(json, "livestream");
    return p != std::string::npos && p < json.size() && json[p] == '{';
}
