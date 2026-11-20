#pragma once
// Favoritos, historico, timer de sono e PIN — arquivo simples em /data
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <ctime>

namespace feat {

inline std::string path(const char *name) { return std::string("/data/kickiptv_") + name + ".txt"; }

inline std::vector<std::string> loadLines(const char *name) {
    std::vector<std::string> out;
    std::ifstream f(path(name));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}
inline void saveLines(const char *name, const std::vector<std::string> &lines) {
    std::ofstream f(path(name), std::ios::trunc);
    for (auto &l : lines) f << l << "\n";
}

// Favoritos: uma linha por item (url|titulo)
inline bool isFav(const std::string &url) {
    for (auto &l : loadLines("favs")) if (l.compare(0, url.size(), url) == 0 && (l.size() == url.size() || l[url.size()] == '|')) return true;
    return false;
}
inline void toggleFav(const std::string &url, const std::string &title) {
    auto lines = loadLines("favs");
    std::string key = url + "|";
    bool found = false;
    std::vector<std::string> out;
    for (auto &l : lines) {
        if (l.compare(0, key.size(), key) == 0) { found = true; continue; }
        out.push_back(l);
    }
    if (!found) out.insert(out.begin(), url + "|" + title);
    if (out.size() > 200) out.resize(200);
    saveLines("favs", out);
}
inline std::vector<std::pair<std::string,std::string>> favs() {
    std::vector<std::pair<std::string,std::string>> out;
    for (auto &l : loadLines("favs")) {
        auto p = l.find('|');
        if (p == std::string::npos) continue;
        out.emplace_back(l.substr(0, p), l.substr(p + 1));
    }
    return out;
}

// Historico recente (url|titulo|epoch)
inline void addHistory(const std::string &url, const std::string &title) {
    auto lines = loadLines("hist");
    std::string key = url + "|";
    std::vector<std::string> out;
    out.push_back(url + "|" + title + "|" + std::to_string((long)time(nullptr)));
    for (auto &l : lines) {
        if (l.compare(0, key.size(), key) == 0) continue;
        out.push_back(l);
        if (out.size() >= 50) break;
    }
    saveLines("hist", out);
}
inline std::vector<std::pair<std::string,std::string>> history() {
    std::vector<std::pair<std::string,std::string>> out;
    for (auto &l : loadLines("hist")) {
        auto p = l.find('|');
        if (p == std::string::npos) continue;
        auto p2 = l.find('|', p + 1);
        out.emplace_back(l.substr(0, p), l.substr(p + 1, p2 == std::string::npos ? std::string::npos : p2 - p - 1));
    }
    return out;
}

// Timer de sono (minutos, 0 = desligado)
inline int sleepMin() {
    auto v = loadLines("sleep");
    return v.empty() ? 0 : atoi(v[0].c_str());
}
inline void setSleepMin(int m) {
    saveLines("sleep", {std::to_string(m)});
}

// PIN (4 digitos, vazio = desligado)
inline std::string pin() {
    auto v = loadLines("pin");
    return v.empty() ? "" : v[0];
}
inline void setPin(const std::string &p) { saveLines("pin", {p}); }
inline bool pinOk(const std::string &try_) { auto p = pin(); return p.empty() || p == try_; }

} // namespace feat
