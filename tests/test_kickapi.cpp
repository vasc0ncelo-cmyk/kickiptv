#include <cassert>
#include <cstdio>
#include "../src/kickapi.hpp"
using namespace kickapi;

static const char *kLives = R"({"data":[
 {"broadcaster_user_id":1,"slug":"Pequeno","stream_title":"jogando ça","viewer_count":120,"language":"pt","thumbnail":"https:\/\/x\/a.webp","category":{"id":5,"name":"Just Chatting","thumbnail":"t"}},
 {"slug":"grande","stream_title":"GRANDE","viewer_count":"45000","category":{"name":"GTA V"}},
 {"slug":"sem espaco ok?","viewer_count":9},
 {"stream_title":"sem slug","viewer_count":99999}
],"message":"OK"})";

int main() {
    assert(tokenBody("a b", "s&=") == "grant_type=client_credentials&client_id=a%20b&client_secret=s%26%3D");
    assert(livesUrl(500, "pt", 7) == "https://api.kick.com/public/v1/livestreams?limit=100&sort=viewer_count&language=pt&category_id=7");
    std::string t; long long ex = 0;
    assert(parseToken("{\"access_token\":\"AB\",\"token_type\":\"Bearer\",\"expires_in\":7200}", t, ex) && t == "AB" && ex == 7200);
    assert(!parseToken("{\"error\":\"x\"}", t, ex) && !parseToken("lixo", t, ex));

    auto v = parseLives(kLives);
    assert(v.size() == 2);                                   // sem slug / slug invalido sao pulados
    assert(v[0].slug == "pequeno" && v[0].viewers == 120 && v[0].category == "Just Chatting" && v[0].language == "pt");
    assert(v[0].thumb == "https://x/a.webp");
    assert(v[1].slug == "grande" && v[1].viewers == 45000);
    assert(parseLives("{\"data\":[]}").empty() && parseLives("").empty() && parseLives("[{\"slug\":\"a\"}]").size() == 1);

    // cliente: token em cache, renovacao, 401, 429, chave errada
    int posts = 0, gets = 0, mode = 0; double now = 0;
    Transport tp = [&](const char *m, const std::string &url, const std::string &auth, const std::string &body, std::string &out, int &st) {
        if (std::string(m) == "POST") {
            posts++; assert(url == "https://id.kick.com/oauth/token" && body.find("client_id=ID") != std::string::npos && auth.empty());
            if (mode == 5) { st = 401; out = "{}"; return true; }
            st = 200; out = "{\"access_token\":\"T" + std::to_string(posts) + "\",\"expires_in\":3600}"; return true;
        }
        gets++; assert(url.find("/public/v1/livestreams") != std::string::npos);
        if (mode == 1 && auth == "Bearer T1") { st = 401; out = ""; return true; }   // 1o token "venceu"
        if (mode == 2) { st = 429; return true; }
        if (mode == 3) return false;
        assert(auth.rfind("Bearer T", 0) == 0);
        st = 200; out = kLives; return true;
    };
    Client c; c.setClock([&] { return now; });
    std::vector<Live> out; std::string err;
    assert(!c.lives(tp, out, err) && err.find("sem chave") != std::string::npos);
    c.setCredentials("ID", "SEC");
    assert(c.lives(tp, out, err) && out.size() == 2 && out[0].slug == "grande" && posts == 1);   // ordenado por espectadores
    assert(c.lives(tp, out, err) && posts == 1 && gets == 2);                                    // token reaproveitado
    now = 3600; assert(c.lives(tp, out, err) && posts == 2);                                     // venceu: pede outro
    c.setCredentials("ID", "SEC"); posts = gets = 0; mode = 1;
    assert(c.lives(tp, out, err) && posts == 2 && gets == 2);                                    // 401: renova e repete uma vez
    mode = 2; assert(!c.lives(tp, out, err) && err.find("esperar") != std::string::npos);
    mode = 3; assert(!c.lives(tp, out, err) && err.find("sem resposta") != std::string::npos);
    mode = 5; c.setCredentials("ID", "SEC"); assert(!c.lives(tp, out, err) && err.find("recusou a chave") != std::string::npos);

    // status de canal (endpoint nao oficial)
    Live a = parseChannelStatus("x", "{\"playback_url\":\"u\",\"livestream\":{\"id\":1,\"session_title\":\"oi\",\"viewer_count\":321}}");
    assert(a.live && a.viewers == 321 && a.title == "oi");
    assert(!parseChannelStatus("x", "{\"livestream\":null,\"viewer_count\":5}").live);
    assert(fmtViewers(999) == "999" && fmtViewers(1500) == "1.5k" && fmtViewers(45000) == "45k");

    // favoritos / historico
    auto f = splitSlugs("A\n\nb\n@a\nbad name!\n");
    assert(f.size() == 2 && f[0] == "a" && f[1] == "b");
    assert(toggle(f, "c") && f[0] == "c" && !toggle(f, "a") && !contains(f, "a") && splitSlugs(joinSlugs(f)) == f);
    std::vector<std::string> h;
    for (int i = 0; i < 30; i++) remember(h, "s" + std::to_string(i));
    remember(h, "s25"); assert(h.size() == 20 && h[0] == "s25" && h[1] == "s29");
    puts("kickapi ok");
}
