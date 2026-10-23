// Kick IPTV para PS4: lives da Kick (HLS) + lista M3U (canais, filmes, series).
//   Home: banner + atalhos KICK TV / FILMES / SERIES / CANAIS + LISTA M3U  (D-pad navega, X seleciona, OPTIONS sai)
//   Kick TV: X em "Digitar @streamer" abre o teclado na tela; o link da lista M3U tambem e digitado nele.
//   Teclado: D-pad move, X digita, QUADRADO apaga, TRIANGULO insere https://, L2 limpa, R1 = OK, O cancela.
//   Durante o video: X = pausar | L2/R2 = -10s/+10s (VOD) | setas = -30s/+30s (VOD) | L1/R1 = episodio ant/prox
//                    QUADRADO = qualidade | TRIANGULO = ajuste de imagem (live) | L1 = memoria (live) | O = voltar
//   HLS (.m3u8): o app baixa playlist/segmentos e entrega ao AvPlayer por callbacks de arquivo (hls.hpp / streambuf.hpp).
// Headers do C++ primeiro (mesma ordem do sample "threading" do OpenOrbis): evita conflito de tipos com os headers orbis.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hls.hpp"
#include "streambuf.hpp"
#include "netstream.hpp"
#include "httpfile.hpp"
#include "tschunk.hpp"
#include "urlutil.hpp"
#include "tsmux.hpp"
#include "blit.hpp"
#include "padin.hpp"
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include "ui.hpp"
#include "fbrender.hpp"
#include "keyboard.hpp"
#include "clock.hpp"

// ---------- Cliente HTTP (libSceNet + libSceSsl + libSceHttp, ligadas no link) ----------
// Prototipos declarados aqui para nao depender dos nomes dos headers do toolchain.
extern "C" {
int sceSysmoduleLoadModuleInternal(uint32_t id);
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t poolSize);
int sceHttpInit(int libnetMemId, int libsslCtxId, size_t poolSize);
int sceHttpCreateTemplate(int libhttpCtxId, const char *userAgent, int httpVer, int isAutoProxyConf);
int sceHttpCreateConnectionWithURL(int tmplId, const char *url, int enableKeepalive);
int sceHttpCreateRequestWithURL(int connId, int method, const char *url, unsigned long long contentLength);
int sceHttpSendRequest(int reqId, const void *postData, size_t size);
int sceHttpGetStatusCode(int reqId, int *statusCode);
int sceHttpGetAllResponseHeaders(int reqId, char **header, size_t *headerSize);
int sceHttpReadData(int reqId, void *data, size_t size);
int sceHttpDeleteRequest(int id);
int sceHttpDeleteConnection(int id);
int sceHttpDeleteTemplate(int id);
int sceHttpsDisableOption(int id, unsigned flags);
int sceHttpSetAutoRedirect(int id, int enable);
int sceHttpAddRequestHeader(int id, const char *name, const char *value, int mode);
int sceHttpSetConnectTimeOut(int id, unsigned usec);
int sceHttpSetSendTimeOut(int id, unsigned usec);
int sceHttpSetRecvTimeOut(int id, unsigned usec);
int sceHttpSetResolveTimeOut(int id, unsigned usec);
}

// Heap da libc sem limite: sem isso o malloc acaba (lista M3U grande + cache do filme + buffers do AvPlayer) e o app fecha.
extern "C" {
unsigned int sceLibcHeapExtendedAlloc = 1;
size_t sceLibcHeapSize = 0xFFFFFFFFFFFFFFFFULL;   // SCE_LIBC_HEAP_SIZE_EXTENDED_ALLOC_NO_LIMIT
}
static void trace(const char *stage);   // (definida mais abaixo)
// Marcadores finos do download da lista M3U: se o app fechar, a proxima abertura mostra o ULTIMO passo que foi gravado.
static bool g_traceHttp = false;                 // so durante o download da lista M3U
static const char *g_m3uTag = "m3u-t-";          // "t" = thread de rede | "s" = modo seguro (mesma thread)
static bool g_safeM3U = false;                   // modo seguro: baixa a lista sem criar thread de rede
static void traceHttp(const char *what) { if (g_traceHttp) { std::string s = std::string(g_m3uTag) + what; trace(s.c_str()); } }
static double monoSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

struct NetClient {
    std::vector<std::string> *log = nullptr;
    bool ready = false, failed = false;
    int httpCtx = -1;

    void L(const std::string &s) { if (log) log->push_back(s); }
    static std::string hex(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    bool init() {
        if (ready) return true;
        if (failed) { L("(inicializacao ja falhou acima)"); return false; }
        failed = true;
        // IDs internos de modulo do PS4: NetCtl 0x80000009, Http 0x8000000A, Ssl 0x8000000B, Net 0x8000001C
        const uint32_t ids[4] = {0x8000001C, 0x80000009, 0x8000000A, 0x8000000B};
        const char *names[4] = {"Net", "NetCtl", "Http", "Ssl"};
        for (int i = 0; i < 4; i++) L(std::string("carregar modulo ") + names[i] + ": " + hex(sceSysmoduleLoadModuleInternal(ids[i])));
        int rc = sceNetInit();                         L("sceNetInit: " + hex(rc));
        int netMem = sceNetPoolCreate("kickiptv", 64 * 1024, 0); L("sceNetPoolCreate: " + hex(netMem));
        int ssl = sceSslInit(300 * 1024);              L("sceSslInit: " + hex(ssl));
        httpCtx = sceHttpInit(netMem, ssl, 128 * 1024); L("sceHttpInit: " + hex(httpCtx));
        if (httpCtx < 0) return false;
        ready = true;
        failed = false;
        return true;
    }

    // GET simples. Retorna true se conseguiu falar com o servidor (mesmo com status de erro).
    bool get(const std::string &url, std::string &body, int &status, size_t maxBytes = 1536 * 1024) {
        status = 0; body.clear();
        if (!init()) return false;
        const char *ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        int tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        L("  CreateTemplate: " + hex(tmpl));
        if (tmpl < 0) return false;
        sceHttpsDisableOption(tmpl, 0x1F);   // sem verificacao de certificado (o app nao carrega os CAs do PS4)
        sceHttpSetAutoRedirect(tmpl, 1);
        const unsigned T = 15 * 1000 * 1000;
        sceHttpSetResolveTimeOut(tmpl, T);
        sceHttpSetConnectTimeOut(tmpl, T);
        sceHttpSetSendTimeOut(tmpl, T);
        sceHttpSetRecvTimeOut(tmpl, T);
        int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1);
        L("  CreateConnection: " + hex(conn));
        bool ok = false;
        if (conn >= 0) {
            int req = sceHttpCreateRequestWithURL(conn, 0 /*GET*/, url.c_str(), 0);
            L("  CreateRequest: " + hex(req));
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "application/json, text/plain, */*", 0);
                sceHttpAddRequestHeader(req, "Accept-Language", "pt-BR,pt;q=0.9,en;q=0.8", 0);
                int rc = sceHttpSendRequest(req, NULL, 0);
                L("  SendRequest: " + hex(rc));
                if (rc >= 0) {
                    sceHttpGetStatusCode(req, &status);
                    std::vector<char> buf(32 * 1024);
                    for (;;) {
                        int n = sceHttpReadData(req, buf.data(), buf.size());
                        if (n <= 0) break;
                        body.append(buf.data(), n);
                        if (body.size() > maxBytes) break;
                    }
                    ok = true;
                }
                sceHttpDeleteRequest(req);
            }
            sceHttpDeleteConnection(conn);
        }
        sceHttpDeleteTemplate(tmpl);
        return ok;
    }

    // GET em streaming (sem guardar nada), usado na lista M3U (dezenas de MB) e nos canais ao vivo em MPEG-TS cru.
    // Roda em QUALQUER thread. 'pos' = bytes ja entregues a onData (soma aqui). Com useRange e pos>0 pede "Range: bytes=pos-":
    // assim, se a conexao cair no meio, o download RETOMA de onde parou em vez de entregar a lista pela metade.
    // Retorna: 1 = leu ate o fim | 0 = caiu no meio / erro temporario (pode retomar) | -1 = servidor recusou | -2 = onData mandou parar
    int streamGet(const std::string &url, uint64_t &pos, const std::function<bool(const char *, size_t)> &onData, int &status,
                  unsigned recvTimeoutSec, bool useRange) {
        status = 0;
        if (!init()) return -1;
        uint64_t offset = useRange ? pos : 0;
        const char *ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        traceHttp("tmpl");
        int tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        if (tmpl < 0) return 0;
        sceHttpsDisableOption(tmpl, 0x1F);
        sceHttpSetAutoRedirect(tmpl, 1);
        traceHttp("tmpl-ok");
        const unsigned T = 20 * 1000 * 1000;                         // resolver/conectar/enviar: 20 s
        sceHttpSetResolveTimeOut(tmpl, T);
        sceHttpSetConnectTimeOut(tmpl, T);
        sceHttpSetSendTimeOut(tmpl, T);
        sceHttpSetRecvTimeOut(tmpl, recvTimeoutSec * 1000u * 1000u);   // paineis IPTV levam 30-60 s para montar a lista antes do 1o byte
        int result = 0;
        traceHttp("conn");
        int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1);
        if (conn >= 0) {
            traceHttp("req");
            int req = sceHttpCreateRequestWithURL(conn, 0 /*GET*/, url.c_str(), 0);
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "*/*", 0);
                if (offset > 0) {
                    char rb[48]; snprintf(rb, sizeof rb, "bytes=%llu-", (unsigned long long)offset);
                    sceHttpAddRequestHeader(req, "Range", rb, 0);
                }
                traceHttp("send");
                int rc = sceHttpSendRequest(req, NULL, 0);
                traceHttp("sent");
                if (rc >= 0 && sceHttpGetStatusCode(req, &status) >= 0) {
                    traceHttp("status");
                    uint64_t skip = 0; bool go = false;
                    if (status == 206) go = true;
                    else if (status == 200) { go = true; skip = offset; }        // o servidor ignorou o Range e mandou tudo de novo: descarta o que ja tinhamos
                    else if (status == 416 && offset > 0) result = 1;            // ja tinhamos o arquivo inteiro
                    else result = (offset > 0 || status >= 500 || status == 0) ? 0 : -1;   // 4xx logo de cara: recusado; depois de ja ter dados: tenta de novo
                    if (go) {
                        std::vector<char> buf(64 * 1024);
                        bool firstData = true;
                        for (;;) {
                            if (firstData) traceHttp("read");
                            int n = sceHttpReadData(req, buf.data(), buf.size());
                            if (n < 0) { result = 0; break; }                    // rede caiu no meio
                            if (n == 0) { result = 1; break; }                   // fim normal
                            size_t off = 0;
                            if (skip) { size_t k = (size_t)std::min<uint64_t>(skip, (uint64_t)n); skip -= k; off = k; }
                            if ((size_t)n > off) {
                                if (firstData) { firstData = false; traceHttp("data"); }
                                if (!onData(buf.data() + off, (size_t)n - off)) { result = -2; break; }
                                pos += (uint64_t)n - off;
                            }
                        }
                    }
                } else result = 0;
                sceHttpDeleteRequest(req);
            }
            sceHttpDeleteConnection(conn);
        }
        sceHttpDeleteTemplate(tmpl);
        return result;
    }

    // ---- Sessao com conexao reaproveitada (keep-alive): o alimentador HLS baixa playlist + segmentos do MESMO servidor
    // varias vezes por segundo. Antes cada GET criava conexao nova (handshake TLS completo no CPU fraco do PS4).
    struct Session { int tmpl = -1, conn = -1; std::string origin; unsigned recvSec = 8; };
    static std::string originOf(const std::string &url) {
        size_t se = url.find("://");
        if (se == std::string::npos) return "";
        size_t he = url.find('/', se + 3);
        return he == std::string::npos ? url : url.substr(0, he);
    }
    void closeSession(Session &s) {
        if (s.conn >= 0) sceHttpDeleteConnection(s.conn);
        if (s.tmpl >= 0) sceHttpDeleteTemplate(s.tmpl);
        unsigned keep = s.recvSec;
        s = Session(); s.recvSec = keep;
    }
    bool openSession(Session &s, const std::string &url) {
        closeSession(s);
        if (!init()) return false;
        const char *ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        s.tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        if (s.tmpl < 0) { s.tmpl = -1; return false; }
        sceHttpsDisableOption(s.tmpl, 0x1F);
        sceHttpSetAutoRedirect(s.tmpl, 1);
        const unsigned T = 15 * 1000 * 1000, TR = s.recvSec * 1000u * 1000u;   // conexao 15 s; envio/recepcao 8 s (conexao morta no meio de um segmento nao trava mais 15 s)
        sceHttpSetResolveTimeOut(s.tmpl, T);
        sceHttpSetConnectTimeOut(s.tmpl, T);
        sceHttpSetSendTimeOut(s.tmpl, TR);
        sceHttpSetRecvTimeOut(s.tmpl, TR);
        s.conn = sceHttpCreateConnectionWithURL(s.tmpl, url.c_str(), 1 /*keep-alive*/);
        if (s.conn < 0) { s.conn = -1; closeSession(s); return false; }
        s.origin = originOf(url);
        return true;
    }
    // GET reaproveitando a conexao de 's' (so use de UMA thread por sessao). Se a conexao antiga caiu, refaz uma vez.
    // Leitura interrompida no meio (erro de rede) conta como falha: antes devolvia o segmento cortado como se fosse bom.
    bool getKA(Session &s, const std::string &url, std::string &body, int &status, size_t maxBytes = 1536 * 1024) {
        for (int attempt = 0; attempt < 2; attempt++) {
            status = 0; body.clear();
            bool fresh = false;
            if (s.conn < 0 || s.origin != originOf(url)) { if (!openSession(s, url)) return false; fresh = true; }
            int req = sceHttpCreateRequestWithURL(s.conn, 0 /*GET*/, url.c_str(), 0);
            bool ok = false, clean = false;
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "*/*", 0);
                if (sceHttpSendRequest(req, NULL, 0) >= 0) {
                    sceHttpGetStatusCode(req, &status);
                    std::vector<char> buf(64 * 1024);
                    ok = clean = true;
                    for (;;) {
                        int n = sceHttpReadData(req, buf.data(), buf.size());
                        if (n < 0) { ok = clean = false; break; }
                        if (n == 0) break;
                        body.append(buf.data(), n);
                        if (body.size() > maxBytes) { clean = false; break; }   // resposta nao lida ate o fim: nao reaproveita
                    }
                }
                sceHttpDeleteRequest(req);
            }
            if (ok) { if (!clean) closeSession(s); return true; }
            closeSession(s);
            if (fresh) return false;                                            // ja era uma conexao nova: falha de verdade
        }
        return false;
    }

    // GET de um trecho [from, to] (inclusive) reaproveitando a conexao de 's' (so UMA thread por sessao). Devolve false em erro de rede.
    // 'status': 206 = ok | 416 = alem do fim | 200 = o servidor ignorou o Range (nao le o resto: derruba a conexao).
    bool getRange(Session &s, const std::string &url, uint64_t from, uint64_t to, std::string &body, int &status, uint64_t *total = nullptr) {
        for (int attempt = 0; attempt < 2; attempt++) {
            status = 0; body.clear();
            bool fresh = false;
            if (s.conn < 0 || s.origin != originOf(url)) { if (!openSession(s, url)) return false; fresh = true; }
            int req = sceHttpCreateRequestWithURL(s.conn, 0 /*GET*/, url.c_str(), 0);
            bool ok = false, clean = false;
            size_t want = (size_t)(to - from + 1);
            body.reserve(want);
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "*/*", 0);
                char rb[64]; snprintf(rb, sizeof rb, "bytes=%llu-%llu", (unsigned long long)from, (unsigned long long)to);
                sceHttpAddRequestHeader(req, "Range", rb, 0);
                if (sceHttpSendRequest(req, NULL, 0) >= 0 && sceHttpGetStatusCode(req, &status) >= 0) {
                    ok = true;
                    if (total) {                                          // "Content-Range: bytes 0-0/TOTAL": o tamanho vem numa requisicao so
                        char *hd = nullptr; size_t hn = 0;
                        if (sceHttpGetAllResponseHeaders(req, &hd, &hn) >= 0 && hd && hn) {
                            std::string h(hd, hn);
                            for (auto &c : h) c = (char)tolower((unsigned char)c);
                            size_t k = h.find("content-range:");
                            if (k != std::string::npos) {
                                size_t sl = h.find('/', k), eol = h.find('\n', k);
                                if (sl != std::string::npos && (eol == std::string::npos || sl < eol)) *total = strtoull(h.c_str() + sl + 1, nullptr, 10);
                            }
                        }
                    }
                    if (status == 206 || status == 416 || (status == 200 && from == 0)) {
                        clean = true;
                        std::vector<char> buf(128 * 1024);
                        for (;;) {
                            int n = sceHttpReadData(req, buf.data(), buf.size());
                            if (n < 0) { ok = clean = false; break; }
                            if (n == 0) break;
                            body.append(buf.data(), (size_t)n);
                            if (body.size() >= want && (status == 200 || body.size() > want + 4096)) { clean = false; break; }   // resposta maior que o pedido: nao reaproveita
                        }
                    }
                }
                sceHttpDeleteRequest(req);
            }
            if (ok) { if (!clean) closeSession(s); if (body.size() > want) body.resize(want); return true; }
            closeSession(s);
            if (fresh) return false;                                            // ja era conexao nova: falha de verdade
        }
        return false;
    }
};

