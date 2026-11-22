// WatchProgress.cpp
#include "WatchProgress.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace kick {

// ---------------------------------------------------------------- JSON mini
namespace {

struct Cur {
    const char* p;
    const char* e;
};

void SkipWs(Cur& c) {
    while (c.p < c.e && (*c.p == ' ' || *c.p == '\n' || *c.p == '\r' || *c.p == '\t')) ++c.p;
}

bool ParseString(Cur& c, std::string& out) {
    SkipWs(c);
    if (c.p >= c.e || *c.p != '"') return false;
    ++c.p;
    out.clear();
    while (c.p < c.e && *c.p != '"') {
        if (*c.p == '\\' && c.p + 1 < c.e) {
            ++c.p;
            switch (*c.p) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u':  // nao usamos \u ao gravar; apenas pula 4 hex
                    if (c.p + 4 < c.e) c.p += 4;
                    out += '?';
                    break;
                default: out += *c.p; break;
            }
        } else {
            out += *c.p;
        }
        ++c.p;
    }
    if (c.p >= c.e) return false;
    ++c.p;  // fecha aspas
    return true;
}

bool ParseNumber(Cur& c, int64_t& v) {
    SkipWs(c);
    if (c.p >= c.e) return false;
    char buf[40];
    size_t n = 0;
    while (c.p < c.e && n < sizeof(buf) - 1 &&
           ((*c.p >= '0' && *c.p <= '9') || *c.p == '-' || *c.p == '+' || *c.p == '.' ||
            *c.p == 'e' || *c.p == 'E')) {
        buf[n++] = *c.p++;
    }
    if (n == 0) return false;
    buf[n] = 0;
    v = static_cast<int64_t>(strtod(buf, nullptr));
    return true;
}

// Pula qualquer valor JSON (string, numero, literal, objeto/array aninhado).
bool SkipValue(Cur& c) {
    SkipWs(c);
    if (c.p >= c.e) return false;
    if (*c.p == '"') {
        std::string tmp;
        return ParseString(c, tmp);
    }
    if (*c.p == '{' || *c.p == '[') {
        int depth = 0;
        while (c.p < c.e) {
            if (*c.p == '"') {
                std::string tmp;
                if (!ParseString(c, tmp)) return false;
                continue;
            }
            if (*c.p == '{' || *c.p == '[') ++depth;
            if (*c.p == '}' || *c.p == ']') --depth;
            ++c.p;
            if (depth == 0) return true;
        }
        return false;
    }
    while (c.p < c.e && *c.p != ',' && *c.p != '}' && *c.p != ']') ++c.p;
    return true;
}

bool ParseItem(Cur& c, ProgressEntry& it) {
    SkipWs(c);
    if (c.p >= c.e || *c.p != '{') return false;
    ++c.p;
    for (;;) {
        SkipWs(c);
        if (c.p >= c.e) return false;
        if (*c.p == '}') { ++c.p; return true; }
        if (*c.p == ',') { ++c.p; continue; }
        std::string key;
        if (!ParseString(c, key)) return false;
        SkipWs(c);
        if (c.p >= c.e || *c.p != ':') return false;
        ++c.p;
        if (key == "id") { if (!ParseString(c, it.id)) return false; }
        else if (key == "pos") { if (!ParseNumber(c, it.positionSec)) return false; }
        else if (key == "dur") { if (!ParseNumber(c, it.durationSec)) return false; }
        else if (key == "ts")  { if (!ParseNumber(c, it.updatedAt)) return false; }
        else if (key == "t")   { if (!ParseString(c, it.title)) return false; }
        else if (key == "sr")  { if (!ParseString(c, it.series)) return false; }
        else if (key == "lg")  { if (!ParseString(c, it.logo)) return false; }
        else if (key == "se")  { int64_t v = 0; if (!ParseNumber(c, v)) return false; it.season = (int)v; }
        else if (key == "ep")  { int64_t v = 0; if (!ParseNumber(c, v)) return false; it.episode = (int)v; }
        else if (key == "ie")  { int64_t v = 0; if (!ParseNumber(c, v)) return false; it.isEpisode = v != 0; }
        else if (!SkipValue(c)) return false;
    }
}

