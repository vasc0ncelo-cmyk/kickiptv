#pragma once
// Capas pequenas + 1 worker + cache em disco (rapido e estavel).
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Cover {
    int w = 0, h = 0;
    std::vector<uint32_t> bgra;
    bool ready = false;
    bool failed = false;
};

using CoverFetch = std::function<bool(const std::string &url, std::string &body)>;

class ImgCache {
public:
    // bem pequeno = download/decode rapido
    static const int kThumbW = 48;
    static const int kThumbH = 72;
    static const size_t kMaxEntries = 150;

    void setFetch(CoverFetch f) { fetch_ = std::move(f); }

    void request(const std::string &url) {
        if (url.empty() || url.compare(0, 4, "http") != 0) return;
        {
            std::lock_guard<std::mutex> l(m_);
            auto it = map_.find(url);
            if (it != map_.end()) return;
            Cover disk;
            if (loadDisk(url, disk)) { map_[url] = std::move(disk); return; }
            if (map_.size() >= kMaxEntries) {
                for (auto it2 = map_.begin(); it2 != map_.end(); ++it2) {
                    if (it2->second.failed || it2->second.ready) { map_.erase(it2); break; }
                }
                if (map_.size() >= kMaxEntries) return;
            }
            map_[url] = Cover{};
            pending_.push_back(url);
        }
        kickWorker();
    }

    // prioridade do que esta na tela
    void requestFront(const std::string &url) {
        if (url.empty() || url.compare(0, 4, "http") != 0) return;
        {
            std::lock_guard<std::mutex> l(m_);
            auto it = map_.find(url);
            if (it != map_.end() && (it->second.ready || it->second.failed)) return;
            if (it == map_.end()) {
                Cover disk;
                if (loadDisk(url, disk)) { map_[url] = std::move(disk); return; }
                map_[url] = Cover{};
            }
            for (size_t i = 0; i < pending_.size(); ++i) {
                if (pending_[i] == url) { pending_.erase(pending_.begin() + (long)i); break; }
            }
            pending_.insert(pending_.begin(), url);
        }
        kickWorker();
    }

    bool get(const std::string &url, Cover &out) {
        std::lock_guard<std::mutex> l(m_);
        auto it = map_.find(url);
        if (it == map_.end() || !it->second.ready) return false;
        out = it->second;
        return true;
    }

    void shutdown() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        running_ = false;
    }

    ~ImgCache() { shutdown(); }

private:
    CoverFetch fetch_;
    std::mutex m_;
    std::map<std::string, Cover> map_;
    std::vector<std::string> pending_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};

    void kickWorker() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        if (th_.joinable()) th_.join();
        th_ = std::thread([this] {
            while (!stop_) {
                std::string url;
                {
                    std::lock_guard<std::mutex> l(m_);
                    if (pending_.empty()) { running_ = false; return; }
                    url = pending_.front();
                    pending_.erase(pending_.begin());
                }
                loadOne(url);
            }
            running_ = false;
        });
    }

    void loadOne(const std::string &url);
    static std::string diskPath(const std::string &url);
    static bool loadDisk(const std::string &url, Cover &out);
    static void saveDisk(const std::string &url, const Cover &c);
};

bool imgDecodeToCover(const std::string &body, Cover &out, int tw, int th);