static NetClient g_net;

// ---------- Plataforma ----------
struct Ps4Platform : Platform {
    App *app = nullptr;
    std::string pendingText;                 // texto digitado no teclado na tela (@streamer ou link M3U)
    bool hasPendingText = false;
    bool kbdRequested = false;               // UI pediu o teclado
    std::string pendingPlayUrl, pendingPlayTitle;
    std::vector<std::string> pendingEpUrls, pendingEpTitles;
    int pendingEpIndex = -1;
    bool httpGet(const std::string &url, std::string &body) override {
        int st = 0;
        bool ok = g_net.get(url, body, st);
        return ok && st >= 200 && st < 300;
    }
    // Download da lista M3U em pedacos; entre os pedacos redesenha o progresso (progressTick).
    std::function<bool()> progressTick;      // chamado ~7x/s durante o download (redesenha o menu e le o O); true = cancelar
    bool cancelled = false;
    bool wasCancelled() override { return cancelled; }
    std::atomic<int> m3uRetry{0};            // >0 = a conexao caiu e o download esta sendo retomado (numero da tentativa)
    // Download da lista (roda na THREAD DE REDE). Se a conexao cair, retoma de onde parou ("Range") em vez de entregar a lista pela metade.
    bool fetchM3U(const std::string &url, const netstream::Push &push, int &st) {
        netstream::Attempt attempt = [&](uint64_t &pos, const std::function<bool(const char *, size_t)> &onData, int &status) {
            return g_net.streamGet(url, pos, onData, status, 60, true);
        };
        bool ok = netstream::resumable(attempt, push, st, [this](int n) {
            if (n > 0) trace("m3u-retomando");
            m3uRetry = n;                                         // >0 = reconectando (a tela avisa); 0 = dados voltaram a chegar
        });
        if (!ok) trace("m3u-desistiu");
        return ok;
    }
    // Entrega em pedacos para a thread principal (que continua desenhando; O cancela mesmo se o servidor demorar a responder).
    bool httpStream(const std::string &url, const std::function<bool(const char *, size_t)> &sink0) override {
        cancelled = false; m3uRetry = 0;
        g_m3uTag = g_safeM3U ? "m3u-s-" : "m3u-t-";
        trace(g_safeM3U ? "m3u-sync-inicio" : "m3u-inicio");
        g_traceHttp = true;
        g_net.init();                                             // inicializa a rede na thread principal
        traceHttp("net-ok");
        bool firstChunk = true;
        std::function<bool(const char *, size_t)> sink = [&](const char *d, size_t n) {
            if (firstChunk) { firstChunk = false; trace("m3u-parse"); }
            return sink0(d, n);
        };
        std::function<bool()> tickFn = [this]() -> bool {
            int rt = m3uRetry.load();
            if (rt > 0 && app) app->say("Conexao caiu - retomando de onde parou (tentativa " + std::to_string(rt) + ")...  (O cancela)");
            return progressTick ? progressTick() : false;
        };
        int st = 0; bool ok = false;
        if (g_safeM3U) {
            // MODO SEGURO: baixa nesta mesma thread (sem thread de rede). O O so e lido entre um pedaco e outro.
            if (tickFn()) cancelled = true;                       // mostra "Baixando..." logo de cara
            double lastTick = monoSec();
            netstream::Push push = [&](const char *d, size_t n) -> bool {
                if (cancelled) return false;
                if (n > 0 && !sink(d, n)) { cancelled = true; return false; }
                double t = monoSec();
                if (n == 0 || t - lastTick >= 0.15) { lastTick = t; if (tickFn()) { cancelled = true; return false; } }
                return true;
            };
            if (!cancelled) ok = fetchM3U(url, push, st);
        } else {
            netstream::Fetch fetch = [this, url](const netstream::Push &push, int &st2) -> bool { return fetchM3U(url, push, st2); };
            ok = netstream::run(fetch, sink, tickFn, st, cancelled);
        }
        g_traceHttp = false;
        trace(ok ? "m3u-download-ok" : cancelled ? "m3u-cancelado" : "m3u-download-falhou");
        return ok;
    }
    void play(const std::string &url, const std::string &title) override {
        pendingPlayUrl = url; pendingPlayTitle = title;
        // play() simples (canal/filme/kick): limpa lista de episodios
        pendingEpUrls.clear(); pendingEpTitles.clear(); pendingEpIndex = -1;
    }
    void setEpisodeList(const std::vector<std::string> &urls, const std::vector<std::string> &titles, int index) override {
        pendingEpUrls = urls; pendingEpTitles = titles; pendingEpIndex = index;
    }
    void requestText(const std::string &) override { kbdRequested = true; }
    // dados guardados em /data (lista M3U, ultimo streamer). Leitura via libkernel (o app nao usa fread).
    static std::string settingPath(const std::string &k) { return "/data/kickiptv_" + k + ".txt"; }
    std::string loadSetting(const std::string &k) override {
        int fd = sceKernelOpen(settingPath(k).c_str(), 0, 0);
        if (fd < 0) return "";
        std::string out; char buf[1024];
        for (;;) {
            long n = (long)sceKernelRead(fd, buf, sizeof buf);
            if (n <= 0) break;
            out.append(buf, (size_t)n);
            if (out.size() > 4096) break;
        }
        sceKernelClose(fd);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
        return out;
    }
    void saveSetting(const std::string &k, const std::string &v) override {
        FILE *f = fopen(settingPath(k).c_str(), "wb");
        if (!f) return;
        if (!v.empty()) fwrite(v.data(), 1, v.size(), f);
        fclose(f);
    }

    // ---- copia local da lista M3U: abrir o app nao baixa tudo de novo ----
    // Dois arquivos (a/b) se revezam: o download novo grava no que NAO esta valido; so quando termina inteiro a configuracao
    // "listcache" ("<0|1>|<url>") passa a apontar para ele. Se a conexao cair no meio, a copia antiga continua valida.
    static std::string cachePath(int slot) { return slot ? "/data/kickiptv_list_b.m3u" : "/data/kickiptv_list_a.m3u"; }
    FILE *cacheF = nullptr;
    int cacheSlot = 0;
    bool cacheBad = false;
    // le "listcache": slot valido (0/1) e a url a que ele pertence. slot = -1 se nao ha copia.
    void cacheInfo(int &slot, std::string &url) {
        slot = -1; url.clear();
        std::string v = loadSetting("listcache");
        size_t bar = v.find('|');
        if (v.size() < 3 || bar != 1 || (v[0] != '0' && v[0] != '1')) return;
        slot = v[0] - '0'; url = v.substr(2);
    }
    bool cacheLoad(const std::string &url, const std::function<bool(const char *, size_t)> &sink) override {
        cancelled = false;
        int slot; std::string cu; cacheInfo(slot, cu);
        if (slot < 0 || cu != url) return false;                       // outra lista (ou nenhuma): baixa
        int fd = sceKernelOpen(cachePath(slot).c_str(), 0, 0);
        if (fd < 0) return false;
        trace("m3u-cache-inicio");
        std::vector<char> buf(256 * 1024);
        bool ok = true, first = true;
        double last = monoSec();
        for (;;) {
            long n = (long)sceKernelRead(fd, buf.data(), buf.size());
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            if (first) { first = false; trace("m3u-cache-parse"); }
            if (!sink(buf.data(), (size_t)n)) { ok = false; break; }
            double t = monoSec();
            if (t - last >= 0.15) { last = t; if (progressTick && progressTick()) { cancelled = true; ok = false; break; } }   // redesenha e deixa cancelar com O
        }
        sceKernelClose(fd);
        trace(ok ? "m3u-cache-ok" : "m3u-cache-falhou");
        return ok;
    }
    void cacheBegin() override {
        if (cacheF) { fclose(cacheF); cacheF = nullptr; }
        int cur; std::string cu; cacheInfo(cur, cu);
        cacheSlot = cur == 0 ? 1 : 0;                                  // grava no arquivo que nao esta em uso
        cacheF = fopen(cachePath(cacheSlot).c_str(), "wb");
        cacheBad = cacheF == nullptr;
    }
    void cacheWrite(const char *d, size_t n) override {
        if (cacheF && !cacheBad && fwrite(d, 1, n, cacheF) != n) cacheBad = true;   // disco cheio etc.
    }
    void cacheEnd(const std::string &url, bool complete) override {
        if (!cacheF) return;
        bool closed = fclose(cacheF) == 0; cacheF = nullptr;
        if (complete && closed && !cacheBad) {
            int old; std::string ou; cacheInfo(old, ou);
            saveSetting("listcache", std::string(cacheSlot ? "1" : "0") + "|" + url);
            if (old >= 0 && old != cacheSlot) remove(cachePath(old).c_str());
        } else remove(cachePath(cacheSlot).c_str());                   // descarta; a copia antiga (se houver) continua valendo
    }
    void cacheClear() override {
        saveSetting("listcache", "");
        remove(cachePath(0).c_str()); remove(cachePath(1).c_str());
    }
};

// (FbRenderer agora vive em fbrender.hpp)

// ---------- Tela de inicializacao (escrita direta no video, sem ler nada) ----------
// Mostra cada passo da abertura. Se o app travar/cair, a foto da tela mostra ate onde chegou.
struct BootScreen {
    uint32_t *fb = nullptr; int W = 0, H = 0, y = 40;
    void init(void *p, int w, int h) {
        fb = (uint32_t *)p; W = w; H = h;
        for (size_t i = 0; i < (size_t)W * H; i++) fb[i] = 0xFF000000u;
        text(48, 28, "Kick IPTV", 0xFF53FC18u, 4);
        y = 120;
    }
    void text(int x, int yy, const char *t, uint32_t c, int sc) {
        for (; *t; t++, x += 8 * sc) {
            unsigned ch = (unsigned char)*t;
            if (ch < 32 || ch > 126) ch = '?';
            const unsigned char *g = kFont8x16[ch - 32];
            for (int r = 0; r < 16; r++)
                for (int k = 0; k < 8; k++)
                    if ((g[r] >> (7 - k)) & 1)
                        for (int dy = 0; dy < sc; dy++)
                            for (int dx = 0; dx < sc; dx++) {
                                int px = x + k * sc + dx, py = yy + r * sc + dy;
                                if (px >= 0 && px < W && py >= 0 && py < H) fb[(size_t)py * W + px] = c;
                            }
        }
    }
    void line(const std::string &t, uint32_t c = 0xFFE8ECF4u) { if (fb) { text(48, y, t.c_str(), c, 2); y += 40; } }
};

// ---------- Registro de passos em /data/kickiptv.log ----------
// Se o app cair, na proxima abertura o menu mostra em qual passo ele parou.
static std::string g_prevStage;           // (leitura do log anterior removida: usava fread, que o app original nao importava)
static void traceInit() {
    // le o log da ultima vez (libkernel, como as configuracoes) para mostrar na tela em que passo o app parou
    int fd = sceKernelOpen("/data/kickiptv.log", 0, 0);
    if (fd >= 0) {
        std::string all; char buf[1024];
        for (;;) { long n = (long)sceKernelRead(fd, buf, sizeof buf); if (n <= 0) break; all.append(buf, (size_t)n); if (all.size() > 65536) break; }
        sceKernelClose(fd);
        while (!all.empty() && (all.back() == '\n' || all.back() == '\r')) all.pop_back();
        size_t p = all.rfind('\n');
        std::string last = p == std::string::npos ? all : all.substr(p + 1);
        if (!last.empty() && last != "fim" && last != "saindo" && last != "menu-ok" && last != "inicio") g_prevStage = last;
    }
    FILE *f = fopen("/data/kickiptv.log", "wb");
    if (f) { fwrite("inicio\n", 1, 7, f); fclose(f); }
}
static std::mutex g_traceMu;
static void trace(const char *stage) {
    std::lock_guard<std::mutex> lk(g_traceMu);
    FILE *f = fopen("/data/kickiptv.log", "ab");
    if (f) { fwrite(stage, 1, strlen(stage), f); fwrite("\n", 1, 1, f); fclose(f); }
}