bool ParseItems(Cur& c, std::vector<ProgressEntry>& out) {
    SkipWs(c);
    if (c.p >= c.e || *c.p != '[') return false;
    ++c.p;
    for (;;) {
        SkipWs(c);
        if (c.p >= c.e) return false;
        if (*c.p == ']') { ++c.p; return true; }
        if (*c.p == ',') { ++c.p; continue; }
        ProgressEntry it;
        if (!ParseItem(c, it)) return false;
        if (!it.id.empty()) out.push_back(std::move(it));
    }
}

bool ParseRoot(const std::string& text, std::vector<ProgressEntry>& out) {
    Cur c{text.c_str(), text.c_str() + text.size()};
    SkipWs(c);
    if (c.p >= c.e || *c.p != '{') return false;
    ++c.p;
    for (;;) {
        SkipWs(c);
        if (c.p >= c.e) return false;
        if (*c.p == '}') return true;
        if (*c.p == ',') { ++c.p; continue; }
        std::string key;
        if (!ParseString(c, key)) return false;
        SkipWs(c);
        if (c.p >= c.e || *c.p != ':') return false;
        ++c.p;
        if (key == "items") { if (!ParseItems(c, out)) return false; }
        else if (!SkipValue(c)) return false;
    }
}

void AppendEscaped(std::string& s, const std::string& in) {
    s += '"';
    for (unsigned char ch : in) {
        switch (ch) {
            case '"':  s += "\\\""; break;
            case '\\': s += "\\\\"; break;
            case '\n': s += "\\n"; break;
            case '\r': s += "\\r"; break;
            case '\t': s += "\\t"; break;
            default:
                if (ch < 0x20) s += '?';
                else s += static_cast<char>(ch);
        }
    }
    s += '"';
}

void EnsureParentDir(const std::string& file) {
    size_t slash = file.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return;
    std::string dir = file.substr(0, slash);
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            std::string part = dir.substr(0, i);
            mkdir(part.c_str(), 0777);  // ignora erro (ja existe / sem permissao)
        }
    }
}

}  // namespace

// ------------------------------------------------------------ WatchProgress

WatchProgress& WatchProgress::Instance() {
    static WatchProgress inst;
    return inst;
}

void WatchProgress::Init(const std::string& filePath) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path_ = filePath;
        entries_.clear();
        dirty_ = false;
        lastSave_ = std::chrono::steady_clock::now();
    }
    EnsureParentDir(filePath);
    Load();
}

void WatchProgress::Load() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path = path_;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;  // primeira execucao: sem arquivo

    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if (text.size() > (1u << 20)) break;  // sanidade: > 1 MB = corrompido
    }
    fclose(f);

    std::vector<ProgressEntry> items;
    if (text.size() > (1u << 20) || !ParseRoot(text, items)) {
        // Arquivo corrompido: guarda copia e recomeca vazio.
        std::string bad = path + ".bad";
        rename(path.c_str(), bad.c_str());
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& it : items) {
        if (it.positionSec < 0) continue;
        entries_[it.id] = std::move(it);
    }
    Prune();
}

bool WatchProgress::IsNearEnd(int64_t pos, int64_t dur) const {
    if (dur <= 0) return false;
    if (dur - pos <= kEndMarginSec) return true;
    return static_cast<double>(pos) >= static_cast<double>(dur) * kEndFraction;
}

bool WatchProgress::Get(const std::string& id, ProgressEntry& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (it->second.positionSec < kMinResumeSec) return false;
    if (IsNearEnd(it->second.positionSec, it->second.durationSec)) return false;
    out = it->second;
    return true;
}

bool WatchProgress::GetResumePoint(const std::string& id, int64_t& outSec) const {
    ProgressEntry e;
    if (!Get(id, e)) return false;
    outSec = e.positionSec;
    return true;
}

