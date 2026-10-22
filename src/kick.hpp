#pragma once
#include <string>
#include <cctype>

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

// Extrai o valor string de uma chave JSON (suficiente para "playback_url").
// Trata \/ e \uXXXX (ex.: \u0026 = &).
inline std::string jsonString(const std::string &json, const std::string &key) {
    std::string k = "\"" + key + "\":\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
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
    return json.find("\"livestream\":null") == std::string::npos && json.find("\"livestream\":{") != std::string::npos;
}