// ---------- AvPlayer (libSceAvPlayer) - structs proprias para nao depender do header ----------
typedef void *(*KAvAlloc)(void *arg, uint32_t align, uint32_t size);
typedef void (*KAvFree)(void *arg, void *mem);
typedef int (*KAvFileOpen)(void *arg, const char *name);
typedef int (*KAvFileClose)(void *arg);
typedef int (*KAvFileRead)(void *arg, uint8_t *buf, uint64_t pos, uint32_t len);
typedef uint64_t (*KAvFileSize)(void *arg);
struct KAvInitData {
    void *memObj; KAvAlloc allocate; KAvFree deallocate; KAvAlloc allocateTexture; KAvFree deallocateTexture;   // memoryReplacement
    void *fileObj; KAvFileOpen fOpen; KAvFileClose fClose; KAvFileRead fRead; KAvFileSize fSize;                // fileReplacement
    void *eventObj; void *eventCb;                                                                              // eventReplacement
    int32_t debugLevel;
    uint32_t basePriority;
    int32_t numOutputVideoFrameBuffers;
    bool autoStart;
    uint8_t reserved[3];
    const char *defaultLanguage;
};
struct KAvFrameInfo {
    uint8_t *pData;
    uint32_t reserved;
    uint64_t timeStamp;
    uint8_t details[216];      // union de detalhes (video: width@0 height@4; audio: ch@0 rate@4 size@8)
};
typedef unsigned long KAvHandle;          // 64 bits (LP64); nao usa uintptr_t
extern "C" {
int sceSysmoduleLoadModule(uint16_t id);
KAvHandle sceAvPlayerInit(KAvInitData *init);
int sceAvPlayerAddSource(KAvHandle h, const char *url);
bool sceAvPlayerIsActive(KAvHandle h);
bool sceAvPlayerGetVideoData(KAvHandle h, KAvFrameInfo *fi);
bool sceAvPlayerGetAudioData(KAvHandle h, KAvFrameInfo *fi);
int sceAvPlayerStop(KAvHandle h);
int sceAvPlayerClose(KAvHandle h);
int sceAvPlayerPause(KAvHandle h);
int sceAvPlayerResume(KAvHandle h);
int sceAvPlayerJumpToTime(KAvHandle h, uint64_t jumpTimeMsec);
uint64_t sceAvPlayerCurrentTime(KAvHandle h);
typedef int (*KAvLogCb)(void *ud, const char *fmt, va_list ap);
int sceAvPlayerSetLogCallback(KAvLogCb cb, void *ud);
}

// Quantas desmontagens (fechar player/threads) estao rodando em segundo plano. O botao O nao espera por elas.
static std::atomic<int> g_tearing{0};

// Stop+Close em outra thread com limite de tempo: se o player travar, abandona o handle e segue em frente
static bool avStopClose(KAvHandle h, int timeoutMs = 5000) {
    if (!h) return true;
    std::atomic<bool> *done = new std::atomic<bool>(false);
    std::thread t([h, done] { sceAvPlayerStop(h); sceAvPlayerClose(h); done->store(true); });
    for (int i = 0; i < timeoutMs / 5 && !done->load(); i++) sceKernelUsleep(5000);
    if (done->load()) { t.join(); delete done; return true; }
    t.detach();                               // vaza de proposito: a thread ainda usa 'done'
    return false;
}

// Log interno do AvPlayer (capturado para o diagnostico) e contadores dos alocadores
static std::mutex g_avlogM;
static std::vector<std::string> g_avlog;
static int avLogCb(void *, const char *fmt, va_list ap) {
    char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    std::string t = b;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    std::lock_guard<std::mutex> l(g_avlogM);
    if (g_avlog.size() < 400) g_avlog.push_back(t);
    return 0;
}
static std::atomic<int> g_allocN{0}, g_allocFail{0}, g_texN{0}, g_texFail{0};

static void *alignedAlloc(size_t align, size_t size) {
    if (align < 16) align = 16;
    void *raw = malloc(size + align + sizeof(void *));
    if (!raw) return NULL;
    unsigned long a = ((unsigned long)raw + sizeof(void *) + align - 1) & ~(unsigned long)(align - 1);
    ((void **)a)[-1] = raw;
    return (void *)a;
}
static void alignedFree(void *p) { if (p) free(((void **)p)[-1]); }
static void *avAlloc(void *, uint32_t align, uint32_t size) {
    g_allocN++; void *p = alignedAlloc(align, size);
    if (!p) { if (g_allocFail++ == 0) trace("av-malloc-FALHOU"); }
    return p;
}
static void avFree(void *, void *p) { alignedFree(p); }

// memoria "de textura" (os quadros de video): memoria direta mapeada, como nos samples da Sony
struct TexMem { off_t phys; size_t size; };
// Tipo da memoria dos quadros de video: 0 = WB_ONION (RAM comum COM cache: a CPU le rapido) | 3 = WC_GARLIC (memoria de GPU:
// a CPU le a ~15 MB/s, foi o que travava o video). Comecamos com Onion; se o decoder nao entregar quadros, cai para Garlic.
static std::atomic<int> g_texType{0};
static std::map<void *, TexMem> g_tex;
static std::mutex g_texM;                      // o AvPlayer chama alocar/liberar de threads proprias
// Quantos bytes existem de 'p' ate o fim do bloco de textura que o contem (0 = nao e um bloco nosso). Evita ler alem do mapeado.
static size_t texAvail(const void *p) {
    std::lock_guard<std::mutex> l(g_texM);
    auto it = g_tex.upper_bound((void *)p);
    if (it == g_tex.begin()) return 0;
    --it;
    const char *base = (const char *)it->first, *q = (const char *)p;
    if (q < base || q >= base + it->second.size) return 0;
    return (size_t)(base + it->second.size - q);
}
static void *avAllocTex(void *, uint32_t align, uint32_t size) {
    g_texN++;
    size_t al = align < 0x10000 ? 0x10000 : align;
    size_t sz = (size + 0xFFFF) & ~(size_t)0xFFFF;
    off_t phys = 0; void *addr = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, g_texType.load(), &phys) < 0) { if (g_texFail++ == 0) trace("av-textura-FALHOU"); return NULL; }
    if (sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, al) < 0) { g_texFail++; return NULL; }
    { std::lock_guard<std::mutex> l(g_texM); g_tex[addr] = {phys, sz}; }
    return addr;
}
static void avFreeTex(void *, void *p) {
    TexMem tm;
    {
        std::lock_guard<std::mutex> l(g_texM);
        auto it = g_tex.find(p);
        if (it == g_tex.end()) return;
        tm = it->second; g_tex.erase(it);
    }
    sceKernelReleaseDirectMemory(tm.phys, tm.size);
}

// ---------- Copia rapida de memoria "write-combined" (a memoria de textura do decoder e type 3 = WC/GARLIC) ----------
// LER essa memoria byte a byte pela CPU e lentissimo (cada leitura e um acesso sem cache). Era ISSO que travava o video: o HUD
// mostrava "desenho 201 ms" (4 fps) so para ler um quadro 864x480. Aqui lemos em blocos de 64 bytes com "movntdqa"
// (leitura em fila para memoria WC, SSE4.1; o Jaguar do PS4 tem) para um buffer comum em RAM com cache, e o blit le de la.
typedef long long KV2 __attribute__((vector_size(16)));
static void copyFromWC(uint8_t *dst, const uint8_t *src, size_t n) {
    size_t i = 0;
    while (i < n && ((uintptr_t)(src + i) & 15)) { dst[i] = src[i]; i++; }
    for (; i + 64 <= n; i += 64) {
        KV2 a, b, c, d;
        __asm__ volatile("movntdqa (%4), %0\n\tmovntdqa 16(%4), %1\n\tmovntdqa 32(%4), %2\n\tmovntdqa 48(%4), %3"
                         : "=&x"(a), "=&x"(b), "=&x"(c), "=&x"(d) : "r"(src + i) : "memory");
        __builtin_memcpy(dst + i, &a, 16); __builtin_memcpy(dst + i + 16, &b, 16);
        __builtin_memcpy(dst + i + 32, &c, 16); __builtin_memcpy(dst + i + 48, &d, 16);
    }
    for (; i < n; i++) dst[i] = src[i];
}

// ---------- NV12 -> BGRA: ver blit.hpp (rapido, em varias threads) ----------
static void blitNV12(const uint8_t *src, int sw, int sh, int pitch, int yRows, uint32_t *dst, int dw, int dh) {
    blit::pool().start(4);                     // 3 trabalhadores + a thread principal (so cria na primeira vez)
    blit::nv12(src, sw, sh, pitch, yRows, dst, dw, dh);
}

// ---------- Audio: thread propria (sceAudioOutOutput bloqueia ate o buffer tocar) ----------
// O AvPlayer entrega PCM S16 intercalado (1, 2 ou 6+ canais, 44.1k/48k). Aqui vira estereo 48k.
struct AudioPump {
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<int> state{0};          // 0 parado | 1 aguardando audio | 2 tocando | -1 erro ao abrir o audio
    std::atomic<int> openRc{0};
    std::atomic<unsigned> rate{0}, chans{0};
    std::atomic<unsigned> chunks{0};
    KAvHandle h = 0;
    std::mutex hm;                       // protege 'h': a troca de pedaco muda o player SEM fechar a porta de audio
    int port = -1;
    static const int GRAIN = 1024;

    void start(KAvHandle handle) {
        stop();
        { std::lock_guard<std::mutex> l(hm); h = handle; }
        run = true; state = 1; chunks = 0; rate = chans = 0; openRc = 0;
        th = std::thread([this] { loop(); });
    }
    // Troca o player que alimenta o audio. Antes cada troca de pedaco (a cada poucos segundos) fechava e reabria a porta
    // do AudioOut: isso causa um "buraco" de audio e arrasta o video junto. Agora a porta continua aberta.
    // Ao retornar, a thread de audio nao esta mais dentro de uma chamada ao player antigo (so entao ele pode ser fechado).
    void retarget(KAvHandle handle) { std::lock_guard<std::mutex> l(hm); h = handle; }
    void stop() {
        run = false;
        if (th.joinable()) th.join();
        if (port > 0) { sceAudioOutClose(port); port = -1; }
        state = 0;
    }
    static int16_t clip(float v) { return v > 32767.f ? 32767 : (v < -32768.f ? -32768 : (int16_t)v); }

    void loop() {
        std::vector<int16_t> acc;
        std::vector<uint8_t> pcm;
        float prevL = 0, prevR = 0, phase = 0;
        while (run) {
            KAvFrameInfo au; memset(&au, 0, sizeof au);
            unsigned ch = 0, sr = 0, sz = 0;
            bool got = false;
            {   // a chamada ao player e a COPIA do quadro de audio ficam sob o mutex (o buffer e do player e some quando ele fecha)
                std::lock_guard<std::mutex> l(hm);
                if (h && sceAvPlayerGetAudioData(h, &au) && au.pData) {
                    ch = *(uint16_t *)(au.details + 0); sr = *(uint32_t *)(au.details + 4); sz = *(uint32_t *)(au.details + 8);
                    if (ch >= 1 && ch <= 8 && sr >= 8000 && sr <= 192000 && sz >= 2u * ch && sz <= (1u << 20)) {
                        pcm.assign((const uint8_t *)au.pData, (const uint8_t *)au.pData + sz);
                        got = true;
                    }
                }
            }
            if (!got) { sceKernelUsleep(3000); continue; }
            rate = sr; chans = ch;
            if (port < 0 && state != -1) {
                int p = sceAudioOutOpen(0xFF /*usuario do sistema*/, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                openRc = p;
                if (p > 0) { port = p; state = 2; } else state = -1;      // se falhar, so descarta o audio (o relogio do player continua)
            }
            if (port <= 0) continue;
            const int16_t *in = (const int16_t *)pcm.data();
            size_t frames = sz / (2 * ch);
            float step = (float)sr / 48000.0f;
            for (size_t i = 0; i < frames; i++) {
                const int16_t *f = in + i * ch;
                float L, R;
                if (ch == 1) L = R = f[0];
                else if (ch == 2) { L = f[0]; R = f[1]; }
                else if (ch >= 6) { L = f[0] + 0.707f * f[2] + 0.707f * f[4]; R = f[1] + 0.707f * f[2] + 0.707f * f[5]; }
                else { L = f[0]; R = f[1]; }
                while (phase < 1.0f) {
                    acc.push_back(clip(prevL + (L - prevL) * phase));
                    acc.push_back(clip(prevR + (R - prevR) * phase));
                    phase += step;
                }
                phase -= 1.0f; prevL = L; prevR = R;
            }
            size_t used = 0;
            while (run && acc.size() - used >= (size_t)GRAIN * 2) {
                sceAudioOutOutput(port, acc.data() + used);
                used += (size_t)GRAIN * 2; chunks++;
            }
            if (used) acc.erase(acc.begin(), acc.begin() + used);
            if (acc.size() > 48000 * 2) acc.clear();            // seguranca: nunca acumula mais de ~1s
        }
    }
};

// Qualidade: 0 = "Fluido" (720p max, 30 fps) | 1 = "Maxima" (720p60) | 2 = "Ultra" (1080p60)
static std::atomic<int> g_quality{1};
static const char *kQualityName[3] = {"Fluido 30fps", "Maxima 720p60", "Ultra 1080p60"};

// Reconexao automatica: se a live congelar ou a rede/pipeline quebrar, o app reabre a transmissao sozinho (ver Player::wantReconnect).
static std::string g_reconnectNote;          // motivo da ultima reconexao (mostrado na tela "Conectando...")
static int g_autoRestarts = 0;               // quantas reconexoes automaticas ja houve (HUD)

// ---------- Alimentador HLS -> AvPlayer ----------
// O AvPlayer do PS4 nao abre https/HLS direto (AddSource=806A0002). Entao o app baixa o HLS (playlist + segmentos .ts)
// com o cliente HTTP proprio e entrega os bytes ao player pelos callbacks de arquivo ("fileReplacement").
struct HlsFeeder {
    StreamBuf sb;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<int> segs{0}, opens{0}, reads{0}, failed{0};
    std::atomic<bool> streamEnded{false};       // a playlist trouxe EXT-X-ENDLIST (a live acabou)
    unsigned long long localId = 0;             // numero PROPRIO de cada segmento baixado (so cresce). O numero da playlist pode voltar atras
                                                // (streamer reconectou); antes o player ignorava para sempre os segmentos novos e a imagem congelava.
    FILE *outFile = nullptr;                    // modo "arquivo que cresce": segmentos vao para /data/live.ts
    std::atomic<unsigned long long> outBytes{0};
    std::atomic<bool> discardBuf{false};        // modo "pedacos": ninguem le o StreamBuf, entao nao acumula (antes crescia ate 48 MB)
    bool raw = false;                           // canal em MPEG-TS cru (sem playlist): o fluxo continuo e cortado em pedacos de ~2 s (tschunk.hpp)
    NetClient::Session sess;                    // conexao HTTP reaproveitada (so a thread do alimentador usa)
    std::mutex lm;
    std::string status;
    std::string url;
    std::atomic<int> variantFps{0}, variantH{0};
    std::atomic<double> dlMs{0}, dlMsMax{0}, lastSegDur{0};        // tempo de download do ultimo segmento (diagnostico)
    static double nowSecF() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
    std::mutex rm;                                                   // ultimos segmentos (para tocar em "pedacos")
    std::deque<std::pair<unsigned long long, std::string>> recent;

