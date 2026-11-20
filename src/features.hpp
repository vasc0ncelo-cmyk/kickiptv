#pragma once
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

// Tema: 0=verde 1=azul 2=roxo
inline int theme() {
    auto v = loadLines("theme");
    return v.empty() ? 0 : atoi(v[0].c_str()) % 3;
}
inline void setTheme(int t) { saveLines("theme", {std::to_string(t % 3)}); }
inline unsigned themeColor() {
    static const unsigned c[3] = {0xFF53FC18u, 0xFF3B82F6u, 0xFFA855F7u};
    return c[theme()];
}

// Velocidade: 0=0.5 1=1.0 2=1.25 3=1.5 4=2.0
inline int speed() {
    auto v = loadLines("speed");
    return v.empty() ? 1 : atoi(v[0].c_str()) % 5;
}
inline void setSpeed(int s) { saveLines("speed", {std::to_string(s % 5)}); }
inline double speedValue() {
    static const double v[5] = {0.5, 1.0, 1.25, 1.5, 2.0};
    return v[speed()];
}
inline const char *speedName() {
    static const char *n[5] = {"0.5x", "1x", "1.25x", "1.5x", "2x"};
    return n[speed()];
}

// PIN
inline std::string pin() {
    auto v = loadLines("pin");
    return v.empty() ? "" : v[0];
}
inline void setPin(const std::string &p) { saveLines("pin", {p}); }
inline bool pinOk(const std::string &try_) { auto p = pin(); return p.empty() || p == try_; }

// Estatisticas simples
inline void addWatchTime(int sec) {
    auto v = loadLines("stats");
    long cur = v.empty() ? 0 : atol(v[0].c_str());
    saveLines("stats", {std::to_string(cur + sec)});
}
inline long watchTime() {
    auto v = loadLines("stats");
    return v.empty() ? 0 : atol(v[0].c_str());
}

} // namespace feat
