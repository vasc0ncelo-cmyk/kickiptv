// Inicio: backdrop do painel Xtream e tamanho da imagem pedida ao TMDB.
#include "../src/ui.hpp"
#include <cassert>
#include <iostream>
int main() {
    { Json j; jsonParse("{\"backdrop_path\":[\"http://i/b1.jpg\",\"http://i/b2.jpg\"]}", j); assert(xtBackdrop(j) == "http://i/b1.jpg"); }
    { Json j; jsonParse("{\"backdrop_path\":\"https://i/b.jpg\"}", j); assert(xtBackdrop(j) == "https://i/b.jpg"); }
    { Json j; jsonParse("{\"backdrop_path\":[\"\",\"nao-url\"]}", j); assert(xtBackdrop(j).empty()); }
    { Json j; jsonParse("{\"plot\":\"x\",\"backdrop_path\":[\"http://i/b1.jpg\"]}", j); DetailMeta m; xtFillMeta(j, m); assert(m.backdrop == "http://i/b1.jpg" && m.plot == "x"); }
    std::string u = "https://image.tmdb.org/t/p/original/abc.jpg";
    assert(coverFetchUrl(coverVariantKey(u, 880, 340)) == "https://image.tmdb.org/t/p/w780/abc.jpg");
    assert(coverFetchUrl(coverVariantKey(u, 264, 288)) == "https://image.tmdb.org/t/p/w500/abc.jpg");
    assert(coverFetchUrl(coverVariantKey("https://image.tmdb.org/t/p/w500/logo.png", kCoverLogoW, kCoverLogoH)) == "https://image.tmdb.org/t/p/w500/logo.png");
    assert(coverFetchUrl(coverVariantKey("https://image.tmdb.org/t/p/w300/logo.png", kCoverLogoW, kCoverLogoH)) == "https://image.tmdb.org/t/p/w500/logo.png");
    std::cout << "hero ok\n";
}