    void setStatus(const std::string &t) { std::lock_guard<std::mutex> l(lm); status = t; }
    std::string getStatus() { std::lock_guard<std::mutex> l(lm); return status; }
    void fail(const std::string &t) { setStatus("ERRO: " + t); failed = 1; }

    void start(const std::string &u, bool rawTs = false) {
        join(); sb.reset();
        { std::lock_guard<std::mutex> l(rm); recent.clear(); }
        raw = rawTs;
        url = u; segs = opens = reads = failed = 0; localId = 0; streamEnded = false; run = true; discardBuf = false; dlMs = dlMsMax = 0; setStatus("iniciando");
        th = std::thread([this] { loop(); g_net.closeSession(sess); });
    }
    void abort() { run = false; sb.abort(); }                // libera leituras bloqueadas do player
    void join() { abort(); if (th.joinable()) th.join(); }

    static void sleepMs(int ms, std::atomic<bool> &run) { for (int i = 0; i < ms / 50 && run; i++) sceKernelUsleep(50000); }

    // Canal ao vivo em MPEG-TS cru: uma conexao que nunca termina. Corta em pedacos (cada um comeca em PAT+PMT+quadro-chave,
    // como um segmento HLS) e entrega ao mesmo pipeline do HLS. Se a conexao cair, reconecta sozinho algumas vezes.
    void loopRaw() {
        setStatus("conectando ao canal (MPEG-TS)");
        TsChunker ck;
        ck.emit = [this](std::string &&c) {
            size_t sz = c.size();
            if (!discardBuf.load()) sb.append((const uint8_t *)c.data(), c.size());
            { std::lock_guard<std::mutex> l(rm); recent.push_back({++localId, std::move(c)}); while (recent.size() > 8) recent.pop_front(); }
            segs++;
            setStatus("ao vivo: pedaco " + std::to_string(segs.load()) + " (" + std::to_string(sz / 1024) + " KB)");
        };
        int errs = 0; uint64_t pos = 0; double okSince = 0;
        while (run) {
            int st = 0; uint64_t total0 = pos;
            int r = g_net.streamGet(url, pos, [&](const char *d, size_t n) -> bool {
                if (!run) return false;
                ck.feed(d, n, nowSecF());
                if (okSince == 0) okSince = nowSecF();
                return !ck.notTs;
            }, st, 12, false);
            if (!run) return;
            if (ck.notTs) { fail("o canal nao devolveu video MPEG-TS (HTTP " + std::to_string(st) + ")"); return; }
            if (r == -1) { fail("canal recusado: HTTP " + std::to_string(st)); return; }
            if (pos == total0 && segs.load() == 0) { if (++errs > 2) { fail("canal sem resposta (HTTP " + std::to_string(st) + ")"); return; } }
            else if (okSince > 0 && nowSecF() - okSince > 20) errs = 0;
            if (r == 1 && segs.load() == 0 && ck.vPid < 0 && pos < 200000) { fail("o canal terminou sem video (HTTP " + std::to_string(st) + ")"); return; }
            if (++errs > 6) { fail("a transmissao parou (rede)"); return; }
            setStatus("reconectando ao canal");
            okSince = 0;
            sleepMs(1000, run);
        }
    }

    void loop() {
        if (raw) { loopRaw(); return; }
        std::string body; int st = 0;
        setStatus("baixando playlist mestre");
        if (!g_net.getKA(sess, url, body, st) || st < 200 || st >= 300) { fail("playlist mestre HTTP " + std::to_string(st) + (body.empty() ? " (sem resposta)" : "")); return; }
        std::string mediaUrl = url;
        if (hlsIsMaster(body)) {
            auto v = hlsParseMaster(body);
            int i = hlsPickVariant(v, g_quality.load() == 2 ? 1080 : 720, g_quality.load() == 0 ? 30 : 0);
            if (i < 0) { fail("playlist mestre sem variantes"); return; }
            mediaUrl = hlsResolve(url, v[i].uri);
            setStatus("variante " + std::to_string(v[i].width) + "x" + std::to_string(v[i].height) + " " + std::to_string((int)(v[i].fps + 0.5)) +
                      "fps bw=" + std::to_string(v[i].bandwidth));
            variantFps = (int)(v[i].fps + 0.5); variantH = v[i].height;
        }
        unsigned long long next = 0; bool first = true; int errs = 0;
        while (run) {
            if (!g_net.getKA(sess, mediaUrl, body, st) || st < 200 || st >= 300) {
                if (++errs > 6) { fail("playlist de midia HTTP " + std::to_string(st)); return; }
                sleepMs(1000, run); continue;
            }
            errs = 0;
            HlsMedia md = hlsParseMedia(body);
            if (md.hasMap) { fail("fMP4 (EXT-X-MAP) ainda nao suportado"); return; }
            if (md.segs.empty()) { sleepMs(1000, run); continue; }
            if (first) { size_t k = md.segs.size() > 3 ? md.segs.size() - 3 : 0; next = md.segs[k].seq; first = false; }
            else if (md.segs.back().seq + 1 < next) {            // a numeracao da playlist voltou atras (live reiniciou): ressincroniza nos 3 mais novos
                size_t k = md.segs.size() > 3 ? md.segs.size() - 3 : 0; next = md.segs[k].seq;
                setStatus("playlist reiniciou: ressincronizando");
            }
            bool got = false;
            for (auto &sg : md.segs) {
                if (!run) break;
                if (sg.seq < next) continue;
                std::string seg; int ss = 0;
                double d0 = nowSecF();
                if (!g_net.getKA(sess, hlsResolve(mediaUrl, sg.uri), seg, ss, 12u << 20) || ss < 200 || ss >= 300 || seg.empty()) {
                    if (++errs > 6) { fail("segmento HTTP " + std::to_string(ss)); return; }
                    break;
                }
                if (outFile) {
                    fwrite(seg.data(), 1, seg.size(), outFile); fflush(outFile); outBytes += seg.size();
                    if (outBytes > 1200000000ULL) { fail("limite de gravacao (1,2 GB) atingido"); return; }
                } else if (!discardBuf.load()) sb.append((const uint8_t *)seg.data(), seg.size());
                { double dm = (nowSecF() - d0) * 1000.0; dlMs = dm; if (dm > dlMsMax.load()) dlMsMax = dm; }
                lastSegDur = sg.dur;
                { std::lock_guard<std::mutex> l(rm); recent.push_back({++localId, seg}); while (recent.size() > 8) recent.pop_front(); }
                next = sg.seq + 1; segs++; got = true;
                setStatus("ao vivo: segmento " + std::to_string(sg.seq) + " (" + std::to_string(seg.size() / 1024) + " KB)");
            }
            if (md.endList && !got) { setStatus("fim da transmissao"); streamEnded = true; return; }
            if (!got) { int ms = (int)(md.target * 500); sleepMs(ms < 300 ? 300 : (ms > 1500 ? 1500 : ms), run); }
        }
    }
};
static HlsFeeder g_feed;
static int feedOpen(void *, const char *) { g_feed.opens++; return 0; }
static int feedClose(void *) { return 0; }
static int feedRead(void *, uint8_t *dst, uint64_t pos, uint32_t len) { g_feed.reads++; return g_feed.sb.read(dst, pos, len); }
static std::atomic<unsigned long long> g_feedSize{1ULL << 40};
static uint64_t feedSize(void *) { return g_feedSize.load(); }   // ao vivo: tamanho "infinito"; MP4 de teste: tamanho real
static void closeFeedFile() {                 // para o alimentador, fecha e apaga /data/live.ts
    g_feed.join();
    if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; remove("/data/live.ts"); }
}

// ---------- Filme/serie (.mp4 .mkv ...) -> AvPlayer ----------
// O AvPlayer abre "http://..." com a rede dele (outro User-Agent, sem redirecionamento) e os paineis recusam:
// "AddSource = 806A0002 / Open file failed". Entao o app le o arquivo com o PROPRIO cliente HTTP (blocos de 1 MB via Range, httpfile.hpp)
// e entrega ao player pelos callbacks de arquivo.
static std::mutex g_hfM;
static std::shared_ptr<httpfile::Reader> g_hf;
static std::shared_ptr<httpfile::Reader> getHf() { std::lock_guard<std::mutex> l(g_hfM); return g_hf; }
static void setHf(std::shared_ptr<httpfile::Reader> r) { std::lock_guard<std::mutex> l(g_hfM); g_hf = std::move(r); }
static std::shared_ptr<httpfile::Reader> takeHf() { std::lock_guard<std::mutex> l(g_hfM); auto r = std::move(g_hf); g_hf.reset(); return r; }
static int hfOpen(void *, const char *) { return 0; }
static int hfClose(void *) { return 0; }
static int hfRead(void *, uint8_t *dst, uint64_t pos, uint32_t len) { auto r = getHf(); return r ? r->read(dst, pos, len) : 0; }
static uint64_t hfSize(void *) { auto r = getHf(); return r ? r->size() : 0; }
static void abortHf() { auto r = getHf(); if (r) r->abort(); }                  // acorda leituras bloqueadas
static void releaseHfAsync() {                                                  // para e apaga em segundo plano (a rede pode demorar a responder)
    auto r = takeHf(); if (!r) return;
    r->abort(); g_tearing++;
    std::thread([r]() mutable { r->join(); r.reset(); g_tearing--; }).detach();
}

static double nowSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

struct Player {
    KAvHandle h = 0;
    bool active = false;
    AudioPump audio;
    int addRc = 0;
    bool failed = false, useFeed = false;
    std::vector<std::string> log;
    int frames = 0, strideMode = 0;
    unsigned vw = 0, vh = 0;
    double t0 = 0, lastFps = 0; int fpsFrames = 0; double fps = 0;

    // Controles de reproducao (VOD/filmes/series): pausa e seek via AvPlayer
    bool paused = false;
    bool isVod = false;                    // true = arquivo (mp4/mkv/...), false = live/HLS
    bool quietWait = false;                // true = nao mostra o cartao de diagnostico ao conectar (qualidade/reconexao)
    // Episodios da mesma temporada (para L1/R1 pular)
    std::vector<std::string> epUrls, epTitles;
    int epIndex = -1;

    // ---- modo "pedacos": o AvPlayer toca arquivos locais (provado pelo teste F); no ao vivo, a cada fim de arquivo
    //      gravamos os segmentos mais novos em outro arquivo e abrimos de novo.
    bool chunkMode = false, remux = false;
    std::string chunkExt;
    int chunkIdx = 0;
    unsigned long long lastSeq = 0;
    double inactiveSince = 0, lastFrameT = 0, hudUntil = 0;
    std::string title, url;
    // diagnostico de fluidez (HUD)
    double lastDrawT = 0, worstGap = 0, worstShown = 0;      // maior intervalo entre quadros desenhados no ultimo segundo
    double convMs = 0;                                       // tempo da ultima conversao TS->MP4
    double openMs = 800, openT0 = 0;                         // tempo medido para abrir o 2o player (abertura adaptativa)
    uint8_t *stage = nullptr; size_t stageCap = 0;           // copia em RAM com cache do quadro (ver copyFromWC)
    bool autoStride = true, strideLocked = false; int lockW = 0, lockH = 0;
    int strideUndecided = 0;                                 // quadros seguidos em que a deteccao de stride nao teve certeza (nao desenha lixo)
    double copyMs = 0;
    uint8_t *stageBuf(size_t n) {
        if (n > stageCap) { alignedFree(stage); stageCap = (n + 0xFFFF) & ~(size_t)0xFFFF; stage = (uint8_t *)alignedAlloc(64, stageCap); if (!stage) stageCap = 0; }
        return stage;
    }

    // ---- pipeline do ao vivo por pedacos: preparo assincrono + segundo player (troca sem emenda) ----
    double curDur = 0;                       // duracao (s) do pedaco que toca agora (0 = desconhecida)
    double chunkT0 = 0;                      // instante do primeiro quadro do pedaco atual
    KAvHandle h2 = 0;                        // proximo pedaco ja aberto num segundo player
    bool gapless = true;                     // desliga sozinho se o segundo player falhar
    std::atomic<bool> prepBusy{false}, nextReady{false};
    std::string nextPath; unsigned long long nextLast = 0; double nextDur = 0; int nextIdx = 0;
    std::thread prepTh, openTh;
    std::atomic<int> bState{0};              // segundo player: 0 nada | 1 abrindo | 2 pronto | 3 falhou
    KAvHandle bHandle = 0;
    int restarts = 0, switchesOk = 0;
    double lastGapMs = 0, gapStartT = 0, switchedAt = 0, blitMs = 0;
    std::function<bool()> cancelCb;          // main liga: true = o usuario cancelou (O) ou pediu para sair
    bool cancelled = false;

