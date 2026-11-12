// Testa os motivos de falha mostrados na tela quando a lista M3U nao baixa.
#include "../src/app.hpp"
#include <cassert>
#include <iostream>

struct Fake : Platform {
    int st = 0; std::string head;
    bool httpGet(const std::string &, std::string &) override { return false; }
    bool httpStream(const std::string &, const std::function<bool(const char *, size_t)> &sink) override {
        if (!head.empty()) { sink(head.data(), head.size()); return true; }
        return false;
    }
    void play(const std::string &, const std::string &) override {}
    void requestText(const std::string &) override {}
    int lastHttpStatus() override { return st; }
    std::string lastBodyHead() override { return head; }
};

int main() {
    assert(httpReason(0).find("sem resposta") != std::string::npos);
    assert(httpReason(403).find("HTTP 403") != std::string::npos && httpReason(403).find("recusou") != std::string::npos);
    assert(httpReason(404).find("404") != std::string::npos);
    assert(httpReason(429).find("limite") != std::string::npos);
    assert(httpReason(503).find("problema") != std::string::npos);
    assert(bodyPreview(std::string("  Max\r\n connections\treached \x01\xff!")) == "Max connections reached !");
    assert(bodyPreview(std::string(500, 'a')).size() == 90);
    { Fake f; f.st = 403; App a(f); assert(!a.loadM3U("http://h/lista.m3u")); assert(a.lastStatus.find("HTTP 403") != std::string::npos); }
    { Fake f; f.st = 0; App a(f); assert(!a.loadM3U("http://h/lista.m3u")); assert(a.lastStatus.find("sem resposta") != std::string::npos); }
    { Fake f; f.head = "Account blocked: too many connections"; App a(f); assert(!a.loadM3U("http://h/lista.m3u"));
      assert(a.lastStatus.find("Account blocked") != std::string::npos); }
    std::cout << "OK\n";
}
