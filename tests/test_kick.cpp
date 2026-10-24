// Kick (consulta com alternativas, JSON com espacos, mensagens de erro) e miniaturas de capa do TMDB.
#include "../src/app.hpp"
#include "../src/coverurl.hpp"
#include <cassert>
#include <iostream>

struct KFake : Platform {
    std::vector<std::string> asked; std::string played;
    int failFirst = 0, status = 0;                 // falha as N primeiras consultas com esse status
    std::string json = "{\"playback_url\":\"https:\\/\\/x.live\\/a.m3u8\",\"livestream\":{\"id\":1}}";
    bool httpGet(const std::string &, std::string &) override { return false; }
    bool httpGetKick(const std::string &u, std::string &b) override {
        asked.push_back(u);
        if ((int)asked.size() <= failFirst) { st_ = status; return false; }
        b = json; return true;
    }
    int lastHttpStatus() override { return st_; }
    void play(const std::string &u, const std::string &) override { played = u; }
    void requestText(const std::string &) override {}
    int st_ = 0;
};

int main() {
    // JSON com espacos e quebras de linha (a API pode formatar assim)
    assert(kickIsLive("{ \"livestream\" : { \"id\": 1 } }") && !kickIsLive("{\"livestream\": null}") && !kickIsLive("{\"a\":1}"));
    assert(jsonString("{ \"playback_url\" : \"https:\\/\\/h\\/a.m3u8?x=1\\u0026y=2\" }", "playback_url") == "https://h/a.m3u8?x=1&y=2");
    assert(jsonString("{\"x\":\"playback_url\"}", "playback_url").empty());                  // valor nao e chave
    assert(kickApiUrls("abc").size() == 2 && kickApiUrls("abc")[0] == "https://kick.com/api/v2/channels/abc");

    { KFake f; App a(f); a.watchKick("@Abc"); assert(f.played == "https://x.live/a.m3u8" && f.asked.size() == 1); }
    // 1a consulta recusada (403): tenta a 2a e assiste
    { KFake f; f.failFirst = 1; f.status = 403; App a(f); a.watchKick("abc"); assert(f.asked.size() == 2 && f.asked[1].find("/api/v1/") != std::string::npos && f.played == "https://x.live/a.m3u8"); }
    // as duas recusadas: mensagem com o codigo
    { KFake f; f.failFirst = 9; f.status = 403; App a(f); a.watchKick("abc"); assert(f.played.empty() && a.lastStatus.find("Falha ao consultar a Kick (HTTP 403)") == 0); }
    { KFake f; f.failFirst = 9; f.status = 0; App a(f); a.watchKick("abc"); assert(a.lastStatus.find("sem resposta") != std::string::npos); }
    { KFake f; f.failFirst = 9; f.status = 404; App a(f); a.watchKick("abc"); assert(f.asked.size() == 1 && a.lastStatus.find("nao existe") != std::string::npos); }
    { KFake f; f.json = "{\"livestream\":null,\"playback_url\":\"x\"}"; App a(f); a.watchKick("abc"); assert(f.played.empty() && a.lastStatus == "@abc esta offline"); }
    { KFake f; App a(f); a.watchKick("a b!"); assert(a.lastStatus == "@ invalido" && f.asked.empty()); }

    // TMDB: pede o tamanho de miniatura
    assert(coverThumbUrl("https://image.tmdb.org/t/p/original/abc.jpg") == "https://image.tmdb.org/t/p/w154/abc.jpg");
    assert(coverThumbUrl("http://image.tmdb.org/t/p/w600_and_h900_bestv2/abc.jpg?x=1") == "http://image.tmdb.org/t/p/w154/abc.jpg?x=1");
    assert(coverThumbUrl("https://image.tmdb.org/t/p/w500/a/b.jpg") == "https://image.tmdb.org/t/p/w154/a/b.jpg");
    assert(coverThumbUrl("https://image.tmdb.org/t/p/w154/abc.jpg") == "https://image.tmdb.org/t/p/w154/abc.jpg");
    assert(coverThumbUrl("https://image.tmdb.org/t/p/w92/abc.jpg") == "https://image.tmdb.org/t/p/w92/abc.jpg");
    assert(coverThumbUrl("https://outro.com/p/original/abc.jpg") == "https://outro.com/p/original/abc.jpg");
    assert(coverThumbUrl("https://image.tmdb.org/t/p/") == "https://image.tmdb.org/t/p/" && coverThumbUrl("").empty());
    std::cout << "OK\n";
}
