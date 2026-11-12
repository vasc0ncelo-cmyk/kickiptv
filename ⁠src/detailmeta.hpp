#pragma once
// Dados da pagina de detalhes (filme / serie). Funcoes puras (testaveis no PC).
// A lista M3U so traz nome, capa, url e categoria: ano e qualidade saem do nome, o resto (sinopse, nota, generos, duracao)
// so aparece se alguem preencher DetailMeta (ver UI::metaLookup). O que nao existe NAO e desenhado nem inventado.
#include <cctype>
#include <string>
#include <vector>

struct DetailMeta {
    std::string year;                 // "2016"
    std::vector<std::string> genres;  // chips: "Aventura", "Drama"...
    std::string plot;                 // sinopse
    std::string rating;               // "8.7" (de 0 a 10)
    int runtimeMin = 0;               // filme: duracao total; serie: duracao media do episodio
    std::string quality;              // selo verde: "HD", "FHD", "4K"
    std::vector<int> epMin;           // opcional: duracao de cada episodio da temporada aberta (indice = posicao na lista)
};

namespace dm {
inline std::string lowerA(std::string s) { for (char &c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline std::string trimA(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
inline bool isYear(const std::string &s) {
    if (s.size() != 4) return false;
    for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
    return (s[0] == '1' && s[1] == '9') || (s[0] == '2' && s[1] == '0');
}
// "HD", "FHD", "4K"... (so a palavra inteira)
inline std::string qualityOf(const std::string &tok) {
    std::string t = lowerA(tok);
    if (t == "4k" || t == "uhd" || t == "2160p") return "4K";
    if (t == "fhd" || t == "1080p") return "FHD";
    if (t == "hd" || t == "720p") return "HD";
    return "";
}
}  // namespace dm

// "Stranger Things (2016) [HD]" -> titulo "Stranger Things", ano "2016", qualidade "HD".
// Tira so (ano), [ano], {ano}, marcas de qualidade entre ()[]{} e a qualidade solta no fim; o resto do nome fica como esta.
inline std::string detailCleanTitle(const std::string &name, std::string *year = nullptr, std::string *quality = nullptr) {
    std::string s = dm::trimA(name), y, q;
    // 1) grupos (...) [...] {...} que sao so ano ou so qualidade
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0; i < s.size(); i++) {
            char o = s[i], c = o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : 0;
            if (!c) continue;
            size_t e = s.find(c, i + 1);
            if (e == std::string::npos) continue;
            std::string in = dm::trimA(s.substr(i + 1, e - i - 1));
            std::string iq = dm::qualityOf(in);
            if (dm::isYear(in) || !iq.empty()) {
                if (dm::isYear(in)) { if (y.empty()) y = in; }
                else if (q.empty()) q = iq;
                size_t b = i;
                while (b > 0 && s[b - 1] == ' ') b--;
                s.erase(b, e + 1 - b);
                again = true;
                break;
            }
        }
    }
    // 2) marca de qualidade solta no FIM ("Filme 4K", "Filme FHD"). Ano solto NAO e removido: "Blade Runner 2049" e "1984" sao titulos.
    for (bool again = true; again;) {
        again = false;
        s = dm::trimA(s);
        size_t sp = s.find_last_of(' ');
        if (sp == std::string::npos) break;
        std::string lq = dm::qualityOf(s.substr(sp + 1));
        if (!lq.empty()) { if (q.empty()) q = lq; s.erase(sp); again = true; }
    }
    while (!s.empty() && (s.back() == '-' || s.back() == '|' || s.back() == ':' || s.back() == ' ')) s.pop_back();
    s = dm::trimA(s);
    if (s.empty()) s = dm::trimA(name);
    if (year) *year = y;
    if (quality) *quality = q;
    return s;
}

// Duracao: 44 -> "44min" | 130 -> "2h 10min" | 120 -> "2h"
inline std::string detailMinutes(int m) {
    if (m <= 0) return "";
    if (m < 60) return std::to_string(m) + "min";
    int h = m / 60, r = m % 60;
    return r ? std::to_string(h) + "h " + std::to_string(r) + "min" : std::to_string(h) + "h";
}

// Titulo do episodio para o cartao: "01. Mundo Invertido". O nome na lista costuma ser "Serie S01E01 Titulo" ou "Serie - S01E01":
// tira o nome da serie e o marcador SxxEyy; se sobrar nada, "Episódio N".
inline std::string detailEpisodeLabel(const std::string &series, const std::string &epName, int epNum, int fallbackNum) {
    std::string s = dm::trimA(epName);
    std::string ls = dm::lowerA(s), lser = dm::lowerA(series);
    if (!lser.empty() && ls.compare(0, lser.size(), lser) == 0) s = dm::trimA(s.substr(lser.size()));
    // tira marcador S01E02 / S01 E02 / T01E02 / 1x02 no inicio (com separadores em volta)
    auto sep = [](char c) { return c == ' ' || c == '-' || c == ':' || c == '|' || c == '.' || c == '_'; };
    while (!s.empty() && sep(s[0])) s.erase(0, 1);
    size_t i = 0;
    if (i < s.size() && (s[i] == 'S' || s[i] == 's' || s[i] == 'T' || s[i] == 't') && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) {
        size_t j = i + 1;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
        if (j < s.size() && sep(s[j])) j++;
        if (j < s.size() && (s[j] == 'E' || s[j] == 'e') && j + 1 < s.size() && std::isdigit((unsigned char)s[j + 1])) {
            j++;
            while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
            s.erase(0, j);
        }
    }
    while (!s.empty() && sep(s[0])) s.erase(0, 1);
    s = dm::trimA(s);
    int n = epNum > 0 ? epNum : fallbackNum;
    std::string num = (n < 10 ? "0" : "") + std::to_string(n);
    if (s.empty()) return num + ". Epis\xC3\xB3" "dio " + std::to_string(n);
    return num + ". " + s;
}
