#include "../src/urlutil.hpp"
#include <cassert>
#include <iostream>
int main() {
    assert(urlExtension("http://180381.sbs/series/Dirce0273/Sar5438/585332.mp4") == "mp4");
    assert(urlExtension("http://h/a/b.MKV?x=1.ts") == "mkv");
    assert(urlExtension("http://h:80/user/pass/123") == "");
    assert(urlExtension("http://host.com") == "" && urlExtension("http://h/a.b/c") == "");
    assert(urlIsVideoFile("http://h/movie/u/p/1.mkv") && !urlIsVideoFile("http://h/live/u/p/1.ts") && !urlIsVideoFile("http://h/x.m3u8"));
    assert(xtreamHlsUrl("http://h:8080/live/u/p/123.ts") == "http://h:8080/live/u/p/123.m3u8");
    assert(xtreamHlsUrl("http://h/u/p/123") == "http://h/u/p/123.m3u8");
    assert(xtreamHlsUrl("http://h/u/p/123.ts?x=1") == "http://h/u/p/123.m3u8");
    assert(xtreamHlsUrl("http://h/u/p/123.m3u8").empty() && xtreamHlsUrl("http://h/movie/u/p/1.mp4").empty());
    assert(xtreamHlsUrl("http://h/canal/abc.ts").empty() && xtreamHlsUrl("http://h/u/p/abc").empty() && xtreamHlsUrl("http://h").empty());
    std::cout << "OK\n";
}
