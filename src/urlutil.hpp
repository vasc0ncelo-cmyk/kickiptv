#pragma once
// Utilitarios de URL (independentes de plataforma, testados no PC).
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

inline std::string urlLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Extensao do caminho da URL em minusculas, sem a query ("http://h/a/B.MP4?x=1" -> "mp4"). Vazio se nao tiver.
inline std::string urlExtension(const std::string &url) {
    std::string lu = urlLower(url);
    size_t se = lu.find("://"), start = se == std::string::npos ? 0 : se + 3;
    size_t qs = lu.find_first_of("?#", start);
    std::string path = qs == std::string::npos ? lu : lu.substr(0, qs);
    size_t hs = path.find('/', start), sl = path.rfind('/'), dot = path.rfind('.');       // so conta ponto depois do host
    if (hs == std::string::npos || dot == std::string::npos || dot < hs || dot < sl) return "";
    return path.substr(dot + 1);
}

inline bool urlIsVideoFile(const std::string &url) {
    static const char *e[] = {"mp4", "mkv", "avi", "mov", "m4v", "webm", "wmv", "flv", "mpg", "mpeg"};
    std::string x = urlExtension(url);
    for (const char *s : e) if (x == s) return true;
    return false;
}

// Painel Xtream Codes: filmes/series quase sempre em /movie/... ou /series/... (com ou SEM extensao).
// Sem isso o app trata a URL como canal ao vivo (MPEG-TS) e o PS4 fecha com CE-34878-0.
inline bool urlIsXtreamVod(const std::string &url) {
    std::string lu = urlLower(url);
    if (lu.find("/movie/") != std::string::npos) return true;
    if (lu.find("/series/") != std::string::npos) return true;
    if (lu.find("/vod/") != std::string::npos) return true;
    return false;
}

// Qualquer VOD: arquivo de video por extensao OU caminho tipico de painel IPTV.
inline bool urlIsVod(const std::string &url) {
    if (urlIsVideoFile(url)) return true;
    if (urlIsXtreamVod(url)) return true;
    return false;
}

// Extensao segura para o AvPlayer (ele escolhe o demuxer pelo nome). Prefere mp4.
inline std::string vodPlayerName(const std::string &url) {
    std::string x = urlExtension(url);
    if (x == "mp4" || x == "m4v" || x == "mov") return "video." + x;
    // sem extensao ou formato que o AvPlayer do PS4 costuma rejeitar: finge mp4
    return "video.mp4";
}

// Canal de painel Xtream: http://host[:porta]/[live/]usuario/senha/ID[.ts]  ->  http://host[:porta]/[live/]usuario/senha/ID.m3u8
// (o HLS e bem mais confiavel que o fluxo TS cru). Devolve "" se a URL nao tem esse formato.
inline std::string xtreamHlsUrl(const std::string &url) {
    size_t se = url.find("://");
    if (se == std::string::npos) return "";
    size_t hs = url.find('/', se + 3);
    if (hs == std::string::npos) return "";
    size_t q = url.find_first_of("?#", hs);
    std::string path = url.substr(hs + 1, q == std::string::npos ? std::string::npos : q - hs - 1);
    std::vector<std::string> seg; size_t a = 0;
    for (;;) { size_t b = path.find('/', a); seg.push_back(path.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
    size_t k = (!seg.empty() && urlLower(seg[0]) == "live") ? 1 : 0;
    if (seg.size() != k + 3) return "";
    std::string id = seg[k + 2];
    size_t dot = id.rfind('.');
    if (dot != std::string::npos) { if (urlLower(id.substr(dot + 1)) != "ts") return ""; id = id.substr(0, dot); }
    if (id.empty() || seg[k].empty() || seg[k + 1].empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); })) return "";
    return url.substr(0, hs) + "/" + (k ? "live/" : "") + seg[k] + "/" + seg[k + 1] + "/" + id + ".m3u8";
}

// Container REAL do arquivo, olhando os primeiros bytes (o AvPlayer escolhe o demuxer pelo NOME do arquivo local:
// um .mkv gravado como .mp4 abre o player mas nunca mostra imagem). Filmes de painel IPTV costumam ser .mkv; series, .mp4.
// Devolve "mp4" (padrao, igual ao que ja funcionava), "mkv", "avi" ou "ts".
inline std::string vodContainerExt(const std::string &head, const std::string &url) {
    auto u = [&](size_t i) { return i < head.size() ? (unsigned char)head[i] : 0u; };
    if (head.size() >= 8 && head.compare(4, 4, "ftyp") == 0) return "mp4";
    if (head.size() >= 4 && u(0) == 0x1A && u(1) == 0x45 && u(2) == 0xDF && u(3) == 0xA3) return "mkv";
    if (head.size() >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "AVI ") == 0) return "avi";
    if (head.size() >= 1 && u(0) == 0x47 && (head.size() < 189 || u(188) == 0x47)) return "ts";
    std::string x = urlExtension(url);
    if (x == "mkv" || x == "avi") return x;
    return "mp4";
}
