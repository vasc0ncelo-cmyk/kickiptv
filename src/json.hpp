#pragma once
// JSON minimo e tolerante (sem dependencias) para a API dos paineis Xtream.
//  - Respostas com milhares de itens: forEachElement() le um elemento por vez (a memoria nao cresce com o tamanho do array).
//  - Resposta cortada no meio (rede caiu): entrega o que foi lido ate ali em vez de falhar.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    bool isObj() const { return type == Obj; }
    bool isArr() const { return type == Arr; }
    const Json *get(const char *key) const {
        if (type != Obj) return nullptr;
        for (auto &kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // texto de um campo: aceita string OU numero (paineis misturam "123" e 123)
    std::string str(const char *key, const std::string &def = "") const {
        const Json *j = get(key);
        if (!j) return def;
        if (j->type == Str) return j->s;
        if (j->type == Num) { char t[40]; if (j->n == (double)(long long)j->n) snprintf(t, sizeof t, "%lld", (long long)j->n); else snprintf(t, sizeof t, "%g", j->n); return t; }
        if (j->type == Bool) return j->b ? "1" : "0";
        return def;
    }
    long long num(const char *key, long long def = 0) const {
        const Json *j = get(key);
        if (!j) return def;
        if (j->type == Num) return (long long)j->n;
        if (j->type == Str) return atoll(j->s.c_str());
        if (j->type == Bool) return j->b ? 1 : 0;
        return def;
    }
};

namespace jsonp {

struct Reader {
    const char *p, *e;
    bool bad = false;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || (unsigned char)*p == 0xEF || (unsigned char)*p == 0xBB || (unsigned char)*p == 0xBF)) p++; }
};

inline void putUtf8(std::string &o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
    else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}
inline int hex4(const char *p, const char *e) {
    if (e - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i]; v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0'; else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10; else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10; else return -1;
    }
    return v;
}

inline bool parseString(Reader &r, std::string &out) {
    if (r.p >= r.e || *r.p != '"') { r.bad = true; return false; }
    r.p++; out.clear();
    while (r.p < r.e) {
        char c = *r.p++;
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (r.p >= r.e) break;
        char k = *r.p++;
        switch (k) {
        case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break;
        case 'u': {
            int v = hex4(r.p, r.e);
            if (v < 0) { r.bad = true; return false; }
            r.p += 4;
            unsigned cp = (unsigned)v;
            if (cp >= 0xD800 && cp < 0xDC00 && r.e - r.p >= 6 && r.p[0] == '\\' && r.p[1] == 'u') {   // par substituto (emoji)
                int lo = hex4(r.p + 2, r.e);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00); r.p += 6; }
            }
            putUtf8(out, cp);
            break; }
        default: out += k;   // \" \\ \/
        }
    }
    r.bad = true;   // string cortada
    return false;
}

inline bool parseValue(Reader &r, Json &j, int depth = 0) {
    r.ws();
    if (r.p >= r.e || depth > 64) { r.bad = true; return false; }
    char c = *r.p;
    if (c == '"') { j.type = Json::Str; return parseString(r, j.s); }
    if (c == '{') {
        j.type = Json::Obj; r.p++;
        for (;;) {
            r.ws();
            if (r.p >= r.e) { r.bad = true; return false; }
            if (*r.p == '}') { r.p++; return true; }
            if (*r.p == ',') { r.p++; continue; }
            std::string k;
            if (!parseString(r, k)) return false;
            r.ws();
            if (r.p >= r.e || *r.p != ':') { r.bad = true; return false; }
            r.p++;
            Json v;
            bool ok = parseValue(r, v, depth + 1);
            j.obj.emplace_back(std::move(k), std::move(v));    // mesmo cortado, guarda o que deu
            if (!ok) return false;
        }
    }
    if (c == '[') {
        j.type = Json::Arr; r.p++;
        for (;;) {
            r.ws();
            if (r.p >= r.e) { r.bad = true; return false; }
            if (*r.p == ']') { r.p++; return true; }
            if (*r.p == ',') { r.p++; continue; }
            Json v;
            bool ok = parseValue(r, v, depth + 1);
            j.arr.push_back(std::move(v));
            if (!ok) return false;
        }
    }
    if (r.e - r.p >= 4 && !strncmp(r.p, "true", 4)) { j.type = Json::Bool; j.b = true; r.p += 4; return true; }
    if (r.e - r.p >= 5 && !strncmp(r.p, "false", 5)) { j.type = Json::Bool; j.b = false; r.p += 5; return true; }
    if (r.e - r.p >= 4 && !strncmp(r.p, "null", 4)) { j.type = Json::Null; r.p += 4; return true; }
    char *endp = nullptr;
    std::string tmp(r.p, (size_t)(r.e - r.p) < 40 ? (size_t)(r.e - r.p) : 40);
    double d = strtod(tmp.c_str(), &endp);
    if (endp == tmp.c_str()) { r.bad = true; return false; }
    j.type = Json::Num; j.n = d; r.p += endp - tmp.c_str();
    return true;
}

}  // namespace jsonp

// JSON completo (objetos pequenos). false = nao deu para ler nada util.
inline bool jsonParse(const std::string &text, Json &out) {
    jsonp::Reader r{text.data(), text.data() + text.size()};
    out = Json();
    bool ok = jsonp::parseValue(r, out);
    return ok || out.type != Json::Null;
}

// Para um array no topo ("[ {...}, {...} ]"): chama f() para cada elemento lido, um de cada vez.
// Retorna a quantidade de elementos. Resposta cortada: entrega os completos e para. Se o topo nao for array (painel devolveu
// {"user_info":...} ou erro), retorna 0.
inline size_t jsonForEach(const std::string &text, const std::function<void(const Json &)> &f) {
    jsonp::Reader r{text.data(), text.data() + text.size()};
    r.ws();
    if (r.p >= r.e || *r.p != '[') return 0;
    r.p++;
    size_t n = 0;
    for (;;) {
        r.ws();
        if (r.p >= r.e || *r.p == ']') break;
        if (*r.p == ',') { r.p++; continue; }
        Json v;
        if (!jsonp::parseValue(r, v)) break;      // cortado: descarta so o ultimo, incompleto
        f(v); n++;
    }
    return n;
}
