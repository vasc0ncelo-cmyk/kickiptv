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
inline std::string jsonString(const std::string &json, const std::string &key) {
    std::string k = "\"" + key + "\":\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    std::string out;
    for (; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) { p++; if (json[p] == '/') { out += '/'; continue; } }
        out += json[p];
    }
    return out;
}

// Canal offline: "livestream":null
inline bool kickIsLive(const std::string &json) {
    return json.find("\"livestream\":null") == std::string::npos && json.find("\"livestream\":{") != std::string::npos;
}
