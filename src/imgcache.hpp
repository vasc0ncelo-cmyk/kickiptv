#pragma once
// Capas (posteres) em miniatura. Para aparecerem NA HORA:
//  1) o que ja esta no disco e lido no mesmo quadro em que a tela pede (setWanted), sem esperar thread nenhuma;
//  2) o resto baixa em paralelo (visivel primeiro, depois as proximas paginas);
//  3) um baixador em segundo plano (setBackground) enche o disco com as capas da lista inteira enquanto o app esta parado
//     (pausa durante videos: setPaused), entao na proxima vez que abrir uma aba as capas ja estao la.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct Cover {
    int w = 0, h = 0;
    std::vector<uint32_t> bgra;   // 0xAARRGGBB, opaco (ja composto sobre o fundo)
    bool ready = false;
    bool failed = false;
    double failedAt = 0;
};

// Decodifica JPEG/PNG e gera o poster tw x th (posters preenchem; logos/imagens quadradas ficam inteiras no meio).
bool imgDecodeToCover(const std::string &body, Cover &out, int tw, int th);

using CoverFetch = std::function<bool(const std::string &url, std::string &body)>;

class ImgCache {
public:
    static const int kThumbW = 148;           // tamanho do poster na grade (2:3)
    static const int kThumbH = 222;
    static const size_t kMaxEntries = 300;    // na memoria (~130 KB cada)
    static const int kWorkers = 4;

    // pasta do cache em disco (os testes usam outra)
    static std::string &diskDir() { static std::string d = "/data/kickiptv_cov"; return d; }
    static bool diskHas(const std::string &url);

    void setFetch(CoverFetch f) { fetch_ = std::move(f); }

    // front = visivel (ordem de prioridade); back = proximas paginas. Troca a fila inteira.
    void setWanted(const std::vector<std::string> &front, const std::vector<std::string> &back) {
        {   // o que ja esta no disco entra agora, antes de desenhar: a capa aparece no mesmo quadro
            std::vector<std::string> miss;
            {
                std::lock_guard<std::mutex> l(m_);
                for (auto &u : front) { if (u.size() < 8 || u.compare(0, 4, "http") != 0) continue; auto it = map_.find(u); if (it == map_.end() || (!it->second->ready && !inflight_.count(u) && !it->second->failed)) miss.push_back(u); }
            }
            for (auto &u : miss) {
                Cover c;
                if (!loadDisk(u, c)) continue;
                std::lock_guard<std::mutex> l(m_);
                auto p = std::make_shared<Cover>(std::move(c)); p->ready = true;
                map_[u] = p;
            }
        }
        {
            std::lock_guard<std::mutex> l(m_);
            double now = nowSec();
            wanted_.clear(); pending_.clear();
            auto add = [&](const std::string &u) {
                if (u.size() < 8 || u.compare(0, 4, "http") != 0) return;
                if (!wanted_.insert(u).second) return;
                auto it = map_.find(u);
                if (it == map_.end()) { map_[u] = std::make_shared<Cover>(); pending_.push_back(u); return; }
                Cover &c = *it->second;
                if (c.ready || inflight_.count(u)) return;
                if (c.failed) { if (now - c.failedAt < 20.0) return; it->second = std::make_shared<Cover>(); }   // tenta de novo depois de 20 s
                pending_.push_back(u);
            };
            for (auto &u : front) add(u);
            for (auto &u : back) add(u);
            if (map_.size() > kMaxEntries) {                      // libera o que nao esta mais em uso
                for (auto it = map_.begin(); it != map_.end() && map_.size() > kMaxEntries;) {
                    if (!wanted_.count(it->first) && !inflight_.count(it->first)) it = map_.erase(it); else ++it;
                }
            }
        }
        startWorkers();
        cv_.notify_all();
    }

    std::shared_ptr<const Cover> getPtr(const std::string &url) {
        std::lock_guard<std::mutex> l(m_);
        auto it = map_.find(url);
        if (it == map_.end() || !it->second->ready) return nullptr;
        return it->second;
    }
    bool get(const std::string &url, Cover &out) {          // copia (compatibilidade)
        auto p = getPtr(url);
        if (!p) return false;
        out = *p;
        return true;
    }
    // lista de capas (em ordem de importancia) para guardar no disco em segundo plano
    void setBackground(std::vector<std::string> urls) {
        {
            std::lock_guard<std::mutex> l(m_);
            bg_.clear(); std::set<std::string> seen;
            for (auto &u : urls) {
                if (u.size() < 8 || u.compare(0, 4, "http") != 0 || !seen.insert(u).second) continue;
                bg_.push_back(u);
            }
            bgPos_ = 0;
        }
        startBg();
    }
    void setPaused(bool p) { paused_ = p; }                  // true durante o video: nao disputa rede/CPU
    size_t backgroundLeft() { std::lock_guard<std::mutex> l(m_); return bg_.size() - std::min(bgPos_, bg_.size()); }
    uint32_t version() const { return version_.load(); }     // muda toda vez que uma capa fica pronta (a tela se redesenha)

    void shutdown() {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        for (auto &t : ths_) if (t.joinable()) t.join();
        ths_.clear();
        if (bgTh_.joinable()) bgTh_.join();
    }
    ~ImgCache() { shutdown(); }

private:
    CoverFetch fetch_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Cover>> map_;
    std::deque<std::string> pending_;
    std::set<std::string> wanted_, inflight_;
    std::vector<std::thread> ths_;
    std::thread bgTh_;
    std::vector<std::string> bg_; size_t bgPos_ = 0;
    std::atomic<bool> paused_{false};
    bool stop_ = false;
    std::atomic<uint32_t> version_{0};

    static double nowSec();
    void startWorkers() {
        std::lock_guard<std::mutex> l(m_);
        if (!ths_.empty() || stop_) return;
        for (int i = 0; i < kWorkers; i++) ths_.emplace_back([this] { workerLoop(); });
    }
    void startBg() {
        std::lock_guard<std::mutex> l(m_);
        if (bgTh_.joinable() || stop_) return;
        bgTh_ = std::thread([this] { bgLoop(); });
    }
    void bgLoop() {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> l(m_);
                if (stop_) return;
                bool busy = !pending_.empty() || !inflight_.empty() || paused_.load() || bgPos_ >= bg_.size();   // a tela e o video tem prioridade
                if (busy) { cv_.wait_for(l, std::chrono::milliseconds(300)); continue; }
                url = bg_[bgPos_++];
            }
            try {
                if (!diskHas(url)) {
                    Cover c; std::string body;
                    if (fetch_ && fetch_(url, body) && body.size() > 32 && body.size() < 3 * 1024 * 1024 && imgDecodeToCover(body, c, kThumbW, kThumbH)) saveDisk(url, c);
                }
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    void workerLoop() {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !pending_.empty(); });
                if (stop_) return;
                url = pending_.front(); pending_.pop_front();
                inflight_.insert(url);
            }
            try { loadOne(url); } catch (...) { finish(url, Cover{}, false); }
        }
    }
    void finish(const std::string &url, Cover c, bool ok) {
        {
            std::lock_guard<std::mutex> l(m_);
            inflight_.erase(url);
            auto p = std::make_shared<Cover>();
            if (ok) { *p = std::move(c); p->ready = true; }
            else { p->failed = true; p->failedAt = nowSec(); }
            map_[url] = p;
        }
        version_++;
    }
    void loadOne(const std::string &url);
    static std::string diskPath(const std::string &url);
    static bool loadDisk(const std::string &url, Cover &out);
    static void saveDisk(const std::string &url, const Cover &c);
};