    static std::string chunkPath(const std::string &ext, int idx) { return "/data/live" + std::to_string(idx) + "." + ext; }
    static std::string hx8(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    // junta segmentos: first=true -> os 3 mais novos; senao -> so os mais novos que lastSeq. Retorna quantos.
    int gatherChunk(bool first, std::string &out, unsigned long long &lastOut) {
        std::lock_guard<std::mutex> l(g_feed.rm);
        size_t from = 0;
        if (first) from = g_feed.recent.size() > 3 ? g_feed.recent.size() - 3 : 0;
        int n = 0; out.clear();
        for (size_t i = from; i < g_feed.recent.size(); i++) {
            if (!first && g_feed.recent[i].first <= lastSeq) continue;
            out += g_feed.recent[i].second; lastOut = g_feed.recent[i].first; n++;
        }
        return n;
    }
    static bool writeFile(const std::string &path, const std::string &data) {
        FILE *f = fopen(path.c_str(), "wb"); if (!f) return false;
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size(); fclose(f); return ok;
    }

    // Abre uma fonte. waitMs>0: so aceita se chegar um quadro de video nesse tempo. Guarda o handle em 'h'.
    bool openSource(const std::string &path, int cb, int dbg, int waitMs, bool note, KAvHandle *target = nullptr) {   // cb: 0 = o player abre sozinho | 1 = HLS (g_feed) | 2 = arquivo HTTP (g_hf)
        KAvInitData id; memset(&id, 0, sizeof id);
        id.allocate = avAlloc; id.deallocate = avFree; id.allocateTexture = avAllocTex; id.deallocateTexture = avFreeTex;
        id.debugLevel = dbg; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = true; id.defaultLanguage = "eng";
        if (cb == 1) { id.fileObj = (void *)&g_feed; id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
        else if (cb == 2) { id.fileObj = (void *)&g_feed; id.fOpen = hfOpen; id.fClose = hfClose; id.fRead = hfRead; id.fSize = hfSize; }
        { std::lock_guard<std::mutex> l(g_avlogM); g_avlog.clear(); }
        if (dbg) sceAvPlayerSetLogCallback(avLogCb, NULL);
        g_allocN = g_allocFail = g_texN = g_texFail = 0;
        g_feed.opens = g_feed.reads = 0;
        KAvHandle hh = sceAvPlayerInit(&id);
        if (!hh) { if (note) log.push_back("   Init falhou"); return false; }
        int rc = sceAvPlayerAddSource(hh, path.c_str());
        addRc = rc;
        if (note || waitMs > 0) sceKernelUsleep(300000);
        if (note) {
            log.push_back("   AddSource(" + path + ") = " + hx8(rc) + "  open=" + std::to_string(g_feed.opens.load()) + " leituras=" + std::to_string(g_feed.reads.load()));
            std::lock_guard<std::mutex> l(g_avlogM);
            int shown = 0;
            for (auto &m : g_avlog) {
                if (shown >= 3) break;
                if (m.find("OpenCheck") != std::string::npos || m.find("rror") != std::string::npos || m.find("ail") != std::string::npos ||
                    m.find("nsupport") != std::string::npos || m.find("nvalid") != std::string::npos) { log.push_back("      [av] " + m.substr(0, 150)); shown++; }
            }
        }
        if (rc < 0) { avStopClose(hh); return false; }
        if (waitMs > 0) {
            double t0 = nowSec(); bool got = false;
            while (nowSec() - t0 < waitMs / 1000.0 && !got) {
                KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
                if (sceAvPlayerGetVideoData(hh, &fi) && fi.pData) got = true;
                KAvFrameInfo au; memset(&au, 0, sizeof au);
                sceAvPlayerGetAudioData(hh, &au);
                sceKernelUsleep(8000);
                if (cancelCb && cancelCb()) { cancelled = true; break; }
            }
            if (note) log.push_back(got ? "      -> chegou video!" : "      -> aceitou, mas nenhum quadro em " + std::to_string(waitMs / 1000) + "s");
            if (!got) { avStopClose(hh); return false; }
        }
        (target ? *target : h) = hh;
        return true;
    }

    bool start(const std::string &url, const std::string &title) {
        log.clear(); frames = 0; vw = vh = 0; fps = 0; fpsFrames = 0; failed = false; addRc = 0; chunkMode = false; chunkIdx = 0; inactiveSince = 0;
        cancelled = false; restarts = switchesOk = 0; lastGapMs = 0; gapStartT = switchedAt = 0; curDur = 0; chunkT0 = 0; blitMs = 0;
        paused = false; isVod = false;
        h2 = 0; bHandle = 0; bState = 0; nextReady = false; prepBusy = false; gapless = true;
        if (g_tearing.load() > 0) {
            log.push_back("Encerrando a exibicao anterior...");
            while (g_tearing.load() > 0) {
                if (cancelCb && cancelCb()) { cancelled = true; failed = true; return false; }
                presentCb(); sceKernelUsleep(30000);
            }
        }
        if (!g_reconnectNote.empty()) { log.push_back("Reconexao automatica #" + std::to_string(g_autoRestarts) + ": " + g_reconnectNote); g_reconnectNote.clear(); }
        this->title = title; this->url = url; hudUntil = nowSec() + 8;
        lastDrawT = 0; worstGap = worstShown = 0; convMs = 0; openMs = 800; copyMs = 0; autoStride = true; strideLocked = false; lockW = lockH = 0; strideMode = 1; strideUndecided = 0;
        log.push_back("Reproduzindo: " + title);
        {   // host e tamanho da URL
            size_t a = url.find("://"); a = a == std::string::npos ? 0 : a + 3;
            size_t e = url.find('/', a);
            log.push_back("URL: " + url.substr(0, 8) + "... host=" + url.substr(a, e == std::string::npos ? std::string::npos : e - a) +
                          "  tamanho=" + std::to_string(url.size()));
        }
        g_net.log = nullptr;
        g_net.init();
        log.push_back("modulo AvPlayer (0xA5): " + hx8(sceSysmoduleLoadModule(0x00A5)));
        log.push_back("modulo AvPlayerStreaming: " + hx8(sceSysmoduleLoadModuleInternal(0x80000050)));
        bool isHls = url.find(".m3u8") != std::string::npos;
        useFeed = false;
        if (!isHls && urlIsVideoFile(url)) {
            isVod = true;
            try { return startFile(url); }
            catch (...) { failed = true; log.push_back("ERRO: sem memoria ao abrir o arquivo"); trace("vod-excecao"); releaseHfAsync(); return false; }
        }   // filme/serie: o app le o arquivo por HTTP Range e entrega ao player

        // Ao vivo: HLS (.m3u8) vai direto. Canal "cru" (.ts / sem extensao): primeiro tenta o HLS equivalente do painel Xtream
        // (mais estavel), e se o painel nao tiver, le o MPEG-TS continuo e corta em pedacos.
        useFeed = true;
        std::vector<std::pair<std::string, bool>> tries;            // {url, mpegts_cru}
        if (isHls) tries.push_back({url, false});
        else {
            std::string hls = xtreamHlsUrl(url);
            if (!hls.empty()) tries.push_back({hls, false});
            tries.push_back({url, true});
        }
        for (size_t ti = 0; ti < tries.size(); ti++) {
            g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0; g_feedSize = 1ULL << 40;
            if (tries.size() > 1) log.push_back(std::string("Canal ao vivo: tentando ") + (tries[ti].second ? "fluxo MPEG-TS direto" : "HLS (.m3u8)"));
            g_feed.start(tries[ti].first, tries[ti].second);        // sem outFile: segmentos vao para o buffer (callbacks) e para 'recent' (arquivos)
            for (int i = 0; i < 400 && g_feed.segs.load() < 3 && !g_feed.failed.load(); i++) {
                sceKernelUsleep(50000);
                if (cancelCb && cancelCb()) { cancelled = true; break; }
                if (i % 2 == 0) presentCb();                    // a animacao de "Conectando..." continua rodando
            }
            if (cancelled) { failed = true; stop(); return false; }
            log.push_back(std::string(tries[ti].second ? "MPEG-TS" : "HLS") + ": pedacos baixados=" + std::to_string(g_feed.segs.load()) + "  " + g_feed.getStatus());
            if (g_feed.segs.load() > 0) break;
            g_feed.join();
        }
        if (g_feed.segs.load() == 0) { failed = true; log.push_back("ERRO: nenhum segmento baixado"); g_feed.join(); return false; }

        std::string chunk; unsigned long long last = 0;
        int n = gatherChunk(true, chunk, last);
        {   // formato do que baixamos (um .ts valido tem 0x47 a cada 188 bytes)
            bool ts = chunk.size() > 376 && (uint8_t)chunk[0] == 0x47 && (uint8_t)chunk[188] == 0x47 && (uint8_t)chunk[376] == 0x47;
            log.push_back("dados: " + std::to_string(n) + " segmentos, " + std::to_string(chunk.size() / 1024) + " KB, MPEG-TS valido=" + (ts ? "sim" : "NAO"));
        }
        // Tentativa principal: converter o TS em MP4 (o AvPlayer toca MP4 local - provado pelo teste F)
        remux = false;
        {
            std::string mp4, err; tsmux::Info ti;
            double c0 = nowSec();
            if (tsmux::convert(chunk, mp4, err, &ti)) {
                char b[160];
                snprintf(b, sizeof b, "conversao TS->MP4 ok: %dx%d, %d quadros, %d audio (%dHz), %.1fs, %d KB, %.0f ms",
                         ti.width, ti.height, ti.vSamples, ti.aSamples, ti.sampleRate, ti.seconds, (int)(mp4.size() / 1024), (nowSec() - c0) * 1000);
                log.push_back(b);
                std::string path = chunkPath("mp4", 0);
                log.push_back("Tentando MP4 convertido " + path + " ...");
                presentCb();
                bool okOpen = writeFile(path, mp4) && openSource(path, false, 3, 7000, true);
                if (!okOpen && !cancelled && g_texType.load() == 0) {      // o decoder nao entregou quadros em memoria Onion: tenta Garlic
                    log.push_back("memoria Onion sem quadros: tentando Garlic");
                    g_texType = 3; presentCb();
                    okOpen = openSource(path, false, 3, 7000, true);
                }
                if (okOpen) {
                    chunkMode = true; remux = true; chunkExt = "mp4"; chunkIdx = 0; lastSeq = last; curDur = ti.seconds;
                    log.push_back("OK: tocando MP4 convertido");
                } else remove(path.c_str());
            } else log.push_back("ERRO conversao TS->MP4: " + err);
        }
        if (cancelled) { failed = true; stop(); return false; }
        if (chunkMode) { g_feed.discardBuf = true; g_feed.sb.reset(); }     // os segmentos so seguem para 'recent' (nao enche mais o StreamBuf)
        // Plano B: escada de tentativas com o TS cru (varias extensoes) e depois leitura propria (callbacks)
        static const char *exts[] = {"mp4", "m2ts", "mpg", "ts", "mkv"};
        if (!chunkMode)
        for (const char *ext : exts) {
            if (cancelled) break;
            std::string path = chunkPath(ext, 0);
            log.push_back("Tentando arquivo " + path + " ...");
            presentCb();
            if (!writeFile(path, chunk)) { log.push_back("   nao consegui gravar"); continue; }
            if (openSource(path, false, 3, 5000, true)) {
                chunkMode = true; chunkExt = ext; chunkIdx = 0; lastSeq = last;
                log.push_back("OK: tocando por arquivos (." + std::string(ext) + ")");
                break;
            }
            remove(path.c_str());
        }
        if (chunkMode) { g_feed.discardBuf = true; g_feed.sb.reset(); }
        if (!chunkMode) {
            static const char *names[] = {"live.mp4", "live.ts", "live.m2ts"};
            for (const char *nm : names) {
                if (cancelled) break;
                log.push_back(std::string("Tentando leitura propria, nome ") + nm + " ...");
                presentCb();
                g_feedSize = 1ULL << 40;
                if (openSource(nm, 1, 3, 6000, true)) { log.push_back("OK: tocando por leitura propria"); break; }
            }
        }
        if (cancelled) { failed = true; stop(); return false; }
        if (!h) {
            failed = true;
            log.push_back("ERRO: nenhuma forma funcionou. Tire print desta tela.");
            g_feed.join();
            return false;
        }
        audio.start(h);
        active = true; t0 = lastFps = lastFrameT = nowSec();
        return true;
    }
    // Filme/serie: arquivo remoto lido por blocos (httpfile.hpp). Depois da leitura propria, tenta o jeito antigo (o player abrir a URL sozinho).
    bool startFile(const std::string &url) {
        std::string ext = urlExtension(url);
        log.push_back("Arquivo ." + ext + ": leitura propria por HTTP (3 conexoes, blocos de 1 MB)");
        presentCb();
        // 3 conexoes em paralelo (cada uma com a sua sessao): um unico download por vez nao acompanha filme de bitrate alto e a imagem travava.
        const int kConns = 3;
        std::vector<std::shared_ptr<NetClient::Session>> sessions;
        for (int i = 0; i < kConns; i++) { auto ss = std::make_shared<NetClient::Session>(); ss->recvSec = 25; sessions.push_back(ss); }   // paineis IPTV demoram para o 1o byte
        auto mkFetch = [url](std::shared_ptr<NetClient::Session> ss) -> httpfile::Fetch {
            return [ss, url](uint64_t from, uint64_t to, std::string &body) -> int {
                int st = 0;
                if (!g_net.getRange(*ss, url, from, to, body, st)) return 0;
                return st;
            };
        };
        auto rd = std::make_shared<httpfile::Reader>(mkFetch(sessions[0]), 20, 8);   // 20 blocos x 1 MB de cache, ate 8 blocos adiante
        for (int i = 1; i < kConns; i++) { auto ss = sessions[i]; rd->addWorker(mkFetch(ss), [ss] { g_net.closeSession(*ss); }); }
        rd->setOnExit([s0 = sessions[0]] { g_net.closeSession(*s0); });
        rd->setProbe([s0 = sessions[0], url]() -> uint64_t {      // tamanho do arquivo em UMA requisicao (antes eram ~25 requisicoes: 1 minuto na tela de espera)
            std::string b; int st = 0; uint64_t total = 0;
            if (!g_net.getRange(*s0, url, 0, 0, b, st, &total) || st != 206) return 0;
            return total;
        });
        trace("vod-baixando-tamanho");
        setHf(rd); rd->start();
        double w0 = nowSec();
        while (rd->state() == 0 && nowSec() - w0 < 60) {
            sceKernelUsleep(50000);
            if (cancelCb && cancelCb()) { cancelled = true; break; }
            presentCb();
        }
        if (cancelled) { failed = true; releaseHfAsync(); return false; }
        if (rd->state() != 1) {
            log.push_back("ERRO: " + (rd->state() == 2 ? rd->error() : std::string("o servidor demorou demais para responder")));
            releaseHfAsync();
        } else {
            log.push_back("   tamanho do arquivo: " + std::to_string(rd->size() >> 20) + " MB");
            presentCb();
            trace("vod-abrindo-player");
            std::string name = "video." + ext;
            if (openSource(name, 2, 3, 25000, true)) {
                audio.start(h); active = true; t0 = lastFps = lastFrameT = nowSec();
                trace("vod-tocando");
                log.push_back("OK: tocando o arquivo");
                return true;
            }
            releaseHfAsync();
            if (cancelled) { failed = true; return false; }
        }
        log.push_back("Tentando o player abrir a URL sozinho...");
        presentCb();
        if (openSource(url, 0, 3, 0, true)) {
            audio.start(h); active = true; t0 = lastFps = lastFrameT = nowSec();
            return true;
        }
        failed = true; log.push_back("ERRO: o player nao abriu esta URL.");
        return false;
    }
    std::function<void()> presentCb = [] {};   // main liga isto para redesenhar a tela de espera durante as tentativas

    // ---- preparo assincrono do proximo pedaco: junta, converte e grava enquanto o atual ainda toca ----
    void startPrep() {
        if (prepBusy.load() || nextReady.load()) return;
        std::string data; unsigned long long last = lastSeq;
        if (gatherChunk(false, data, last) == 0) return;
        lastSeq = last;                                      // esses segmentos ja tem dono (o pedaco em preparo)
        int idx = (chunkIdx + 1) % 4;
        bool rm = remux; std::string ext = chunkExt;
        if (prepTh.joinable()) prepTh.join();
        prepBusy = true;
        prepTh = std::thread([this, data, last, idx, rm, ext]() {
            std::string out, err; tsmux::Info ti; bool ok = true;
            double c0 = nowSec();
            if (rm) ok = tsmux::convert(data, out, err, &ti); else out = data;
            convMs = (nowSec() - c0) * 1000.0;
            std::string path = chunkPath(ext, idx);
            if (ok && writeFile(path, out)) { nextPath = path; nextLast = last; nextDur = rm ? ti.seconds : 0; nextIdx = idx; nextReady = true; }
            prepBusy = false;
        });
    }
    // abre o proximo pedaco num SEGUNDO player, em segundo plano, um pouco antes do atual acabar
    void openNextAsync() {
        if (openTh.joinable()) openTh.join();
        bState = 1; openT0 = nowSec();
        std::string path = nextPath;
        openTh = std::thread([this, path] { KAvHandle nh = 0; bool ok = openSource(path, false, 0, 0, false, &nh); bHandle = nh; bState = ok ? 2 : 3; });
    }
    static void closeLater(KAvHandle old, const std::string &oldPath) {
        g_tearing++;
        std::thread([old, oldPath] { avStopClose(old); remove(oldPath.c_str()); g_tearing--; }).detach();
    }
    // troca para o proximo pedaco (ja pronto em disco). Com segundo player aberto nao ha emenda.
    bool switchChunk(double t) {
        if (!nextReady.load()) return false;
        std::string oldPath = chunkPath(chunkExt, chunkIdx);
        gapStartT = lastFrameT > 0 ? lastFrameT : t;
        if (h2) {
            KAvHandle old = h; h = h2; h2 = 0;
            audio.retarget(h);                       // porta de audio continua aberta (sem buraco); so muda de onde vem o PCM
            closeLater(old, oldPath);                // so fecha o antigo DEPOIS do retarget (a thread de audio ja saiu dele)
            switchesOk++;
        } else {
            audio.retarget(0);
            avStopClose(h); h = 0;
            remove(oldPath.c_str());
            if (!openSource(nextPath, false, 0, 0, false)) { failed = true; log.push_back("ERRO ao reabrir pedaco: " + hx8(addRc)); nextReady = false; return false; }
            audio.retarget(h);
            restarts++;
        }
        chunkIdx = nextIdx; curDur = nextDur; nextReady = false;
        chunkT0 = 0; inactiveSince = 0; lastFrameT = t; switchedAt = t;
        return true;
    }
    void managePipeline(double t) {
        bool inactive = !sceAvPlayerIsActive(h);
        if (inactive && inactiveSince == 0) inactiveSince = t;
        int fresh = 0; { std::lock_guard<std::mutex> l(g_feed.rm); for (auto &sg : g_feed.recent) if (sg.first > lastSeq) fresh++; }
        // 1) comeca a preparar o proximo pedaco assim que houver 2 segmentos novos
        if (!prepBusy.load() && !nextReady.load() && !h2 && fresh >= 2) startPrep();
        // 2) segundo player
        if (bState.load() == 2) {
            h2 = bHandle; bState = 0;
            double m = (nowSec() - openT0) * 1000.0;                  // quanto levou para abrir: ajusta quando abrir o proximo
            if (m > 50 && m < 4000) openMs = openMs * 0.5 + m * 0.5;
        }
        else if (bState.load() == 3) { bState = 0; gapless = false; log.push_back("segundo player nao abriu: troca com emenda"); }
        double el = chunkT0 > 0 ? t - chunkT0 : 0;
        if (gapless && nextReady.load() && !h2 && bState.load() == 0 && chunkT0 > 0 && curDur > 0 && el >= curDur - (openMs / 1000.0 + 0.25)) openNextAsync();
        // 3) troca
        bool ended = inactive && t - inactiveSince > 0.03;
        bool overdue = curDur > 0 && chunkT0 > 0 && el > curDur + 0.6;
        bool stalled = t - lastFrameT > 3.0;
        if (nextReady.load()) {
            if (h2 && (ended || overdue || stalled)) switchChunk(t);
            else if (!h2 && bState.load() == 0 && (ended || stalled)) switchChunk(t);
        } else if ((ended || stalled) && !prepBusy.load() && fresh >= 1) startPrep();
        // segundo player que nao entregou video depois da troca: desliga o modo sem emenda
        if (switchedAt > 0 && chunkT0 == 0 && t - switchedAt > 2.5 && gapless) { gapless = false; log.push_back("sem quadros apos a troca: modo sem emenda desligado"); }
    }
    // Desmonta TUDO em segundo plano: o botao O volta ao menu na hora (antes travava ate a rede responder).
    void stop() {
        KAvHandle hh = h, hh2 = h2;
        h = 0; h2 = 0; active = false; chunkMode = false;
        g_feed.abort();                                      // acorda leituras bloqueadas
        abortHf();
        g_tearing++;
        std::thread([this, hh, hh2] {
            audio.stop();
            if (openTh.joinable()) openTh.join();
            KAvHandle hb = bState.load() == 2 ? bHandle : 0;
            bState = 0;
            avStopClose(hh);
            if (hh2) avStopClose(hh2);
            if (hb) avStopClose(hb);
            if (prepTh.joinable()) prepTh.join();
            g_feed.join();
            { auto r = takeHf(); if (r) r->join(); }             // o player ja fechou: ninguem mais le o arquivo HTTP
            for (int i = 0; i < 4; i++) { static const char *e[] = {"mp4","m2ts","mpg","ts","mkv"}; for (const char *x : e) remove(chunkPath(x, i).c_str()); }
            if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; }
            remove("/data/live.ts");
            nextReady = false; prepBusy = false;
            g_tearing--;
        }).detach();
    }
    // espera as desmontagens terminarem (usado ao fechar o app)
    static void waitTeardown(double maxSec) {
        double t0 = nowSec();
        while (g_tearing.load() > 0 && nowSec() - t0 < maxSec) sceKernelUsleep(20000);
    }
    // chama a cada iteracao; retorna true se desenhou um quadro novo
    bool poll(FbRenderer &r) {
        KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
        bool drew = false;
        if (sceAvPlayerGetVideoData(h, &fi) && fi.pData) {
            uint32_t w = *(uint32_t *)(fi.details + 0), hh = *(uint32_t *)(fi.details + 4);
            if (w > 0 && hh > 0 && w <= 4096 && hh <= 4096) {
                vw = w; vh = hh;
                if (w != (unsigned)lockW || hh != (unsigned)lockH) { lockW = (int)w; lockH = (int)hh; strideLocked = false; strideUndecided = 0; }
                const uint8_t *src = fi.pData; size_t srcN = 0;
                size_t avail = texAvail(fi.pData);
                double b0 = nowSec();
                if (avail >= (size_t)w * hh * 3 / 2 && g_texType.load() == 0) {
                    // memoria Onion (com cache): le direto, sem copiar
                    size_t want = blit::strideBytes(strideLocked || !autoStride ? strideMode : 3, (int)w, (int)hh);
                    if (!strideLocked && autoStride) { size_t m = blit::strideBytes(3, (int)w, (int)hh); if (m > want) want = m; }
                    srcN = (want < avail ? want : avail) & ~(size_t)15;
                } else if (avail >= (size_t)w * hh * 3 / 2) {
                    // memoria Garlic (sem cache): copia o quadro para RAM com cache. Enquanto o stride nao foi detectado, copia o maximo que cabe (todos os modos)
                    size_t want = blit::strideBytes(strideLocked || !autoStride ? strideMode : 3, (int)w, (int)hh);
                    if (!strideLocked && autoStride) { size_t m = blit::strideBytes(3, (int)w, (int)hh); if (m > want) want = m; }
                    srcN = want < avail ? want : avail; srcN &= ~(size_t)15;
                    uint8_t *st = stageBuf(srcN);
                    if (st) { copyFromWC(st, fi.pData, srcN); src = st; } else srcN = 0;
                }
                copyMs = copyMs == 0 ? (nowSec() - b0) * 1000.0 : copyMs * 0.9 + (nowSec() - b0) * 100.0;
                bool skipDraw = false;
                if (srcN && autoStride && !strideLocked) {
                    int m = blit::detectStride(src, srcN, (int)w, (int)hh);
                    if (m > 0) { strideMode = m; strideLocked = true; strideUndecided = 0; }
                    else if (++strideUndecided < 90) skipDraw = true;          // ainda sem certeza (ex.: quadro preto): nao mostra imagem torta/verde; depois de ~3 s desenha mesmo assim
                }
                int pitch, yRows; blit::strideCandidate(strideMode, (int)w, (int)hh, pitch, yRows);
                bool fits = srcN ? blit::strideBytes(strideMode, (int)w, (int)hh) <= srcN : true;   // sem copia: leitura direta (lenta, como antes)
                if (fits && !skipDraw) {
                    b0 = nowSec();
                    blitNV12(src, w, hh, pitch, yRows, r.fb, r.W, r.H);
                    double bm = (nowSec() - b0) * 1000.0;
                    blitMs = blitMs == 0 ? bm : blitMs * 0.9 + bm * 0.1;
                    frames++; fpsFrames++;
                    drew = true;
                }                                                                               // modo manual que nao cabe no que foi copiado: pula o quadro
            }
        }
        double t = nowSec();
        if (drew) {
            if (lastDrawT > 0 && (t - lastDrawT) * 1000.0 > worstGap) worstGap = (t - lastDrawT) * 1000.0;
            lastDrawT = t;
            lastFrameT = t; inactiveSince = 0;
            if (chunkT0 == 0) { chunkT0 = t; if (gapStartT > 0) { lastGapMs = (t - gapStartT) * 1000.0; gapStartT = 0; } }
        }
        if (chunkMode && h && !failed) managePipeline(t);
        if (t - lastFps >= 1.0) { fps = fpsFrames / (t - lastFps); fpsFrames = 0; lastFps = t; worstShown = worstGap; worstGap = 0; }
        return drew;
    }
    // ---- vigia (anti-congelamento): true = a live parou de verdade e o app deve reabri-la sozinho ----
    // So age no ao vivo por pedacos. 'why' recebe o motivo (aparece na tela "Conectando..." e no log).
    bool wantReconnect(std::string &why) {
        if (!useFeed || !chunkMode || !active || paused) return false;               // pausado: nao reconecta
        double t = nowSec();
        double still = lastFrameT > 0 ? t - lastFrameT : 0;                          // ha quanto tempo nenhum quadro novo foi desenhado
        if (failed) { why = "falha ao abrir o proximo pedaco (" + hx8(addRc) + ")"; return true; }
        if (g_feed.streamEnded.load()) return false;                                  // a live acabou: nao adianta reconectar
        if (g_feed.failed.load() && still > 2.0) { why = "rede: " + g_feed.getStatus(); return true; }   // espera o que ja foi baixado acabar de tocar
        if (still > 10.0) { why = "imagem parada ha " + std::to_string((int)still) + " s"; return true; }
        return false;
    }
    bool feedProblem() const { return useFeed && (g_feed.failed.load() || g_feed.streamEnded.load()); }
    bool hudActive() const { return nowSec() <= hudUntil || feedProblem() || paused; }

    void togglePause() {
        if (!h || !active || failed) return;
        if (paused) { sceAvPlayerResume(h); paused = false; }
        else { sceAvPlayerPause(h); paused = true; }
        hudUntil = nowSec() + 5;
    }

    // Seek relativo em ms (so VOD/arquivos). Live/HLS nao suporta.
    void seekBy(int deltaMs) {
        if (!h || !active || failed || !isVod) return;
        uint64_t cur = sceAvPlayerCurrentTime(h);
        int64_t target = (int64_t)cur + deltaMs;
        if (target < 0) target = 0;
        sceAvPlayerJumpToTime(h, (uint64_t)target);
        if (paused) { sceAvPlayerResume(h); paused = false; }
        hudUntil = nowSec() + 5;
    }

    bool hasEpisodes() const { return epIndex >= 0 && (int)epUrls.size() > 1; }
    bool canPrevEp() const { return hasEpisodes() && epIndex > 0; }
    bool canNextEp() const { return hasEpisodes() && epIndex + 1 < (int)epUrls.size(); }

    // HUD moderno sobre o video (aparece nos primeiros segundos e quando qualquer botao e apertado)
    void overlay(FbRenderer &r) {
        using namespace theme;
        if (nowSec() > hudUntil && !feedProblem() && !paused) return;
        if (nowSec() <= hudUntil || paused) {
            char b[256];
            if (isVod) {
                uint64_t curMs = h ? sceAvPlayerCurrentTime(h) : 0;
                int sec = (int)(curMs / 1000), mm = sec / 60, ss = sec % 60;
                snprintf(b, sizeof b, "%ux%u  |  %d:%02d  |  %s  |  %s%s",
                         vw, vh, mm, ss, kQualityName[g_quality.load() % 3],
                         g_texType.load() == 0 ? "Onion" : "Garlic", paused ? "  |  PAUSADO" : "");
            } else {
                snprintf(b, sizeof b, "%ux%u  |  %.0f fps  |  %s  |  %s%s",
                         vw, vh, fps, kQualityName[g_quality.load() % 3],
                         g_texType.load() == 0 ? "Onion" : "Garlic", paused ? "  |  PAUSADO" : "");
            }
            std::string stats = b;
            int tw = r.textWidth(title, 24), sw = r.textWidth(stats, 18);
            int pw = 24 + 92 + 16 + tw + 24 + sw + 28;
            r.roundRect(32, 28, pw, 60, 30, 0xB8000000);
            r.roundRect(44, 42, 92, 32, 16, isVod ? 0xFF3B82F6 : RED);
            uiCenter(r, 44 + 46, 58, isVod ? (paused ? "PAUSA" : "VOD") : "AO VIVO", TEXT, 16, true);
            uiText(r, 44 + 92 + 16, 58 - 12, title, TEXT, 24, true);
            uiText(r, 44 + 92 + 16 + tw + 24, 58 - 9, stats, MUTED, 18);

            const char *hint;
            if (isVod) {
                if (hasEpisodes())
                    hint = "X  Pausar   L2/R2  -10s/+10s   < >  -30s/+30s   L1/R1  Episodio   QUADRADO  Qualidade   O  Sair";
                else
                    hint = "X  Pausar   L2/R2  -10s/+10s   < >  -30s/+30s   QUADRADO  Qualidade   O  Sair";
            } else {
                hint = "X  Pausar   QUADRADO  Qualidade   L1  Memoria   TRIANGULO  Imagem   O  Sair";
            }
            int hw = r.textWidth(hint, 18) + 48;
            r.roundRect(r.W - 32 - hw, r.H - 130, hw, 44, 22, 0xB8000000);
            uiCenter(r, r.W - 32 - hw / 2, r.H - 108, hint, SOFT, 18);

            if (!isVod) {
                char rb[48] = ""; if (g_autoRestarts > 0) snprintf(rb, sizeof rb, "  |  reconexoes: %d", g_autoRestarts);
                snprintf(b, sizeof b, "pico %.0f ms  |  baixa %.0f/%.0f ms (seg %.1fs)  |  conv %.0f ms  |  abre %.0f ms  |  trocas: %d ok, %d com emenda, ultima %.0f ms%s%s",
                         worstShown, g_feed.dlMs.load(), g_feed.dlMsMax.load(), g_feed.lastSegDur.load(), convMs, openMs, switchesOk, restarts, lastGapMs, gapless ? "" : "  (sem-emenda OFF)", rb);
                r.roundRect(32, r.H - 76, r.textWidth(b, 16) + 40, 40, 20, 0x90000000);
                uiText(r, 52, r.H - 76 + 12, b, DIM, 16);
            } else if (hasEpisodes()) {
                snprintf(b, sizeof b, "Episodio %d / %d", epIndex + 1, (int)epUrls.size());
                r.roundRect(32, r.H - 76, r.textWidth(b, 16) + 40, 40, 20, 0x90000000);
                uiText(r, 52, r.H - 76 + 12, b, DIM, 16);
            }
        }
        if (feedProblem()) {
            std::string m = uiFit(r, g_feed.getStatus(), 20, r.W - 160);
            r.roundRect(32, 100, r.textWidth(m, 20) + 48, 44, 22, 0xD0801520);
            uiText(r, 56, 100 + 12, m, TEXT, 20);
        }
        if (paused) {
            r.roundRect(r.W / 2 - 80, r.H / 2 - 40, 160, 80, 20, 0xC0000000);
            uiCenter(r, r.W / 2, r.H / 2, "II", TEXT, 48, true);
        }
    }
    void renderWaiting(FbRenderer &r) {
        using namespace theme;
        uiBackground(r);
        // cabecalho: spinner (ou alerta) + titulo
        const int scx = 96, scy = 112;
        if (failed) {
            r.roundRect(scx - 34, scy - 34, 68, 68, 34, RED);
            uiCenter(r, scx, scy, "!", TEXT, 40, true);
        } else {
            double tt = nowSec();
            for (int i = 0; i < 12; i++) {
                static const float kc[12] = {1.f, .8660254f, .5f, 0.f, -.5f, -.8660254f, -1.f, -.8660254f, -.5f, 0.f, .5f, .8660254f};
                static const float ks[12] = {0.f, .5f, .8660254f, 1.f, .8660254f, .5f, 0.f, -.5f, -.8660254f, -1.f, -.8660254f, -.5f};
                float xx = (float)tt * 1.3f - i / 12.0f + 4.0f;
                float ph = xx - (float)(int)xx;
                int al = 40 + (int)(215 * (1.0f - ph));
                int px = scx + (int)(30 * kc[i]), py = scy + (int)(30 * ks[i]);
                r.roundRect(px - 6, py - 6, 12, 12, 6, ((unsigned)al << 24) | 0x0053FC18);
            }
        }
        // quietWait (troca de qualidade / reconexao): so spinner central, sem log de diagnostico
        // abertura normal: tela limpa por ate 25s; so depois mostra o log (se ainda nao abriu)
        const bool showClean = !failed;                      // o log de diagnostico so aparece se falhar
        if (showClean) {
            int cx = r.W / 2, cy = r.H / 2 - 40;
            int pulse = (int)(((int)(nowSec() * 3)) % 3);
            uiGlow(r, cx - 60, cy - 61, 120, 122, 8, GREEN, 18 + pulse * 3, 120);
            uiKMark(r, cx - 60, cy - 61, 120, 122, GREEN);
            uiCenterHeavy(r, cx, cy + 130, uiFit(r, title, 38, r.W - 300), TEXT, 38);
            uiCenter(r, cx, cy + 180, quietWait ? "Trocando qualidade..." : isVod ? "Abrindo o vídeo..." : "Abrindo a transmissão...", MUTED, 24);
            uiHints(r, r.W - 73, r.H - 50, {{"O", "Cancelar"}});
            return;
        }
        uiText(r, 164, 70, failed ? "Não foi possível abrir" : "Conectando...", failed ? RED : TEXT, 38, true);
        char b[160];
        if (failed) snprintf(b, sizeof b, "AddSource = %08X   |   aperte O para voltar ao menu", (unsigned)addRc);
        else snprintf(b, sizeof b, "%s   |   %.0fs   |   O para cancelar", title.c_str(), nowSec() - t0);
        uiText(r, 166, 122, uiFit(r, b, 20, r.W - 240), MUTED, 20);
        if (!failed && nowSec() - t0 > 25) uiText(r, 166, 150, "Demorando mais que o normal. Aperte O para cancelar.", AMBER, 20);
        // cartao com o registro das tentativas
        const int cy0 = 196, ch = r.H - cy0 - 110;
        r.roundRect(48, cy0, r.W - 96, ch, 22, CARD);
        int y = cy0 + 22;
        size_t from = log.size() > 29 ? log.size() - 29 : 0;
        for (size_t i = from; i < log.size(); i++) {
            const std::string &l = log[i];
            unsigned c = SOFT; bool bold = false;
            if (l.compare(0, 4, "ERRO") == 0 || l.find("nenhum quadro") != std::string::npos) c = RED;
            else if (l.compare(0, 2, "OK") == 0 || l.find("chegou video") != std::string::npos) { c = GREEN; bold = true; }
            else if (l.compare(0, 8, "Tentando") == 0) c = AMBER;
            else if (l.find("[av]") != std::string::npos) c = DIM;
            else if (l.compare(0, 3, "   ") == 0) c = MUTED;
            uiText(r, 76, y, uiFit(r, l, 18, r.W - 160), c, 18, bold);
            y += 24;
        }
        if (useFeed) {
            snprintf(b, sizeof b, "HLS: %d segmentos  |  %s", g_feed.segs.load(), g_feed.getStatus().c_str());
            uiText(r, 52, r.H - 82, uiFit(r, b, 18, r.W - 420), g_feed.failed ? RED : MUTED, 18);
        }
        uiHints(r, r.W - 48, r.H - 50, {{"O", "Voltar"}});
    }
};

enum class Mode { Menu, Play, Kbd };

// Leitura do controle (logica em padin.hpp, testada no PC): adaptador para o scePad do PS4.
struct OrbisPadSrc {
    static int read(int h, PadSample *out, int n) {
        OrbisPadData buf[16]; if (n > 16) n = 16;
        int k = scePadRead(h, buf, n);
        if (k < 0) k = 0;
        for (int i = 0; i < k; i++) { out[i].buttons = buf[i].buttons; out[i].lx = buf[i].leftStick.x; out[i].ly = buf[i].leftStick.y; }
        return k;
    }
    static bool state(int h, PadSample &s) {
        OrbisPadData d;
        if (scePadReadState(h, &d) != 0) return false;
        s.buttons = d.buttons; s.lx = d.leftStick.x; s.ly = d.leftStick.y;
        return true;
    }
    static double now() { return nowSec(); }
};
using PadIn = PadInT<OrbisPadSrc>;

int main() {
    sceUserServiceInitialize(NULL);
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    scePadInit();
    sceAudioOutInit();
    int pad = scePadOpen(userId, 0, 0, NULL);
    PadIn pin; pin.h = pad;
    bool wantQuit = false;

    const int W = 1920, H = 1080;
    int vh = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, NULL);
    if (vh < 0) return 1;
    const size_t bufSize = 0x800000, total = bufSize * 3;      // 3 telas em anel: nunca desenha na tela que esta aparecendo/aguardando o flip
    off_t phys = 0;
    void *mem = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &phys) < 0) return 2;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, 0x200000) < 0) return 3;
    void *bufs[3] = {mem, (char *)mem + bufSize, (char *)mem + bufSize * 2};
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    if (sceVideoOutRegisterBuffers(vh, 0, bufs, 3, &attr) < 0) return 4;
    BootScreen boot;
    boot.init(bufs[0], W, H);                                       // so escrita: igual ao codigo original
    sceVideoOutSubmitFlip(vh, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    boot.line("[1] video aberto");

    traceInit();
    trace("video-ok");
    if (!g_prevStage.empty()) boot.line("da ultima vez o app parou em: " + g_prevStage, 0xFFFFB020u);
    Ps4Platform pf;
    App app(pf);
    pf.app = &app;
    UI ui(app);
    ui.loadSaved();                           // ultimo streamer e lista M3U guardados
    if (!g_prevStage.empty()) app.say("Aviso: da ultima vez o app parou no passo: " + g_prevStage);
    trace("estado-criado");
    boot.line("[2] estado do app criado");


    Mode mode = Mode::Menu;
    Player player;
    Keyboard kbd;
    bool gotFrame = false;
    double lastWaitDraw = 0;
    uint32_t prev = 0;
    int back = 1;
    bool dirty = true;
    // Reconexao automatica: no maximo 5 tentativas em 2 minutos (depois desiste e avisa, para nao ficar em loop se a live acabou)
    double rtT[5] = {0, 0, 0, 0, 0}; int rtI = 0; bool autoMode = false;
    auto allowAuto = [&]() -> bool {
        double tn = nowSec(); int n = 0;
        for (double x : rtT) if (x > 0 && tn - x < 120.0) n++;
        if (n >= 5) { for (double &x : rtT) x = 0; return false; }
        rtT[rtI++ % 5] = tn; return true;
    };

    // A memoria de video e "write-combined": ESCREVER e rapido, mas LER e lentissimo. O desenho novo (transparencia,
    // texto suave) le pixels, entao desenhamos tudo numa tela de rascunho na RAM normal e so copiamos para o video.
    // O rascunho fica em memoria "cacheada" do sistema (tipo 0), fora do heap do libc.
    uint32_t *shadow = NULL;
    std::vector<uint32_t> shadowFallback;
    {
        off_t phys2 = 0; void *m2 = NULL;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bufSize, 0x200000, 0, &phys2) >= 0 &&
            sceKernelMapDirectMemory(&m2, bufSize, 0x33, 0, phys2, 0x200000) >= 0) shadow = (uint32_t *)m2;
        if (!shadow) { shadowFallback.assign((size_t)W * H, 0); shadow = shadowFallback.data(); trace("rascunho-no-heap"); }
        for (size_t i = 0; i < (size_t)W * H; i++) shadow[i] = 0xFF000000u;
    }
    trace("rascunho-ok");
    boot.line(shadow == shadowFallback.data() ? "[3] rascunho de tela: heap" : "[3] rascunho de tela: memoria direta");
    auto flip = [&]() {
        memcpy(bufs[back], shadow, (size_t)W * H * 4);          // copia sequencial (rapida)
        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3;
    };
    auto presentMenu = [&]() { FbRenderer r(shadow, W, H); ui.render(r); flip(); };
    auto presentWait = [&]() { FbRenderer r(shadow, W, H); player.renderWaiting(r); flip(); };
    auto presentKbd = [&]() { FbRenderer r(shadow, W, H); kbd.render(r, ((int)(nowSec() * 2)) % 2 == 0, ui.timeText, ui.dateText); flip(); };
    auto updateClock = [&]() -> bool {         // true se o minuto mudou
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        std::string t, d; clockStrings((long long)ts.tv_sec, t, d);
        if (t == ui.timeText && d == ui.dateText) return false;
        ui.timeText = t; ui.dateText = d; return true;
    };
    updateClock();
    boot.line("[4] abrindo a interface...");
    sceKernelUsleep(400000);
    trace("abertura");

    {   // ---- abertura animada: marca K aparece, titulo, barra de carregamento ----
        double ti = nowSec();
        for (;;) {
            double e = nowSec() - ti;
            if (e > 1.8) break;
            FbRenderer r(shadow, W, H);
            using namespace theme;
            uiBackground(r);
            float k = (float)(e / 0.7); if (k > 1.f) k = 1.f;
            float ease = 1.f - (1.f - k) * (1.f - k) * (1.f - k);
            int cx = W / 2, cy = H / 2 - 90 + (int)((1.f - ease) * 40);
            uiGlow(r, cx - 70, cy - 72, 140, 144, 8, GREEN, 26, 140);
            uiKMark(r, cx - 70, cy - 72, 140, 144, GREEN);
            int lh = 84, lw = uiLogoWidth(lh);
            uiLogo(r, cx - lw / 2, H / 2 + 80, lh);
            uiCenter(r, cx, H / 2 + 200, "Lives da Kick direto no seu PS4", MUTED, 24);
            float pg = (float)(e / 1.6); if (pg > 1.f) pg = 1.f;
            r.roundRect(cx - 220, H / 2 + 250, 440, 10, 5, CARD2);
            r.roundRect(cx - 220, H / 2 + 250, (int)(440 * pg) < 10 ? 10 : (int)(440 * pg), 10, 5, GREEN);
            int fade = (int)(255.f * (1.f - ease));                      // entra do preto
            if (fade > 0) r.fillRect(0, 0, W, H, ((unsigned)fade << 24));
            flip();
            sceKernelUsleep(8000);
        }
    }
    trace("intro-ok");
    pf.progressTick = [&]() -> bool {         // durante o download da lista: redesenha o status e deixa cancelar com O
        presentMenu();
        pin.poll();
        if (pin.pending & B_CIRCLE) { pin.pending &= ~B_CIRCLE; return true; }
        if ((pin.held | pin.pending) & B_OPTIONS) { wantQuit = true; return true; }
        return false;
    };
    // Protecao contra "fecha sempre ao abrir": se a ultima abertura caiu durante a lista M3U, muda de estrategia.
    //  1a queda -> a proxima abertura usa o MODO SEGURO (baixa a lista sem thread de rede) e isso fica gravado;
    //  caiu ate no modo seguro -> nao carrega a lista sozinho (o app abre normalmente e mostra o passo onde parou).
    {
        auto starts = [&](const char *p) { return g_prevStage.rfind(p, 0) == 0; };
        bool crashM3u = starts("m3u-inicio") || starts("m3u-t-") || starts("m3u-sync") || starts("m3u-s-") || starts("m3u-parse") ||
                        starts("m3u-cache-inicio") || starts("m3u-cache-parse");
        bool wasSafe = pf.loadSetting("safem3u") == "1";
        if (wasSafe) g_safeM3U = true;
        bool skipAuto = false;
        if (crashM3u) {
            if (!wasSafe) { g_safeM3U = true; pf.saveSetting("safem3u", "1"); trace("modo-seguro-ligado"); }
            else skipAuto = true;
        }
        if (ui.hasSavedList() && skipAuto) {
            app.say("A lista M3U fechou o app na ultima vez (passo: " + g_prevStage + ") - nao carreguei sozinho. Abra LISTA M3U e aperte X.");
        } else if (ui.hasSavedList()) {          // reabre a lista M3U da ultima vez
            app.say(g_safeM3U ? "Carregando sua lista M3U (modo seguro)..." : "Carregando sua lista M3U...");
            presentMenu();
            ui.loadSavedList();
            trace("lista-m3u-carregada");
        }
    }

    for (;;) {
        pin.poll();
        uint32_t now = pin.held, pressed = pin.take();
        if (wantQuit || ((now | pressed) & B_OPTIONS)) break;
        (void)prev;

        if (mode == Mode::Kbd) {
            bool changed = false, done = false;
            {   // setas/analogico: 1 passo ao apertar + repeticao rapida enquanto seguradas (por tempo, nao por quadro)
                int st[4]; pin.steps(st);
                int nU = ((pressed & B_UP) ? 1 : 0) + st[0], nD = ((pressed & B_DOWN) ? 1 : 0) + st[1];
                int nL = ((pressed & B_LEFT) ? 1 : 0) + st[2], nR = ((pressed & B_RIGHT) ? 1 : 0) + st[3];
                for (int i = 0; i < nU; i++) kbd.move(0, -1);
                for (int i = 0; i < nD; i++) kbd.move(0, 1);
                for (int i = 0; i < nL; i++) kbd.move(-1, 0);
                for (int i = 0; i < nR; i++) kbd.move(1, 0);
                if (nU + nD + nL + nR > 0) changed = true;
            }
            if (pressed & B_L1)       { kbd.toggleShift(); changed = true; }      // L1: maiusculas (1 letra -> travado -> desligado)
            {   // apagar: QUADRADO ou R3; segurando apaga em sequencia
                static double bsNext = 0;
                double tn = nowSec();
                if (pressed & (B_SQUARE | B_R3)) { kbd.backspace(); changed = true; bsNext = tn + 0.4; }
                else if ((now & (B_SQUARE | B_R3)) && tn >= bsNext) { kbd.backspace(); changed = true; bsNext = tn + 0.05; }
            }
            if (pressed & B_TRIANGLE) { kbd.insert("https://"); changed = true; }
            if (pressed & B_L2)       { kbd.clear(); changed = true; }
            if (pressed & (B_CROSS | B_L3)) { done = kbd.press(); changed = true; }   // X ou clique no L3 digita a tecla
            if (pressed & B_R1)       done = true;
            if (pressed & B_CIRCLE)   { ui.textKind = 0; mode = Mode::Menu; dirty = true; continue; }
            if (done) {
                mode = Mode::Menu; dirty = true;
                if (!kbd.text.empty() || ui.textKind == 2) { pf.pendingText = kbd.text; pf.hasPendingText = true; }
                else ui.textKind = 0;
                continue;
            }
            int blinkNow = ((int)(nowSec() * 2)) % 2;
            static int lastBlink = -1;
            if (changed || dirty) { lastBlink = blinkNow; presentKbd(); dirty = false; }       // desenho completo so quando algo mudou
            else if (blinkNow != lastBlink) {                                                // cursor piscando: so o retangulo dele
                lastBlink = blinkNow;
                FbRenderer r(shadow, W, H); kbd.drawCaret(r, blinkNow == 0); flip();
            }
            sceKernelUsleep(4000);
            continue;
        }

        if (mode == Mode::Play) {
            if (pressed & B_CIRCLE) {
                player.stop(); player.quietWait = false; player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
                mode = Mode::Menu; dirty = true; sceKernelUsleep(16000); continue;
            }
            if (pressed) player.hudUntil = nowSec() + 5;      // qualquer botao mostra o HUD por 5s

            // X = pausar / continuar
            if (pressed & B_CROSS) player.togglePause();

            // Seek VOD: L2 -10s, R2 +10s, esquerda -30s, direita +30s
            if (player.isVod) {
                if (pressed & B_L2) player.seekBy(-10000);
                if (pressed & B_R2) player.seekBy(+10000);
                if (pressed & B_LEFT) player.seekBy(-30000);
                if (pressed & B_RIGHT) player.seekBy(+30000);
                // L1/R1 = episodio anterior / proximo (quando veio de uma temporada)
                if ((pressed & B_L1) && player.canPrevEp()) {
                    player.epIndex--;
                    pf.pendingPlayUrl = player.epUrls[player.epIndex];
                    pf.pendingPlayTitle = player.epTitles[player.epIndex];
                    pf.pendingEpIndex = player.epIndex;   // mantem o indice ao reabrir
                    player.quietWait = true;
                    player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
                }
                if ((pressed & B_R1) && player.canNextEp()) {
                    player.epIndex++;
                    pf.pendingPlayUrl = player.epUrls[player.epIndex];
                    pf.pendingPlayTitle = player.epTitles[player.epIndex];
                    pf.pendingEpIndex = player.epIndex;
                    player.quietWait = true;
                    player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
                }
            } else {
                // Live: L1 ainda alterna memoria de video; TRIANGULO ajusta stride
                if (pressed & B_TRIANGLE) { player.autoStride = false; player.strideMode = (player.strideMode + 1) % 4; }
                if ((pressed & B_L1) && !player.url.empty()) {
                    g_texType = g_texType.load() == 0 ? 3 : 0;
                    pf.pendingPlayUrl = player.url; pf.pendingPlayTitle = player.title;
                    player.quietWait = true;
                    player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
                }
            }

            // QUADRADO: troca qualidade e reabre sem a tela de log (quietWait)
            if ((pressed & B_SQUARE) && !player.url.empty() && !player.isVod) {
                g_quality = (g_quality.load() + 1) % 3;
                pf.pendingPlayUrl = player.url; pf.pendingPlayTitle = player.title;
                player.quietWait = true;
                player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
            }

            {   // anti-congelamento: se a live parou, reabre sozinho
                std::string why;
                if (player.wantReconnect(why)) {
                    if (!allowAuto()) {
                        autoMode = false;
                        app.say("Conexao instavel: a live nao voltou depois de varias tentativas (" + why + ")");
                        trace("auto-reconexao-desistiu");
                        player.stop(); player.quietWait = false; mode = Mode::Menu; dirty = true; continue;
                    }
                    g_autoRestarts++; autoMode = true;
                    g_reconnectNote = why;
                    trace("auto-reconexao");
                    pf.pendingPlayUrl = player.url; pf.pendingPlayTitle = player.title;
                    player.quietWait = true;
                    player.stop(); mode = Mode::Menu; dirty = true; continue;
                }
            }
            if (player.h) {
                // Sem HUD o video e desenhado DIRETO no quadro de video (so escrita, sem a copia de 8 MB).
                // Com HUD (precisa ler pixels para a transparencia) passa pelo rascunho, como antes.
                // Pausado: mantem o ultimo quadro + overlay.
                bool hud = player.hudActive();
                FbRenderer r(hud ? shadow : (uint32_t *)bufs[back], W, H);
                if (player.paused) {
                    if (hud) { player.overlay(r); flip(); }
                    sceKernelUsleep(16000);
                    continue;
                }
                if (player.poll(r)) {
                    gotFrame = true;
                    if (hud) { player.overlay(r); flip(); }
                    else { sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3; }
                }
                else if (!gotFrame && nowSec() - lastWaitDraw > 0.12) { lastWaitDraw = nowSec(); presentWait(); }
            } else if (nowSec() - lastWaitDraw > 0.12) { lastWaitDraw = nowSec(); presentWait(); }
            sceKernelUsleep(4000);
            continue;
        }

        // ---- Menu ----
        {   // setas/analogico: repetem sozinhos enquanto seguradas; segurando mais tempo, listas longas andam mais itens por passo
            int st[4]; pin.steps(st);
            int nU = ((pressed & B_UP) ? 1 : 0) + st[0] * (st[0] ? pin.accel(0) : 1);
            int nD = ((pressed & B_DOWN) ? 1 : 0) + st[1] * (st[1] ? pin.accel(1) : 1);
            int nL = ((pressed & B_LEFT) ? 1 : 0) + st[2], nR = ((pressed & B_RIGHT) ? 1 : 0) + st[3];
            for (int i = 0; i < nU; i++) ui.handle(Input::Up);
            for (int i = 0; i < nD; i++) ui.handle(Input::Down);
            for (int i = 0; i < nL; i++) ui.handle(Input::Left);
            for (int i = 0; i < nR; i++) ui.handle(Input::Right);
            if (nU + nD + nL + nR > 0) dirty = true;
        }
        if (pressed & B_L1)     { ui.handle(Input::PrevTab);  dirty = true; }
        if (pressed & B_R1)     { ui.handle(Input::NextTab);  dirty = true; }
        if (pressed & B_L2)     { ui.handle(Input::PageUp);   dirty = true; }
        if (pressed & B_R2)     { ui.handle(Input::PageDown); dirty = true; }
        if (pressed & B_CROSS)  { ui.handle(Input::Confirm);  dirty = true; }
        if (pressed & B_CIRCLE) { ui.handle(Input::Back);     dirty = true; }
        if (updateClock()) dirty = true;

        if (pf.kbdRequested) {                    // X em "Digitar @streamer": abre o teclado na tela
            pf.kbdRequested = false;
            kbd.open(ui.textInitial(), ui.textKind == 2);
            mode = Mode::Kbd; dirty = true;
            continue;
        }
        if (pf.hasPendingText) {                  // texto confirmado no teclado: busca o canal ou baixa a lista M3U
            std::string typed = pf.pendingText; pf.pendingText.clear(); pf.hasPendingText = false;
            if (ui.textKind == 2) app.say(typed.empty() ? "Removendo a lista M3U..." : "Baixando a lista M3U...");
            else app.say("Buscando @" + kickSlug(typed) + " na Kick...");
            presentMenu();
            ui.onText(typed);                    // httpGet + (se ao vivo) play()
            trace("texto-processado");
            dirty = true;
        }
        if (!pf.pendingPlayUrl.empty()) {
            std::string url = pf.pendingPlayUrl, title = pf.pendingPlayTitle;
            pf.pendingPlayUrl.clear();
            // aplica lista de episodios (se veio de uma temporada); senao limpa
            if (!pf.pendingEpUrls.empty()) {
                player.epUrls = pf.pendingEpUrls;
                player.epTitles = pf.pendingEpTitles;
                player.epIndex = pf.pendingEpIndex;
            } else {
                player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
            }
            mode = Mode::Play; gotFrame = false; lastWaitDraw = 0;
            presentWait();                       // mostra "Player" antes de bloquear
            player.presentCb = presentWait;
            player.cancelCb = [&]() -> bool {        // O cancela o carregamento na hora; OPTIONS fecha o app
                pin.poll();
                if (pin.pending & B_CIRCLE) { pin.pending &= ~B_CIRCLE; return true; }
                if ((pin.held | pin.pending) & B_OPTIONS) { wantQuit = true; return true; }
                return false;
            };
            trace("player-start");
            player.start(url, title);
            player.quietWait = false;   // so vale para esta abertura (qualidade/reconexao/episodio)
            if (player.cancelled) { player.cancelled = false; mode = Mode::Menu; dirty = true; trace("player-cancelado"); continue; }
            trace(player.failed ? "player-falhou" : "player-aberto");
            if (player.failed) presentWait();
            if (!player.failed) autoMode = false;
            else if (autoMode && allowAuto()) {                   // a reconexao automatica falhou (rede fora?): espera 3 s e tenta de novo
                g_autoRestarts++;
                g_reconnectNote = "nova tentativa (a anterior falhou)";
                bool abort = false;
                for (int i = 0; i < 25 && !abort; i++) { presentWait(); sceKernelUsleep(120000); if (player.cancelCb && player.cancelCb()) abort = true; }
                player.stop();
                if (abort) { autoMode = false; g_reconnectNote.clear(); mode = Mode::Menu; dirty = true; continue; }
                pf.pendingPlayUrl = url; pf.pendingPlayTitle = title;
                mode = Mode::Menu; dirty = true;
            } else autoMode = false;
            continue;
        }
        if (dirty) { presentMenu(); dirty = false; static bool once = false; if (!once) { once = true; trace("menu-ok"); } }
        sceKernelUsleep(4000);                     // 4 ms: o controle e lido ~200x por segundo (antes 16 ms)
    }
    // ---- encerramento limpo: nenhuma thread pode estar viva ao sair (std::thread ativa no fim = "terminate" = erro no PS4) ----
    trace("saindo");
    if (player.h || player.h2 || mode == Mode::Play) player.stop();
    Player::waitTeardown(10.0);
    if (g_tearing.load() > 0) { trace("fim-forcado"); _Exit(0); }   // algo travou (rede): sai sem rodar destrutores
    g_feed.join();
    { auto r = takeHf(); if (r) r->join(); }
    blit::pool().shutdown();
    trace("fim");
    return 0;
}