void WatchProgress::Update(const std::string& id, int64_t positionSec, int64_t durationSec,
                           bool force, const ProgressMeta* meta) {
    if (id.empty() || positionSec < 0) return;

    bool shouldSave = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (path_.empty()) return;

        if (IsNearEnd(positionSec, durationSec)) {
            // Terminou: remove da lista de "continuar assistindo".
            if (entries_.erase(id) > 0) dirty_ = true;
        } else if (positionSec >= kMinResumeSec) {
            ProgressEntry& e = entries_[id];
            if (e.positionSec != positionSec || e.durationSec != durationSec ||
                e.id.empty()) {
                e.id = id;
                e.positionSec = positionSec;
                e.durationSec = durationSec;
                e.updatedAt = static_cast<int64_t>(time(nullptr));
                dirty_ = true;
            }
            if (meta) {   // titulo/serie/capa para a lista "Assistido recentemente"
                if (e.title != meta->title || e.series != meta->series || e.logo != meta->logo || e.season != meta->season ||
                    e.episode != meta->episode || e.isEpisode != meta->isEpisode) {
                    e.title = meta->title; e.series = meta->series; e.logo = meta->logo;
                    e.season = meta->season; e.episode = meta->episode; e.isEpisode = meta->isEpisode;
                    dirty_ = true;
                }
            }
            if (entries_.size() > kMaxEntries) Prune();
        }

        if (dirty_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - lastSave_).count();
            shouldSave = force || elapsed >= kSaveIntervalSec;
        }
    }
    if (shouldSave) Flush();
}

std::vector<ProgressEntry> WatchProgress::List(size_t maxN) const {
    std::vector<ProgressEntry> out;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : entries_) {
            const ProgressEntry& e = kv.second;
            if (e.positionSec < kMinResumeSec || IsNearEnd(e.positionSec, e.durationSec)) continue;
            if (e.title.empty()) continue;   // sem titulo (gravado por versao antiga): nao da para listar
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), [](const ProgressEntry& a, const ProgressEntry& b) { return a.updatedAt > b.updatedAt; });
    if (out.size() > maxN) out.resize(maxN);
    return out;
}

void WatchProgress::Clear(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (entries_.erase(id) == 0) return;
        dirty_ = true;
    }
    Flush();
}

void WatchProgress::Flush() {
    std::string data;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!dirty_ || path_.empty()) return;
        data = Serialize();
        dirty_ = false;
        lastSave_ = std::chrono::steady_clock::now();
    }
    if (!WriteAtomic(data)) {
        std::lock_guard<std::mutex> lk(mutex_);
        dirty_ = true;  // tenta de novo no proximo ciclo
    }
}

void WatchProgress::Prune() {
    if (entries_.size() <= kMaxEntries) return;
    std::vector<std::pair<int64_t, std::string>> order;
    order.reserve(entries_.size());
    for (auto& kv : entries_) order.emplace_back(kv.second.updatedAt, kv.first);
    std::sort(order.begin(), order.end());  // mais antigos primeiro
    size_t remove = entries_.size() - kMaxEntries;
    for (size_t i = 0; i < remove; ++i) entries_.erase(order[i].second);
    dirty_ = true;
}

std::string WatchProgress::Serialize() const {
    std::string s;
    s.reserve(entries_.size() * 80 + 32);
    s += "{\"version\":1,\"items\":[";
    bool first = true;
    char num[96];
    for (auto& kv : entries_) {
        const ProgressEntry& e = kv.second;
        if (!first) s += ',';
        first = false;
        s += "{\"id\":";
        AppendEscaped(s, e.id);
        snprintf(num, sizeof(num), ",\"pos\":%lld,\"dur\":%lld,\"ts\":%lld,\"se\":%d,\"ep\":%d,\"ie\":%d",
                 (long long)e.positionSec, (long long)e.durationSec, (long long)e.updatedAt, e.season, e.episode, e.isEpisode ? 1 : 0);
        s += num;
        s += ",\"t\":";  AppendEscaped(s, e.title);
        s += ",\"sr\":"; AppendEscaped(s, e.series);
        s += ",\"lg\":"; AppendEscaped(s, e.logo);
        s += '}';
    }
    s += "]}";
    return s;
}

// Escreve em arquivo temporario + fsync + rename: se o PS4 desligar/travar no
// meio, o arquivo antigo continua integro (nunca fica meio escrito).
bool WatchProgress::WriteAtomic(const std::string& data) {
    std::lock_guard<std::mutex> flk(fileMutex_);
    std::string path;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path = path_;
    }
    std::string tmp = path + ".tmp";

    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = ok && fflush(f) == 0;
    if (ok) fsync(fileno(f));
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string WatchProgress::FormatTime(int64_t sec) {
    if (sec < 0) sec = 0;
    int64_t h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    else       snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)m, (long long)s);
    return buf;
}

}  // namespace kick
