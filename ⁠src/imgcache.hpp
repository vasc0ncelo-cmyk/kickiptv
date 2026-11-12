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
// Igual, mas diz QUAL thread esta baixando (0..kWorkers-1; kWorkers = o baixador de fundo): cada uma pode reaproveitar a sua conexao.
using CoverFetchW = std::function<bool(int worker, const std::string &url, std::string &body)>;

class ImgCache {
public:
    static const int kThumbW = 148;           // tamanho do poster na grade (2:3)
    static const int kThumbH = 222;
    static const size_t kMaxEntries = 300;    // na memoria (~130 KB cada)
    static const int kWorkers = 3;
    static const int kBgThreads = 3;          // baixam em lote (setBackground); usam os numeros kWorkers..kWorkers+kBgThreads-1

    // pasta do cache em disco (os testes usam outra)
    static std::string &diskDir() { static std::string d = "/data/kickiptv_cov"; return d; }
    static bool diskHas(const std::string &url);

    void setFetch(CoverFetch f) { fetch_ = [f](int, const std::string &u, std::string &b) { return f(u, b); }; }
    void setFetchW(CoverFetchW f) { fetch_ = std::move(f); }

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
                if (c.failed) { if (now - c.failedAt < 8.0) return; it->second = std::make_shared<Cover>(); }   // tenta de novo depois de 20 s
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
    // lista de capas (em ordem de importancia) para guardar no disco em segundo plano.
    // maxNew: para depois de baixar tantas capas NOVAS (as que ja estao no disco nao contam e sao puladas sem esperar).
    void setBackground(std::vector<std::string> urls, size_t maxNew = (size_t)-1) {
        {
            std::lock_guard<std::mutex> l(m_);
            bg_.clear(); std::set<std::string> seen;
            for (auto &u : urls) {
                if (u.size() < 8 || u.compare(0, 4, "http") != 0 || !seen.insert(u).second) continue;
                bg_.push_back(u);
            }
            bgPos_ = 0; bgMaxNew_ = maxNew;
            bgTotal_ = bg_.size(); bgDone_ = 0; bgNew_ = 0;
            bgActive_ = !bg_.empty();
        }
        startBg();
    }
    void cancelBackground() {                                 // para o lote (as capas ja baixadas continuam salvas)
        std::lock_guard<std::mutex> l(m_);
        bgPos_ = bg_.size();
        if (bgBusy_ == 0) bgActive_ = false;
    }
    bool backgroundActive() const { return bgActive_.load(); }
    size_t backgroundTotal() const { return bgTotal_.load(); }
    size_t backgroundDone() const { return bgDone_.load(); }     // capas ja olhadas (baixadas ou que ja estavam no disco)
    size_t backgroundNew() const { return bgNew_.load(); }       // capas baixadas neste lote
    void setPaused(bool p) { paused_ = p; }                  // true durante o video: nao disputa rede/CPU
    size_t backgroundLeft() { std::lock_guard<std::mutex> l(m_); return bg_.size() - std::min(bgPos_, bg_.size()); }
    uint32_t version() const { return version_.load(); }     // muda toda vez que uma capa fica pronta (a tela se redesenha)

    void shutdown() { shutdownFor(1e9); }
    // Para as threads esperando no maximo 'maxSec'. false = alguma esta presa na rede: elas sao soltas (detach) e quem chamou
    // deve encerrar o processo sem rodar destrutores (_Exit). Antes o fechar do app esperava a rede sem limite de tempo.
    bool shutdownFor(double maxSec) {
        { std::lock_guard<std::mutex> l(m_); stop_ = true; }
        cv_.notify_all();
        auto t0 = std::chrono::steady_clock::now();
        while (alive_.load() > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < maxSec)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        bool clean = alive_.load() == 0;
        for (auto &t : ths_) if (t.joinable()) { if (clean) t.join(); else t.detach(); }
        ths_.clear();
        for (auto &t : bgTh_) if (t.joinable()) { if (clean) t.join(); else t.detach(); }
        bgTh_.clear();
        return clean;
    }
    ~ImgCache() { shutdown(); }

private:
    CoverFetchW fetch_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Cover>> map_;
    std::deque<std::string> pending_;
    std::set<std::string> wanted_, inflight_;
    std::vector<std::thread> ths_;
    std::vector<std::thread> bgTh_;
    std::atomic<size_t> bgTotal_{0}, bgDone_{0}, bgNew_{0};
    size_t bgMaxNew_ = (size_t)-1; int bgBusy_ = 0;
    std::atomic<bool> bgActive_{false};
    std::vector<std::string> bg_; size_t bgPos_ = 0;
    std::atomic<bool> paused_{false};
    std::atomic<int> alive_{0};                // threads ainda rodando (para o shutdownFor)
    bool stop_ = false;
    std::atomic<uint32_t> version_{0};

    static double nowSec();
    void startWorkers() {
        std::lock_guard<std::mutex> l(m_);
        if (!ths_.empty() || stop_) return;
        for (int i = 0; i < kWorkers; i++) { alive_++; ths_.emplace_back([this, i] { workerLoop(i); alive_--; }); }
    }
    void startBg() {
        std::lock_guard<std::mutex> l(m_);
        if (!bgTh_.empty() || stop_) return;
        for (int i = 0; i < kBgThreads; i++) { alive_++; bgTh_.emplace_back([this, i] { bgLoop(kWorkers + i); alive_--; }); }
    }
    void bgLoop(int id) {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> l(m_);
                if (stop_) return;
                if (bgPos_ >= bg_.size() || bgNew_.load() >= bgMaxNew_) {      // acabou (ou chegou no limite): so marca "parado" quando ninguem esta no meio de uma capa
                    if (bgBusy_ == 0) bgActive_ = false;
                    cv_.wait_for(l, std::chrono::milliseconds(300)); continue;
                }
                bool busy = !pending_.empty() || !inflight_.empty() || paused_.load();   // a tela e o video tem prioridade
                if (busy) { cv_.wait_for(l, std::chrono::milliseconds(300)); continue; }
                url = bg_[bgPos_++]; bgBusy_++;
            }
            bool downloaded = false;
            try {
                if (!diskHas(url)) {
                    Cover c; std::string body;
                    if (fetch_ && fetch_(id, url, body) && body.size() > 32 && body.size() < 3 * 1024 * 1024 && imgDecodeToCover(body, c, kThumbW, kThumbH)) { saveDisk(url, c); downloaded = true; }
                }
            } catch (...) {}
            {
                std::lock_guard<std::mutex> l(m_);
                bgBusy_--; bgDone_++; if (downloaded) bgNew_++;
                if (bgPos_ >= bg_.size() && bgBusy_ == 0) bgActive_ = false;
            }
            if (downloaded) { version_++; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }   // capa nova: a tela pode redesenhar
        }
    }
    void workerLoop(int id) {
        for (;;) {
            std::string url;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || !pending_.empty(); });
                if (stop_) return;
                url = pending_.front(); pending_.pop_front();
                inflight_.insert(url);
            }
            try { loadOne(url, id); } catch (...) { finish(url, Cover{}, false); }
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
    void loadOne(const std::string &url, int worker);
    static std::string diskPath(const std::string &url);
    static bool loadDisk(const std::string &url, Cover &out);
    static void saveDisk(const std::string &url, const Cover &c);
};

