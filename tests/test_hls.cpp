#include "../src/hls.hpp"
#include "../src/streambuf.hpp"
#include <cassert>
#include <iostream>
#include <thread>

int main() {
    // resolucao de URLs
    assert(hlsResolve("https://a.com/x/y.m3u8?t=1", "seg1.ts") == "https://a.com/x/seg1.ts");
    assert(hlsResolve("https://a.com/x/y.m3u8", "/z/s.ts") == "https://a.com/z/s.ts");
    assert(hlsResolve("https://a.com/x/y.m3u8", "//b.com/s.ts") == "https://b.com/s.ts");
    assert(hlsResolve("https://a.com/x/y.m3u8", "http://c/s.ts") == "http://c/s.ts");
    assert(hlsResolve("https://a.com", "s.ts") == "https://a.com/s.ts");

    // playlist mestre (estilo IVS): so-audio, 160p, 720p60, 1080p60
    std::string master =
        "#EXTM3U\n#EXT-X-VERSION:3\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=64000,CODECS=\"mp4a.40.2\"\naudio/playlist.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=8000000,RESOLUTION=1920x1080,FRAME-RATE=60.000\n1080p60/playlist.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=3500000,RESOLUTION=1280x720,FRAME-RATE=60.000\n720p60/playlist.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=284x160\n160p/playlist.m3u8\n";
    assert(hlsIsMaster(master));
    auto v = hlsParseMaster(master);
    assert(v.size() == 4 && v[1].height == 1080 && v[2].bandwidth == 3500000);
    assert(v[hlsPickVariant(v, 720)].uri == "720p60/playlist.m3u8");
    assert(v[hlsPickVariant(v, 1080)].uri == "1080p60/playlist.m3u8");
    assert(v[hlsPickVariant(v, 100)].uri == "160p/playlist.m3u8");   // nada cabe: usa a mais leve (sem audio)

    // limite de fps: 60 fps pesa o dobro no PS4 (desenho por CPU); com limite 30 escolhe a variante de 30 fps
    std::string m2 =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=8000000,RESOLUTION=1920x1080,FRAME-RATE=60.000\n1080p60/p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=3500000,RESOLUTION=1280x720,FRAME-RATE=60.000\n720p60/p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=852x480,FRAME-RATE=30.000\n480p30/p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=300000,RESOLUTION=284x160,FRAME-RATE=30.000\n160p/p.m3u8\n";
    auto v2 = hlsParseMaster(m2);
    assert(v2[1].fps == 60.0 && v2[2].fps == 30.0);
    assert(v2[hlsPickVariant(v2, 720)].uri == "720p60/p.m3u8");                // sem limite: igual a antes
    assert(v2[hlsPickVariant(v2, 720, 30)].uri == "480p30/p.m3u8");            // limite 30 fps
    std::string m3 =   // canal que transmite em 30 fps: 720p30 continua sendo escolhido
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=3000000,RESOLUTION=1280x720,FRAME-RATE=30.000\n720p30/p.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=852x480,FRAME-RATE=30.000\n480p30/p.m3u8\n";
    auto v3 = hlsParseMaster(m3);
    assert(v3[hlsPickVariant(v3, 720, 30)].uri == "720p30/p.m3u8");
    std::string m4 =   // so 60 fps disponivel: o limite e ignorado em vez de falhar
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=3500000,RESOLUTION=1280x720,FRAME-RATE=60.000\n720p60/p.m3u8\n";
    auto v4 = hlsParseMaster(m4);
    assert(v4[hlsPickVariant(v4, 720, 30)].uri == "720p60/p.m3u8");

    // playlist de midia
    std::string media = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n#EXTINF:2.000,\nseg100.ts\n#EXTINF:2.0,\nseg101.ts\n";
    assert(!hlsIsMaster(media));
    auto md = hlsParseMedia(media);
    assert(md.segs.size() == 2 && md.segs[0].seq == 100 && md.segs[1].uri == "seg101.ts" && !md.hasMap && !md.endList && md.target == 2);
    assert(hlsParseMedia("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:2,\na.m4s\n#EXT-X-ENDLIST\n").hasMap);

    // StreamBuf
    StreamBuf sb;
    uint8_t a[10], out[32];
    for (int i = 0; i < 10; i++) a[i] = (uint8_t)i;
    sb.append(a, 10);
    assert(sb.read(out, 0, 4) == 4 && out[3] == 3);
    assert(sb.read(out, 4, 100) == 6 && out[5] == 9);                  // le so o que existe
    assert(sb.read(out, 10 + (2u << 20), 4, 1000) == 0);                // sondagem longe do fim: nao bloqueia
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); uint8_t b[3] = {50, 51, 52}; sb.append(b, 3); });
    assert(sb.read(out, 10, 8) == 3 && out[0] == 50);                   // bloqueia ate chegar dado
    t.join();
    std::thread t2([&] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); sb.abort(); });
    assert(sb.read(out, 13, 8) == 0);                                   // abort libera o leitor
    t2.join();
    // janela deslizante
    StreamBuf s2; s2.softCap = 100; s2.hardCap = 400;
    std::vector<uint8_t> big(500, 7);
    s2.append(big.data(), 500);
    assert(s2.total() == 500 && s2.base == 100 && s2.read(out, 0, 4) == 0 && s2.read(out, 100, 4) == 4);
    std::cout << "OK\n";
}
