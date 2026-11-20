// Kick IPTV para PS4: lives da Kick (HLS) + lista M3U (canais, filmes, series).
//   Home: banner + atalhos KICK TV / FILMES / SERIES / CANAIS + LISTA M3U  (D-pad navega, X seleciona, OPTIONS sai)
//   Kick TV: X em "Digitar @streamer" abre o teclado na tela; o link da lista M3U tambem e digitado nele.
//   Teclado: Setas move, X digita, QUADRADO apaga, TRIANGULO insere http://, L2 limpa, R1 = OK, O cancela.
//   Durante o video: X = pausar | L2/R2 = toque -10s/+10s, SEGURAR anda a barra (cada vez mais rapido) e pula ao soltar (VOD) | L1/R1 = episodio ant/prox
//                    QUADRADO = qualidade | TRIANGULO = ajuste de imagem (live) | L1 = memoria (live) | O = voltar
//   HLS (.m3u8): o app baixa playlist/segmentos e entrega ao AvPlayer por callbacks de arquivo (hls.hpp / streambuf.hpp).
// Headers do C++ primeiro (mesma ordem do sample "threading" do OpenOrbis): evita conflito de tipos com os headers orbis.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include "hls.hpp"
#include "aes128.hpp"
#include "nativeplayer.hpp"
#include "mpv_player.hpp"
#include "streambuf.hpp"
#include "netstream.hpp"
#include "httpfile.hpp"
#include "progfile.hpp"
#include "tschunk.hpp"
#include "urlutil.hpp"
#include "imgcache.hpp"
#include "coverurl.hpp"
#include "netgate.hpp"
#include "listload.hpp"
#include "tsmux.hpp"
#include "blit.hpp"
#include "padin.hpp"
#include "stalldetect.hpp"
#include "texpool.hpp"
#include "tmdbkey.hpp"
#include "livebuf.hpp"
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include "ui.hpp"
#include "defaultlist.hpp"

#include "fbrender.hpp"
#include "damage.hpp"
#include "keyboard.hpp"
#include "sfx.hpp"
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

// Heap da libc sem limite: sem isso o malloc acaba (lista M3U grande + capas + cache do filme + buffers do AvPlayer) e o app fecha
// logo depois que o menu aparece (a lista e as capas comecam a encher a memoria). A versao que abria e tocava filmes tinha isto.
extern "C" {
unsigned int sceLibcHeapExtendedAlloc = 1;
size_t sceLibcHeapSize = 0xFFFFFFFFFFFFFFFFULL;   // SCE_LIBC_HEAP_SIZE_EXTENDED_ALLOC_NO_LIMIT
}
static void trace(const char *stage);   // (definida mais abaixo)

// ===== SAIR DO APP: sempre PELO SISTEMA =====
// Terminar com `return` do main, exit() ou _Exit() da CE-34878-0: essas rotinas acabam no syscall "exit" cru, que o sandbox do app recusa
// (o klog mostra SIGSYS na thread principal logo depois do ultimo log do app). Os apps da Sony e os homebrews que fecham sem erro pedem
// ao sistema: sceSystemServiceLoadExec("exit", NULL). (Fontes: PRs do sse-97/ps4-bt-audio e do FPKGi-OpenOrbis, que acharam o mesmo erro.)
extern "C" int sceSystemServiceLoadExec(const char *path, char *const argv[]);
static std::atomic<uint64_t> g_mainTick{0};      // sobe a cada volta do loop principal / quadro / passo do log (o vigia le isto)
[[noreturn]] static void ps4Exit(const char *why) {
    trace(why);
    int rc = sceSystemServiceLoadExec("exit", NULL);
    char b[64]; snprintf(b, sizeof b, "exit-pelo-sistema rc=%08X", (unsigned)rc); trace(b);
    // O sistema recusou: NAO volta ao main nem usa _Exit (os dois dariam CE-34878-0). Fica parado; o menu do PS4 ainda consegue fechar o app.
    for (;;) sceKernelUsleep(1000000);
}
// Marcadores finos do download da lista M3U: se o app fechar, a proxima abertura mostra o ULTIMO passo que foi gravado.
static bool g_traceHttp = false;                 // so durante o download da lista M3U
static const char *g_m3uTag = "m3u-t-";          // "t" = thread de rede | "s" = modo seguro (mesma thread)
static bool g_safeM3U = true;                    // modo seguro: baixa a lista sem criar thread de rede
static void traceHttp(const char *what) { if (g_traceHttp) { std::string s = std::string(g_m3uTag) + what; trace(s.c_str()); } }
// Erro ao criar template/conexao/requisicao HTTP (pool esgotado, por exemplo): grava os primeiros no log /data, com o codigo.
static void netErr(const char *what, int rc) {
    static std::atomic<int> n{0};
    if (n.fetch_add(1) >= 12) return;
    char b[64]; snprintf(b, sizeof b, "net-erro-%s-%08X", what, (unsigned)rc);
    trace(b);
}
static double monoSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

// User-Agent dos fluxos de video. Paineis IPTV recusam (ou nao entregam o video para) User-Agent de navegador; o do VLC e aceito por todos.
// Kick (CDN propria) continua com o de navegador, que e o que ja funcionava la.
static const char *streamUserAgent(const std::string &url) {
    if (url.find("kick.com") != std::string::npos || url.find("live-video.net") != std::string::npos || url.find("kick-") != std::string::npos)
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
    return "VLC/3.0.20 LibVLC/3.0.20";
}

static bool isKickCdn(const std::string &url) {
    return url.find("kick.com") != std::string::npos || url.find("live-video.net") != std::string::npos || url.find("kick-") != std::string::npos;
}

struct NetClient {
    std::vector<std::string> *log = nullptr;
    bool ready = false, failed = false;
    int httpCtx = -1;

    void L(const std::string &s) { if (log) log->push_back(s); }
    static std::string hex(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    std::mutex initMu;
    bool init() {
        std::lock_guard<std::mutex> initLk(initMu);       // capas/lista/canais podem chamar de threads diferentes: so uma inicializa
        if (ready) return true;
        if (failed) { L("(inicializacao ja falhou acima)"); return false; }
        failed = true;
        // IDs internos de modulo do PS4: NetCtl 0x80000009, Http 0x8000000A, Ssl 0x8000000B, Net 0x8000001C
        const uint32_t ids[4] = {0x8000001C, 0x80000009, 0x8000000A, 0x8000000B};
        const char *names[4] = {"Net", "NetCtl", "Http", "Ssl"};
        for (int i = 0; i < 4; i++) L(std::string("carregar modulo ") + names[i] + ": " + hex(sceSysmoduleLoadModuleInternal(ids[i])));
        int rc = sceNetInit();                         L("sceNetInit: " + hex(rc));
        int netMem = sceNetPoolCreate("kickiptv", 512 * 1024, 0); L("sceNetPoolCreate: " + hex(netMem));   // pools maiores: varias conexoes HTTPS ao mesmo tempo (capas + video)
        int ssl = sceSslInit(1024 * 1024);             L("sceSslInit: " + hex(ssl));
        httpCtx = sceHttpInit(netMem, ssl, 1024 * 1024); L("sceHttpInit: " + hex(httpCtx));
        if (httpCtx < 0) return false;
        ready = true;
        failed = false;
        return true;
    }

    // GET simples. Retorna true se conseguiu falar com o servidor (mesmo com status de erro).
    bool get(const std::string &url, std::string &body, int &status, size_t maxBytes = 1536 * 1024,
             const char *userAgent = nullptr, const char *referer = nullptr, unsigned timeoutSec = 15) {
        std::lock_guard<std::recursive_mutex> httpLk(httpMx);
        status = 0; body.clear();
        if (!init()) return false;
        const char *ua = userAgent ? userAgent : "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
        int tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        L("  CreateTemplate: " + hex(tmpl));
        if (tmpl < 0) return false;
        sceHttpsDisableOption(tmpl, 0x1F);   // sem verificacao de certificado (o app nao carrega os CAs do PS4)
        sceHttpSetAutoRedirect(tmpl, 1);
        const unsigned T = timeoutSec * 1000 * 1000;
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
                if (referer) {
                    sceHttpAddRequestHeader(req, "Referer", referer, 0);
                    std::string org = originOf(referer);
                    if (!org.empty()) sceHttpAddRequestHeader(req, "Origin", org.c_str(), 0);
                }
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

    // Requisicao da API oficial da Kick: GET com "Authorization: Bearer ..." ou POST de formulario (token). Resposta pequena (JSON).
    bool request(bool post, const std::string &url, const std::string &auth, const std::string &postBody, std::string &body, int &status) {
        std::lock_guard<std::recursive_mutex> httpLk(httpMx);
        status = 0; body.clear();
        if (!init()) return false;
        int tmpl = sceHttpCreateTemplate(httpCtx, "KickIPTV/1.0 (PlayStation 4)", 1 /*HTTP/1.1*/, 1);
        if (tmpl < 0) return false;
        sceHttpsDisableOption(tmpl, 0x1F);
        sceHttpSetAutoRedirect(tmpl, 1);
        const unsigned T = 15 * 1000 * 1000;
        sceHttpSetResolveTimeOut(tmpl, T); sceHttpSetConnectTimeOut(tmpl, T); sceHttpSetSendTimeOut(tmpl, T); sceHttpSetRecvTimeOut(tmpl, T);
        int conn = sceHttpCreateConnectionWithURL(tmpl, url.c_str(), 1);
        bool ok = false;
        if (conn >= 0) {
            int req = sceHttpCreateRequestWithURL(conn, post ? 1 /*POST*/ : 0 /*GET*/, url.c_str(), post ? (unsigned long long)postBody.size() : 0);
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "application/json", 0);
                if (post) sceHttpAddRequestHeader(req, "Content-Type", "application/x-www-form-urlencoded", 0);
                if (!auth.empty()) sceHttpAddRequestHeader(req, "Authorization", auth.c_str(), 0);
                int rc = post ? sceHttpSendRequest(req, postBody.data(), postBody.size()) : sceHttpSendRequest(req, NULL, 0);
                if (rc >= 0) {
                    sceHttpGetStatusCode(req, &status);
                    std::vector<char> buf(32 * 1024);
                    for (;;) {
                        int n = sceHttpReadData(req, buf.data(), buf.size());
                        if (n <= 0) break;
                        body.append(buf.data(), n);
                        if (body.size() > 2u * 1024 * 1024) break;
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
        const char *ua = streamUserAgent(url);
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
    std::recursive_mutex sessMx;              // abrir/fechar conexoes uma de cada vez
    std::recursive_mutex httpMx;              // TODA operacao sceHttp serializada (evita CE ao misturar capas/M3U/nativo)
    void closeSession(Session &s) {
        std::lock_guard<std::recursive_mutex> lk(sessMx);
        if (s.conn >= 0) sceHttpDeleteConnection(s.conn);
        if (s.tmpl >= 0) sceHttpDeleteTemplate(s.tmpl);
        unsigned keep = s.recvSec;
        s = Session(); s.recvSec = keep;
    }
    bool openSession(Session &s, const std::string &url) {
        std::lock_guard<std::recursive_mutex> lk(sessMx);
        closeSession(s);
        if (!init()) return false;
        const char *ua = streamUserAgent(url);
        s.tmpl = sceHttpCreateTemplate(httpCtx, ua, 1 /*HTTP/1.1*/, 1);
        if (s.tmpl < 0) { netErr("template", s.tmpl); s.tmpl = -1; return false; }
        sceHttpsDisableOption(s.tmpl, 0x1F);
        sceHttpSetAutoRedirect(s.tmpl, 1);
        const unsigned T = 15 * 1000 * 1000, TR = s.recvSec * 1000u * 1000u;   // conexao 15 s; envio/recepcao 8 s (conexao morta no meio de um segmento nao trava mais 15 s)
        sceHttpSetResolveTimeOut(s.tmpl, T);
        sceHttpSetConnectTimeOut(s.tmpl, T);
        sceHttpSetSendTimeOut(s.tmpl, TR);
        sceHttpSetRecvTimeOut(s.tmpl, TR);
        s.conn = sceHttpCreateConnectionWithURL(s.tmpl, url.c_str(), 1 /*keep-alive*/);
        if (s.conn < 0) { netErr("conexao", s.conn); s.conn = -1; closeSession(s); return false; }
        s.origin = originOf(url);
        return true;
    }
    // GET reaproveitando a conexao de 's' (so use de UMA thread por sessao). Se a conexao antiga caiu, refaz uma vez.
    // Leitura interrompida no meio (erro de rede) conta como falha: antes devolvia o segmento cortado como se fosse bom.
    // maxSecs > 0: prazo TOTAL da resposta. O timeout do sceHttp vale por leitura; uma conexao que entrega bytes devagar nunca estourava.
    bool getKA(Session &s, const std::string &url, std::string &body, int &status, size_t maxBytes = 1536 * 1024, double maxSecs = 0) {
        for (int attempt = 0; attempt < 2; attempt++) {
            status = 0; body.clear();
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
            bool fresh = false;
            if (s.conn < 0 || s.origin != originOf(url)) { if (!openSession(s, url)) return false; fresh = true; }
            int req = sceHttpCreateRequestWithURL(s.conn, 0 /*GET*/, url.c_str(), 0);
            bool ok = false, clean = false;
            if (req >= 0) {
                sceHttpAddRequestHeader(req, "Accept", "*/*", 0);
                if (isKickCdn(url)) {                      // CDN da Kick: manda os cabecalhos do player web (Referer/Origin da Kick)
                    sceHttpAddRequestHeader(req, "Referer", "https://kick.com/", 0);
                    sceHttpAddRequestHeader(req, "Origin", "https://kick.com", 0);
                }
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
                        if (maxSecs > 0) {
                            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
                            if ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9 > maxSecs) { ok = clean = false; break; }   // lento demais: conta como falha
                        }
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
        std::lock_guard<std::recursive_mutex> httpLk(httpMx);
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

// Objetos globais com threads/rede ficam "vazados" de proposito (nunca destruidos): se o PS4 encerrar o processo pelo caminho normal
// (exit), os destrutores nao ficam esperando thread presa em rede (travava o fechamento) nem chamam std::terminate com thread ativa.
static NetClient &g_net = *new NetClient;

// HTTP do app -> libmpv stream_cb (khttp://). Sessao HTTP reaproveitada por stream.
// Leitura em blocos de 1 MB (antes: 1 requisicao HTTP por leitura de 128 KB => ~350 KB/s). Sem thread extra: a leitura roda
// na thread do demuxer do mpv, como na versao que ja funcionava (a thread de leitura antecipada derrubava o app).
struct KickMpvStream {
    std::string url;
    NetClient::Session sess;
    uint64_t size = 0;
    bool opened = false;
    std::string cache;         // bytes [cacheOff, cacheOff + cache.size())
    uint64_t cacheOff = 0;
    bool live = false;         // live Kick via HlsFeeder (kicklive://)
    uint64_t abs = 0;          // posicao propria no StreamBuf do alimentador
};
// Live da Kick: a URL .m3u8 NAO vai para o mpv (ele recebia o TEXTO da playlist como se fosse o video: "unrecognized file format").
// O HlsFeeder (mais abaixo) escolhe a resolucao, baixa os segmentos .ts e o mpv le o fluxo MPEG-TS continuo por "kicklive://live.ts".
static std::string g_kickLiveUrl;
static void *kickLiveOpen(uint64_t *size_out);
static int64_t kickLiveRead(KickMpvStream *s, char *buf, uint64_t n);
static void kickLiveClose();
static bool kickLooksLikeMedia(const std::string &b) {
    if (b.size() < 4) return false;
    // ISO BMFF / MP4: ....ftyp
    if (b.size() >= 8 && b[4]=='f' && b[5]=='t' && b[6]=='y' && b[7]=='p') return true;
    // Matroska / WebM
    if ((unsigned char)b[0]==0x1A && (unsigned char)b[1]==0x45 && (unsigned char)b[2]==0xDF && (unsigned char)b[3]==0xA3) return true;
    // MPEG-TS
    if ((unsigned char)b[0]==0x47) return true;
    // RIFF / AVI
    if (b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F') return true;
    // MPEG-PS / pack
    if ((unsigned char)b[0]==0x00 && (unsigned char)b[1]==0x00 && (unsigned char)b[2]==0x01 &&
        ((unsigned char)b[3]==0xBA || (unsigned char)b[3]==0xB3 || (unsigned char)b[3]==0xB6)) return true;
    // ID3 then maybe audio - still ok for mpv
    if (b[0]=='I' && b[1]=='D' && b[2]=='3') return true;
    return false;
}
static bool kickLooksLikeHtml(const std::string &b) {
    // pagina de erro do painel / CDN
    if (b.size() < 12) return false;
    if (b[0]=='<' || (b[0]=='\xef' && b.size()>3)) {
        auto l = b.substr(0, 64);
        for (char &c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return l.find("<!doctype") != std::string::npos || l.find("<html") != std::string::npos ||
               l.find("<head") != std::string::npos || l.find("<error") != std::string::npos;
    }
    return false;
}
static const uint64_t kMpvChunk = 2 * 1024 * 1024;   // 2 MB por pedido: menos idas a rede

static void *kickMpvOpen(void * /*ud*/, const char *url, uint64_t *size_out) {
    if (!url || !*url) return nullptr;
    if (!strncmp(url, "kicklive://", 11)) return kickLiveOpen(size_out);
    if (!g_net.init()) return nullptr;
    auto *s = new KickMpvStream();
    s->url = url;
    if (!g_net.openSession(s->sess, s->url)) { delete s; return nullptr; }
    s->opened = true;
    std::string tip; int st = 0; uint64_t total = 0;
    bool got = g_net.getRange(s->sess, s->url, 0, kMpvChunk - 1, tip, st, &total);
    // alguns paineis recusam Range: tenta GET simples se o Range falhou
    if (!got || (st != 206 && st != 200) || tip.empty()) {
        g_net.closeSession(s->sess); s->opened = false;
        if (!g_net.openSession(s->sess, s->url)) { delete s; return nullptr; }
        s->opened = true;
        tip.clear(); st = 0; total = 0;
        // GET sem Range: le so o comeco (o getRange com from=0 ainda manda Range; se falhar de novo, desiste)
        got = g_net.getRange(s->sess, s->url, 0, 64 * 1024 - 1, tip, st, &total);
        if (!got || tip.empty() || (st != 206 && st != 200)) {
            g_net.closeSession(s->sess); delete s; return nullptr;
        }
    }
    if (kickLooksLikeHtml(tip)) { g_net.closeSession(s->sess); delete s; return nullptr; }
    if (st == 200 && total == 0) total = tip.size();
    s->size = total;
    if (size_out) *size_out = total;
    s->cache = std::move(tip); s->cacheOff = 0;
    trace("mpv-stream-aberto");
    return s;
}
static int64_t kickMpvRead(void *cookie, uint64_t offset, char *buf, uint64_t n) {
    auto *s = (KickMpvStream *)cookie;
    if (!s || !buf || n == 0) return -1;
    if (s->live) return kickLiveRead(s, buf, n);
    static std::atomic<int> nrd{0};
    if (nrd.fetch_add(1) < 6) { char t[96]; snprintf(t, sizeof t, "mpv-read off=%llu n=%llu", (unsigned long long)offset, (unsigned long long)n); trace(t); }
    if (s->size > 0 && offset >= s->size) return 0;
    if (offset < s->cacheOff || offset >= s->cacheOff + s->cache.size()) {
        if (!s->opened) { if (!g_net.openSession(s->sess, s->url)) return -1; s->opened = true; }
        std::string body; int st = 0; uint64_t total = 0; bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            body.clear(); st = 0; total = 0;
            if (g_net.getRange(s->sess, s->url, offset, offset + kMpvChunk - 1, body, st, &total) && (st == 206 || st == 416 || (st == 200 && offset == 0))) { ok = true; break; }
            g_net.closeSession(s->sess); s->opened = false;
            if (!g_net.openSession(s->sess, s->url)) return -1;
            s->opened = true;
        }
        if (!ok) return -1;
        if (total > 0) s->size = total;
        if (st == 416 || body.empty()) return 0;
        s->cache = std::move(body); s->cacheOff = offset;
    }
    uint64_t from = offset - s->cacheOff;
    uint64_t take = s->cache.size() - from;
    if (take > n) take = n;
    memcpy(buf, s->cache.data() + from, (size_t)take);
    return (int64_t)take;
}
static void kickMpvClose(void *cookie) {
    auto *s = (KickMpvStream *)cookie;
    if (!s) return;
    if (s->live) { kickLiveClose(); delete s; return; }
    if (s->opened) g_net.closeSession(s->sess);
    delete s;
}
static void kickMpvRegisterIo() {
    mpv_player::StreamIo io;
    io.open = &kickMpvOpen;
    io.read = &kickMpvRead;
    io.close = &kickMpvClose;
    io.ud = nullptr;
    mpv_player::setStreamIo(io);
    mpv_player::setTrace([](const char *st) { trace(st); });
}

static ImgCache &g_covers = *new ImgCache;
static NetGate g_coverGate(3, 2);                // no maximo 3 capas baixando juntas; o lote de fundo usa no maximo 2 (sempre sobra vaga para a tela)
static std::atomic<bool> g_quitting{false};

// ---------- Plataforma ----------
struct Ps4Platform : Platform {
    App *app = nullptr;
    std::string pendingText;                 // texto digitado no teclado na tela (@streamer ou link M3U)
    bool hasPendingText = false;
    bool kbdRequested = false;               // UI pediu o teclado
    std::string pendingPlayUrl, pendingPlayTitle;
    std::string pendingKickSlug; // reconectar live Kick com URL nova

    std::vector<std::string> pendingEpUrls, pendingEpTitles;
    int pendingEpIndex = -1;
    std::atomic<int> lastStatus_{0};
    bool httpGet(const std::string &url, std::string &body) override {
        int st = 0;
        bool ok = g_net.get(url, body, st, 48u * 1024 * 1024);      // categorias de paineis grandes passam de 1,5 MB de JSON
        lastStatus_ = ok ? st : 0;
        return ok && st >= 200 && st < 300;
    }
    std::function<void(const std::string &)> busyFn;             // desenha um quadro antes de uma consulta que bloqueia (categoria, detalhes)
    void busy(const std::string &m) override { if (busyFn) busyFn(m); }
    int lastHttpStatus() override { return lastStatus_.load(); }
    bool httpApi(const char *method, const std::string &url, const std::string &auth, const std::string &body, std::string &out, int &status) override {
        bool ok = g_net.request(std::string(method) == "POST", url, auth, body, out, status);
        char b[64]; snprintf(b, sizeof b, "kick-api-%s-%d", method, ok ? status : 0); trace(b);
        return ok;
    }
    // Kick: o Cloudflare da Kick recusa (403) clientes cujo "User-Agent" nao combina com o jeito de conectar. O PS4 conecta como o
    // navegador do proprio PS4, entao primeiro tentamos com o User-Agent dele (e Referer da Kick); depois com o do Chrome.
    bool httpGetKick(const std::string &url, std::string &body) override {
        static const char *kUa[2] = {
            "Mozilla/5.0 (PlayStation; PlayStation 4/11.00) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.0 Safari/605.1.15",
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36"};
        // No log do PS4 a 1a tentativa (User-Agent do PS4) caia sem resposta e a 2a (Chrome) funcionava: cada abertura de live esperava a 1a falhar.
        // Agora lembra qual funcionou por ultimo e comeca por ele; sem historico, a 1a tentativa desiste mais cedo (6 s).
        static std::atomic<int> good{-1};
        int st = 0;
        const int first = good.load() >= 0 ? good.load() : 0;
        for (int k = 0; k < 2; k++) {
            const int i = (first + k) % 2;
            body.clear();
            const unsigned to = (k == 0 && good.load() < 0) ? 6 : 15;
            bool ok = g_net.get(url, body, st, 1536 * 1024, kUa[i], "https://kick.com/", to);
            lastStatus_ = ok ? st : 0;
            char b[48]; snprintf(b, sizeof b, "kick-http-%d-%d", i, ok ? st : 0); trace(b);
            if (ok && st >= 200 && st < 300 && !body.empty()) { good = i; return true; }
            if (ok && st == 404) return false;              // o canal nao existe: outro User-Agent nao muda isso
        }
        return false;
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
        lastStatus_ = st;                                         // a tela mostra o motivo da falha (app.hpp: httpReason)
        if (!ok) trace("m3u-desistiu");
        return ok;
    }
    // Primeiros bytes da ultima lista baixada: se o provedor devolver um aviso em vez da lista, a tela mostra o que ele disse.
    std::mutex headMu_; std::string head_;
    void noteHead(const char *d, size_t n) {
        std::lock_guard<std::mutex> l(headMu_);
        if (head_.size() < 160) head_.append(d, std::min<size_t>(n, 160 - head_.size()));
    }
    void resetHead() { std::lock_guard<std::mutex> l(headMu_); head_.clear(); }
    std::string lastBodyHead() override { std::lock_guard<std::mutex> l(headMu_); return head_; }
    // Entrega em pedacos para a thread principal (que continua desenhando; O cancela mesmo se o servidor demorar a responder).
    bool httpStream(const std::string &url, const std::function<bool(const char *, size_t)> &sink0) override {
        cancelled = false; m3uRetry = 0;
        g_m3uTag = g_safeM3U ? "m3u-s-" : "m3u-t-";
        trace(g_safeM3U ? "m3u-sync-inicio" : "m3u-inicio");
        g_traceHttp = true;
        g_net.init();                                             // inicializa a rede na thread principal
        traceHttp("net-ok");
        bool firstChunk = true;
        resetHead();
        std::function<bool(const char *, size_t)> sink = [&](const char *d, size_t n) {
            if (firstChunk) { firstChunk = false; trace("m3u-parse"); }
            noteHead(d, n);
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
    // ---- Versoes SEGURAS PARA THREAD (carregamento em segundo plano): nao desenham, nao leem o controle e nao mexem em
    // membros compartilhados da plataforma. 'slot' vem pronto da thread principal (cacheInfo).
    static bool cacheReadSlot(int slot, const std::function<bool(const char *, size_t)> &sink, const std::atomic<bool> &stop) {
        if (slot < 0) return false;
        int fd = sceKernelOpen(cachePath(slot).c_str(), 0, 0);
        if (fd < 0) return false;
        std::vector<char> buf(256 * 1024);
        bool ok = true;
        for (;;) {
            if (stop.load()) { ok = false; break; }
            long n = (long)sceKernelRead(fd, buf.data(), buf.size());
            if (n < 0) { ok = false; break; }
            if (n == 0) break;
            if (!sink(buf.data(), (size_t)n)) { ok = false; break; }
        }
        sceKernelClose(fd);
        return ok;
    }
    // Baixa a lista (retomando se a conexao cair) e grava a copia local num arquivo PROPRIO desta chamada.
    bool netDownloadBg(const std::string &url, int curSlot, const std::function<bool(const char *, size_t)> &sink, const std::atomic<bool> &stop) {
        int slot = curSlot == 0 ? 1 : 0;                                 // grava no arquivo que nao esta em uso
        FILE *f = fopen(cachePath(slot).c_str(), "wb");
        bool bad = f == nullptr;
        resetHead();
        netstream::Push push = [&](const char *d, size_t n) -> bool {
            if (stop.load()) return false;
            if (n == 0) return true;
            noteHead(d, n);
            if (f && !bad && fwrite(d, 1, n, f) != n) bad = true;       // disco cheio etc.
            return sink(d, n);
        };
        int st = 0;
        bool ok = fetchM3U(url, push, st);
        bool closed = true;
        if (f) closed = fclose(f) == 0;
        if (ok && closed && !bad && !stop.load()) {
            saveSetting("listcache", std::string(slot ? "1" : "0") + "|" + url);
            if (curSlot >= 0 && curSlot != slot) remove(cachePath(curSlot).c_str());
        } else remove(cachePath(slot).c_str());                          // descarta; a copia antiga (se houver) continua valendo
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
    void line(const std::string &, uint32_t = 0xFFE8ECF4u) {}      // sem lista de passos na abertura (o log continua gravado em /data)
};

// ---------- Registro de passos em /data/kickiptv.log ----------
// Se o app cair, na proxima abertura o menu mostra em qual passo ele parou.
static std::string g_prevStage;           // (leitura do log anterior removida: usava fread, que o app original nao importava)
static const bool kCoversOn = true;    // capas de filmes, series e canais (versao estavel: 2 trabalhadores, rede serializada, sem baixador em massa)
static std::string g_prevRaw = "(nenhum)";      // ultima linha do log da vez anterior, mesmo se foi saida normal
static std::vector<std::string> g_prevTail;   // ultimas linhas do log da vez anterior (so quando o app nao saiu direito)
static void traceInit() {
    // le o log da ultima vez (libkernel, como as configuracoes) para mostrar na tela em que passo o app parou
    std::string all;                          // conteudo do log da vez anterior (fica visivel fora do bloco para ser guardado abaixo)
    int fd = sceKernelOpen("/data/kickiptv.log", 0, 0);
    if (fd >= 0) {
        char buf[1024];
        for (;;) { long n = (long)sceKernelRead(fd, buf, sizeof buf); if (n <= 0) break; all.append(buf, (size_t)n); if (all.size() > 400000) break; }
        sceKernelClose(fd);
        while (!all.empty() && (all.back() == '\n' || all.back() == '\r')) all.pop_back();
        size_t p = all.rfind('\n');
        std::string last = p == std::string::npos ? all : all.substr(p + 1);
        if (!last.empty()) g_prevRaw = last;
        if (!last.empty() && last[0] == '[') { size_t rb = last.find("] "); if (rb != std::string::npos && rb < 14) last = last.substr(rb + 2); }     // tira o carimbo de tempo
        if (!last.empty() && last != "fim" && last != "saindo" && last != "inicio") {
            g_prevStage = last;
            std::vector<std::string> v; size_t st = 0;
            while (st < all.size()) { size_t nl = all.find('\n', st); if (nl == std::string::npos) nl = all.size(); if (nl > st) v.push_back(all.substr(st, nl - st)); st = nl + 1; }
            for (size_t i = v.size() > 8 ? v.size() - 8 : 0; i < v.size(); i++) g_prevTail.push_back(v[i]);
        }
    }
    if (!all.empty()) {                       // guarda o log da vez anterior: abrir o app de novo nao apaga mais o log do travamento
        FILE *pv = fopen("/data/kickiptv_anterior.log", "wb");
        if (pv) { fwrite(all.data(), 1, all.size(), pv); fwrite("\n", 1, 1, pv); fclose(pv); }
    }
    FILE *f = fopen("/data/kickiptv.log", "wb");
    if (f) { fwrite("inicio\n", 1, 7, f); fclose(f); }
}
static std::mutex g_traceMu;
static char g_lastTrace[96];                        // ultimo passo gravado (o registrador de falha mostra qual era)
static void trace(const char *stage) {
    g_mainTick++;
    std::lock_guard<std::mutex> lk(g_traceMu);
    strncpy(g_lastTrace, stage, sizeof g_lastTrace - 1); g_lastTrace[sizeof g_lastTrace - 1] = 0;
    FILE *f = fopen("/data/kickiptv.log", "ab");
    if (f) {
        static const double t0 = monoSec();                       // segundos desde o 1o registro: mostra ONDE o tempo vai
        char ts[24]; int tn = snprintf(ts, sizeof ts, "[%7.2f] ", monoSec() - t0);
        fwrite(ts, 1, (size_t)tn, f); fwrite(stage, 1, strlen(stage), f); fwrite("\n", 1, 1, f); fclose(f);
    }
    // Tambem sai no log do sistema (klog): a mesma captura que mostra "Suspend App"/"App Crash" passa a mostrar, na ordem certa,
    // o que o app estava fazendo antes (menu-ps, av-close, avmod-descarregado...). Prefixo [kickiptv] para dar grep.
    fprintf(stderr, "[kickiptv] %s\n", stage);
}

// ===== VIGIA: se a thread principal parar de andar (travada), registra onde e fecha o app PELO SISTEMA em vez de deixar o PS4 mostrar erro =====
// Conta em segundos do proprio vigia; se o console dormiu/suspendeu (volta de repouso, menu do PS) o intervalo fica grande e nao conta.
static void startWatchdog() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    try {
        std::thread([] {
            StallDetector det;                                                      // (testado em tests/test_stalldetect.cpp)
            for (;;) {
                sceKernelUsleep(1000000);
                int stale = det.step(g_mainTick.load(), monoSec());
                if (stale == 30 || stale == 60 || stale == 90) { char b[96]; snprintf(b, sizeof b, "watchdog-main-parado-%ds ultimo=%s", stale, g_lastTrace); trace(b); g_mainTick--; }
                if (stale >= 120) { char b[96]; snprintf(b, sizeof b, "watchdog-fechando ultimo=%s", g_lastTrace); ps4Exit(b); }
            }
        }).detach();
    } catch (...) {}
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
// "weak": se o stub do toolchain nao tiver o simbolo, o link NAO quebra (a chamada e pulada)
int sceSysmoduleUnloadModule(uint16_t id) __attribute__((weak));
int sceSysmoduleUnloadModuleInternal(uint32_t id) __attribute__((weak));
KAvHandle sceAvPlayerInit(KAvInitData *init);
int sceAvPlayerAddSource(KAvHandle h, const char *url);
bool sceAvPlayerIsActive(KAvHandle h);
bool sceAvPlayerGetVideoData(KAvHandle h, KAvFrameInfo *fi);
bool sceAvPlayerGetAudioData(KAvHandle h, KAvFrameInfo *fi);
int sceAvPlayerStop(KAvHandle h);
int sceAvPlayerClose(KAvHandle h);
// Pause/Resume/CurrentTime: em alguns firmwares/stubs do homebrew essas NIDs
// crasham o app (CE-34878-0) ao abrir VOD. Pausa e tempo ficam em software no Player.
// JumpToTime (seek de verdade, frente e tras) e declarada "weak": se o stub do toolchain nao tiver o simbolo o link NAO quebra
// (o app usa o seek antigo). Como essa chamada ja deu CE-34878-0 em alguns firmwares, ha uma trava (ver kJumpGuardFile abaixo).
int sceAvPlayerJumpToTime(KAvHandle h, uint64_t ms) __attribute__((weak));
typedef int (*KAvLogCb)(void *ud, const char *fmt, va_list ap);
int sceAvPlayerSetLogCallback(KAvLogCb cb, void *ud);
}

// Trava do salto (JumpToTime): o marcador kJumpGuardFile fica gravado enquanto um salto esta em andamento (ate passar um tempo
// com imagem estavel). Se o app CAIR nesse intervalo, na abertura seguinte o salto e desligado de vez (kNoJumpFile) e o app volta ao
// seek antigo. Para religar: apague /data/kickiptv_nojump (ex.: por FTP).
static const char *kJumpGuardFile = "/data/kickiptv_jumpguard";
static const char *kNoJumpFile = "/data/kickiptv_nojump";
// Segundo plano: por padrao o app ENCERRA SOZINHO logo depois de ir para o inicio do PS4 (com player/rede ja parados). Se ele ficasse
// vivo, o sistema precisava mata-lo ao "Fechar aplicativo" / ao abrir outro app e dava CE-34878-0 (o app nao morria a tempo).
// Para manter o comportamento antigo (app continua vivo no segundo plano), crie o arquivo /data/kickiptv_keepbg (ex.: por FTP).
static const char *kKeepBgFile = "/data/kickiptv_keepbg";
static std::atomic<int> g_jumpMode{0};                 // 1 = usa JumpToTime | 0 = desligado (definido na abertura do app)
static bool fileExists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return false; fclose(f); return true; }
static void touchFile(const char *p) { FILE *f = fopen(p, "wb"); if (f) { fwrite("1", 1, 1, f); fclose(f); } }

// ---------- Registrador de falha ----------
// Quando o app cai com SIGBUS/SIGSEGV (o erro 0xa0d0c00a do PS4 e SIGBUS), grava em /data/kickiptv_crash.log (e no klog) o ENDERECO onde caiu
// (rip), os registradores e um trecho da pilha, mais o ultimo passo do log. Depois devolve a falha ao PS4 (relatorio normal de erro).
// So usa write() com texto montado a mao (nada de malloc/stdio dentro do tratador). Offsets do ucontext/siginfo sao os do FreeBSD/amd64.
static volatile int g_inCrash = 0;
static int g_crashFd = -1;
static void cwrite(const char *s) {
    size_t n = 0; while (s[n]) n++;
    if (g_crashFd >= 0) write(g_crashFd, s, n);
    write(2, s, n);
}
static void chex(const char *label, unsigned long v) {
    char b[56]; int p = 0;
    while (label[p] && p < 16) { b[p] = label[p]; p++; }
    if (p) b[p++] = '=';
    b[p++] = '0'; b[p++] = 'x';
    for (int i = 60; i >= 0; i -= 4) b[p++] = "0123456789abcdef"[(v >> i) & 15];
    b[p++] = ' '; b[p] = 0; cwrite(b);
}
#if defined(__FreeBSD__)
typedef struct __siginfo CrashSigInfo;      // o signal.h do toolchain do PS4 (estilo FreeBSD) declara o tratador assim
#else
typedef siginfo_t CrashSigInfo;
#endif
static void crashHandler(int sig, CrashSigInfo *si, void *uc) {
    if (g_inCrash) _exit(1);                // falha dentro do proprio tratador: nao tenta de novo
    g_inCrash = 1;
    cwrite("[kickiptv] CRASH ");
    chex("sig", (unsigned long)sig);
    chex("si", (unsigned long)si); chex("uc", (unsigned long)uc);    // os ponteiros recebidos (se forem lixo, da para ver aqui)
    const unsigned char *s = (const unsigned char *)si, *u = (const unsigned char *)uc;
    // O log anterior parava logo depois de "sig=": o tratador caia lendo o siginfo (ponteiro invalido neste sistema) e nada mais era gravado.
    // Agora cada campo e gravado NA HORA (write) e na ordem do mais util para o menos seguro: rip e registradores, pilha, ultimo passo
    // e so por ultimo o siginfo. Se ele ainda derrubar o tratador, o que importa ja esta no arquivo.
    if (u) {
        chex("rip", *(const unsigned long *)(u + 176)); chex("rsp", *(const unsigned long *)(u + 200)); chex("rbp", *(const unsigned long *)(u + 88));
        chex("rax", *(const unsigned long *)(u + 72)); chex("rdi", *(const unsigned long *)(u + 24)); chex("rsi", *(const unsigned long *)(u + 32));
        chex("rdx", *(const unsigned long *)(u + 40)); chex("rcx", *(const unsigned long *)(u + 48));
        chex("trap", (unsigned long)*(const int *)(u + 144)); chex("err", *(const unsigned long *)(u + 168));
        unsigned long rsp = *(const unsigned long *)(u + 200);
        if (rsp > 0x10000 && (rsp & 7) == 0) { cwrite("\n[kickiptv] STACK "); for (int i = 0; i < 24; i++) chex("", *(const unsigned long *)(rsp + (unsigned long)i * 8)); }
    }
    cwrite("\n[kickiptv] ultimo-passo: "); cwrite(g_lastTrace); cwrite("\n");
    if (s) { chex("code", (unsigned long)*(const int *)(s + 8)); chex("addr", *(const unsigned long *)(s + 24)); cwrite("\n"); }
    struct sigaction dfl; memset(&dfl, 0, sizeof dfl); dfl.sa_handler = SIG_DFL; sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);             // volta ao padrao: a instrucao e reexecutada e o PS4 trata a queda como sempre
    g_inCrash = 0;
}
// O sistema fecha um app suspenso mandando um sinal de encerramento. Sem tratador, o padrao do libc chama exit() -> syscall "exit" cru -> CE-34878-0.
// Com este tratador o app pede a saida AO SISTEMA (como os apps da Sony).
static void closeSignalHandler(int sig) {
    char b[48]; snprintf(b, sizeof b, "sinal-fechar sig=%d", sig);
    ps4Exit(b);
}
static void installCrashLog() {
    {   // sinais de encerramento -> saida pelo sistema
        struct sigaction st; memset(&st, 0, sizeof st);
        { void (*fp)(int) = closeSignalHandler; memcpy(&st, &fp, sizeof fp); }
        sigemptyset(&st.sa_mask);
        const int tsigs[] = {SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSYS};
        for (int sg : tsigs) sigaction(sg, &st, NULL);
    }
    g_crashFd = open("/data/kickiptv_crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    // O signal.h do toolchain define a macro 'sa_sigaction' quebrada (expande para um membro que nao existe) e nao compila.
    // O tratador fica no PRIMEIRO campo da struct (union sa_handler/sa_sigaction, igual no FreeBSD/amd64): grava o ponteiro la direto.
    static_assert(sizeof(void *) == 8, "amd64");
    { void (*fp)(int, CrashSigInfo *, void *) = crashHandler; memcpy(&sa, &fp, sizeof fp); }
    sa.sa_flags = SA_SIGINFO; sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGBUS, SIGSEGV, SIGILL, SIGFPE, SIGABRT};
    int ok = 0; for (int sg : sigs) if (sigaction(sg, &sa, NULL) == 0) ok++;
    // hnd=1: o ponteiro gravado esta mesmo no campo sa_handler (confirma o layout da struct neste toolchain)
    char b[96]; snprintf(b, sizeof b, "crashlog fd=%d sigaction-ok=%d/5 hnd=%d", g_crashFd, ok, (int)((void *)sa.sa_handler == (void *)crashHandler)); trace(b);
    if (g_crashFd >= 0) { cwrite("[kickiptv] --- app iniciado ---\n"); }
}


// Duracao do MP4 a partir do atomo mvhd (ms). 0 = nao achou.
static uint64_t mp4DurationMsFromFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    // le ate 4 MB do inicio (moov costuma estar no comeco ou no fim — tenta inicio primeiro)
    const size_t CAP = 4 * 1024 * 1024;
    std::vector<char> buf(CAP);
    size_t n = fread(buf.data(), 1, CAP, f);
    if (n < 16) { fclose(f); return 0; }
    auto scan = [](const uint8_t *d, size_t n) -> uint64_t {
        size_t a = 0;
        while (a + 8 <= n) {
            uint32_t sz = (uint32_t)d[a] << 24 | (uint32_t)d[a+1] << 16 | (uint32_t)d[a+2] << 8 | d[a+3];
            size_t hdr = 8;
            if (sz == 1) {
                if (a + 16 > n) break;
                uint64_t sz64 = 0;
                for (int i = 0; i < 8; i++) sz64 = (sz64 << 8) | d[a + 8 + i];
                sz = (uint32_t)sz64; // truncado ok para bounds
                hdr = 16;
            } else if (sz == 0) sz = (uint32_t)(n - a);
            if (sz < hdr || a + sz > n) break;
            const char *typ = (const char *)(d + a + 4);
            if (typ[0]=='m' && typ[1]=='o' && typ[2]=='o' && typ[3]=='v') {
                // procura mvhd dentro do moov
                size_t b = a + hdr, end = a + sz;
                while (b + 8 <= end) {
                    uint32_t s2 = (uint32_t)d[b] << 24 | (uint32_t)d[b+1] << 16 | (uint32_t)d[b+2] << 8 | d[b+3];
                    size_t h2 = 8;
                    if (s2 == 1) { if (b + 16 > end) break; h2 = 16; }
                    else if (s2 == 0) s2 = (uint32_t)(end - b);
                    if (s2 < h2 || b + s2 > end) break;
                    if (d[b+4]=='m' && d[b+5]=='v' && d[b+6]=='h' && d[b+7]=='d') {
                        const uint8_t *p = d + b + h2;
                        if (b + h2 + 20 > end) break;
                        uint8_t ver = p[0];
                        uint32_t timescale = 0;
                        uint64_t duration = 0;
                        if (ver == 1) {
                            if (b + h2 + 32 > end) break;
                            timescale = (uint32_t)p[20] << 24 | (uint32_t)p[21] << 16 | (uint32_t)p[22] << 8 | p[23];
                            for (int i = 0; i < 8; i++) duration = (duration << 8) | p[24 + i];
                        } else {
                            timescale = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
                            duration = (uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19];
                        }
                        if (timescale > 0 && duration > 0) {
                            return (duration * 1000ULL) / timescale;
                        }
                        return 0;
                    }
                    b += s2;
                }
            }
            a += sz;
        }
        return 0;
    };
    uint64_t ms = scan((const uint8_t *)buf.data(), n);
    // se nao achou no inicio, tenta os ultimos 2 MB (moov no fim)
    if (ms == 0) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long sz = ftell(f);
            if (sz > 0) {
                long off = sz > (long)CAP ? sz - (long)CAP : 0;
                if (fseek(f, off, SEEK_SET) == 0) {
                    n = fread(buf.data(), 1, CAP, f);
                    ms = scan((const uint8_t *)buf.data(), n);
                }
            }
        }
    }
    fclose(f);
    return ms;
}

// Quantas desmontagens (fechar player/threads) estao rodando em segundo plano. O botao O nao espera por elas.
static std::atomic<int> g_tearing{0};

// Stop+Close em outra thread com limite de tempo: se o player travar, abandona o handle e segue em frente
static std::atomic<int> g_avOpen{0};           // AvPlayers abertos agora (init feito e ainda nao fechado)
static bool g_avModLoaded = false, g_avLeaked = false;      // modulos do AvPlayer carregados / algum player ficou preso
static double g_avClosedAt = 0;               // quando o ultimo AvPlayer foi fechado (0 = nenhum nesta sessao)
static void texAfterClose();                 // definida mais abaixo (depende do alocador de texturas)
static bool avStopClose(KAvHandle h, int timeoutMs = 5000) {
    if (!h) return true;
    std::atomic<bool> *done = new std::atomic<bool>(false);
    auto clk = [] { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; };
    double t0 = clk();
    std::atomic<int> *rcs = new std::atomic<int>[2]{{0}, {0}};          // codigos de retorno (vaza se o player travar)
    std::thread t([h, done, rcs] { rcs[0] = sceAvPlayerStop(h); rcs[1] = sceAvPlayerClose(h); done->store(true); });
    for (int i = 0; i < timeoutMs / 5 && !done->load(); i++) sceKernelUsleep(5000);
    if (done->load()) {
        t.join(); delete done;
        if (g_avOpen.load() > 0) g_avOpen--;
        char b[96]; snprintf(b, sizeof b, "av-close-ok %dms stop=0x%x close=0x%x", (int)((clk() - t0) * 1000), (unsigned)rcs[0].load(), (unsigned)rcs[1].load()); trace(b);
        delete[] rcs;
        texAfterClose();                       // registra quantas texturas o AvPlayer deixou vivas e (se pedido) limpa o resto
        g_avClosedAt = clk();                  // o encerramento espera o AvPlayer "assentar" antes de matar o processo
        return true;
    }
    g_avLeaked = true;                        // player preso: nao descarrega os modulos dele
    trace("av-close-TIMEOUT");                // o AvPlayer nao fechou: o decodificador fica preso e o PS4 pode dar erro ao sair
    t.detach();                               // vaza de proposito: a thread ainda usa 'done'
    return false;
}

// Descarrega os modulos do AvPlayer quando nao ha nenhum player aberto. Com eles carregados o PS4 nao consegue suspender/fechar o app
// ao apertar PS (so dava erro CE-34878-0 DEPOIS de assistir algo; no menu sem video fechava normal). Sao recarregados no proximo video.
// Teste: crie o arquivo vazio /data/kickiptv_x_nounload para NAO descarregar os modulos do AvPlayer ao parar tudo.
static std::atomic<bool> g_expNoUnload{false};
static void unloadAvModules() {
    if (!g_avModLoaded || g_avLeaked) return;
    if (g_expNoUnload.load()) { trace("avmod-NAO-descarregado(teste)"); return; }
    std::atomic<bool> *done = new std::atomic<bool>(false);
    std::atomic<int> *rc = new std::atomic<int>[2]{{-1}, {-1}};
    std::thread t([done, rc] {
        if (sceSysmoduleUnloadModuleInternal || sceSysmoduleUnloadModule) {
            if (sceSysmoduleUnloadModuleInternal) rc[0] = sceSysmoduleUnloadModuleInternal(0x80000050);
            if (sceSysmoduleUnloadModule) rc[1] = sceSysmoduleUnloadModule(0x00A5);
        } else {                                  // o stub nao tem as funcoes: acha em libSceSysmodule em tempo de execucao
            int a = -1, b = -1;
            hwplayer::unloadSysmodules(&a, &b);
            rc[0] = a; rc[1] = b;
        }
        done->store(true);
    });
    for (int i = 0; i < 300 && !done->load(); i++) sceKernelUsleep(5000);     // ate 1,5 s
    if (done->load()) {
        t.join();
        char b[96]; snprintf(b, sizeof b, "avmod-descarregado str=0x%x av=0x%x", (unsigned)rc[0].load(), (unsigned)rc[1].load()); trace(b);
        g_avModLoaded = false;
        delete done; delete[] rc;
    } else { trace("avmod-descarregar-TIMEOUT"); t.detach(); g_avLeaked = true; }
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
struct TexMem { off_t phys; size_t size; int type = 0; };
// Tipo da memoria dos quadros de video: 0 = WB_ONION (RAM comum COM cache: a CPU le rapido) | 3 = WC_GARLIC (memoria de GPU:
// a CPU le a ~15 MB/s, foi o que travava o video). Comecamos com Onion; se o decoder nao entregar quadros, cai para Garlic.
static std::atomic<int> g_texType{0};
static std::map<void *, TexMem> g_tex;
static texpool::Pool g_texPool;                // blocos que um player largou: ficam aqui e sao reusados (ver texpool.hpp)
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
    const int ty = g_texType.load();
    {   // 1o tenta reusar um bloco que um player anterior largou (evita esgotar a memoria direta depois de varias trocas de pedaco)
        texpool::Block b;
        if (g_texPool.take(sz, ty, al, b)) {
            memset(b.addr, 0, b.size);                                  // como memoria nova (sem restos do video anterior)
            { std::lock_guard<std::mutex> l(g_texM); g_tex[b.addr] = {(off_t)b.phys, b.size, b.type}; }
            return b.addr;
        }
    }
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, ty, &phys) < 0) {
        if (g_texFail++ == 0) {
            char b[120]; snprintf(b, sizeof b, "av-textura-FALHOU (pedido %zu KB, guardadas %zu blocos / %zu MB)", sz / 1024, g_texPool.count(), g_texPool.bytes() >> 20); trace(b);
        }
        return NULL;
    }
    if (sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, al) < 0) { g_texFail++; return NULL; }
    { std::lock_guard<std::mutex> l(g_texM); g_tex[addr] = {phys, sz, ty}; }
    return addr;
}
// TESTE DE ISOLAMENTO (temporario): true = NAO devolve a memoria dos quadros de video ao sistema (vaza alguns MB por video).
// O AvPlayer fecha em ~1 ms; se o hardware de video ainda estiver escrevendo nessa memoria quando ela e devolvida, o PS4 acusa queda.
// Agora e escolhido SEM recompilar: crie o arquivo vazio /data/kickiptv_x_free no PS4 para devolver a memoria (false = vaza, como antes).
static std::atomic<bool> g_expFreeTex{false};
// Teste: crie o arquivo vazio /data/kickiptv_x_unmap para tirar a textura do espaco de enderecos (munmap) ANTES de devolver a memoria.
// Hoje o app mapeia a memoria do decoder (sceKernelMapDirectMemory) mas nunca desfaz o mapeamento: ele fica preso no processo ate o PS4 suspende-lo.
static std::atomic<bool> g_expUnmapTex{false};
extern "C" int sceKernelMunmap(void *addr, size_t len);
static void avFreeTex(void *, void *p) {
    TexMem tm;
    {
        std::lock_guard<std::mutex> l(g_texM);
        auto it = g_tex.find(p);
        if (it == g_tex.end()) return;
        tm = it->second; g_tex.erase(it);
    }
    if (g_expUnmapTex.load()) sceKernelMunmap(p, tm.size);          // 1o desfaz o mapeamento...
    if (!g_expFreeTex.load()) {                                      // padrao: NAO devolve ao sistema; guarda para o proximo player reusar
        g_texPool.put({p, (long long)tm.phys, tm.size, tm.type});
        return;
    }
    sceKernelReleaseDirectMemory(tm.phys, tm.size);                  // ...2o devolve a memoria fisica
}
// Chamado logo depois do sceAvPlayerClose. Os testes "free"/"unmap" so valem se o AvPlayer realmente chama avFreeTex ao fechar:
// se sobrarem blocos vivos aqui, ele NAO chamou, e a limpeza abaixo e que faz o trabalho (com as mesmas flags).
static void texAfterClose() {
    std::vector<std::pair<void *, TexMem>> rest;
    size_t live;
    {
        std::lock_guard<std::mutex> l(g_texM);
        live = g_tex.size();
        if (g_expFreeTex.load() || g_expUnmapTex.load()) { for (auto &kv : g_tex) rest.push_back(kv); g_tex.clear(); }
        else if (g_avOpen.load() == 0 && !g_avLeaked) {
            // Nenhum player aberto e nenhum preso: tudo que ainda consta como "vivo" ficou orfao (o AvPlayer nao chamou o free ao fechar).
            // Vai para a lista de reuso, em vez de ficar preso para sempre.
            for (auto &kv : g_tex) g_texPool.put({kv.first, (long long)kv.second.phys, kv.second.size, kv.second.type});
            g_tex.clear();
        }
    }
    {   char pb[140]; snprintf(pb, sizeof pb, "tex-reuso: guardadas=%zu (%zu MB) reusadas=%zu", g_texPool.count(), g_texPool.bytes() >> 20, g_texPool.reused()); trace(pb); }
    char b[128]; snprintf(b, sizeof b, "av-tex vivas=%zu alocadas=%d limpa=%zu", live, g_texN.load(), rest.size()); trace(b);
    for (auto &kv : rest) {
        if (g_expUnmapTex.load()) sceKernelMunmap(kv.first, kv.second.size);
        if (g_expFreeTex.load()) sceKernelReleaseDirectMemory(kv.second.phys, kv.second.size);
    }
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
// TESTE DE ISOLAMENTO (temporario): true = o video toca SEM abrir a porta de audio do PS4 (fica mudo, mas segue na velocidade certa).
// Serve para descobrir se o erro CE-34878-0 ao fechar depois de assistir vem da porta de audio. Volte para false depois do teste.
static const bool kTestNoAudioOut = false;      // teste do audio: NAO era a causa (o erro continuou). Desligado.
struct AudioPump {
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<bool> muted{false};     // pausa em software: nao le/escreve audio
    std::atomic<bool> flush{false};     // apos um salto no filme: descarta o audio que ja estava convertido
    std::atomic<int> state{0};          // 0 parado | 1 aguardando audio | 2 tocando | -1 erro ao abrir o audio
    std::atomic<int> openRc{0};
    std::atomic<unsigned> rate{0}, chans{0};
    std::atomic<unsigned> chunks{0};
    KAvHandle h = 0;
    std::mutex hm;                       // protege 'h': a troca de pedaco muda o player SEM fechar a porta de audio
    int port = -1;
    static const int GRAIN = 1024;
    std::mutex ctl;                      // start/stop podem vir de threads diferentes (a desmontagem roda em segundo plano): uma por vez
    static double nowS() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

    void start(KAvHandle handle) {
        std::lock_guard<std::mutex> cl(ctl);
        stopLocked();
        { std::lock_guard<std::mutex> l(hm); h = handle; }
        muted = false;
        run = true; state = 1; chunks = 0; rate = chans = 0; openRc = 0;
        th = std::thread([this] { loop(); });
    }
    // Troca o player que alimenta o audio. Antes cada troca de pedaco (a cada poucos segundos) fechava e reabria a porta
    // do AudioOut: isso causa um "buraco" de audio e arrasta o video junto. Agora a porta continua aberta.
    // Ao retornar, a thread de audio nao esta mais dentro de uma chamada ao player antigo (so entao ele pode ser fechado).
    void retarget(KAvHandle handle) { std::lock_guard<std::mutex> l(hm); h = handle; }
    void stop() { std::lock_guard<std::mutex> cl(ctl); stopLocked(); }
    void stopLocked() {
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
        double nextOpen = 0;
        while (run) {
            if (flush.exchange(false)) { acc.clear(); prevL = prevR = 0; }
            if (muted.load()) { sceKernelUsleep(8000); continue; }
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
            size_t frames = sz / (2 * ch);
            if (!kTestNoAudioOut && port < 0 && nowS() >= nextOpen) {       // se a porta nao abrir, tenta de novo a cada 0,3 s (antes desistia para sempre: video sem som e acelerado)
                int p = sceAudioOutOpen(0xFF /*usuario do sistema*/, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, GRAIN, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                openRc = p;
                if (p > 0) { port = p; state = 2; } else { state = -1; nextOpen = nowS() + 0.3; }
            }
            if (port <= 0) {
                // Sem porta de audio: o AvPlayer regula o relogio pelo ritmo em que o audio e consumido. Descartar sem esperar fazia o
                // video correr acelerado. Aqui o audio e consumido em tempo real (so sem som) e o video segue na velocidade certa.
                double wait = (double)frames / (double)(sr ? sr : 48000);
                double until = nowS() + (wait > 0.25 ? 0.25 : wait);
                while (run && nowS() < until) sceKernelUsleep(2000);
                continue;
            }
            const int16_t *in = (const int16_t *)pcm.data();
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
// Qualidade da live (Kick): 0 Fluido = ate 30 fps (no 60 fps da Kick isso cai em 480p) | 1 Equilibrado (padrao) = 720p com o video em 60 fps
// baixado, mas a TELA desenha 1 quadro a cada 2 (custo de CPU de 30 fps, imagem 720p); apos um congelamento volta sozinho para o Fluido |
// 2 Maxima = 720p60 desenhando tudo | 3 Ultra = 1080p60.
static std::atomic<int> g_quality{1};
static const int kQualityCount = 4;
static const char *kQualityName[kQualityCount] = {"Fluido 30fps", "Equilibrado 720p", "Maxima 720p60", "Ultra 1080p60"};

// Reconexao automatica: se a live congelar ou a rede/pipeline quebrar, o app reabre a transmissao sozinho (ver Player::wantReconnect).
static std::string g_reconnectNote;          // motivo da ultima reconexao (mostrado na tela "Conectando...")
static int g_autoRestarts = 0;               // quantas reconexoes automaticas ja houve (HUD)
static double g_stallT[8] = {};              // instantes (relogio monotonico) dos ultimos congelamentos: a proxima abertura fica mais conservadora (livebuf.hpp)
static unsigned g_stallI = 0;
static double monoNow() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static int liveRecentStalls() { return livebuf::recentStalls(g_stallT, 8, monoNow()); }

// ---------- Sons da interface (bips sintetizados em src/sfx.hpp) ----------
// Thread propria com a SUA porta de audio. Regras para nao brigar com o audio dos videos:
//  - a porta so abre quando o 1o som e pedido (nunca no boot) e fecha SEMPRE antes de um video abrir (suspend) e durante PS/segundo plano;
//  - falha ao abrir = fica mudo e tenta de novo depois (nunca derruba nada); sem som ligado nao existe thread tocando nada.
struct SfxPlayer {
    std::thread th;
    std::atomic<bool> run{false}, suspended{false}, enabled{true};
    std::atomic<int> pending{-1};
    std::atomic<int> portOpen{0};
    int port = -1;
    std::vector<int16_t> bank[sfx::Count];
    static double nowS() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
    void start() {
        if (run.load()) return;
        try {
            for (int k = 0; k < sfx::Count; k++) bank[k] = sfx::render(k);
            run = true;
            th = std::thread([this] { loop(); });
        } catch (...) { run = false; }
    }
    void play(int k) { if (run.load() && enabled.load() && !suspended.load() && k >= 0 && k < sfx::Count) pending = k; }
    void suspend() {                                       // fecha a porta e espera (ate 0,3 s) ela fechar de verdade
        suspended = true; pending = -1;
        for (int i = 0; i < 150 && portOpen.load(); i++) sceKernelUsleep(2000);
    }
    void resume() { suspended = false; }
    void stop() { run = false; if (th.joinable()) th.join(); }
    void loop() {
        double nextOpen = 0;
        while (run.load()) {
            if (suspended.load() || !enabled.load()) {
                if (port > 0) { sceAudioOutClose(port); port = -1; portOpen = 0; }
                pending = -1; sceKernelUsleep(10000); continue;
            }
            int k = pending.exchange(-1);
            if (k < 0) { sceKernelUsleep(6000); continue; }
            if (port < 0) {
                double tn = nowS();
                if (tn < nextOpen) continue;
                int p = sceAudioOutOpen(0xFF, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 256, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                if (p <= 0) { nextOpen = tn + 3.0; continue; }
                port = p; portOpen = 1;
            }
            const std::vector<int16_t> &pcm = bank[k];
            for (size_t off = 0; off + 512 <= pcm.size() && run.load() && !suspended.load(); off += 512) {
                sceAudioOutOutput(port, pcm.data() + off);
                if (pending.load() >= 0) break;            // chegou outro som: troca na hora (sem fila)
            }
        }
        if (port > 0) { sceAudioOutClose(port); port = -1; portOpen = 0; }
    }
};
static SfxPlayer g_sfx;

// ---------- Alimentador HLS -> AvPlayer ----------
// O AvPlayer do PS4 nao abre https/HLS direto (AddSource=806A0002). Entao o app baixa o HLS (playlist + segmentos .ts)
// com o cliente HTTP proprio e entrega os bytes ao player pelos callbacks de arquivo ("fileReplacement").
struct HlsFeeder {
    StreamBuf sb;
    std::thread th;
    std::atomic<bool> run{false};
    std::atomic<int> segs{0}, opens{0}, reads{0}, failed{0};
    std::atomic<int> wantSegs{3};                // segmentos/pedacos do buffer inicial (o player espera juntar isso antes de tocar)
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
    std::map<std::string, std::string> keyCache;                     // chaves AES-128 do HLS (so a thread do alimentador usa)

    void setStatus(const std::string &t) { std::lock_guard<std::mutex> l(lm); status = t; }
    std::string getStatus() { std::lock_guard<std::mutex> l(lm); return status; }
    void fail(const std::string &t) { setStatus("ERRO: " + t); failed = 1; }

    void start(const std::string &u, bool rawTs = false) {
        join(); sb.reset();
        { std::lock_guard<std::mutex> l(rm); recent.clear(); }
        raw = rawTs; keyCache.clear();
        url = u; segs = opens = reads = failed = 0; localId = 0; streamEnded = false; run = true; discardBuf = false; dlMs = dlMsMax = 0; setStatus("iniciando");
        wantSegs = rawTs ? livebuf::startSegs(2.0, livebuf::kStartSecsRaw) : 3;
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
            { std::lock_guard<std::mutex> l(rm); recent.push_back({++localId, std::move(c)}); while ((int)recent.size() > livebuf::kWindowRaw) recent.pop_front(); }
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
            setStatus("reconectando ao canal (HTTP " + std::to_string(st) + ")");
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
            const int stalls = liveRecentStalls();                                     // congelamentos nos ultimos 5 min
            const int q = g_quality.load();
            const int maxH = livebuf::capHeight(q == 3 ? 1080 : 720, stalls);
            const int fpsCap = (q == 0 || (q == 1 && stalls > 0)) ? 30 : 0;       // Equilibrado: depois de congelar, volta a variante de 30 fps
            int i = hlsPickVariant(v, maxH, fpsCap);
            if (i < 0) { fail("playlist mestre sem variantes"); return; }
            mediaUrl = hlsResolve(url, v[i].uri);
            setStatus("variante " + std::to_string(v[i].width) + "x" + std::to_string(v[i].height) + " " + std::to_string((int)(v[i].fps + 0.5)) +
                      "fps bw=" + std::to_string(v[i].bandwidth) + (stalls > 0 ? "  (ajuste automatico: " + std::to_string(stalls) + " congelamento(s) recente(s))" : ""));
            variantFps = (int)(v[i].fps + 0.5); variantH = v[i].height;
        }
        unsigned long long next = 0; bool first = true; int errs = 0;
        int segsSinceSess = 0;
        double lastSegWall = nowSecF();
        while (run) {
            // CDN Kick: conexao keep-alive costuma morrer ~alguns minutos — renova
            if (segsSinceSess >= 30 || (errs > 0 && segsSinceSess >= 3)) {
                g_net.closeSession(sess);
                segsSinceSess = 0;
                setStatus("renovando conexao...");
            }
            if (!first && nowSecF() - lastSegWall > 12.0) {
                g_net.closeSession(sess);
                first = true;
                lastSegWall = nowSecF();
                setStatus("live sem segmento: ressincronizando");
            }
            if (!g_net.getKA(sess, mediaUrl, body, st) || st < 200 || st >= 300) {
                g_net.closeSession(sess);
                if (++errs > 40) { fail("playlist de midia HTTP " + std::to_string(st)); return; }
                setStatus("playlist: tentando de novo (" + std::to_string(st) + ")");
                sleepMs(livebuf::backoffMs(errs), run); continue;
            }
            HlsMedia md = hlsParseMedia(body);
            if (md.hasMap) { fail("fMP4 (EXT-X-MAP) ainda nao suportado"); return; }
            if (md.segs.empty()) { sleepMs(800, run); continue; }
            if (first) {
                double sd = md.segs.back().dur > 0 ? md.segs.back().dur : md.target;
                const int stl = liveRecentStalls();
                int want = livebuf::startSegs(sd, livebuf::kStartSecsHls + livebuf::extraStartSecs(stl), (int)md.segs.size(),
                                              stl > 0 ? livebuf::kExtraMaxSegs : livebuf::kMaxStartSegs);
                wantSegs = want;
                size_t k = md.segs.size() > (size_t)want ? md.segs.size() - want : 0;
                next = md.segs[k].seq; first = false;
            } else if (md.segs.back().seq + 1 < next) {
                size_t k = md.segs.size() > 2 ? md.segs.size() - 2 : 0;
                next = md.segs[k].seq;
                sb.reset();
                setStatus("playlist reiniciou: ressincronizando");
            } else if (next + 6 < md.segs.front().seq) {
                size_t k = md.segs.size() > 2 ? md.segs.size() - 2 : 0;
                next = md.segs[k].seq;
                sb.reset();
                setStatus("adiantando para a borda ao vivo");
            } else if (next > md.segs.back().seq + 2) {
                size_t k = md.segs.size() > 2 ? md.segs.size() - 2 : 0;
                next = md.segs[k].seq;
            }
            bool got = false;
            int downloaded = 0;
            for (auto &sg : md.segs) {
                if (!run) break;
                if (sg.seq < next) continue;
                if (downloaded >= 4) break;
                std::string seg; int ss = 0;
                double d0 = nowSecF();
                if (!g_net.getKA(sess, hlsResolve(mediaUrl, sg.uri), seg, ss, 12u << 20, livebuf::segDeadlineSecs(sg.dur > 0 ? sg.dur : md.target)) || ss < 200 || ss >= 300 || seg.empty()) {
                    g_net.closeSession(sess);
                    if (++errs > 25) { fail("segmento HTTP " + std::to_string(ss)); return; }
                    setStatus("segmento falhou (HTTP " + std::to_string(ss) + "), renovando...");
                    sleepMs(350, run);
                    break;
                }
                errs = 0;
                segsSinceSess++;
                lastSegWall = nowSecF();
                if (!sg.keyMethod.empty()) {                  // HLS criptografado (comum em canais pagos/PPV, ex.: lutas)
                    if (sg.keyMethod != "AES-128") { fail("canal com criptografia " + sg.keyMethod + " (nao suportada)"); return; }
                    const std::string ku = hlsResolve(mediaUrl, sg.keyUri);
                    auto kit = keyCache.find(ku);
                    if (kit == keyCache.end()) {
                        std::string kb; int ks = 0;
                        if (!g_net.getKA(sess, ku, kb, ks, 4096) || ks < 200 || ks >= 300 || kb.size() != 16) {
                            g_net.closeSession(sess);
                            if (++errs > 8) { fail("chave de criptografia indisponivel (HTTP " + std::to_string(ks) + ")"); return; }
                            setStatus("buscando a chave de criptografia (HTTP " + std::to_string(ks) + ")...");
                            sleepMs(500, run);
                            break;
                        }
                        if (keyCache.size() > 8) keyCache.clear();
                        kit = keyCache.emplace(ku, kb).first;
                    }
                    unsigned char iv[16];
                    if (sg.hasIv) memcpy(iv, sg.iv, 16);
                    else { memset(iv, 0, 16); unsigned long long q = sg.seq; for (int b = 15; b >= 8; b--) { iv[b] = (unsigned char)(q & 0xFF); q >>= 8; } }
                    if (!aes128::cbcDecrypt((const uint8_t *)kit->second.data(), iv, seg)) {
                        if (++errs > 10) { fail("segmento criptografado invalido"); return; }
                        next = sg.seq + 1; continue;
                    }
                }
                if (outFile) {
                    fwrite(seg.data(), 1, seg.size(), outFile); fflush(outFile); outBytes += seg.size();
                    if (outBytes > 1200000000ULL) { fail("limite de gravacao (1,2 GB) atingido"); return; }
                } else if (!discardBuf.load()) sb.append((const uint8_t *)seg.data(), seg.size());
                { double dm = (nowSecF() - d0) * 1000.0; dlMs = dm; if (dm > dlMsMax.load()) dlMsMax = dm; }
                lastSegDur = sg.dur;
                { std::lock_guard<std::mutex> l(rm); recent.push_back({++localId, seg}); while ((int)recent.size() > livebuf::kWindowHls) recent.pop_front(); }
                next = sg.seq + 1; segs++; got = true; downloaded++;
                setStatus("ao vivo: segmento " + std::to_string(sg.seq) + " (" + std::to_string(seg.size() / 1024) + " KB)");
            }
            if (md.endList && !got) { setStatus("fim da transmissao"); streamEnded = true; return; }
            if (!got) {
                int ms = (int)(md.target > 0.5 ? md.target * 400 : 800);
                if (ms < 400) ms = 400;
                if (ms > 2000) ms = 2000;
                sleepMs(ms, run);
            } else if (downloaded >= 4) {
                sleepMs(100, run);
            }
        }
    }
};
static HlsFeeder &g_feed = *new HlsFeeder;
static int feedOpen(void *, const char *) { g_feed.opens++; return 0; }
static int feedClose(void *) { return 0; }
static int feedRead(void *, uint8_t *dst, uint64_t pos, uint32_t len) { g_feed.reads++; return g_feed.sb.read(dst, pos, len); }
static std::atomic<unsigned long long> g_feedSize{1ULL << 40};
static uint64_t feedSize(void *) { return g_feedSize.load(); }   // ao vivo: tamanho "infinito"; MP4 de teste: tamanho real
static std::atomic<int> g_liveOpen{0};                          // quantas aberturas do kicklive:// estao vivas (o mpv sonda e reabre o stream)
static void *kickLiveOpen(uint64_t *size_out) {
    if (g_kickLiveUrl.empty() || !g_net.init()) return nullptr;
    // Ja existe um alimentador rodando para esta URL (o mpv reabriu o stream): reaproveita, senao o close da 1a abertura matava a 2a.
    const bool reuse = g_liveOpen.load() > 0 && g_feed.run.load() && !g_feed.failed.load() && g_feed.url == g_kickLiveUrl && g_feed.segs.load() > 0;
    if (!reuse) {
        g_feed.start(g_kickLiveUrl, false);                     // escolhe a variante (resolucao), baixa playlist + segmentos
        trace("kicklive-feeder");
        const int need = std::max(2, g_feed.wantSegs.load());
        for (int i = 0; i < 500 && g_feed.segs.load() < need && !g_feed.failed.load() && g_feed.run.load(); i++) sceKernelUsleep(50000);   // ate 25 s
        if (g_feed.failed.load() || g_feed.segs.load() == 0) { trace("kicklive-falhou"); g_feed.abort(); return nullptr; }
    } else trace("kicklive-reabriu");
    auto *s = new KickMpvStream();
    s->live = true; s->opened = false; s->size = 0;
    { std::lock_guard<std::mutex> l(g_feed.sb.m); s->abs = g_feed.sb.base; }    // comeca no inicio do que esta guardado
    if (size_out) *size_out = 0;                                // ao vivo: tamanho desconhecido
    g_liveOpen++;
    trace("kicklive-aberto");
    return s;
}
static int64_t kickLiveRead(KickMpvStream *s, char *buf, uint64_t n) {
    if (n > (1u << 20)) n = 1u << 20;
    static std::atomic<int> nrd{0};
    for (int i = 0; i < 40; i++) {                              // ate ~40 s esperando dados
        {   // o alimentador pode ressincronizar (zera o buffer): volta para o comeco do que existe
            std::lock_guard<std::mutex> l(g_feed.sb.m);
            uint64_t end = g_feed.sb.base + g_feed.sb.buf.size();
            if (s->abs < g_feed.sb.base || s->abs > end) s->abs = g_feed.sb.base;
        }
        int r = g_feed.sb.read((uint8_t *)buf, s->abs, (uint32_t)n, 1000);
        if (r > 0) { s->abs += (uint64_t)r; if (nrd.fetch_add(1) < 3) trace("kicklive-leu"); return r; }
        if (!g_feed.run.load() || g_feed.failed.load() || g_feed.streamEnded.load()) {
            trace(!g_feed.run.load() ? "kicklive-eof-parado" : g_feed.failed.load() ? "kicklive-eof-falha" : "kicklive-eof-fim");
            return 0;
        }
    }
    trace("kicklive-eof-timeout");
    return 0;
}
static void kickLiveClose() {
    trace("kicklive-close");
    if (g_liveOpen.fetch_sub(1) <= 1) { g_liveOpen = 0; g_feed.abort(); }   // so a ultima abertura derruba o alimentador
}
static void closeFeedFile() {                 // para o alimentador, fecha e apaga /data/live.ts
    g_feed.join();
    if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; remove("/data/live.ts"); }
}

// ---------- Filme/serie (.mp4 .mkv ...) -> AvPlayer ----------
// O AvPlayer abre "http://..." com a rede dele (outro User-Agent, sem redirecionamento) e os paineis recusam:
// "AddSource = 806A0002 / Open file failed". Entao o app le o arquivo com o PROPRIO cliente HTTP (blocos de 1 MB via Range, httpfile.hpp)
// e entrega ao player pelos callbacks de arquivo.
static std::mutex g_hfM;
static std::shared_ptr<httpfile::Reader> &g_hf = *new std::shared_ptr<httpfile::Reader>;
static std::shared_ptr<httpfile::Reader> getHf() { std::lock_guard<std::mutex> l(g_hfM); return g_hf; }
static void setHf(std::shared_ptr<httpfile::Reader> r) { std::lock_guard<std::mutex> l(g_hfM); g_hf = std::move(r); }
static std::shared_ptr<httpfile::Reader> takeHf() { std::lock_guard<std::mutex> l(g_hfM); auto r = std::move(g_hf); g_hf.reset(); return r; }
static int hfOpen(void *, const char *) { trace("vod-cb-open"); return 0; }
static int hfClose(void *) { return 0; }
static int hfRead(void *, uint8_t *dst, uint64_t pos, uint32_t len) {
    // Callback do AvPlayer (outra thread). Nunca devolver lixo: so bytes validos ou 0.
    auto r = getHf();
    if (!r || !dst || len == 0) return 0;
    // algumas tentativas: o baixador pode estar um bloco atras
    for (int a = 0; a < 4; a++) {
        int got = r->read(dst, pos, len, 20000);
        if (got > 0) return got;
        if (r->state() == 2) return 0;   // falhou de verdade
        sceKernelUsleep(50000);
    }
    return 0;
}
static uint64_t hfSize(void *) {
    auto r = getHf();
    return r ? r->size() : 0;
}
static void abortHf() { auto r = getHf(); if (r) r->abort(); }                  // acorda leituras bloqueadas
static void releaseHfAsync() {                                                  // para e apaga em segundo plano (a rede pode demorar a responder)
    auto r = takeHf(); if (!r) return;
    r->abort(); g_tearing++;
    std::thread([r]() mutable { r->join(); r.reset(); g_tearing--; }).detach();
}

// ---------- Filme/serie: download progressivo (progfile.hpp) ----------
static std::mutex g_pfM;
static std::shared_ptr<progfile::Downloader> &g_pf = *new std::shared_ptr<progfile::Downloader>;
static void removeVodFiles() {                                                  // o arquivo local leva a extensao do container (mp4/mkv/avi/ts)
    static const char *e[] = {"mp4", "mkv", "avi", "ts"};
    for (const char *x : e) remove((std::string("/data/kickiptv_vod.") + x).c_str());
}
static std::shared_ptr<progfile::Downloader> getPf() { std::lock_guard<std::mutex> l(g_pfM); return g_pf; }
static void setPf(std::shared_ptr<progfile::Downloader> r) { std::lock_guard<std::mutex> l(g_pfM); g_pf = std::move(r); }
static std::shared_ptr<progfile::Downloader> takePf() { std::lock_guard<std::mutex> l(g_pfM); auto r = std::move(g_pf); g_pf.reset(); return r; }
static void abortPf() { auto r = getPf(); if (r) r->abort(); }
static void releasePfAsync() {                                                  // para em segundo plano (a rede pode demorar a responder)
    auto r = takePf(); if (!r) return;
    r->abort(); g_tearing++;
    std::thread([r]() mutable { r->join(); r.reset(); removeVodFiles(); g_tearing--; }).detach();
}

static double nowSec() { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

struct Player {
    KAvHandle h = 0;
    bool active = false;
    AudioPump audio;
    int addRc = 0;
    bool failed = false, useFeed = false;
    unsigned lastFeedSegs = 0;
    double lastFeedSegWall = 0;
    std::string kickSlug;
    std::string nextKickUrl;
    double kickUrlAt = 0;

    std::vector<std::string> log;
    int frames = 0, strideMode = 0;
    unsigned vw = 0, vh = 0;
    double t0 = 0, lastFps = 0; int fpsFrames = 0; double fps = 0;

    // Controles de reproducao: pausa em SOFTWARE (sem sceAvPlayerPause — evita CE-34878-0)
    bool paused = false;
    bool isVod = false;                    // true = arquivo (mp4/mkv/...), false = live/HLS
    bool useNative = false;                // true = sceVideodec (sem AvPlayer)
    bool useMpv = false;                   // true = libmpv (PacBrew, igual GMCA)
    bool mpvGot = false, mpvLastHud = false;  // ja chegou 1 quadro do mpv / estado do OSD no ultimo desenho
    double mpvT0 = 0, mpvLastOv = 0;
    int mpvSeekAcc = 0;                    // segundos acumulados dos saltos L1/R1 (para o aviso central)
    std::string toastText; double toastUntil = 0;
    nativeplayer::Session native;
    // ---- seek por janela (ver progfile.hpp::want) e retomada da pausa ----
    mp4s::Demuxer seekDm;                  // indice do arquivo LOCAL: tempo -> endereco do quadro-chave
    bool seekDmTried = false, seekDmOk = false;
    uint64_t pendWinA = 0, pendWinB = 0;   // janela de bytes que precisa estar no disco antes de pular
    bool forceJump = false;                // seekTo no mesmo ponto (usado ao despausar)
    int64_t pausePosMs = 0;                // onde o filme estava quando pausou
    // Protecao de buffer: o AvPlayer le o arquivo local direto; se a internet for mais lenta que o filme e ele alcancar o que ja foi
    // baixado, ele le zeros (imagem bugada). Entao o app pausa sozinho ("Carregando...") e retoma quando houver colchao.
    bool bufPaused = false; double bufPausedAt = 0, bufHoldUntil = 0, bufChk = 0;
    int bufPct = 0;                        // enchimento durante a pausa automatica (0-100)
    uint64_t bufAheadMs = 0;               // quanto do filme ja esta baixado a frente do ponto atual (barra cinza clara)
    bool bufKnown = false;
    bool quietWait = false;                // true = nao mostra o cartao de diagnostico ao conectar (qualidade/reconexao)
    std::string waitLabel;                 // texto do spinner: "Proximo episodio..." etc. Vazio = padrao
    uint64_t lastPtsMs = 0;                // posicao atual (ms) do ultimo quadro
    uint64_t maxPtsMs = 0;                 // maior pts visto (estima duracao minima)
    uint64_t durationMs = 0;               // duracao conhecida/estimada (ms)
    uint64_t vodEstDurMs = 0;              // estimativa por tamanho HTTP
    uint64_t vodContentLength = 0;         // bytes totais (Range)
    uint64_t vodBytesAtStart = 0;
    double   vodTimeAtStart = 0;
    double   playStartWall = 0;           // relogio de parede ao iniciar (progresso)
    double   pauseAccum = 0;              // segundos acumulados em pausa (parede)
    double   pauseStartedAt = 0;          // quando entrou em pausa
    uint64_t lastGoodPtsMs = 0;           // ultimo pts que avancou
    double   lastPtsWall = 0;             // quando lastPtsMs mudou
    uint64_t fileDurMs = 0;               // duracao lida do MP4 (mvhd) se disponivel
    uint64_t playedMs = 0;                // tempo de reproducao (so avanca com frames)
    double   lastPaceWall = 0;            // pacing A/V
    int64_t seekPendingMs = -1;            // seek solicitado (software ate o player aceitar)

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
    std::string waitNote;                  // linha de progresso/diagnostico na tela "Abrindo o video..." (ex.: MB baixados, por que nao desenhou)
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
    double diagT = 0, prepBusySince = 0, tearSince = 0; int greenDrops = 0, greenRun = 0;   // diagnostico/vigia da live
    std::function<bool()> cancelCb;          // main liga: true = o usuario cancelou (O) ou pediu para sair
    bool cancelled = false;
    // Filme sem som: o AvPlayer escolhe a faixa de audio pelo idioma padrao. Se nenhum audio chegar, reabre o arquivo
    // com outro idioma (ver retryAudioLang). avLang precisa continuar vivo enquanto o player usa o ponteiro.
    std::string avLang = "eng", vodPath;
    int langTry = 0;
    bool vodAudioExpected = false;

    static std::string chunkPath(const std::string &ext, int idx) { return "/data/live" + std::to_string(idx) + "." + ext; }
    static std::string hx8(int v) { char b[16]; snprintf(b, sizeof b, "%08X", (unsigned)v); return b; }

    // junta segmentos: first=true -> os 3 mais novos; senao -> so os mais novos que lastSeq. Retorna quantos.
    int gatherChunk(bool first, std::string &out, unsigned long long &lastOut) {
        std::lock_guard<std::mutex> l(g_feed.rm);
        size_t from = 0;
        if (first) { size_t want = (size_t)std::max(3, g_feed.wantSegs.load()); from = g_feed.recent.size() > want ? g_feed.recent.size() - want : 0; }
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
        id.debugLevel = dbg; id.basePriority = 700; id.numOutputVideoFrameBuffers = 3; id.autoStart = true; id.defaultLanguage = avLang.c_str();
        if (cb == 1) { id.fileObj = (void *)&g_feed; id.fOpen = feedOpen; id.fClose = feedClose; id.fRead = feedRead; id.fSize = feedSize; }
        else if (cb == 2) { id.fileObj = (void *)&g_feed; id.fOpen = hfOpen; id.fClose = hfClose; id.fRead = hfRead; id.fSize = hfSize; }
        { std::lock_guard<std::mutex> l(g_avlogM); g_avlog.clear(); }
        if (dbg) sceAvPlayerSetLogCallback(avLogCb, NULL);
        g_allocN = g_allocFail = g_texN = g_texFail = 0;
        g_feed.opens = g_feed.reads = 0;
        if (cb == 2) trace("vod-av-init");
        KAvHandle hh = sceAvPlayerInit(&id);
        if (!hh) { if (note) log.push_back("   Init falhou"); return false; }
        g_avOpen++;
        if (cb == 2) trace("vod-av-addsource");
        int rc = sceAvPlayerAddSource(hh, path.c_str());
        if (cb == 2) trace(rc < 0 ? "vod-av-addsource-erro" : "vod-av-addsource-ok");
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
            if (cb == 2) trace("vod-esperando-quadro");
            double t0 = nowSec(); bool got = false;
            while (nowSec() - t0 < waitMs / 1000.0 && !got) {
                KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
                if (sceAvPlayerGetVideoData(hh, &fi) && fi.pData) got = true;
                KAvFrameInfo au; memset(&au, 0, sizeof au);
                sceAvPlayerGetAudioData(hh, &au);
                sceKernelUsleep(8000);
                if (cancelCb && cancelCb()) { cancelled = true; break; }
            }
            if (cb == 2) trace(got ? "vod-chegou-quadro" : "vod-sem-quadro");
            if (note) log.push_back(got ? "      -> chegou video!" : "      -> aceitou, mas nenhum quadro em " + std::to_string(waitMs / 1000) + "s");
            if (!got) { avStopClose(hh); return false; }
        }
        (target ? *target : h) = hh;
        return true;
    }

    bool start(const std::string &url, const std::string &title) {
        log.clear(); frames = 0; vw = vh = 0; fps = 0; fpsFrames = 0; failed = false; addRc = 0; chunkMode = false; chunkIdx = 0; inactiveSince = 0;
        cancelled = false; restarts = switchesOk = 0; lastGapMs = 0; gapStartT = switchedAt = 0; curDur = 0; chunkT0 = 0; blitMs = 0;
        paused = false; isVod = false; lastPtsMs = 0; maxPtsMs = 0; durationMs = 0; seekPendingMs = -1; waitNote.clear(); showOpts = false; optCur = 0;
        scrubMode = false; scrubMs = -1; seekSkip = false; seekTargetMs = 0;
        trigScrub = false; trigDir = 0; trigBlock = true;       // trigBlock: um L2/R2 ainda apertado do menu nao vira seek
        pendJumpMs = -1; pendJumpNeed = 0; jumpPct = 0; jumpTargetMs = -1; jumpSettleUntil = 0; jumpErrors = 0;
        pendWinA = pendWinB = 0; seekDm = mp4s::Demuxer(); seekDmTried = seekDmOk = false; forceJump = false; pausePosMs = 0;
        bufPaused = false; bufHoldUntil = 0; bufChk = 0; bufPct = 0; bufAheadMs = 0; bufKnown = false;
        if (jumpGuardOn) { remove(kJumpGuardFile); jumpGuardOn = false; }
        vodEstDurMs = 0; vodContentLength = 0; vodBytesAtStart = 0; vodTimeAtStart = 0;
        playStartWall = nowSec(); pauseAccum = 0; pauseStartedAt = 0; lastGoodPtsMs = 0; lastPtsWall = playStartWall; fileDurMs = 0; playedMs = 0; lastPaceWall = 0;
        avLang = "eng"; langTry = 0; vodPath.clear(); vodAudioExpected = false;
        decimN = 0;
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
        kickSlug.clear(); nextKickUrl.clear(); kickUrlAt = nowSec();
        if (!title.empty() && title[0] == '@') {
            kickSlug = title.substr(1);
            while (!kickSlug.empty() && (kickSlug.back()==' '||kickSlug.back()=='\n')) kickSlug.pop_back();
        }

        // --- SOMENTE libmpv (sem AvPlayer). Se falhar, mostra o erro e nao abre o player antigo. ---
        useMpv = false;
#ifdef USE_LIBMPV
        if (mpv_player::available()) {
            kickMpvRegisterIo();   // obrigatorio: stream_cb usa o HTTP do app
            g_net.init();
            log.push_back("abrindo com libmpv (stream_cb/khttp)...");
            trace("mpv-tentando");
            const bool kickLive = !kickSlug.empty() && url.find(".m3u8") != std::string::npos;
            if (kickLive) { g_kickLiveUrl = url; log.push_back("Kick: resolucao/segmentos pelo alimentador HLS"); }
            if (mpv_player::play(kickLive ? std::string("kicklive://live.ts") : url)) {
                trace("mpv-play-ok");
                useMpv = true;
                mpvGot = false; mpvT0 = nowSec(); mpvLastOv = 0; mpvLastHud = false; mpvSeekAcc = 0; toastUntil = 0;
                paused = false; muted = false; failed = false;
                active = true;
                isVod = (url.find(".m3u8") == std::string::npos) && kickSlug.empty();
                log.push_back("player: libmpv OK");
                return true;
            }
            std::string e = mpv_player::status().error;
            if (e.empty()) e = "loading failed";
            log.push_back("ERRO libmpv: " + e);
            { std::string tr = "mpv-erro: " + e; trace(tr.c_str()); }
            try { mpv_player::stop(); } catch (...) {}
            failed = true; active = false;
            return false;
        }
        log.push_back("ERRO: libmpv indisponivel neste build");
        trace("mpv-indisponivel");
#else
        log.push_back("ERRO: PKG compilado SEM libmpv");
        trace("mpv-nao-compilado");
#endif
        failed = true; active = false;
        return false;

        // ---- codigo AvPlayer abaixo fica inalcançavel de proposito (sem fallback) ----

        // titulo "@slug" = live Kick: guarda slug para renovar o m3u8 na reconexao
        kickSlug.clear(); nextKickUrl.clear(); kickUrlAt = nowSec();
        if (!title.empty() && title[0] == '@') {
            kickSlug = title.substr(1);
            while (!kickSlug.empty() && (kickSlug.back()==' '||kickSlug.back()=='\n')) kickSlug.pop_back();
        }

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
        g_avModLoaded = true;
        bool isHls = url.find(".m3u8") != std::string::npos;
        useFeed = false; lastFeedSegs = 0; lastFeedSegWall = 0;
        // Filmes/series: URL com extensao (.mp4...) OU caminho /movie/ /series/ do painel (muitas vezes SEM extensao).
        // Se cair no pipeline de "ao vivo" (MPEG-TS), o PS4 fecha com CE-34878-0.
        if (!isHls && urlIsVod(url)) {
            isVod = true;
            // Player nativo (sceVideodec) DESLIGADO: ainda e experimental e derruba o app no PS4.
            // Padrao: o mesmo caminho que funcionava antes (baixa por HTTP Range e o AvPlayer do sistema toca).
            // Para voltar a testar o nativo, troque para true.
            const bool kNativeVod = false;
            if (!kNativeVod) {
                try { return startFile(url); }
                catch (...) { failed = true; log.push_back("ERRO: sem memoria ao abrir o filme"); trace("vod-excecao"); releaseHfAsync(); releasePfAsync(); return false; }
            }
            try {
                if (tryStartNative(url)) return true;
                failed = true;
                if (log.empty() || log.back().find("Nativo:") == std::string::npos)
                    log.push_back("Nativo: falhou ao abrir");
                return false;
            }
            catch (...) { failed = true; log.push_back("ERRO: sem memoria no player nativo"); trace("vod-excecao"); releaseHfAsync(); releasePfAsync(); return false; }
        }

        // Ao vivo: HLS (.m3u8) vai direto. Canal "cru" (.ts / sem extensao): primeiro tenta o HLS equivalente do painel Xtream
        // (mais estavel), e se o painel nao tiver, le o MPEG-TS continuo e corta em pedacos.
        useFeed = true;
        std::vector<std::pair<std::string, bool>> tries;            // {url, mpegts_cru}
        // Canal "cru" (.ts / sem extensao): o fluxo MPEG-TS comeca a tocar em ~2 s; o HLS espera segmentos de 6-10 s.
        // Entao tenta o TS direto primeiro e so cai no HLS equivalente do painel se o TS nao vier.
        if (isHls) tries.push_back({url, false});
        else {
            tries.push_back({url, true});
            std::string hls = xtreamHlsUrl(url);
            if (!hls.empty()) tries.push_back({hls, false});
        }
        for (size_t ti = 0; ti < tries.size(); ti++) {
            g_feed.join(); g_feed.opens = g_feed.reads = g_feed.segs = 0; g_feedSize = 1ULL << 40;
            if (tries.size() > 1) log.push_back(std::string("Canal ao vivo: tentando ") + (tries[ti].second ? "fluxo MPEG-TS direto" : "HLS (.m3u8)"));
            g_feed.start(tries[ti].first, tries[ti].second);
            const bool isKick = !kickSlug.empty();
            const int needSegs = isKick ? 3 : (tries[ti].second ? 1 : 2);
            const int maxWait = isKick ? 500 : 400;
            for (int i = 0; i < maxWait && (int)g_feed.segs.load() < std::max(needSegs, g_feed.wantSegs.load()) && !g_feed.failed.load(); i++) {
                if (tries[ti].second && ti + 1 < tries.size() && i >= 160 && g_feed.segs.load() == 0) break;     // TS direto sem nenhum dado em 8 s: cai no HLS do painel
                sceKernelUsleep(50000);
                if (cancelCb && cancelCb()) { cancelled = true; break; }
                if (i % 2 == 0) {
                    waitNote = "Pre-buffer " + std::to_string(g_feed.segs.load()) + "/" + std::to_string(std::max(needSegs, g_feed.wantSegs.load())) + " segmentos...";
                    presentCb();
                }
            }
            if (cancelled) { failed = true; stop(); return false; }
            log.push_back(std::string(tries[ti].second ? "MPEG-TS" : "HLS") + ": pedacos baixados=" + std::to_string(g_feed.segs.load()) + "  " + g_feed.getStatus());
            if (g_feed.segs.load() > 0) break;
            if (tries[ti].second && g_feed.getStatus().find("nao devolveu video MPEG-TS") != std::string::npos) {     // a URL devolveu uma playlist, nao TS
                bool have = false; for (auto &t : tries) if (!t.second && t.first == url) have = true;
                if (!have) tries.push_back({url, false});
            }
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
        // (HEVC/MPEG-2/outros codecs: a conversao para MP4 nao cobre, mas o AvPlayer pode tocar o .ts cru - o plano B abaixo tenta.)
        if (chunkMode) { g_feed.discardBuf = true; g_feed.sb.reset(); }     // os segmentos so seguem para 'recent' (nao enche mais o StreamBuf)
        // Plano B (so se a conversao falhou): TS cru em poucas extensoes e depois leitura propria. Timeouts curtos: antes eram ~45 s no total.
        static const char *exts[] = {"ts", "mpg", "m2ts"};
        if (!chunkMode)
        for (const char *ext : exts) {
            if (cancelled) break;
            std::string path = chunkPath(ext, 0);
            log.push_back("Tentando arquivo " + path + " ...");
            presentCb();
            if (!writeFile(path, chunk)) { log.push_back("   nao consegui gravar"); continue; }
            if (openSource(path, false, 3, ext == exts[0] ? 6000 : 3500, true)) {      // 1a tentativa com mais folga (decoder de HEVC/MPEG-2 demora a entregar o 1o quadro)
                chunkMode = true; chunkExt = ext; chunkIdx = 0; lastSeq = last;
                log.push_back("OK: tocando por arquivos (." + std::string(ext) + ")");
                break;
            }
            remove(path.c_str());
        }
        if (chunkMode) { g_feed.discardBuf = true; g_feed.sb.reset(); }
        if (!chunkMode) {
            static const char *names[] = {"live.ts"};
            for (const char *nm : names) {
                if (cancelled) break;
                log.push_back(std::string("Tentando leitura propria, nome ") + nm + " ...");
                presentCb();
                g_feedSize = 1ULL << 40;
                if (openSource(nm, 1, 3, 3500, true)) { log.push_back("OK: tocando por leitura propria"); break; }
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
    // Filme/serie em STREAMING (igual canais): o app baixa por HTTP Range sob demanda e
    // entrega bytes ao AvPlayer pelos mesmos callbacks de arquivo usados no HLS/live.
    // Nao baixa o filme inteiro para o disco — comeca a tocar assim que o buffer enche.
    // VOD estavel: 1) URL HTTP direta  2) download local completo. NUNCA cb=2 (fecha o app).
    bool tryStartNative(const std::string &url) {
        // STREAMING: nao baixa o episodio inteiro. So o indice (moov) + 1 sample por frame via Range.
        useNative = false;
        failed = false;
        cancelled = false;
        waitNote = "Nativo: indice (sem baixar tudo)...";
        presentCb();

        hwplayer::traceHook = [](const char *st) { trace(st); };    // passos do decoder em /data/kickiptv.log (se o app cair, o menu mostra onde)
        if (!nativeplayer::boot()) {
            log.push_back("Nativo: sceVideodec indisponivel");
            waitNote = "sceVideodec indisponivel";
            presentCb();
            return false;
        }

        try { g_covers.setPaused(true); } catch (...) {}

        NetClient::Session sess;
        sess.recvSec = 25;
        if (!g_net.openSession(sess, url)) {
            log.push_back("Nativo: openSession falhou");
            waitNote = "Sem conexao HTTP";
            presentCb();
            try { g_covers.setPaused(false); } catch (...) {}
            return false;
        }

        std::string tip; int st = 0; uint64_t total = 0;
        waitNote = "Nativo: tamanho do arquivo...";
        presentCb();
        bool okProbe = false;
        for (int a = 0; a < 3 && !okProbe; a++) {
            tip.clear(); st = 0; total = 0;
            okProbe = g_net.getRange(sess, url, 0, 15, tip, st, &total)
                      && (st == 206 || st == 200) && total >= 1024;
            if (!okProbe && st >= 400 && st < 500) break;
        }
        if (!okProbe) {
            g_net.closeSession(sess);
            log.push_back("Nativo: probe HTTP " + std::to_string(st));
            waitNote = "Sem Range no servidor (HTTP " + std::to_string(st) + ")";
            presentCb();
            try { g_covers.setPaused(false); } catch (...) {}
            return false;
        }
        {
            char b[64];
            snprintf(b, sizeof b, "Nativo: indice (arquivo %.0f MB)...", total / 1048576.0);
            waitNote = b;
            presentCb();
        }

        // Fetch compartilhado: mesma sessao HTTP serializada (httpMx)
        auto fetch = [this, url, sess](uint64_t from, uint64_t to, std::string &body) mutable -> bool {
            int st2 = 0;
            body.clear();
            if (!g_net.getRange(sess, url, from, to, body, st2)) return false;
            return (st2 == 206 || st2 == 200) && !body.empty();
        };

        if (!native.openUrl(url, total, fetch)) {
            g_net.closeSession(sess);
            log.push_back("Nativo: " + native.lastError());
            waitNote = native.lastError().empty() ? "Falha no indice MP4" : native.lastError();
            presentCb();
            native.close();
            try { g_covers.setPaused(false); } catch (...) {}
            return false;
        }

        // Mantem sessao viva dentro do fetch (copiado no lambda) — sess é daqui; ok enquanto player ativo
        // Guarda sess no Player? O lambda captura sess por valor (Session copy) — Session is copyable with tmpl/conn ints
        // Danger: when tryStartNative returns, sess destructor doesn't close - Session is POD-like
        // We must NOT closeSession here — fetch still uses it. Store on Player.
        useNative = true;
        isVod = true;
        h = 0;
        active = true;
        failed = false;
        paused = false;
        playStartWall = nowSec();
        t0 = lastFps = lastFrameT = nowSec();
        log.push_back("Nativo: STREAM OK samples=" + std::to_string(native.sampleCount())
                      + " (" + std::to_string(total / 1048576) + " MB no servidor, 0 baixado inteiro)");
        waitNote.clear();
        presentCb();
        return true;
    }

    bool startFile(const std::string &url) {
        std::string ext = urlExtension(url);
        trace("vod-inicio");
        log.push_back(std::string("Filme/serie") + (ext.empty() ? "" : " ." + ext));
        presentCb();
        releaseHfAsync();
        releasePfAsync();
        while (g_tearing.load() > 0) {                       // o arquivo da exibicao anterior ainda esta sendo apagado
            if (cancelCb && cancelCb()) { cancelled = true; failed = true; return false; }
            presentCb(); sceKernelUsleep(30000);
        }

        // 1) tamanho do arquivo (uma requisicao com Range). Sem tentar o AvPlayer direto na URL: o painel recusa e isso so atrasava ~10 s.
        log.push_back("   conectando ao servidor...");
        presentCb();
        trace("vod-probe");
        NetClient::Session probe; probe.recvSec = 20;
        std::string tip; int st = 0; uint64_t total = 0;
        bool okProbe = false;
        for (int a = 0; a < 3 && !okProbe; a++) {
            if (cancelCb && cancelCb()) { cancelled = true; g_net.closeSession(probe); return false; }
            okProbe = g_net.getRange(probe, url, 0, 15, tip, st, &total) && st == 206 && total >= 1024;
            if (!okProbe && st == 206 && total == 0) break;
            if (!okProbe && st >= 400 && st < 500) break;
        }
        g_net.closeSession(probe);
        if (okProbe && total > 0) vodContentLength = total;
        if (!okProbe) {
            failed = true;
            if (st == 200) log.push_back("ERRO: o servidor nao aceita Range (nao da para tocar enquanto baixa)");
            else log.push_back("ERRO: sem resposta do servidor (HTTP " + std::to_string(st) + ")");
            return false;
        }
        char tb[96]; snprintf(tb, sizeof tb, "   arquivo de %.0f MB", total / 1048576.0);
        log.back() = tb;

        // 2) baixa o comeco + indice em segundo plano; o resto continua baixando enquanto toca
        const std::string vodExt = vodContainerExt(tip, url);        // mkv gravado como .mp4 nunca abria: o nome precisa casar com o container
        const std::string pathS = "/data/kickiptv_vod." + vodExt;
        const char *path = pathS.c_str();
        vodPath = pathS;
        removeVodFiles();
        std::string u = url;
        auto sess = std::make_shared<NetClient::Session>(); sess->recvSec = 25;
        auto dl = std::make_shared<progfile::Downloader>([u, sess](uint64_t from, uint64_t to, std::string &body) -> int {
            int status = 0;
            if (!g_net.getRange(*sess, u, from, to, body, status)) return 0;
            return status;
        }, path, total);
        setPf(dl);
        trace("vod-baixa-inicio");
        dl->start();
        log.push_back("   preparando o inicio do filme...");
        presentCb();
        double tw = nowSec(), lastUi = 0;
        while (dl->state() == 0) {
            if (cancelCb && cancelCb()) { cancelled = true; break; }
            if (nowSec() - tw > 90) { dl->abort(); failed = true; log.push_back("ERRO: o servidor esta lento demais (90 s)"); break; }
            if (nowSec() - lastUi > 0.2) {
                lastUi = nowSec();
                char b[96]; snprintf(b, sizeof b, "   preparando o inicio do filme... %.1f MB  (O cancela)", dl->have() / 1048576.0);
                log.back() = b;
                snprintf(b, sizeof b, "Baixando o início do arquivo... %.1f MB", dl->have() / 1048576.0);
                waitNote = b;
                presentCb();
            }
            sceKernelUsleep(20000);
        }
        if (cancelled || failed) { releasePfAsync(); failed = true; return false; }
        if (dl->state() == 2) { log.push_back("ERRO: " + dl->error()); failed = true; releasePfAsync(); return false; }
        log.back() = std::string("   inicio pronto") + (dl->relocated() ? " (indice movido para o comeco)" : "");
        if (!dl->info().empty()) log.push_back("   " + dl->info());
        if (!dl->warn().empty()) log.push_back("   AVISO: " + dl->warn());
        else if (!dl->audioWarn().empty()) log.push_back("   AVISO: " + dl->audioWarn());
        vodAudioExpected = dl->info().find("sem audio") == std::string::npos;
        trace("vod-prebuffer");
        // 2b) pre-buffer: o AvPlayer le o arquivo local direto, sem esperar o download. Se ele ler alem do que ja chegou, enxerga zeros
        //     (imagem bugada). Arquivos grandes/de alto bitrate precisam de mais colchao antes de abrir: 0,4% do arquivo (8 a 48 MB).
        {
            uint64_t need = total / 250;
            if (need < (8u << 20)) need = 8u << 20;
            if (need > (48u << 20)) need = 48u << 20;
            if (need > total) need = total;
            double pw = nowSec(), lastUi = 0;
            while (dl->state() == 1 && dl->frontier() < need && nowSec() - pw < 30) {
                if (cancelCb && cancelCb()) { cancelled = true; break; }
                if (nowSec() - lastUi > 0.2) {
                    lastUi = nowSec();
                    char b[96]; snprintf(b, sizeof b, "Enchendo o buffer... %.0f / %.0f MB", dl->frontier() / 1048576.0, need / 1048576.0);
                    waitNote = b; presentCb();
                }
                sceKernelUsleep(20000);
            }
            if (cancelled) { releasePfAsync(); failed = true; return false; }
            if (dl->state() == 2) { log.push_back("ERRO: " + dl->error()); failed = true; releasePfAsync(); return false; }
        }
        waitNote = "Abrindo o player...";
        log.push_back("   abrindo o player...");
        presentCb();

        // 3) toca o arquivo local (o resto ja esta baixando)
        trace("vod-abre-player");
        bool opened = openSource(path, 0, 0, dl->warn().empty() ? 20000 : 8000, true);   // codec que o PS4 nao decodifica: nao espera 20 s
        if (!opened && !cancelled && dl->state() != 2 && dl->warn().empty()) {
            // Abrir falha as vezes por o arquivo ainda estar curto demais: espera mais colchao e tenta UMA vez de novo.
            log.push_back("   nao abriu de primeira, tentando de novo...");
            presentCb();
            uint64_t need2 = std::min<uint64_t>(total, std::max<uint64_t>(24u << 20, total / 120));
            double pw = nowSec();
            while (dl->state() == 1 && dl->frontier() < need2 && nowSec() - pw < 25) {
                if (cancelCb && cancelCb()) { cancelled = true; break; }
                char b[96]; snprintf(b, sizeof b, "Enchendo mais o buffer... %.0f / %.0f MB", dl->frontier() / 1048576.0, need2 / 1048576.0);
                waitNote = b; presentCb();
                sceKernelUsleep(50000);
            }
            if (!cancelled && dl->state() != 2) { sceKernelUsleep(400000); opened = openSource(path, 0, 0, 25000, true); }
        }
        if (opened) {
            trace("vod-player-aberto");
            audio.start(h); active = true; t0 = lastFps = lastFrameT = nowSec();
            isVod = true;
            // Duracao real do arquivo (mvhd) — barra e tempo total corretos
            trace("vod-duracao");
            fileDurMs = mp4DurationMsFromFile(path);
            if (fileDurMs == 0) fileDurMs = dl->durationMs();      // MKV: duracao lida do cabecalho (Segment\Info)
            // mvhd parcial/errado: se o arquivo e grande e a duracao veio < 12 min, descarta
            if (fileDurMs > 0 && fileDurMs < 12ULL * 60ULL * 1000ULL && vodContentLength > 150ULL * 1024ULL * 1024ULL)
                fileDurMs = 0;
            if (fileDurMs == 0 && vodContentLength > 1024 * 1024) {
                // estimativa ~2.0 Mbps (comum em IPTV 1080p comprimido)
                uint64_t est = (vodContentLength * 8ULL) / 2000ULL; // ms se 2 Mbps
                if (est > 5ULL * 60ULL * 1000ULL && est < 6ULL * 3600ULL * 1000ULL) {
                    fileDurMs = 0;
                    vodEstDurMs = est;
                    durationMs = est;
                    char db[64]; snprintf(db, sizeof db, "   duracao est.: %u min", (unsigned)(est / 60000ULL));
                    log.push_back(db);
                }
            }
            if (fileDurMs > 0) {
                durationMs = fileDurMs;
                vodEstDurMs = fileDurMs;
                char db[64]; snprintf(db, sizeof db, "   duracao: %u min", (unsigned)(fileDurMs / 60000ULL));
                log.push_back(db);
            }
            playStartWall = nowSec(); pauseAccum = 0; pauseStartedAt = 0; playedMs = 0; lastPaceWall = nowSec();
            log.push_back("OK: tocando enquanto baixa"); return true;
        }
        failed = true;
        if (!cancelled) log.push_back(dl->warn().empty() ? "ERRO: o player nao abriu o arquivo (mande um print desta tela)" : "ERRO: " + dl->warn());
        releasePfAsync();
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
        if (!chunkMode || !h || !active || failed || g_tearing.load() > 0) return;
        // Sem segundo AvPlayer (gapless=false): so prepara e troca no mesmo handle — bem menos CE
        gapless = false;
        h2 = 0;
        bool inactive = false;
        try { inactive = !sceAvPlayerIsActive(h); } catch (...) { inactive = true; }
        if (inactive && inactiveSince == 0) inactiveSince = t;
        if (!inactive) inactiveSince = 0;
        int fresh = 0;
        { std::lock_guard<std::mutex> l(g_feed.rm); for (auto &sg : g_feed.recent) if (sg.first > lastSeq) fresh++; }
        double el = chunkT0 > 0 ? t - chunkT0 : 0;
        if (!prepBusy.load() && !nextReady.load() && livebuf::shouldPrep(fresh, curDur, el, convMs)) startPrep();
        bool ended = inactive && inactiveSince > 0 && (t - inactiveSince) > 0.35;
        bool overdue = curDur > 0 && chunkT0 > 0 && el > curDur + 0.8;
        bool stalled = lastFrameT > 0 && (t - lastFrameT) > 3.5;
        if (nextReady.load() && (ended || overdue || stalled)) {
            switchChunk(t);
        } else if ((ended || stalled) && !prepBusy.load() && fresh >= 1) {
            startPrep();
        }
    }
    // Desmonta TUDO em segundo plano: o botao O volta ao menu na hora (antes travava ate a rede responder).
    void stop() {
        if (useMpv) {
            try { mpv_player::stop(); } catch (...) {}
            useMpv = false;
            active = false;
        }
        if (useNative) {
            try { native.close(); } catch (...) {}
            useNative = false;
        }
        KAvHandle hh = h, hh2 = h2;
        h = 0; h2 = 0; active = false; chunkMode = false;
        pendJumpMs = -1; trigScrub = false; trigDir = 0;
        if (jumpGuardOn) { remove(kJumpGuardFile); jumpGuardOn = false; }          // saiu de proposito: nao e queda
        try { audio.retarget(0); } catch (...) {}
        g_feed.abort();                                      // acorda leituras bloqueadas
        abortHf();
        abortPf();
        g_tearing++;
        std::thread([this, hh, hh2] {
            try { audio.stop(); } catch (...) {}
            if (openTh.joinable()) openTh.join();
            KAvHandle hb = bState.load() == 2 ? bHandle : 0;
            bState = 0;
            avStopClose(hh);
            if (hh2) avStopClose(hh2);
            if (hb) avStopClose(hb);
            if (prepTh.joinable()) prepTh.join();
            g_feed.join();
            { auto r = takeHf(); if (r) r->join(); }             // o player ja fechou: ninguem mais le o arquivo HTTP
            { auto r = takePf(); if (r) { r->join(); r.reset(); } }   // idem para o download progressivo
            for (int i = 0; i < 4; i++) { static const char *e[] = {"mp4","m2ts","mpg","ts","mkv"}; for (const char *x : e) remove(chunkPath(x, i).c_str()); }
            if (g_feed.outFile) { fclose(g_feed.outFile); g_feed.outFile = nullptr; }
            remove("/data/live.ts");
            removeVodFiles();   // filme/serie baixado para disco
            nextReady = false; prepBusy = false;
            trace("stop-thread-fim");
            g_tearing--;
        }).detach();
    }
    // espera as desmontagens terminarem (usado ao fechar o app)
    static void waitTeardown(double maxSec) {
        double t0 = nowSec();
        while (g_tearing.load() > 0 && nowSec() - t0 < maxSec) sceKernelUsleep(20000);
    }
    // chama a cada iteracao; retorna true se desenhou um quadro novo
    unsigned decimN = 0;                         // contador de quadros (modo Equilibrado)
    bool poll(FbRenderer &r) {
        if (paused || !active || failed || g_tearing.load() > 0) return false;
        if (useNative) {
            if (!native.isOpen()) return false;
            hwplayer::Frame fr;
            if (!native.pump(fr) || !fr.valid || !fr.nv12) return false;
            int w = fr.width, hh = fr.height;
            if (w <= 0 || hh <= 0) return false;
            vw = (unsigned)w; vh = (unsigned)hh;
            if (fr.ptsMs > 0) {
                lastPtsMs = fr.ptsMs;
                if (lastPtsMs > maxPtsMs) maxPtsMs = lastPtsMs;
                playedMs = lastPtsMs;
            }
            int pitch = fr.pitch > 0 ? fr.pitch : w;
            int yRows = fr.yRows > 0 ? fr.yRows : hh;
            for (int y = 0; y < r.H; y++) {
                uint32_t *row = r.fb + (size_t)y * r.W;
                for (int x = 0; x < r.W; x++) row[x] = 0xFF000000u;
            }
            blitNV12(fr.nv12, (uint32_t)w, (uint32_t)hh, pitch, yRows, r.fb, r.W, r.H);
            frames++; fpsFrames++;
            double tt = nowSec();
            lastDrawT = tt; lastFrameT = tt;
            return true;
        }
        if (!h) return false;
        KAvFrameInfo fi; memset(&fi, 0, sizeof fi);
        bool drew = false;

        // Seek frente: descarta ate 40 quadros por tick ate o PTS alvo
        if (seekSkip && h && !failed) {
            for (int i = 0; i < 40; i++) {
                KAvFrameInfo sfi; memset(&sfi, 0, sizeof sfi);
                if (!sceAvPlayerGetVideoData(h, &sfi) || !sfi.pData) break;
                if (sfi.timeStamp > 0) {
                    lastPtsMs = sfi.timeStamp / 1000;
                    if (lastPtsMs > maxPtsMs) maxPtsMs = lastPtsMs;
                }
                // descarta audio correspondente (evita atraso de som)
                { KAvFrameInfo au; memset(&au, 0, sizeof au); sceAvPlayerGetAudioData(h, &au); }
                if (lastPtsMs + 250 >= seekTargetMs) {
                    seekSkip = false; seekPendingMs = -1; scrubMs = -1;
                    fi = sfi; // desenha este quadro
                    goto have_frame;
                }
            }
            // ainda nao chegou: desenha HUD sem atualizar frame se possivel
            return false;
        }

        if (sceAvPlayerGetVideoData(h, &fi) && fi.pData) {
        have_frame:
            // timeStamp do AvPlayer costuma ser em microsegundos; guardamos em ms para o HUD
            if (fi.timeStamp > 0 && !staleAfterJump(fi.timeStamp)) {
                // timeStamp do AvPlayer: microsegundos → ms
                uint64_t pts = fi.timeStamp / 1000;
                // se o valor parecer nanos/100 (muito grande), ajusta
                if (pts > 48ULL * 3600ULL * 1000ULL) pts = fi.timeStamp / 10000;
                lastPtsMs = pts;
                if (lastPtsMs > maxPtsMs) maxPtsMs = lastPtsMs;
                if (lastPtsMs != lastGoodPtsMs) {
                    lastGoodPtsMs = lastPtsMs;
                    lastPtsWall = nowSec();
                }
                // progresso: PTS se valido; senao +tempo desde ultimo frame (congela se tela preta)
                {
                    double nw = nowSec();
                    if (lastPtsMs > playedMs + 200) {
                        playedMs = lastPtsMs;
                    } else if (lastPaceWall > 0) {
                        double dt = nw - lastPaceWall;
                        if (dt > 0 && dt < 0.12)
                            playedMs += (uint64_t)(dt * 1000.0);
                        else if (dt >= 0.12 && dt < 0.5)
                            playedMs += 33; // um frame
                        // dt grande = stall: nao avanca
                    } else {
                        playedMs += 33;
                    }
                    lastPaceWall = nw;
                }
                // pacing A/V: nao deixar o video correr na frente do tempo real (som some + imagem acelerada)
                if (!seekSkip && isVod && playStartWall > 0) {
                    double mediaSec = (double)playedMs / 1000.0;
                    double wallSec = nowSec() - playStartWall - pauseAccum;
                    if (paused && pauseStartedAt > 0) wallSec -= (nowSec() - pauseStartedAt);
                    if (mediaSec > wallSec + 0.05) {
                        int us = (int)((mediaSec - wallSec) * 1000000.0);
                        if (us > 5000 && us < 80000) sceKernelUsleep(us);
                    }
                }
                // barra: so estende duration se PTS for confiavel e perto do fim conhecido
                if (lastPtsMs > 1000 && durationMs > 0 && lastPtsMs + 3000 > durationMs)
                    durationMs = lastPtsMs + 5000;
                if (isVod && vodContentLength > 1024 * 1024 && vodEstDurMs == 0 && fileDurMs == 0) {
                    // estima duracao: tamanho total / taxa media observada
                    uint64_t posMs = lastPtsMs > 3000 ? lastPtsMs : wallPosMs();
                    if (posMs > 5000) {
                        // bytes "consumidos" ~ proporcional ao tempo se download acompanhou
                        double mbs = (double)vodContentLength / ((double)posMs / 1000.0); // bytes por segundo se ja tivesse acabado no posMs? wrong
                        // melhor: assumir que a taxa de reproducao = tamanho / duracao
                        // medimos quantos bytes o progressive ja tem na fronteira
                        auto pf = getPf();
                        uint64_t front = pf ? pf->frontier() : 0;
                        if (front > 512 * 1024 && posMs > 5000) {
                            double bps = (double)front / ((double)posMs / 1000.0);
                            if (bps > 10000) {
                                uint64_t est = (uint64_t)((double)vodContentLength / bps * 1000.0);
                                if (est > 30ULL * 1000ULL && est < 10ULL * 3600ULL * 1000ULL) {
                                    vodEstDurMs = est;
                                    if (durationMs < est) durationMs = est;
                                }
                            }
                        } else if (posMs > 8000) {
                            // fallback: 2.5 Mbps tipico IPTV
                            uint64_t est = (vodContentLength * 8ULL * 1000ULL) / (2500000ULL);
                            if (est > 30ULL * 1000ULL && est < 10ULL * 3600ULL * 1000ULL) {
                                vodEstDurMs = est;
                                if (durationMs < est) durationMs = est;
                            }
                        }
                    }
                }
                if (vodEstDurMs > durationMs) durationMs = vodEstDurMs;

                // seek para frente: descarta quadros ate o alvo
                if (seekSkip) {
                    if (lastPtsMs + 200 >= seekTargetMs) {
                        seekSkip = false;
                        seekPendingMs = -1;
                        scrubMs = -1;
                    } else if (seekSkipBudget > 0) {
                        seekSkipBudget--;
                        // nao desenha este quadro — continua pedindo o proximo no mesmo poll
                        // (loop abaixo)
                    }
                }
            }
            uint32_t w = *(uint32_t *)(fi.details + 0), hh = *(uint32_t *)(fi.details + 4);
            if (w > 0 && hh > 0 && w <= 4096 && hh <= 4096) {
                vw = w; vh = hh;
                if (w != (unsigned)lockW || hh != (unsigned)lockH) { lockW = (int)w; lockH = (int)hh; strideLocked = false; strideUndecided = 0; }
                const uint8_t *src = fi.pData; size_t srcN = 0;
                size_t avail = texAvail(fi.pData);
                // Equilibrado com video de 60 fps: desenha 1 quadro a cada 2 (o decoder entrega todos; so a copia/desenho para a tela e pulada)
                const bool decimSkip = !isVod && g_quality.load() == 1 && g_feed.variantFps.load() > 40 && (++decimN & 1) == 0;
                if (isVod && frames == 0) { char b[128]; snprintf(b, sizeof b, "vod-1o-quadro %ux%u avail=%zu tex=%d", w, hh, avail, g_texType.load()); trace(b); }
                double b0 = nowSec();
                if (decimSkip) {
                    srcN = 0;                                  // quadro descartado de proposito (nada a copiar nem desenhar)
                } else if (avail >= (size_t)w * hh * 3 / 2 && g_texType.load() == 0) {
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
                // NUNCA desenhar com srcN==0: isso lia memoria invalida e fechava o app (CE-34878-0)
                bool fits = srcN > 0 && blit::strideBytes(strideMode, (int)w, (int)hh) <= srcN;
                if (!fits && srcN > 0 && blit::strideBytes(0, (int)w, (int)hh) <= srcN) {      // buffer do decoder sem a linha extra de alinhamento (ex.: 1080 sem virar 1088)
                    pitch = (int)w; yRows = (int)hh; fits = true;
                }
                if (!(fits && !skipDraw) && frames == 0) {
                    char nb[160];
                    snprintf(nb, sizeof nb, "Sem imagem: quadro %ux%u, buffer %zu B, precisa %zu B, %s", w, hh, avail, blit::strideBytes(strideMode, (int)w, (int)hh),
                             skipDraw ? "detectando alinhamento" : (srcN ? "nao cabe" : "sem acesso ao quadro"));
                    waitNote = nb;
                }
                if (fits && !skipDraw && !isVod) {
                    // quadro VERDE (decoder entregou buffer zerado: Y=0 U=0 V=0): preto de verdade e Y16/U128/V128. Nao desenha; mantem o ultimo quadro bom
                    size_t uvOff = (size_t)pitch * (size_t)yRows;
                    if (uvOff + 64 <= srcN) {
                        // 3 faixas (topo, meio, base) com 24 pontos cada: quadro PARCIALMENTE decodificado (metade de baixo verde) tambem e descartado
                        size_t uvLen = srcN - uvOff; bool bad = false;
                        for (int band = 0; band < 3 && !bad; band++) {
                            int dark = 0, n = 0; size_t b0o = uvOff + uvLen * (size_t)band / 3, bl = uvLen / 3;
                            for (int i = 0; i < 24; i++) { size_t o = b0o + ((bl / 24) * (size_t)i & ~(size_t)1); if (o + 1 < srcN) { n++; if (src[o] < 24 && src[o + 1] < 24) dark++; } }
                            if (n >= 16 && dark * 10 >= n * 9) bad = true;
                        }
                        if (bad) {
                            skipDraw = true; greenDrops++;
                            if (++greenRun == 1 && greenDrops <= 20) { char gb[64]; snprintf(gb, sizeof gb, "quadro-verde-descartado n=%d", greenDrops); trace(gb); }
                        } else greenRun = 0;
                    }
                }
                if (fits && !skipDraw) {
                    b0 = nowSec();
                    // proporcao: 0=16:9 (fit), 1=esticar, 2=zoom
                    if (aspectMode == 1) {
                        blitNV12(src, w, hh, pitch, yRows, r.fb, r.W, r.H);
                    } else {
                        double ar = (double)w / (double)hh, sar = (double)r.W / (double)r.H;
                        int dw = r.W, dh = r.H, ox = 0, oy = 0;
                        if (aspectMode == 0) { // contain (letterbox/pillarbox)
                            if (ar > sar) { dh = std::max(1, (int)(r.W / ar)); oy = (r.H - dh) / 2; }
                            else { dw = std::max(1, (int)(r.H * ar)); ox = (r.W - dw) / 2; }
                        } else { // zoom (cover)
                            if (ar > sar) { dw = std::max(1, (int)(r.H * ar)); ox = (r.W - dw) / 2; }
                            else { dh = std::max(1, (int)(r.W / ar)); oy = (r.H - dh) / 2; }
                        }
                        // limpa barras pretas e desenha (nv12 usa stride = dw)
                        for (int y = 0; y < r.H; y++) {
                            uint32_t *row = r.fb + (size_t)y * r.W;
                            for (int x = 0; x < r.W; x++) row[x] = 0xFF000000u;
                        }
                        if (ox == 0 && dw == r.W) {
                            // letterbox vertical: dest comeca em oy, stride = W
                            blitNV12(src, w, hh, pitch, yRows, r.fb + (size_t)oy * r.W, r.W, dh);
                        } else if (oy == 0 && dh == r.H && ox == 0) {
                            blitNV12(src, w, hh, pitch, yRows, r.fb, dw, r.H);
                        } else {
                            // fallback esticar se o offset horizontal complicar o stride
                            blitNV12(src, w, hh, pitch, yRows, r.fb, r.W, r.H);
                        }
                    }
                    double bm = (nowSec() - b0) * 1000.0;
                    blitMs = blitMs == 0 ? bm : blitMs * 0.9 + bm * 0.1;
                    frames++; fpsFrames++;
                    drew = true;
                }
            }
        }
        double t = nowSec();
        if (drew) {
            if (lastDrawT > 0 && (t - lastDrawT) * 1000.0 > worstGap) worstGap = (t - lastDrawT) * 1000.0;
            lastDrawT = t;
            lastFrameT = t; inactiveSince = 0;
            if (chunkT0 == 0) { chunkT0 = t; if (gapStartT > 0) { lastGapMs = (t - gapStartT) * 1000.0; gapStartT = 0; if (!isVod && lastGapMs > 600) { char gb[64]; snprintf(gb, sizeof gb, "troca-lenta %dms", (int)lastGapMs); trace(gb); } } }
        }
        // Filme que abriu mas nao entrega nenhum audio apos ~4 s: tenta outra faixa (idioma) em vez de ficar mudo
        if (isVod && active && !paused && !failed && drew && vodAudioExpected && !vodPath.empty() && langTry < 3 &&
            audio.state.load() == 1 && audio.chunks.load() == 0 && t - t0 > 4.0) retryAudioLang();
        if (chunkMode && h && !failed && active && g_tearing.load() == 0) managePipeline(t);
        if (!isVod && active && useFeed && t - diagT >= 15.0) {
            diagT = t;
            char db[200]; snprintf(db, sizeof db, "live-diag fps=%.0f pior=%.0fms troca=%.0fms conv=%.0fms blit=%.1fms prep=%d prox=%d verdes=%d reinicios=%d trocas=%d", fps, worstShown, lastGapMs, convMs, blitMs, (int)prepBusy.load(), (int)nextReady.load(), greenDrops, restarts, switchesOk);
            trace(db);
        }
        if (t - lastFps >= 1.0) { fps = fpsFrames / (t - lastFps); fpsFrames = 0; lastFps = t; worstShown = worstGap; worstGap = 0; }
        return drew;
    }
    // Reabre o filme (arquivo local que continua baixando) pedindo outra faixa de audio: eng -> por -> spa -> jpn.
    // So roda nos primeiros segundos, quando nenhum audio chegou; o filme recomeca do inicio.
    void retryAudioLang() {
        static const char *langs[] = {"eng", "por", "spa", "jpn"};
        langTry++;
        avLang = langs[langTry];
        log.push_back(std::string("Sem audio: tentando a faixa '") + avLang + "'");
        trace("vod-sem-audio-retry");
        KAvHandle old = h;
        audio.retarget(0);                       // a thread de audio sai do player antigo antes de fechar
        h = 0;
        avStopClose(old);
        if (cancelCb && cancelCb()) { cancelled = true; failed = true; return; }
        if (!openSource(vodPath, 0, 0, 8000, false)) {
            avLang = "eng";
            if (!openSource(vodPath, 0, 0, 8000, false)) { failed = true; log.push_back("ERRO: nao consegui reabrir o filme"); return; }
        }
        audio.retarget(h);
        t0 = lastFps = lastFrameT = nowSec(); chunkT0 = 0;
    }
    // ---- vigia (anti-congelamento): true = a live parou de verdade e o app deve reabri-la sozinho ----
    // So age no ao vivo por pedacos. 'why' recebe o motivo (aparece na tela "Conectando..." e no log).
    bool wantReconnect(std::string &why) {
        // Live Kick/HLS: se a imagem parar ou a rede cair, pede reabertura
        if (!useFeed || !active || paused || isVod) return false;
        double t = nowSec();
        double still = lastFrameT > 0 ? t - lastFrameT : (t0 > 0 ? t - t0 : 0);
        // progresso dos segmentos HLS
        unsigned sg = (unsigned)g_feed.segs.load();
        if (sg > lastFeedSegs) { lastFeedSegs = sg; lastFeedSegWall = t; }
        if (lastFeedSegWall < 1.0) lastFeedSegWall = t0 > 0 ? t0 : t;
        if (failed) { why = "falha no player (" + hx8(addRc) + ")"; return true; }
        if (g_feed.streamEnded.load()) return false;
        if (g_feed.failed.load() && still > 1.2) { why = "rede: " + g_feed.getStatus(); return true; }
        // buffer dessincronizado (player leu posicao ja descartada)
        if (g_feed.sb.underruns.load() > 8 && frames > 30) {
            why = "buffer dessincronizado"; return true;
        }
        // sem segmento novo: so reconecta se a imagem tambem parou (evita reboot desnecessario)
        if (frames > 90 && (t - lastFeedSegWall) > 14.0 && still > 2.0) {
            why = "sem segmentos novos ha " + std::to_string((int)(t - lastFeedSegWall)) + " s";
            return true;
        }
        // preparo do proximo pedaco preso / limpeza do player antigo presa: o pipeline nao anda mais
        if (prepBusy.load()) { if (prepBusySince == 0) prepBusySince = t; if (t - prepBusySince > 15.0 && still > 2.0) { why = "preparo do pedaco travado"; return true; } } else prepBusySince = 0;
        if (g_tearing.load() > 0) { if (tearSince == 0) tearSince = t; if (t - tearSince > 10.0 && still > 2.0) { why = "limpeza do player travada"; return true; } } else tearSince = 0;
        // quadros chegam mas o relogio do video nao anda (imagem repetida/congelada)
        if (frames > 90 && lastGoodPtsMs > 0 && lastPtsWall > 0 && (t - lastPtsWall) > 6.0 && !paused) { why = "video sem avancar ha " + std::to_string((int)(t - lastPtsWall)) + " s"; return true; }
        // 4 s sem quadro = congelou de verdade
        if (still > 4.0 && frames > 0) { why = "imagem parada ha " + std::to_string((int)still) + " s"; return true; }
        if (frames == 0 && t - t0 > 10.0) { why = "sem imagem apos abrir"; return true; }
        return false;
    }
    bool feedProblem() const { return useFeed && (g_feed.failed.load() || g_feed.streamEnded.load()); }
    static bool vodDlFailed() { auto d = getPf(); return d && d->state() == 2; }
    bool hudActive() const { return nowSec() <= hudUntil || feedProblem() || paused || vodDlFailed() || showOpts || showInfo || scrubMode || seekSkip || pendJumpMs >= 0; }

    // Pausa em software: nao chama sceAvPlayerPause (NID instavel no homebrew → CE-34878-0).
    // O loop principal para de buscar quadros e o audio fica mudo enquanto paused==true.
    void togglePause() {
        if (!h || failed) return;
        const bool resuming = paused;
        if (!resuming) pausePosMs = (int64_t)realPosMs();
        const double pausedFor = resuming && pauseStartedAt > 0 ? nowSec() - pauseStartedAt : 0;
        setPausedSoft(!paused);
        audio.muted = paused || muted;
        hudUntil = nowSec() + 12;
        // Enquanto pausado o app nao pega quadros, mas o AvPlayer segue com o relogio dele: ao despausar a imagem voltava atrasada, acelerada
        // e bugada. Entao, depois de uma pausa de verdade, "pula" para o ponto exato onde parou (limpa a fila do player e do audio).
        if (resuming && isVod && pausedFor > 0.6 && jumpUsable()) {
            forceJump = true; seekTo(pausePosMs); forceJump = false;
        }
    }

    // ---- Seek / barra de progresso ----
    // Seek de verdade = sceAvPlayerJumpToTime (frente e tras). O filme e baixado EM ORDEM para o disco; se o ponto pedido ainda nao
    // foi baixado, o salto espera o download chegar la (HUD: "Baixando ate o ponto... NN%") — pular para uma area ainda vazia
    // faria o player ler zeros. Se o JumpToTime estiver desligado (queda anterior / simbolo ausente) cai no seek antigo:
    // frente = descarta quadros ate o PTS alvo; tras = so marca no HUD.
    // L2/R2: toque = -10s/+10s | SEGURAR = a barra anda sozinha (cada vez mais rapido) e o salto acontece ao SOLTAR.
    bool scrubMode = false;           // foco na barra: LEFT/RIGHT movem o alvo
    int64_t scrubMs = -1;             // posicao do cursor na barra (-1 = seguir playhead)
    int64_t scrubFromMs = 0;          // onde a barra estava quando o scrub comecou (o HUD mostra a diferenca)
    uint64_t seekTargetMs = 0;        // alvo do skip de frames (seek antigo)
    bool seekSkip = false;            // true = poll descarta quadros ate seekTargetMs (seek antigo)
    int seekSkipBudget = 0;           // quadros max por tick (evita travar o loop)
    // L2/R2 segurados
    bool trigScrub = false;           // scrub iniciado por L2/R2 (confirma sozinho ao soltar)
    bool trigBlock = false;           // ignora L2/R2 ate soltar (cancelou o scrub com DOWN / gatilho ja estava apertado ao abrir)
    int trigDir = 0;                  // -1 = L2 | +1 = R2
    double trigStart = 0, trigLast = 0;
    // salto de verdade
    int64_t pendJumpMs = -1;          // alvo aguardando o download chegar la (-1 = nenhum)
    uint64_t pendJumpNeed = 0;        // bytes contiguos no disco necessarios antes de pular
    int jumpPct = 0;                  // andamento do download ate o ponto (HUD)
    int64_t jumpTargetMs = -1;        // acabou de pular: quadros velhos que ainda estavam na fila sao ignorados no relogio
    double jumpSettleUntil = 0;
    bool jumpGuardOn = false;         // marcador de seguranca gravado (ver kJumpGuardFile)
    double jumpGuardAt = 0;
    int jumpErrors = 0;

    int64_t seekLimitMs() const {     // nao deixa mirar nos ultimos 5 s (senao o "fim do filme" pularia o episodio sem querer)
        uint64_t d = displayDurMs();
        return (int64_t)(d > 8000 ? d - 5000 : d);
    }
    void clampTime(int64_t &ms) const {
        if (ms < 0) ms = 0;
        int64_t lim = seekLimitMs();
        if (ms > lim) ms = lim;
    }
    // posicao real de reproducao (ignora a barra de scrub)
    uint64_t realPosMs() const {
        uint64_t p = playedMs;
        if (lastPtsMs > p) p = lastPtsMs;
        uint64_t d = displayDurMs();
        return p > d ? d : p;
    }
    bool jumpUsable() const { return g_jumpMode.load() == 1 && sceAvPlayerJumpToTime != nullptr && h != 0; }
    // quantos bytes (contiguos, desde o inicio) precisam estar no disco para tocar a partir de 'ms'.
    // Proporcional ao tamanho + folga (VBR: o ponto real do video varia) — o player nunca le alem do que ja chegou.
    uint64_t jumpNeedBytes(int64_t ms) const {
        uint64_t total = vodContentLength;
        if (total == 0) return 0;
        uint64_t dur = displayDurMs();
        double frac = dur > 0 ? (double)ms / (double)dur : 1.0;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        double need = frac * (double)total * 1.06 + 16.0 * 1048576.0;
        return need >= (double)total ? total : (uint64_t)need;
    }

    // Janela de bytes do arquivo LOCAL que o AvPlayer vai ler ao pular para 'ms': do quadro-chave anterior ate ~16 MB depois.
    // Usa o indice (moov) do proprio arquivo em disco (mp4stream.hpp). false = nao sei (MKV etc.): cai no metodo antigo.
    bool ensureSeekDm() {
        if (vodPath.empty() || vodContentLength == 0 || !getPf()) return false;
        if (!seekDmTried) {
            seekDmTried = true;
            const std::string lp = vodPath;
            mp4s::ReadAt rd = [lp](uint8_t *dst, uint64_t pos, uint32_t len, int) -> int {
                FILE *f = fopen(lp.c_str(), "rb");
                if (!f) return 0;
                int n = 0;
                if (fseeko(f, (off_t)pos, SEEK_SET) == 0) n = (int)fread(dst, 1, len, f);
                fclose(f);
                return n;
            };
            std::string err;
            seekDmOk = seekDm.open(rd, vodContentLength, err, 4000);
            trace(seekDmOk ? "seek-indice-ok" : "seek-indice-indisponivel");
        }
        return seekDmOk && seekDm.count() > 0;
    }
    bool seekWindow(int64_t ms, uint64_t &a, uint64_t &b) {
        if (!ensureSeekDm()) return false;
        size_t i = seekDm.syncBefore(ms);
        uint64_t off = seekDm.at(i).off;
        if (off == 0 || off >= vodContentLength) return false;
        const uint64_t pre = 2u << 20, post = 16u << 20;
        a = off > pre ? off - pre : 0;
        b = std::min<uint64_t>(vodContentLength, off + post);
        return b > a;
    }

    void seekTo(int64_t targetMs) {
        if (!isVod || !active || failed) { hudUntil = nowSec() + 5; return; }
        clampTime(targetMs);
        int64_t cur = (int64_t)realPosMs();
        hudUntil = nowSec() + 10;

        // libmpv: seek real so no fim do preview (L2/R2 solto)
        if (useMpv) {
            // sem quadro ainda: nao busca (buscava no meio do probe e fechava o app)
            if (!mpvGot) { scrubMode = false; scrubMs = -1; return; }
            double sec = (double)targetMs / 1000.0;
            if (sec < 0) sec = 0;
            // perto do inicio e ainda em 0: nao forca seek absoluto (deixa o demuxer terminar)
            if (sec < 1.0 && lastPtsMs < 500) { scrubMode = false; scrubMs = -1; return; }
            if (mpv_player::seekSec(sec)) {
                lastPtsMs = lastGoodPtsMs = (uint64_t)targetMs;
                playedMs = (uint64_t)targetMs;
                lastPtsWall = nowSec();
                playStartWall = nowSec() - sec;
                pauseAccum = 0; pauseStartedAt = 0;
                scrubMode = false; scrubMs = -1; seekSkip = false; pendJumpMs = -1;
                trace("mpv-seek");
            }
            return;
        }

        if (jumpUsable()) {
            scrubMode = false; scrubMs = -1; seekSkip = false;
            if (!forceJump && targetMs + 400 > cur && targetMs < cur + 400) { pendJumpMs = -1; return; }   // praticamente no mesmo ponto
            pendJumpMs = targetMs;
            pendJumpNeed = 0; pendWinA = pendWinB = 0;
            uint64_t wa = 0, wb = 0;
            if (seekWindow(targetMs, wa, wb)) {                                                   // sabe o endereco exato: baixa SO aquele trecho primeiro
                pendWinA = wa; pendWinB = wb;
                if (auto pf = getPf()) pf->want(wa, wb);
            } else pendJumpNeed = targetMs > cur ? jumpNeedBytes(targetMs) : 0;                   // sem indice: espera o download chegar em ordem
            jumpPct = 0;
            serviceJump();                                                                        // pula agora se o download ja chegou la
            return;
        }

        // ---- seek antigo (JumpToTime desligado) ----
        seekPendingMs = targetMs;
        scrubMs = targetMs;
        if (targetMs > cur + 400) {
            // frente: pula quadros ate o PTS (nao mexe lastPtsMs ate o decoder chegar la)
            seekTargetMs = (uint64_t)targetMs;
            seekSkip = true;
            seekSkipBudget = 200;
            if (paused) { paused = false; audio.muted = muted; }
        } else if (targetMs + 400 < cur) {
            // tras: HUD so
            scrubMs = targetMs;
            seekSkip = false;
            waitNote = "Seek para tras indisponivel (salto desligado)";
        } else {
            seekSkip = false;
        }
    }

    // chamada a cada volta do laco principal: espera o download chegar ao ponto pedido e entao pula; solta a trava de seguranca
    void serviceJump() {
        double t = nowSec();
        // a trava so solta depois de um quadro NOVO desenhado e de um tempo sem cair
        if (jumpGuardOn && t - jumpGuardAt > 2.0 && lastFrameT > jumpGuardAt) {
            remove(kJumpGuardFile); jumpGuardOn = false; trace("seek-jump-estavel");
        }
        if (pendJumpMs < 0) return;
        if (!isVod || !active || failed || !h || !jumpUsable()) { pendJumpMs = -1; return; }
        if (pendWinB > pendWinA) {                                  // seek por janela: espera so o trecho do ponto pedido
            auto pf = getPf();
            if (pf && pf->state() == 2) { pendJumpMs = -1; pendWinA = pendWinB = 0; waitNote = "Download falhou: nao deu para pular"; return; }
            if (pf && pf->state() != 3 && !pf->hasRange(pendWinA, pendWinB)) {
                jumpPct = (int)(pf->haveIn(pendWinA, pendWinB) * 100 / (pendWinB - pendWinA));
                if (hudUntil < t + 2) hudUntil = t + 2;
                return;
            }
        } else if (pendJumpNeed > 0) {
            auto pf = getPf();
            if (pf && pf->state() == 2) { pendJumpMs = -1; waitNote = "Download falhou: nao deu para pular"; return; }
            uint64_t front = (!pf || pf->state() == 3) ? vodContentLength : pf->frontier();
            if (front < pendJumpNeed) {
                jumpPct = (int)(front * 100 / pendJumpNeed);
                if (hudUntil < t + 2) hudUntil = t + 2;
                return;
            }
        }
        doJump();
    }
    void doJump() {
        int64_t ms = pendJumpMs; pendJumpMs = -1; pendWinA = pendWinB = 0;
        double t = nowSec();
        if (paused) { setPausedSoft(false); audio.muted = muted; }          // pausado: volta a tocar no novo ponto
        touchFile(kJumpGuardFile); jumpGuardOn = true; jumpGuardAt = t;
        trace("seek-jump-inicio");
        int rc = 0;
        {   // a thread de audio nao pode estar dentro do player enquanto ele pula
            std::lock_guard<std::mutex> l(audio.hm);
            rc = sceAvPlayerJumpToTime(h, (uint64_t)ms);
        }
        if (rc < 0) {
            trace("seek-jump-erro");
            remove(kJumpGuardFile); jumpGuardOn = false;
            waitNote = "Nao consegui pular (" + hx8(rc) + ")";
            if (++jumpErrors >= 3) { g_jumpMode = 0; trace("seek-jump-desligado-erros"); }
            hudUntil = t + 6;
            return;
        }
        jumpErrors = 0;
        trace("seek-jump-ok");
        audio.flush = true;
        jumpTargetMs = ms; jumpSettleUntil = t + 1.5;
        lastPtsMs = lastGoodPtsMs = (uint64_t)ms; playedMs = (uint64_t)ms; lastPtsWall = t;
        playStartWall = t - (double)ms / 1000.0; pauseAccum = 0; pauseStartedAt = 0; lastPaceWall = 0;   // relogio do pacing A/V no novo ponto
        scrubMode = false; scrubMs = -1; seekSkip = false;
        hudUntil = t + 5;
    }
    // logo apos um salto o player ainda pode entregar quadros velhos: ignora-os no relogio ate chegar perto do alvo (ou 1,5 s)
    bool staleAfterJump(uint64_t ts) {
        if (jumpTargetMs < 0) return false;
        uint64_t pts = ts / 1000;
        if (pts > 48ULL * 3600ULL * 1000ULL) pts = ts / 10000;
        int64_t diff = (int64_t)pts - jumpTargetMs;
        if (diff < 0) diff = -diff;
        if (diff <= 10000 || nowSec() > jumpSettleUntil) { jumpTargetMs = -1; return false; }
        return true;
    }

    // L2/R2: dir = -1 (L2) | 0 (nenhum) | +1 (R2). Chamar a cada volta do laco enquanto o filme toca.
    //  - ao apertar: ja anda 10 s (toque rapido = o antigo -10s/+10s)
    //  - segurando (> 0,35 s): a barra acelera: 20 -> 60 -> 150 -> 300 s por segundo
    //  - ao soltar: pula para onde a barra parou
    void triggerScrub(int dir, double t) {
        if (!isVod || !active || failed) { trigScrub = false; trigDir = 0; return; }
        if (dir == 0) {
            trigBlock = false;
            if (trigScrub) {
                trigScrub = false; trigDir = 0;
                if (scrubMode && scrubMs >= 0) scrubConfirm();
            }
            return;
        }
        if (trigBlock) return;
        if (!trigScrub || dir != trigDir) {
            if (!scrubMode || scrubMs < 0) {
                scrubMs = pendJumpMs >= 0 ? pendJumpMs : (int64_t)realPosMs();
                scrubFromMs = scrubMs;
            }
            trigScrub = true; trigDir = dir; trigStart = trigLast = t;
            scrubMode = true;
            // toque curto: 5 s (antes pulava 10 de uma vez)
            scrubMs += (int64_t)dir * 5000;
            clampTime(scrubMs);
            focusCtrl = dir < 0 ? 1 : 3;
        } else {
            double held = t - trigStart, dt = t - trigLast;
            trigLast = t;
            if (dt > 0.05) dt = 0.05;   // fatia curta = movimento suave
            // acelera continuo desde o inicio (sem o pulo a cada 0,35 s)
            double rate = held < 0.6 ? 8.0 : held < 1.5 ? 20.0 : held < 3.0 ? 50.0 : held < 5.0 ? 120.0 : 240.0;
            scrubMs += (int64_t)((double)dir * rate * dt * 1000.0);
            clampTime(scrubMs);
        }
        hudUntil = t + 4;
    }
    void cancelScrub() {
        scrubMode = false; scrubMs = -1; trigScrub = false;
        if (trigDir != 0) trigBlock = true;       // gatilho ainda apertado: so volta a valer depois de soltar
        trigDir = 0;
    }


    // Endereco (no arquivo local) onde o filme esta tocando agora: pelo indice do mp4/mkv; senao, proporcional.
    bool playByte(int64_t ms, uint64_t &off) {
        if (vodContentLength == 0) return false;
        if (ensureSeekDm()) {
            size_t i = seekDm.syncBefore(ms < 0 ? 0 : ms);
            off = seekDm.at(i).off;
            if (off > 0 && off < vodContentLength) return true;
        }
        uint64_t dur = displayDurMs();
        double frac = dur > 0 ? (double)ms / (double)dur : 0;
        if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        off = (uint64_t)(frac * (double)vodContentLength);
        return true;
    }
    void bufferGuard() {
        if (!isVod || !active || failed || !h) { bufPaused = false; bufKnown = false; return; }
        const double now = nowSec();
        if (bufPaused && !paused) { bufPaused = false; bufHoldUntil = now + 15; return; }     // o usuario apertou X: respeita
        if (now - bufChk < 0.25) return;
        bufChk = now;
        auto pf = getPf();
        if (!pf || pf->state() == 3) { if (bufPaused) { bufPaused = false; togglePause(); } bufKnown = false; return; }   // baixou tudo: sem risco
        if (pf->state() != 1) { if (bufPaused) { bufPaused = false; togglePause(); } bufKnown = false; return; }          // falhou: o aviso de erro ja aparece
        if (pendJumpMs >= 0 || scrubMode) return;
        if (paused && !bufPaused) return;                                              // pausa do usuario
        if (playedMs < 3000 && lastPtsMs < 3000) return;                               // so depois de alguns segundos tocando
        uint64_t pos = realPosMs(), off = 0;
        if (!playByte((int64_t)pos, off)) return;
        const uint64_t total = vodContentLength, dur = displayDurMs();
        double Bps = dur > 0 ? (double)total * 1000.0 / (double)dur : 250000.0;       // bytes por segundo do filme
        if (Bps < 50000.0) Bps = 50000.0; if (Bps > 4000000.0) Bps = 4000000.0;
        auto clampU = [](double v, double lo, double hi) { return (uint64_t)(v < lo ? lo : v > hi ? hi : v); };
        const uint64_t low = clampU(Bps * 8, 6.0 * 1048576, 64.0 * 1048576), high = clampU(Bps * 20, 16.0 * 1048576, 160.0 * 1048576);
        const uint64_t end = pf->contigEnd(off);
        const uint64_t ahead = end > off ? end - off : 0;
        const bool toEnd = end >= total;
        bufAheadMs = (uint64_t)((double)ahead * 1000.0 / Bps); bufKnown = true;
        if (!bufPaused) {
            if (now >= bufHoldUntil && !toEnd && ahead < low) { bufPaused = true; bufPausedAt = now; bufPct = 0; trace("buffer-pausa"); togglePause(); }
        } else {
            bufPct = high ? (int)std::min<uint64_t>(100, ahead * 100 / high) : 100;
            hudUntil = now + 2;
            if (toEnd || ahead >= high || now - bufPausedAt > 180) { bufPaused = false; bufHoldUntil = now + 8; trace("buffer-retoma"); togglePause(); }
        }
    }

    void setPausedSoft(bool p) {
        if (p == paused) return;
        if (p) {
            pauseStartedAt = nowSec();
            paused = true;
        } else {
            if (pauseStartedAt > 0)
                pauseAccum += nowSec() - pauseStartedAt;
            pauseStartedAt = 0;
            paused = false;
        }
    }
    void seekBy(int deltaMs) {
        int64_t base = scrubMode && scrubMs >= 0 ? scrubMs : pendJumpMs >= 0 ? pendJumpMs : (int64_t)realPosMs();
        seekTo(base + deltaMs);
    }

    void scrubMove(int deltaMs) {
        if (!isVod) return;
        if (scrubMs < 0) scrubMs = (int64_t)lastPtsMs;
        scrubMs += deltaMs;
        clampTime(scrubMs);
        scrubMode = true;
        hudUntil = nowSec() + 12;
    }

    void scrubConfirm() {
        if (!isVod || scrubMs < 0) return;
        seekTo(scrubMs);
        scrubMode = false;
    }

    // Tempo de parede so para estimativa de bitrate (NAO para a barra — barra usa playedMs)
    uint64_t wallPosMs() const {
        if (playStartWall <= 0) return 0;
        double now = nowSec();
        double pausedNow = 0;
        if (paused && pauseStartedAt > 0) pausedNow = now - pauseStartedAt;
        double elapsed = now - playStartWall - pauseAccum - pausedNow;
        if (elapsed < 0) elapsed = 0;
        return (uint64_t)(elapsed * 1000.0);
    }

    uint64_t displayPosMs() const {
        if (scrubMode && scrubMs >= 0) return (uint64_t)scrubMs;
        if (pendJumpMs >= 0) return (uint64_t)pendJumpMs;          // esperando o download chegar: a barra fica no ponto pedido
        if (seekSkip && seekTargetMs > 0) return seekTargetMs;
        // playedMs so avanca quando chega frame (nao corre na tela preta)
        uint64_t p = playedMs;
        if (lastPtsMs > p) p = lastPtsMs;
        uint64_t dur = 0;
        if (fileDurMs > 1000) dur = fileDurMs;
        else if (vodEstDurMs > 1000) dur = vodEstDurMs;
        else if (durationMs > 1000) dur = durationMs;
        if (dur > 0 && p > dur) p = dur;
        return p;
    }
    uint64_t displayDurMs() const {
        // Prioridade: mvhd do arquivo > estimativa HTTP > maxPts > default
        uint64_t d = 0;
        if (fileDurMs > 1000) d = fileDurMs;
        if (vodEstDurMs > d) d = vodEstDurMs;
        if (durationMs > d) d = durationMs;
        if (maxPtsMs + 1000 > d) d = maxPtsMs + 1000;
        // se ainda nao sabemos: estimativa por tamanho do arquivo (assume ~1.2 MB/s video tipico IPTV)
        if (d < 5000 && vodContentLength > 1024 * 1024) {
            // bitrate tipico ~ 2.5 Mbps = ~320 KB/s — conservador 1 MB/s
            d = (vodContentLength * 1000ULL) / (1024ULL * 1024ULL); // segundos se 1MB/s
            d *= 1000ULL;
            if (d < 60ULL * 1000ULL) d = 60ULL * 1000ULL;
            if (d > 5ULL * 3600ULL * 1000ULL) d = 5ULL * 3600ULL * 1000ULL;
        }
        if (d < 1000) d = 90ULL * 60ULL * 1000ULL; // 1h30 placeholder ate medir
        return d;
    }

    bool hasEpisodes() const { return epIndex >= 0 && (int)epUrls.size() > 1; }
    bool canPrevEp() const { return hasEpisodes() && epIndex > 0; }
    bool canNextEp() const { return hasEpisodes() && epIndex + 1 < (int)epUrls.size(); }
    bool nearEnd() const {
        if (!isVod) return false;
        uint64_t dur = displayDurMs();
        uint64_t pos = displayPosMs();
        if (dur < 20000) return false;
        // ultimos 4 segundos OU player inativo perto do fim
        if (pos + 4000 >= dur) return true;
        if (frames > 30 && (nowSec() - lastFrameT) > 2.5 && pos + 20000 >= dur) return true;
        return false;
    }

    // HUD estilo app de streaming (referencia visual)
    bool showOpts = false;
    bool showInfo = false;
    int optCur = 0;
    int aspectMode = 0;  // 0=16:9  1=Esticar  2=Zoom
    bool muted = false;
    int focusCtrl = 2;   // 0=prev 1=-10 2=pause 3=+10 4=next (destaque visual)
    static const char *aspectName(int m) {
        return m == 1 ? "Esticar" : m == 2 ? "Zoom" : "16:9";
    }
    static void fmtTime(uint64_t ms, char *buf, size_t n) {
        int s = (int)(ms / 1000); if (s < 0) s = 0;
        int h = s / 3600, m = (s / 60) % 60, sec = s % 60;
        if (h > 0) snprintf(buf, n, "%d:%02d:%02d", h, m, sec);
        else snprintf(buf, n, "%d:%02d", m, sec);
    }


    void drawMiniProgress(FbRenderer &r) {
        if (!isVod) return;
        const int W = r.W, H = r.H;
        int trackX = 80, trackW = W - 160, trackY = H - 36, trackH = 6;
        uint64_t pos = displayPosMs(), dur = displayDurMs();
        if (dur < 1) dur = 1;
        int fill = (int)((pos * (uint64_t)trackW) / dur);
        if (fill < 0) fill = 0;
        if (fill > trackW) fill = trackW;
        r.roundRect(trackX, trackY, trackW, trackH, 3, 0xAA1A1A1Au);
        r.roundRect(trackX, trackY, std::max(fill, 2), trackH, 3, 0xFF53FC18u);
        char t0[32], t1[32];
        fmtTime(pos, t0, sizeof t0);
        fmtTime(dur, t1, sizeof t1);
        r.roundRect(trackX - 4, trackY - 28, r.textWidth(t0, 18) + 12, 24, 6, 0xAA000000u);
        uiText(r, trackX, trackY - 26, t0, 0xFFFFFFFFu, 18);
        int tw = r.textWidth(t1, 18);
        r.roundRect(trackX + trackW - tw - 8, trackY - 28, tw + 12, 24, 6, 0xAA000000u);
        uiText(r, trackX + trackW - tw, trackY - 26, t1, 0xFFAAAAAAu, 18);
    }

    // ---- icones do OSD (desenhados com retangulos: a fonte nao tem esses glifos) ----
    static void osdCircle(FbRenderer &r, int cx, int cy, int d, unsigned argb) { r.roundRect(cx - d / 2, cy - d / 2, d, d, d / 2, argb); }
    static void osdLine(FbRenderer &r, int x0, int y0, int x1, int y1, int th, unsigned c) {       // traco diagonal/reto (quadradinhos ao longo da reta)
        int n = std::max(std::abs(x1 - x0), std::abs(y1 - y0)); if (n < 1) n = 1;
        for (int i = 0; i <= n; i++) r.fillRect(x0 + (x1 - x0) * i / n - th / 2, y0 + (y1 - y0) * i / n - th / 2, th, th, c);
    }
    static void osdIconClose(FbRenderer &r, int cx, int cy, unsigned c) { osdLine(r, cx - 10, cy - 10, cx + 10, cy + 10, 4, c); osdLine(r, cx - 10, cy + 10, cx + 10, cy - 10, 4, c); }
    static void osdIconPlay(FbRenderer &r, int cx, int cy, int s, unsigned c) {                   // triangulo apontando p/ direita
        for (int i = 0; i < s; i++) { int hh = (int)((float)(s - i) / s * s * 0.5f + 0.5f); r.fillRect(cx - s / 3 + i, cy - hh, 1, hh * 2, c); }
    }
    static void osdIconPause(FbRenderer &r, int cx, int cy, int s, unsigned c) {
        r.roundRect(cx - s / 2 + s / 12, cy - s / 2, s / 3, s, 3, c); r.roundRect(cx + s / 2 - s / 12 - s / 3, cy - s / 2, s / 3, s, 3, c);
    }
    static void osdIconSliders(FbRenderer &r, int cx, int cy, unsigned c) {                       // engrenagem simplificada: 3 linhas com botoes
        const int ys[3] = {-10, 0, 10}, ks[3] = {-6, 6, -2};
        for (int i = 0; i < 3; i++) { r.fillRect(cx - 14, cy + ys[i] - 1, 28, 3, c); osdCircle(r, cx + ks[i], cy + ys[i], 10, c); }
    }
    static void osdIconVolume(FbRenderer &r, int cx, int cy, bool mute, unsigned c) {
        r.fillRect(cx - 14, cy - 5, 7, 10, c);
        for (int i = 0; i < 10; i++) r.fillRect(cx - 7 + i, cy - 5 - i, 1, 10 + 2 * i, c);
        if (mute) osdLine(r, cx + 6, cy - 8, cx + 18, cy + 8, 3, 0xFFFF5555u);
        else { r.roundRect(cx + 7, cy - 6, 3, 12, 1, c); r.roundRect(cx + 13, cy - 11, 3, 22, 1, c); }
    }
    void osdPill(FbRenderer &r, int rx, int cy, const std::string &t, unsigned col) {            // rotulo em "pilula" alinhado a direita em rx; devolve nada
        int w = r.textWidth(t, 20) + 34;
        r.roundRect(rx - w, cy - 22, w, 44, 22, 0xA0303030u);
        uiText(r, rx - w + 17, cy - 12, t, col, 20);
    }
    int osdPillW(FbRenderer &r, const std::string &t) { return r.textWidth(t, 20) + 34; }

    void overlay(FbRenderer &r) {
        using namespace theme;
        if (nowSec() > hudUntil && !feedProblem() && !paused && !vodDlFailed() && !showOpts && !showInfo) return;
        const int W = r.W, H = r.H;
        const unsigned ACC = 0xFF53FC18u, WHITE = 0xFFFFFFFFu, BTN = 0xA0303030u, ACC_FOCUS = 0x7353FC18u;

        // ---- degradês (topo e base), como o OSD do GMCA: preto ~83% -> transparente ----
        for (int y = 0; y < 150; y++) r.fillRect(0, y, W, 1, ((unsigned)((150 - y) * 213 / 150) << 24));
        for (int y = 0; y < 240; y++) r.fillRect(0, H - 240 + y, W, 1, ((unsigned)(y * 213 / 240) << 24));

        // ---- TOPO: botao fechar + titulo + opcoes ----
        {
            const int cy = 58;
            osdCircle(r, 78, cy, 60, BTN);
            osdIconClose(r, 78, cy, WHITE);
            uiHeavy(r, 128, 30, uiFit(r, title, 30, W - 128 - 160), WHITE, 30);
            std::string sub = isVod ? (hasEpisodes() ? "Série" : "Filme") : "Ao vivo · Kick IPTV";
            uiText(r, 128, 70, sub, 0xFFB8B8B8u, 18);
            if (!useMpv) {
                if (showOpts) osdCircle(r, W - 78, cy, 76, ACC_FOCUS);
                osdCircle(r, W - 78, cy, 60, BTN);
                osdIconSliders(r, W - 78, cy, WHITE);
            }
        }

        // ---- CENTRO: aviso de busca (+/- segundos), no estilo da caixinha do GMCA ----
        const bool seekToast = isVod && scrubMode && trigScrub;
        if (seekToast) {
            int64_t dl = scrubMs - scrubFromMs;
            char dtx[32]; fmtTime((uint64_t)(dl < 0 ? -dl : dl), dtx, sizeof dtx);
            std::string t = std::string(dl < 0 ? "-" : "+") + dtx;
            int tw = r.textWidth(t, 34) + 70;
            r.roundRect(W / 2 - tw / 2, H / 2 - 50, tw, 100, 16, BTN);
            uiCenterHeavy(r, W / 2, H / 2 - 4, t, WHITE, 34);
            uiCenter(r, W / 2, H / 2 + 30, "solte para ir", 0xFFC8C8C8u, 17);
        }

        if (nowSec() < toastUntil && !toastText.empty()) {                 // aviso central generico (salto de tempo, mudo)
            int tw = r.textWidth(toastText, 34) + 70;
            r.roundRect(W / 2 - tw / 2, H / 2 - 40, tw, 80, 16, BTN);
            uiCenterHeavy(r, W / 2, H / 2 - 2, toastText, WHITE, 34);
        }

        // ---- BASE: botao play/pause + barra + tempo + icones ----
        const int cyBar = H - 118;
        const int btnCx = 86;
        osdCircle(r, btnCx, cyBar, 76, BTN);
        if (paused) osdIconPlay(r, btnCx + 2, cyBar, 34, WHITE); else osdIconPause(r, btnCx, cyBar, 30, WHITE);

        const std::string qn = kQualityName[g_quality.load() % kQualityCount];
        const std::string an = aspectName(aspectMode);
        const int rightEdge = W - 48;
        const int wQ = useMpv ? 0 : osdPillW(r, qn), wA = useMpv ? 0 : osdPillW(r, an);
        const int volCx = useMpv ? rightEdge - 28 : rightEdge - wQ - 14 - wA - 14 - 28;
        const int trackX = 150, trackW = std::max(200, volCx - 60 - trackX);
        const int trackH = 7, trackY = cyBar - 22;                // linha de 7px como a do GMCA
        if (isVod) {
            uint64_t pos = displayPosMs(), dur = displayDurMs();
            if (dur < 1) dur = 1;
            if (pos > dur) pos = dur;
            const double frac = (double)pos / (double)dur;
            r.roundRect(trackX, trackY, trackW, trackH, 3, 0x66FFFFFFu);                            // vazio
            {   // quanto ja foi baixado a frente
                auto pfb = getPf();
                double bf = frac;
                if (pfb && pfb->state() == 3) bf = 1.0;
                else if (bufKnown) bf = (double)(pos + bufAheadMs) / (double)dur;
                if (bf > 1.0) bf = 1.0;
                int bw = (int)(trackW * bf);
                if (bw > 2 && bf > frac) r.roundRect(trackX, trackY, bw, trackH, 3, 0x99FFFFFFu);
            }
            int filled = (int)(trackW * frac);
            if (filled < 2 && pos > 0) filled = 2;
            r.roundRect(trackX, trackY, filled, trackH, 3, ACC);
            const int kx = trackX + filled, ky = trackY + trackH / 2;
            if (scrubMode) osdCircle(r, kx, ky, 42, ACC_FOCUS);                                     // halo de foco (acento translucido)
            osdCircle(r, kx, ky, 22, WHITE);
            char t0[32], t1[32];
            fmtTime(pos, t0, sizeof t0); fmtTime(dur, t1, sizeof t1);
            std::string tt = std::string(t0) + " / " + t1;
            uiText(r, trackX + trackW / 2 - r.textWidth(tt, 22) / 2, trackY + 26, tt, WHITE, 22);
            {
                std::string st; unsigned col = WHITE;
                if (scrubMode && !trigScrub) st = "SEEK  L2/R2  L1/R1 1min  X ok";
                else if (bufPaused) { st = "Carregando... " + std::to_string(bufPct) + "%  (internet mais lenta que o filme)"; col = 0xFFFFCC33u; }
                else if (pendJumpMs >= 0) { st = "Baixando até o ponto... " + std::to_string(jumpPct) + "%"; col = 0xFFFFCC33u; }
                else if (seekSkip || (jumpTargetMs >= 0 && nowSec() < jumpSettleUntil)) { st = "Indo para o ponto..."; col = 0xFFFFCC33u; }
                if (!st.empty()) {
                    int tw = r.textWidth(st, 18), cx0 = trackX + trackW / 2;
                    r.roundRect(cx0 - tw / 2 - 16, trackY - 58, tw + 32, 36, 18, BTN);
                    uiText(r, cx0 - tw / 2, trackY - 51, st, col, 18);
                }
            }
        } else {
            r.roundRect(trackX, trackY, trackW, trackH, 3, ACC);
            r.roundRect(trackX + trackW / 2 - 56, trackY + 20, 112, 34, 17, BTN);
            osdCircle(r, trackX + trackW / 2 - 34, trackY + 37, 12, 0xFFFF3333u);
            uiText(r, trackX + trackW / 2 - 20, trackY + 27, "AO VIVO", WHITE, 18, true);
        }

        // icones da direita: som, proporcao, qualidade
        osdIconVolume(r, volCx, cyBar, muted, muted ? 0xFFFF7777u : WHITE);
        if (!useMpv) { osdPill(r, rightEdge - wQ - 14, cyBar, an, 0xFFE0E0E0u); osdPill(r, rightEdge, cyBar, qn, ACC); }

        // dica dos botoes (pequena, embaixo)
        uiText(r, 48, H - 40,
               useMpv ? std::string(isVod ? "X = pausar     L1/R1 = -10/+10 s     " : "X = pausar     ") + "Quadrado = mudo     Triângulo = mostrar/ocultar     O = sair" :
               isVod ? "Quadrado = mudo     Triângulo = opções     O = sair"
                     : (hasEpisodes() ? "L1/R1 = canal anterior/próximo     Quadrado = mudo     Triângulo = opções     O = sair" : "Quadrado = mudo     Triângulo = opções     O = sair"),
               0xFF9A9A9Au, 16);

        // ---- MENU OPCOES (direita) ----
        if (showOpts) {
            const int ox = W - 440, oy = 90, ow = 400, oh = 400;
            r.roundRect(ox, oy, ow, oh, 16, 0xF00C0C0C);
            r.roundRect(ox, oy, 4, oh, 2, GREEN);
            uiHeavy(r, ox + 24, oy + 22, "Opcoes", TEXT, 26);

            const char *items[] = {
                "Qualidade do video",
                "Proporcao da tela",
                "Audio",
                "Informacoes",
                "Voltar"
            };
            const int nOpt = 5;
            for (int i = 0; i < nOpt; i++) {
                int iy = oy + 72 + i * 56;
                bool sel = (i == optCur);
                if (sel) {
                    r.roundRect(ox + 12, iy - 6, ow - 24, 48, 12, 0xFF142014);
                    r.roundRect(ox + 12, iy - 6, 3, 48, 2, GREEN);
                }
                uiText(r, ox + 28, iy + 10, items[i], sel ? TEXT : SOFT, 20, sel);
                const char *val = "";
                char tmp[32];
                if (i == 0) val = kQualityName[g_quality.load() % kQualityCount];
                else if (i == 1) val = aspectName(aspectMode);
                else if (i == 2) val = muted ? "Mudo" : "Ativo";
                else if (i == 3) val = showInfo ? "Abrir" : "";
                if (val && val[0]) {
                    int vw2 = r.textWidth(val, 18);
                    uiText(r, ox + ow - 28 - vw2, iy + 12, val, MUTED, 18);
                }
            }
            uiText(r, ox + 24, oy + oh - 36, "X alterar   O fechar", DIM, 16);
        }

        // ---- INFO ----
        if (showInfo && !showOpts) {
            const int ox = W / 2 - 280, oy = H / 2 - 120, ow = 560, oh = 240;
            r.roundRect(ox, oy, ow, oh, 16, 0xF0101010);
            r.roundRect(ox, oy, ow, oh, 16, 0x00000000);
            r.roundRect(ox, oy, 4, oh, 2, GREEN);
            uiHeavy(r, ox + 24, oy + 20, "Informacoes", TEXT, 26);
            char line[192];
            snprintf(line, sizeof line, "Titulo: %s", title.c_str());
            uiText(r, ox + 24, oy + 70, uiFit(r, line, 20, ow - 48), SOFT, 20);
            snprintf(line, sizeof line, "Resolucao: %ux%u", vw, vh);
            uiText(r, ox + 24, oy + 100, line, SOFT, 20);
            snprintf(line, sizeof line, "Tipo: %s  |  Qualidade: %s", isVod ? "Filme/Serie" : "Ao vivo", kQualityName[g_quality.load() % kQualityCount]);
            uiText(r, ox + 24, oy + 130, line, SOFT, 20);
            snprintf(line, sizeof line, "Proporcao: %s  |  Audio: %s", aspectName(aspectMode), muted ? "Mudo" : "Ativo");
            uiText(r, ox + 24, oy + 160, line, SOFT, 20);
            uiText(r, ox + 24, oy + 200, "O fechar", DIM, 16);
        }

        if (feedProblem()) {
            std::string m = uiFit(r, g_feed.getStatus(), 20, W - 160);
            r.roundRect(32, 140, r.textWidth(m, 20) + 48, 44, 22, 0xD0801520);
            uiText(r, 56, 152, m, TEXT, 20);
        }
        if (auto pfd = getPf(); pfd && pfd->state() == 2) {
            std::string m = uiFit(r, "Download interrompido: " + pfd->error(), 20, W - 160);
            r.roundRect(32, 140, r.textWidth(m, 20) + 48, 44, 22, 0xD0801520);
            uiText(r, 56, 152, m, TEXT, 20);
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
            uiCenter(r, cx, cy + 180, !waitLabel.empty() ? waitLabel.c_str() : (quietWait ? "Trocando qualidade..." : isVod ? "Abrindo o vídeo..." : "Abrindo a transmissão..."), MUTED, 24);
            if (!waitNote.empty()) uiCenter(r, cx, cy + 222, uiFit(r, waitNote, 20, r.W - 200), DIM, 20);
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
        if (h < 0 || !out || n <= 0) return 0;
        OrbisPadData buf[16];
        if (n > 16) n = 16;
        int k = scePadRead(h, buf, n);
        if (k < 0) k = 0;
        for (int i = 0; i < k; i++) {
            out[i].buttons = buf[i].buttons;
            out[i].lx = buf[i].leftStick.x;
            out[i].ly = buf[i].leftStick.y;
        }
        return k;
    }
    static bool state(int h, PadSample &s) {
        if (h < 0) return false;
        OrbisPadData d;
        // zera para nao ler lixo se a API preencher parcial
        memset(&d, 0, sizeof d);
        int rc = scePadReadState(h, &d);
        if (rc != 0) return false;
        s.buttons = d.buttons;
        s.lx = d.leftStick.x;
        s.ly = d.leftStick.y;
        // stick neutro se valores absurdos
        if (s.lx == 0 && s.ly == 0 && s.buttons == 0) {
            // pode ser controle desconectado: trata stick como centro
            s.lx = 128; s.ly = 128;
        }
        return true;
    }
    static double now() { return nowSec(); }
};

// Status do sistema: botao PS / troca de app (background) — precisa parar o player antes do suspend
struct OrbisSystemServiceStatus {
    int32_t eventNum;
    uint8_t isSystemUiOverlaid;
    uint8_t isInBackground;
    uint8_t isCpuMode7u;
    uint8_t pad[5];
    int32_t reserved[8];
};
extern "C" int sceSystemServiceGetStatus(OrbisSystemServiceStatus *status);
// Vigia do sistema (botao PS / "Fechar aplicativo" / troca de app).
// O que ja deu problema: (1) com a deteccao desligada o PS4 nao conseguia fechar o app (video, audio e rede ainda em uso);
// (2) ligada de forma simples, o sistema acendia o aviso por engano ao abrir um video e o app parava/fechava sozinho.
// Solucao: so vale uma SUBIDA do aviso depois de o sistema ter ficado calmo (aviso apagado) por 1,5 s seguidos.
// Aviso que ja nasce aceso, ou que pisca logo ao abrir o video, nunca dispara; apertar PS de verdade (apagado -> aceso) dispara.
// reset() e chamado ao abrir video / voltar ao menu: so volta a valer depois de 1,5 s calmo.
static const bool kPsStopLogic = true;
// false = ao ir para o inicio/fechar pelo menu do PS o app SO para video/rede/capas e espera o proprio sistema encerrar o processo
// (como um app normal). true = o app tambem se encerra sozinho, PELO SISTEMA (ps4Exit). Os testes antigos davam CE-34878-0 porque saiam por _Exit (syscall cru recusado).
// Segundo plano (PS -> Fechar aplicativo, ou trocar de app): o app para TUDO e, estavel 1,5 s, se encerra sozinho PELO SISTEMA depois de juntar todas as
// threads (ps4Exit). E o unico jeito que fecha sem CE-34878-0: deixar o PS4 matar o processo com threads vivas e o que dava o erro (log: bg=1 e nada mais).
// Padrao: o app fica SUSPENSO em segundo plano (o PS4 pode dar erro CE-34878-0 ao fechar nesse modo). Para sair sozinho ao ir para o inicio, crie /data/kickiptv_saida_bg.
static double g_bgLastIter = 0;                          // ultima volta que o laco de segundo plano rodou (0 = fora do segundo plano)
static bool kSelfExitOnBg = false;
static const double kBgExitDelay = 0.0;                  // segundos estavel em segundo plano antes de sair (descarta aviso falso)
struct SysWatch {
    double calmSince = -1, lastOvlTrip = -1e9;     // lastOvlTrip: ultima vez que o menu do PS apareceu de verdade
    bool armed = false, tripped = false, ovl = false, bg = false;
    bool everArmed = false;      // ja ficou calmo 1,5 s alguma vez: dai em diante todo aviso do sistema vale na hora (antes exigia calmaria de novo e ignorava PS -> Fechar rapido)
    void reset() { calmSince = -1; armed = false; tripped = false; }
    // le o sistema; true = pedido VALIDO de parar (menu do PS aberto ou app indo para o inicio)
    bool poll() {
        if (!kPsStopLogic) return false;
        // buffer folgado: se a estrutura real do sistema for maior que a nossa, nao estraga a pilha (e fecharia o app)
        alignas(8) uint8_t raw[256]; memset(raw, 0, sizeof raw);
        if (sceSystemServiceGetStatus((OrbisSystemServiceStatus *)raw) != 0) return tripped;
        const OrbisSystemServiceStatus &st = *(const OrbisSystemServiceStatus *)raw;
        double t = nowSec();
        ovl = st.isSystemUiOverlaid != 0;
        bg = st.isInBackground != 0;
        if (!ovl && !bg) {
            if (calmSince < 0) calmSince = t;
            if (t - calmSince >= 1.5) { armed = true; everArmed = true; }
            tripped = false;
        } else {
            calmSince = -1;
            if (armed || everArmed) tripped = true;
            if (tripped && ovl) lastOvlTrip = t;
            armed = false;
        }
        return tripped;
    }
};
static SysWatch g_sys;
static bool sysWantsStop() { return g_sys.poll(); }

using PadIn = PadInT<OrbisPadSrc>;

int main() {
    sceUserServiceInitialize(NULL);
    kSelfExitOnBg = fileExists("/data/kickiptv_saida_bg");   // padrao: sai sozinho PELO SISTEMA ao ir para o inicio do PS4 (funciona sem erro)
    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    int padInitRc = scePadInit();
    sceAudioOutInit();
    {
        char b[40]; snprintf(b, sizeof b, "pad-init=%d user=%d", padInitRc, userId);
        // trace ainda nao existe — usa boot depois; guarda em variavel
    }
    // Abre o DualShock: tenta user atual e tipos 0/1; repete algumas vezes
    int pad = -1;
    for (int attempt = 0; attempt < 8 && pad < 0; attempt++) {
        for (int type = 0; type <= 1 && pad < 0; type++) {
            pad = scePadOpen(userId, type, 0, NULL);
        }
        if (pad < 0) {
            // lista de usuarios logados
            // ORBIS_USER_SERVICE_MAX_LOGIN_USERS is typically 4
            // tenta userIds 1..4 comuns
            static const int kTryUsers[] = {1, 2, 3, 4, 0, -1};
            for (int ui = 0; ui < 6 && pad < 0; ui++) {
                int uid = kTryUsers[ui];
                if (uid == userId) continue;
                pad = scePadOpen(uid, 0, 0, NULL);
            }
        }
        if (pad < 0) sceKernelUsleep(100000); // 100 ms
    }
    PadIn pin; pin.h = pad;
    bool wantQuit = false;

    const int W = 1920, H = 1080;
    int vh = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, 0, 0, NULL);
    if (vh < 0) ps4Exit("falha-videoout-open");
    const size_t bufSize = 0x800000, total = bufSize * 3;      // 3 telas em anel: nunca desenha na tela que esta aparecendo/aguardando o flip
    off_t phys = 0;
    void *mem = NULL;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, 0x200000, 3, &phys) < 0) ps4Exit("falha-memoria-video");
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, phys, 0x200000) < 0) ps4Exit("falha-mapear-video");
    void *bufs[3] = {mem, (char *)mem + bufSize, (char *)mem + bufSize * 2};
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, W, H, W);
    if (sceVideoOutRegisterBuffers(vh, 0, bufs, 3, &attr) < 0) ps4Exit("falha-registrar-video");
    BootScreen boot;
    boot.init(bufs[0], W, H);                                       // so escrita: igual ao codigo original
    sceVideoOutSubmitFlip(vh, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
    boot.line("[1] video aberto");

    traceInit();
    // aceita o nome com ou sem ".txt" (alguns gerenciadores de arquivo do celular colocam .txt sem mostrar)
    auto expFlag = [](const char *name) {
        char p[128];
        static const char *ext[] = {"", ".txt", ".TXT", ".txt.txt"};
        for (const char *e : ext) { snprintf(p, sizeof p, "/data/%s%s", name, e); if (fileExists(p)) return true; }
        return false;
    };
    g_expFreeTex = expFlag("kickiptv_x_free");
    g_expNoUnload = expFlag("kickiptv_x_nounload");
    g_expUnmapTex = expFlag("kickiptv_x_unmap");
    // Teste: crie o arquivo vazio /data/kickiptv_x_garlic para o decoder escrever os quadros em memoria Garlic (GPU, tipo 3) em vez de Onion (tipo 0).
    // O erro do PS4 (0xa0d0c00a = SIGBUS) e acesso a memoria mapeada sem conteudo; a Onion mapeada com acesso de GPU (0x33) e a suspeita.
    // A leitura da Garlic e rapida (copyFromWC), so um pouco mais lenta no total.
    if (expFlag("kickiptv_x_garlic")) g_texType = 3;
    { char b[128]; snprintf(b, sizeof b, "exp livra-textura=%d nao-descarrega=%d desmapeia=%d garlic=%d", (int)g_expFreeTex.load(), (int)g_expNoUnload.load(), (int)g_expUnmapTex.load(), g_texType.load() == 3 ? 1 : 0); trace(b); }
    #ifdef __OPTIMIZE__
    trace("build " __DATE__ " " __TIME__ " estavel-v4 otimizado=SIM");
#else
    trace("build " __DATE__ " " __TIME__ " estavel-v4 otimizado=NAO");
#endif
#ifdef USE_LIBMPV
    trace("libmpv=COMPILADO no build");
#else
    trace("libmpv=NAO compilado (build sem USE_LIBMPV)");
#endif
    // Sonda dos decodificadores do sistema: crie o arquivo vazio /data/kickiptv_x_decprobe; ao abrir, o app grava /data/kickiptv_probe.txt (apague o flag depois).
    if (expFlag("kickiptv_x_decprobe")) { trace("decprobe-inicio"); hwplayer::traceHook = [](const char *st) { trace(st); }; hwplayer::runProbe("/data/kickiptv_probe.txt"); trace("decprobe-fim"); }
    installCrashLog();
    // Teste do registrador: crie o arquivo vazio /data/kickiptv_x_crashtest. O app cai de proposito ao abrir (apague o arquivo depois!).
    if (expFlag("kickiptv_x_crashtest")) { trace("crashtest-disparando"); *(volatile int *)16 = 1; }
    trace("video-ok");
    PlaylistBuilder::maxBytes() = 1024u * 1024 * 1024;     // lista inteira (antes cortava em 90 MB); o heap do app e sem limite
    Ps4Platform pf;
    trace("pf-ok");
    App app(pf);
    app.heroInfoEnabled = true;
    app.tmdbKey = tmdbKey();
    app.prefetch = true;                       // painel Xtream: baixa o catalogo todo em segundo plano ao conectar
    pf.app = &app;
    UI ui(app);
    trace("ui-ok");
    g_covers.setFetchW([](int worker, const std::string &url0, std::string &body) -> bool {
        if (!kCoversOn) return false;
        // Cada thread de capas tem a SUA conexao (reaproveitada entre capas do mesmo servidor: sem novo handshake TLS a cada imagem).
        // Aberturas/fechamentos de conexao sao serializados dentro do NetClient (sessMx); os downloads correm em paralelo.
        static NetClient::Session sess[ImgCache::kWorkers + ImgCache::kBgThreads];
        if (worker < 0 || worker >= ImgCache::kWorkers + ImgCache::kBgThreads) worker = ImgCache::kWorkers;
        NetClient::Session &ss = sess[worker];
        NetGate::Permit permit(g_coverGate, worker >= ImgCache::kWorkers, &g_quitting);
        if (!permit.ok) return false;                    // app fechando
        ss.recvSec = 8;
        std::string url = coverFetchUrl(url0);           // TMDB: usa coverFetchUrl (variante/tamanho correto; miniatura se nao houver variante)
        int st = 0;
        try {
            bool ok = g_net.getKA(ss, url, body, st, 768 * 1024) && st >= 200 && st < 300 && body.size() > 32;
            if (url.find("kick.com") != std::string::npos) {        // diagnostico das miniaturas da Kick (no maximo 12 linhas no log)
                static std::atomic<int> nk{0};
                if (nk++ < 12) { char b[96]; snprintf(b, sizeof b, "kick-thumb-%s-%d-%zu", ok ? "ok" : "falhou", st, body.size()); trace(b); }
            }
            return ok;
        } catch (...) { return false; }
    });
    ui.coverBackground = [](std::vector<std::string>) {};     // (nada automatico: o lote so roda quando voce aperta TRIANGULO)
    ui.coverBulk = [](std::vector<std::string> urls) {        // TRIANGULO: todas as capas, ate 15 mil NOVAS por vez (~1 GB); apertar de novo continua de onde parou
        if (!kCoversOn) return;
        try { g_covers.setBackground(std::move(urls), 15000); } catch (...) {}
    };
    ui.coverBulkStop = []() { g_covers.cancelBackground(); };
    ui.coverBulkActive = []() -> bool { return g_covers.backgroundActive(); };
    ui.coverBatch = [](const std::vector<std::string> &front, const std::vector<std::string> &back) {
        if (!kCoversOn) return;
        try { g_covers.setWanted(front, back); } catch (...) {}
    };
    ui.coverGet = [](const std::string &url, const uint32_t *&px, int &w, int &h) -> bool {
        static std::map<std::string, std::shared_ptr<const Cover>> hold;     // mantem o poster vivo ate o desenho terminar (sem copiar pixels)
        try {
            auto c = g_covers.getPtr(url);
            if (!c || !c->ready || c->bgra.empty()) return false;
            if (hold.size() > 90) hold.clear();
            hold[url] = c;
            px = c->bgra.data(); w = c->w; h = c->h;
            return true;
        } catch (...) { return false; }
    };

    ui.defaultUrl = defaultListUrl();         // a lista do app e fixa (defaultlist.hpp): sem "adicionar lista", so ATUALIZAR
    ui.loadSaved();                           // ultimo streamer e lista M3U guardados
    if (!g_prevStage.empty()) app.say("Aviso: da ultima vez o app parou no passo: " + g_prevStage);
#ifdef __OPTIMIZE__
    if (g_prevStage.empty()) app.say("Build " __DATE__ " " __TIME__ " (otimizado)");
#else
    if (g_prevStage.empty()) app.say("Build " __DATE__ " " __TIME__ " (SEM otimizacao)");
#endif
    trace("estado-criado");
    boot.line("[2] estado do app criado");


    Mode mode = Mode::Menu;
    Player player;
    Keyboard kbd;
    trace("player-ok");
    bool gotFrame = false;
    double lastWaitDraw = 0;
    uint32_t prev = 0;
    int back = 1;
    bool dirty = true;
    // Lista em segundo plano: o app abre e responde na hora. A thread so le/baixa (nunca desenha nem le o controle);
    // quando termina, o loop principal troca a lista de uma vez. Ordem: copia salva -> se nao houver, baixa da rede.
    ListLoader loader;
    double loaderSay = 0; size_t loaderSayItems = (size_t)-1;
    // Protecao "fecha sempre ao abrir": enquanto a lista carrega (e uns segundos depois de aparecer no menu) fica gravado "bgload=1".
    // Se o app cair nesse intervalo, na proxima abertura a lista NAO e carregada sozinha (o app abre e explica), em vez de cair de novo.
    double bgloadClearAt = 0;
    bool autoLoadList = false; int autoLoadFrames = 0;
    auto startLoader = [&](const std::string &url) {
        trace("lista-net-init");
        g_net.init();                                                 // a rede e inicializada na thread principal
        int slot = -1; std::string cu; pf.cacheInfo(slot, cu);
        int cslot = (slot >= 0 && cu == url) ? slot : -1;              // so vale a copia DESTA url
        loader.start(url,
            [cslot](const std::string &, const ListLoader::Sink &sink, const std::atomic<bool> &stop) { return Ps4Platform::cacheReadSlot(cslot, sink, stop); },
            [&pf, slot](const std::string &u, const ListLoader::Sink &sink, const std::atomic<bool> &stop) { return pf.netDownloadBg(u, slot, sink, stop); });
        loaderSay = 0; loaderSayItems = (size_t)-1;
        pf.saveSetting("bgload", "1");
        trace("lista-bg-inicio");
    };

    // Reconexao automatica: ate 12 tentativas em 3 minutos (Kick pode precisar renovar varias vezes)
    double rtT[12] = {}; int rtI = 0; bool autoMode = false;
    auto allowAuto = [&]() -> bool {
        double tn = nowSec(); int n = 0;
        for (double x : rtT) if (x > 0 && tn - x < 180.0) n++;
        if (n >= 12) return false;
        rtT[rtI++ % 12] = tn; return true;
    };


    // Reinicia live com seguranca (troca de qualidade / reconexao).
    // Nunca chama stop() no meio do poll sem pausar audio e abortar o feeder antes —
    // isso era a causa do CE ao apertar △/OPTIONS na Kick.
    auto safeRestartLive = [&](const std::string &why) {
        if (player.url.empty() && player.kickSlug.empty()) return;
        trace((std::string("safe-restart:") + why).c_str());
        player.quietWait = true;
        player.waitLabel = "Reconectando...";
        player.showOpts = false;
        player.showInfo = false;
        player.scrubMode = false;
        player.seekSkip = false;
        player.paused = true;
        player.audio.muted = true;
        try { player.audio.retarget(0); } catch (...) {}
        try { g_feed.abort(); } catch (...) {}
        sceKernelUsleep(50000);
        std::string u = player.url, ti = player.title, slug = player.kickSlug;
        try { mpv_player::shutdown(); } catch (...) {}
                try { player.stop(); } catch (...) {}
        Player::waitTeardown(0.6);
        try { g_feed.join(); } catch (...) {}
        try { g_feed.sb.reset(); } catch (...) {}
        if (!slug.empty()) {
            pf.pendingPlayTitle = "@" + slug;
            if (!player.nextKickUrl.empty()) {
                pf.pendingPlayUrl = player.nextKickUrl;
                player.nextKickUrl.clear();
                pf.pendingKickSlug.clear();
                app.say("Reconectando @" + slug + " (link novo)...");
            } else {
                pf.pendingKickSlug = slug;
                pf.pendingPlayUrl.clear();
                app.say("Reconectando @" + slug + "...");
            }
        } else {
            pf.pendingPlayUrl = u;
            pf.pendingPlayTitle = ti;
        }
        mode = Mode::Menu;
        dirty = true;
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
    // Menu com redesenho parcial (damage.hpp): so pinta e copia para a tela o que mudou; tela parada = nada a fazer.
    MenuPainter menuPainter;
    FlipStale flipStale;
    double lastMenuPresent = 0;
    auto flip = [&]() {
        g_mainTick++;
        menuPainter.invalidate();                                // algo que nao e o menu usou o rascunho/telas: o menu repinta tudo na volta
        memcpy(bufs[back], shadow, (size_t)W * H * 4);          // copia sequencial (rapida)
        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3;
    };
    // Video do mpv SEM OSD: o mpv escreve o quadro direto na tela de video (bufs[back]) e aqui so se troca a tela.
    // Antes cada quadro era escrito no rascunho, copiado para uma 2a copia (mpvClean) e copiado de novo para a tela: ~16 MB de memcpy por quadro.
    // Com OSD visivel (so ~12 s apos apertar um botao) continua pelo rascunho, porque desenhar o OSD le pixels e a memoria de video so e rapida para escrita.
    // Chave de seguranca: criar o arquivo /data/kickiptv_mpv_shadow volta ao caminho antigo (sempre pelo rascunho).
    const bool mpvDirect = !fileExists("/data/kickiptv_mpv_shadow");
    auto flipDirect = [&]() {
        g_mainTick++;
        menuPainter.invalidate();
        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3;
    };
    ui.dbg = [](const char *st) { static int n = 0; if (n < 6) { n++; trace(st); } };      // so o 1o desenho do menu (nao enche o log)
    double animCost = 0.03;                              // custo do ultimo desenho do menu: define a taxa de quadros da animacao
    auto presentMenu = [&]() {
        static bool firstDraw = true;
        if (firstDraw) trace("menu-1o-desenho");
        double tp0 = nowSec();
        lastMenuPresent = tp0;
        g_mainTick++;
        ui.tick(tp0);                                  // relogio das animacoes
        if (!menuPainter.valid) flipStale.invalidateAll(W, H);   // as 3 telas de video nao tem o menu: copia tudo
        auto dmg = menuPainter.paint(ui, shadow, W, H);          // pinta no rascunho so os retangulos que mudaram
        if (firstDraw) trace("menu-render-ok");
        if (dmg.n > 0) {
            for (int i = 0; i < dmg.n; i++) flipStale.mark(dmg.r[i]);
            auto todo = flipStale.take(back);                    // o que ESTA tela de video ainda nao recebeu
            dCopyRects((uint32_t *)bufs[back], shadow, W, todo.r, todo.n);
            sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3;
        }
        animCost = nowSec() - tp0;
        {   // diagnostico de lentidao: quadros do menu que passam de 70 ms (maximo 30 linhas no log)
            static int slow = 0, total = 0;
            static double pfT = 0, pfSum = 0, pfMax = 0; static int pfN = 0, pfSlow = 0;           // resumo a cada 5 s de uso do menu
            if (total > 5) { pfN++; pfSum += animCost; if (animCost > pfMax) pfMax = animCost; if (animCost > 0.033) pfSlow++; }
            if (pfT == 0) pfT = tp0;
            if (tp0 - pfT >= 5.0) { if (pfN > 0) { char pb[120]; snprintf(pb, sizeof pb, "menu-perf quadros=%d media=%.0fms max=%.0fms acima33ms=%d tela=%d", pfN, pfSum / pfN * 1000, pfMax * 1000, pfSlow, (int)ui.screen); trace(pb); } pfT = tp0; pfN = pfSlow = 0; pfSum = pfMax = 0; }
            if (++total > 5 && animCost > 0.07 && slow < 30) { slow++; char b[96]; snprintf(b, sizeof b, "menu-quadro-lento %.0fms tela=%d", animCost * 1000, (int)ui.screen); trace(b); }
        }
        if (firstDraw) {
            firstDraw = false; trace("menu-flip-ok"); ui.dbg = nullptr;
        }
    };
    auto presentWait = [&]() { FbRenderer r(shadow, W, H); player.renderWaiting(r); flip(); };
    pf.busyFn = [&](const std::string &) { presentMenu(); };
    auto presentKbd = [&]() { FbRenderer r(shadow, W, H); kbd.render(r, ((int)(nowSec() * 2)) % 2 == 0, ui.timeText, ui.dateText); flip(); };
    auto updateClock = [&]() -> bool {         // true se o minuto mudou
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        std::string t, d; clockStrings((long long)ts.tv_sec, t, d);
        if (t == ui.timeText && d == ui.dateText) return false;
        ui.timeText = t; ui.dateText = d; return true;
    };
    updateClock();
    boot.line("[4] abrindo a interface...");
    trace("abertura");

    const bool kShowIntro = false;      // animacao de abertura (K + titulo + barra); false = pula
    if (kShowIntro) {   // ---- abertura animada: marca K aparece, titulo, barra de carregamento ----
        double ti = nowSec();
        for (;;) {
            double e = nowSec() - ti;
            if (e > 1.8) break;
            { static bool q1 = true; if (q1) { q1 = false; trace("intro-q1"); } }
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
        if (sysWantsStop()) { g_quitting = true; return true; }      // PS: para o download da lista agora
        presentMenu();
        pin.poll();
        if (pin.pending & B_CIRCLE) { pin.pending &= ~B_CIRCLE; return true; }
        // OPTIONS nao cancela/fecha mais
        return false;
    };
    trace("antes-lista");
    {   // Salto no filme (JumpToTime): se o app caiu no meio de um salto na vez anterior, desliga o salto de vez (volta ao seek antigo)
        if (fileExists(kJumpGuardFile)) { remove(kJumpGuardFile); touchFile(kNoJumpFile); trace("seek-jump-queda-desligado"); }
        g_jumpMode = (fileExists(kNoJumpFile) || !sceAvPlayerJumpToTime) ? 0 : 1;
        trace(g_jumpMode.load() ? "seek-jump-ligado" : "seek-jump-desligado");
    }
    // Protecao contra "fecha sempre ao abrir": se a ultima abertura caiu durante a lista M3U, muda de estrategia.
    //  1a queda -> a proxima abertura usa o MODO SEGURO (baixa a lista sem thread de rede) e isso fica gravado;
    //  caiu ate no modo seguro -> nao carrega a lista sozinho (o app abre normalmente e mostra o passo onde parou).
    {
        auto starts = [&](const char *p) { return g_prevStage.rfind(p, 0) == 0; };
        // Esta versao corrige a causa das quedas (heap). As marcas de queda gravadas pelas versoes anteriores ("bgload", modo seguro)
        // seriam herdadas e a lista nunca mais carregaria sozinha: limpa UMA vez. Se cair de novo, a protecao volta a funcionar normal.
        if (pf.loadSetting("guardver") != "heap2") {
            pf.saveSetting("guardver", "heap2"); pf.saveSetting("bgload", ""); pf.saveSetting("safem3u", "");
            g_prevStage.clear(); trace("guardas-limpas");
        }
        bool crashM3u = starts("m3u-inicio") || starts("m3u-t-") || starts("m3u-sync") || starts("m3u-s-") || starts("m3u-parse") ||
                        starts("m3u-cache-inicio") || starts("m3u-cache-parse");
        bool wasSafe = pf.loadSetting("safem3u") == "1";
        if (wasSafe) g_safeM3U = true;
        bool skipAuto = false;
        if (pf.loadSetting("bgload") == "1") { skipAuto = true; pf.saveSetting("bgload", ""); trace("bgload-caiu"); }   // pula so ESTA vez; a proxima abertura tenta de novo    // a ultima abertura caiu carregando a lista
        if (crashM3u) {
            if (!wasSafe) { g_safeM3U = true; pf.saveSetting("safem3u", "1"); trace("modo-seguro-ligado"); }
            else skipAuto = true;
        }
        if (ui.hasSavedList() && skipAuto) {
            app.say("A lista M3U fechou o app na ultima vez - abra LISTA M3U e aperte X.");
        } else if (ui.hasSavedList()) {
            // Abre o app e carrega a lista SALVA em segundo plano (le o arquivo guardado no console: nao baixa nada).
            // So baixa da rede se nao houver copia salva desta lista. Se o app cair nisso, a proxima abertura nao repete (protecao acima).
            (void)startLoader;                       // (a carga em thread no inicio fica desligada: o app tem que abrir sempre)
            autoLoadList = true;                     // carrega a lista salva logo depois do 1o quadro do menu (veja no laco principal)
            app.say("Lista M3U salva - carregando...");
        }
    }

    {
        char pb[64];
        snprintf(pb, sizeof pb, "pad-status h=%d", pin.h);
        trace(pb);
    }
    // Teste do audio SDL em segundo plano desde ja (nao bloqueia nada): quando o 1o video abrir o resultado ja esta pronto.
    mpv_player::setTrace([](const char *st) { trace(st); });
    mpv_player::prewarm();
    trace("loop-inicio");
    startWatchdog();
    double hbNext = monoSec() + 60, hbT0 = monoSec();
    bool sysExit = false;                       // true = saindo porque o sistema (menu do PS) esta fechando/trocando o app
    double loopPrevT = monoSec(); int loopSlowN = 0;
    double lastPressT = 0;                                     // ultimo botao/direcional: tarefas pesadas de fundo esperam o app ficar parado
    double lapT = monoSec(); std::string lapS;                 // cronometra as partes da volta do laco: a que passar de 40 ms aparece no log
    auto lap = [&](const char *nm) { double t = monoSec(), d = t - lapT; lapT = t; if (d > 0.04 && lapS.size() < 160) { char b[48]; snprintf(b, sizeof b, " %s=%.0f", nm, d * 1000); lapS += b; } };
    for (;;) {
        g_mainTick++;
        {   // volta do laco demorou? (rede, trava de mutex, copia lenta...): grava quanto e o ultimo passo registrado
            double ln = monoSec(), dl = ln - loopPrevT; loopPrevT = ln;
            if (dl > 0.12 && dl < 5.0 && loopSlowN < 60) { loopSlowN++; char lb[260]; snprintf(lb, sizeof lb, "laco-lento %.0fms modo=%d partes:%s", dl * 1000, (int)mode, lapS.empty() ? " (nenhuma>40ms)" : lapS.c_str()); trace(lb); }
            lapS.clear(); lapT = monoSec();
        }
        if (mode != Mode::Menu) menuPainter.invalidate();      // player/teclado escrevem direto nas telas de video: o menu repinta tudo ao voltar
        if (monoSec() >= hbNext) {                       // 1 linha por minuto no log (/data/kickiptv.log): ate quando o app estava vivo
            hbNext += 60;
            char hb[64]; snprintf(hb, sizeof hb, "vivo-%dmin", (int)((monoSec() - hbT0) / 60.0 + 0.5));
            trace(hb);
        }
        // --- Controle: PadIn + fallback direto do scePad (se a fila falhar, o estado ainda vale) ---
        pin.poll(); lap("pad");
        uint32_t pressed = pin.take();
        if (pressed) lastPressT = nowSec();
        {
            static uint32_t prevHw = 0;
            OrbisPadData hw;
            memset(&hw, 0, sizeof hw);
            if (pin.h >= 0 && scePadReadState(pin.h, &hw) == 0) {
                uint32_t btn = hw.buttons;
                // edge dos botoes digitais (sem analogico)
                pressed |= (btn & ~prevHw);
                prevHw = btn;
                // se PadIn nao preencheu held, usa hardware
                if (pin.held == 0 && btn != 0) pin.held = btn;
            } else if (pin.h < 0) {
                // tenta abrir de novo
                static double nextPad = 0;
                double tn = nowSec();
                if (tn >= nextPad) {
                    nextPad = tn + 1.0;
                    int np = scePadOpen(userId, 0, 0, NULL);
                    if (np < 0) np = scePadOpen(userId, 1, 0, NULL);
                    if (np >= 0) { pin.h = np; trace("pad-hotplug"); }
                }
            }
        }
        uint32_t now = pin.held | pressed;
        // --- Botao PS / Fechar aplicativo / troca de app ---
        // Ao ir pro background: para player + workers o mais rapido possivel
        // (se o usuario escolher "Fechar aplicativo", o sistema mata o processo;
        //  se ja liberamos AvPlayer/threads, nao da CE-34878-0).
        {
            static bool wasBg = false, seenFg = false, keepBg = false, wasUi = false;
            static double bgSince = 0;
            // para TUDO de uma vez: video, capas, download da lista, alimentadores de rede
            auto stopAllNow = [&](const char *why) {
                trace(why);
                g_quitting = true;                 // workers de rede/capas param
                if (mode == Mode::Play || player.h) {
                    try { player.stop(); } catch (...) {}
                    mode = Mode::Menu;
                }
                g_covers.setPaused(true);
                try { g_covers.cancelBackground(); } catch (...) {}
                if (loader.stillAlive()) {
                    try { loader.cancel(0.05); } catch (...) {}
                }
                try { g_feed.abort(); abortHf(); abortPf(); } catch (...) {}
                // da um momento pro stop assincrono soltar o AvPlayer
                Player::waitTeardown(0.35);
                if (g_tearing.load() == 0 && kSelfExitOnBg) unloadAvModules();      // (so no modo de saida automatica; em segundo plano o PS4 vai fechar o app e nao deve achar modulos ja soltos) sem player aberto: solta os modulos do AvPlayer para o PS4 poder fechar o app
                trace("tudo-parado");
                dirty = true;
            };
            if (kPsStopLogic) {
                bool trip = g_sys.poll();               // so conta apos o sistema ter ficado calmo (ver SysWatch)
                bool ovl = trip && g_sys.ovl;
                bool bgRaw = trip && g_sys.bg && (nowSec() - g_sys.lastOvlTrip < 20.0);   // sair sozinho: so apos o menu do PS ter aparecido (aviso de "segundo plano" sozinho ja deu falso positivo e fechava o app)
                bool bg = bgRaw;
                {   // diagnostico: 1 linha no log a cada mudanca (o log mostra o que o sistema informou)
                    static int lastSig = -1;
                    int sig = (g_sys.ovl ? 1 : 0) | (g_sys.bg ? 2 : 0) | (trip ? 4 : 0) | (mode == Mode::Play ? 8 : 0) | (g_sys.armed ? 16 : 0);
                    if (sig != lastSig) { lastSig = sig; char b[80]; snprintf(b, sizeof b, "sys ovl=%d bg=%d valido=%d play=%d armado=%d", g_sys.ovl, g_sys.bg, trip, mode == Mode::Play, g_sys.armed); trace(b); }
                }
                if (!bg) seenFg = true;                // so conta como "foi para o inicio" depois de ter estado na frente uma vez
                // "Fechar aplicativo" manda o app para o segundo plano SEM passar pelo menu do PS (ovl=0): o log mostrava
                // "sys ovl=0 bg=1 valido=1" e nada mais, o app seguia rodando video/rede/capas e o sistema matava o processo no meio (CE-34878-0).
                // Para PARAR tudo basta o sistema dizer que foi pro segundo plano (parar e inofensivo: ao voltar, retoma). A guarda dos 20 s
                // continua valendo so para a variavel 'bg' acima (que e o que podia fechar o app sozinho).
                bool bgAny = trip && g_sys.bg;
                bool stopReq = ovl || bgAny;
                if (stopReq && !wasUi && !bg) { wasUi = true; stopAllNow(ovl ? "sistema-menu-ps" : "sistema-bg-parar"); }       // para tudo, mas o app continua aberto
                if (!stopReq && wasUi) { wasUi = false; if (!bg) { g_quitting = false; g_covers.setPaused(false); dirty = true; } }
                {   // "Fechar aplicativo" do PS4 manda o app ao segundo plano SEM passar pelo menu do PS (ovl=0): estavel por 1,5 s = sai PELO SISTEMA
                    static double bgAnySince = -1; static bool keepAny = false;
                    if (bgAny) { if (bgAnySince < 0) { bgAnySince = nowSec(); keepAny = fileExists(kKeepBgFile); } } else bgAnySince = -1;
                    if (kSelfExitOnBg && bgAny && seenFg && !keepAny && bgAnySince > 0 && nowSec() - bgAnySince >= kBgExitDelay) {
                        // O sistema congela o processo em menos de 0,25 s depois de bg=1 (os logs terminavam em "bg-aguardando-sistema"):
                        // tudo ja foi parado no menu do PS, entao pede a saida AGORA, sem juntar threads.
                        g_quitting = true;
                        ps4Exit("bg-saida-imediata");
                    }
                }
                if (bg && !wasBg) {
                    g_bgLastIter = 0;
                    bgSince = nowSec();
                    keepBg = fileExists(kKeepBgFile);
                    // O menu do PS ja parou tudo (video, rede, capas). So para de novo se ainda tiver algo rodando: cada espera aqui
                    // atrasava a saida e o sistema fechava o app antes (o log terminava logo apos "tudo-parado").
                    if (mode == Mode::Play || player.h) stopAllNow("sistema-background");
                    else trace("sistema-background");
                }
                if (!bg && wasBg) {
                    trace("sistema-foreground"); g_bgLastIter = 0;
                    g_quitting = false;
                    g_covers.setPaused(false);
                    dirty = true;
                }
                wasBg = bg;
                // Enquanto estiver em background: nao toca video nem redesenha menu pesado
                if (bg) {
                    // Ficou no inicio do PS4 por mais de 1 s: encerra sozinho (nada de esperar o sistema matar o processo).
                    // Sem esperar threads de rede (_Exit); o estado (lista/ultimo streamer) ja fica salvo em disco.
                    if (kSelfExitOnBg && seenFg && !keepBg && nowSec() - bgSince >= kBgExitDelay) {   // segundo plano estavel: o sistema esta fechando/trocando o app
                        { char b[96]; snprintf(b, sizeof b, "bg-saindo tearing=%d playerH=%d mode=%d", g_tearing.load(), (int)player.h, (int)mode); trace(b); }
                        // Nao da _Exit seco: depois de assistir video ainda existem threads (download do filme, rede, decodificador, capas).
                        // Matar o processo com elas rodando derruba o app (CE-34878-0). Vai para o encerramento limpo (junta todas as threads).
                        g_quitting = true;
                        ps4Exit("bg-saida-imediata2");
                    }
                    { static bool said = false; if (!said) { said = true; trace("bg-aguardando-sistema"); } }
                    {   // o processo ficou congelado e acordou ainda em segundo plano: o sistema esta fechando o app -> sai pelo sistema
                        double tn2 = nowSec();
                        if (g_bgLastIter > 0 && tn2 - g_bgLastIter > 0.5 && g_sys.poll() && g_sys.bg) ps4Exit("bg-acordou-fechando");
                        g_bgLastIter = tn2;
                    }
                    {   // batimento a cada 1 s: o log mostra ate quando o processo viveu em segundo plano antes do sistema fecha-lo
                        static double nextBeat = 0; double tb = nowSec();
                        if (tb >= nextBeat) { nextBeat = tb + 1.0; char b[64]; snprintf(b, sizeof b, "bg-vivo %.1fs", tb - bgSince); trace(b); }
                    }
                    sceKernelUsleep(30000);
                    continue;
                }
            }
            // Se o pad nao abriu (ou perdeu handle), tenta reabrir a cada ~2 s
            if (pin.h < 0) {
                static double nextTry = 0;
                double tn = nowSec();
                if (tn >= nextTry) {
                    nextTry = tn + 2.0;
                    int np = scePadOpen(userId, 0, 0, NULL);
                    if (np < 0) np = scePadOpen(userId, 1, 0, NULL);
                    if (np >= 0) {
                        pin.h = np;
                        char b[32]; snprintf(b, sizeof b, "pad-reopen=%d", np);
                        trace(b);
                    }
                }
            }
        }
        // OPTIONS: NAO fecha o app (PS button faz isso).
        //  - No player ao vivo: troca qualidade (Fluido/720/1080) e reabre (ajuda se a Kick travar)
        //  - No player VOD: mostra/esconde HUD
        //  - No menu: abre busca (lupa)
        if (pressed & B_OPTIONS) {
            if (mode == Mode::Play) {
                if (!player.isVod && !player.url.empty()) {
                    g_quality = (g_quality.load() + 1) % kQualityCount;
                    app.say(std::string("Qualidade: ") + kQualityName[g_quality.load() % kQualityCount]);
                    safeRestartLive("options-quality");
                    sceKernelUsleep(30000);
                    continue;
                } else {
                    // VOD: alterna HUD
                    player.hudUntil = (nowSec() <= player.hudUntil) ? 0 : nowSec() + 12;
                }
            } else if (mode == Mode::Menu) {
                ui.handle(Input::Extra);  // busca
                dirty = true;
            }
        }
        if (wantQuit) break;
        (void)prev;
        { static bool first = true; if (first) { first = false; trace("loop-q1"); } }

        g_covers.setPaused(mode == Mode::Play);      // durante o video o baixador de capas em segundo plano fica parado

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
            if (pressed & B_TRIANGLE) { kbd.insert(" "); changed = true; }   // △ = espaco
            if (pressed & B_R2)       { kbd.toggleSymbols(); changed = true; }
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

        if (mode == Mode::Play && player.useMpv) {
            // ---- libmpv (igual GMCA): o mpv decodifica e entrega o quadro; aqui so desenhamos o OSD por cima e fazemos o flip ----
            mpv_player::poll();
            const auto st = mpv_player::status();
            const double tn = nowSec();
            player.lastPtsMs = (int64_t)(st.positionSec * 1000.0);
            if (st.durationSec > 0) player.durationMs = (int64_t)(st.durationSec * 1000.0);
            player.paused = st.paused;
            player.bufPaused = st.buffering && player.mpvGot;
            player.bufPct = st.bufferPct;
            // volta ao menu (ou enfileira outra URL, como os botoes L1/R1 do ao vivo fazem)
            auto requeue = [&](const std::string &u, const std::string &t, int ep) {
                pf.pendingPlayUrl = u; pf.pendingPlayTitle = t; pf.pendingEpIndex = ep;
                pf.pendingEpUrls = player.epUrls; pf.pendingEpTitles = player.epTitles;
                // Nao e troca de qualidade: mantem g_quality e mostra texto de episodio
                player.quietWait = false;
                player.waitLabel = "Próximo episódio...";
                try { mpv_player::stop(); } catch (...) {}
                player.stop();
                sceKernelUsleep(120000);   // deixa o mpv/stream_cb soltar antes do proximo start
                mode = Mode::Menu; dirty = true;
            };
            if (pressed & B_CIRCLE) {
                player.stop(); player.quietWait = false;
                player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
                mode = Mode::Menu; dirty = true; sceKernelUsleep(16000); continue;
            }
            if ((st.failed && !player.mpvGot) || (!player.mpvGot && tn - player.mpvT0 > 60.0)) {
                // sem fallback: mostra o motivo e volta ao menu
                trace("mpv-sem-video");
                app.say(st.failed ? "libmpv: " + st.error : std::string("libmpv: nenhum quadro em 60 s"));
                player.stop(); player.quietWait = false;
                player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
                mode = Mode::Menu; dirty = true; sceKernelUsleep(16000); continue;
            }
            if (st.failed || (st.eof && !st.paused)) {
                // Canal (curto ou ao vivo): nao reabre, nao troca qualidade. Volta pro inicio e segue.
                // Canal = nao e filme/serie (URL sem /movie/ ou /series/) OU duracao curta.
                const bool looksLikeChannel = player.url.find("/movie/") == std::string::npos
                    && player.url.find("/series/") == std::string::npos
                    && (player.url.find(".m3u8") != std::string::npos || st.durationSec < 30.0 || !player.isVod);
                if (st.eof && looksLikeChannel && !player.canNextEp()) {
                    // canal unico: loop silencioso
                    mpv_player::seekSec(0);
                    player.lastPtsMs = 0; player.playedMs = 0; player.maxPtsMs = 0;
                    player.playStartWall = nowSec();
                    sceKernelUsleep(4000); continue;
                }
                if (st.eof && looksLikeChannel && player.canNextEp()) {
                    // canal com lista: NAO avanca sozinho. So o usuario troca com L1/R1.
                    // Loop no mesmo canal.
                    mpv_player::seekSec(0);
                    player.lastPtsMs = 0; player.playedMs = 0; player.maxPtsMs = 0;
                    player.playStartWall = nowSec();
                    sceKernelUsleep(4000); continue;
                }
                if (st.eof && player.canNextEp()) {
                    player.epIndex++;
                    requeue(player.epUrls[player.epIndex], player.epTitles[player.epIndex], player.epIndex);
                } else {
                    if (st.failed) app.say("Falha na reprodução: " + st.error);
                    player.stop(); player.quietWait = false; player.waitLabel.clear();
                    player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
                    mode = Mode::Menu; dirty = true;
                }
                sceKernelUsleep(16000); continue;
            }
            // ---- controles (como no GMCA): X pausa, L1/R1 saltam, quadrado muda o som, triangulo mostra/oculta o OSD ----
            if (pressed) player.hudUntil = tn + 12;
            if (player.mpvGot) {
                if (pressed & B_CROSS) { mpv_player::pause(!st.paused); player.paused = !st.paused; player.hudUntil = tn + 12; }
                // L2/R2: preview continuo (estilo YouTube). Seek real so ao soltar.
                if (player.isVod && !player.showOpts) {
                    int dir = 0;
                    if (pin.held & B_L2) dir = -1;
                    else if (pin.held & B_R2) dir = 1;
                    player.triggerScrub(dir, tn);
                }
                if (player.isVod && (pressed & (B_L1 | B_R1 | B_LEFT | B_RIGHT)) && !player.trigScrub) {
                    const int d = (pressed & (B_L1 | B_LEFT)) ? -10 : 10;
                    if (tn - player.toastUntil > 0.6) player.mpvSeekAcc = 0;
                    player.mpvSeekAcc += d;
                    mpv_player::seekRelative((double)d);
                    char b[32]; snprintf(b, sizeof b, "%+d s", player.mpvSeekAcc);
                    player.toastText = b; player.toastUntil = tn + 1.2;
                }
                if (pressed & B_SQUARE) {
                    player.muted = !player.muted; mpv_player::setMute(player.muted);
                    player.toastText = player.muted ? "Mudo" : "Som ligado"; player.toastUntil = tn + 1.2;
                }
                if (pressed & B_TRIANGLE) player.hudUntil = (tn <= player.hudUntil && !player.paused) ? 0 : tn + 12;
            }
            if (!player.mpvGot) {                                    // ate o 1o quadro: tela de espera com o spinner
                static double lastW = 0;
                FbRenderer r(shadow, W, H);
                if (mpv_player::renderToBgra((uint8_t *)shadow, W, H, W * 4, true)) {  // force: pega o 1o quadro mesmo se o mpv achar que nao mudou
                    player.mpvGot = true; player.hudUntil = tn + 8; gotFrame = true;
                    player.lastPtsMs = (int64_t)(st.positionSec * 1000.0);
                    trace("mpv-1o-quadro");
                } else if (tn - lastW > 0.12) { lastW = tn; presentWait(); }
                else sceKernelUsleep(4000);
                if (!player.mpvGot) continue;
                if (player.hudActive() || tn < player.toastUntil) player.overlay(r);
                flip(); player.mpvLastOv = tn; player.mpvLastHud = true;
                continue;
            }
            // preso em 0 s com quadro: um empurrãozinho de 0,2 s destravar sem fechar
            if (player.isVod && st.positionSec < 0.05 && tn - player.mpvT0 > 4.0 && !player.trigScrub) {
                static double lastNudge = 0;
                if (tn - lastNudge > 5.0) { lastNudge = tn; mpv_player::seekSec(0.2); trace("mpv-nudge"); }
            }
            {
                const bool hudNow = player.hudActive() || tn < player.toastUntil;
                const bool direct = mpvDirect && !hudNow && player.kickSlug.empty();      // sem OSD: mpv -> tela, zero copias (live Kick: sempre pelo rascunho em RAM; a queda era logo apos o 1o quadro)
                uint8_t *dst = direct ? (uint8_t *)bufs[back] : (uint8_t *)shadow;
                auto present = [&]() {
                    if (direct) flipDirect();
                    else { FbRenderer r(shadow, W, H); if (hudNow) player.overlay(r); flip(); }
                    player.mpvLastOv = tn; player.mpvLastHud = hudNow;
                };
                if (mpv_player::renderToBgra(dst, W, H, W * 4)) {                      // quadro novo
                    static int nfr = 0;
                    if (nfr < 6) { char tb[48]; snprintf(tb, sizeof tb, "mpv-quadro-%d-%s", ++nfr, direct ? "direto" : "rascunho"); trace(tb); }
                    present();
                    if (nfr > 0 && nfr <= 6) trace("mpv-quadro-apresentado");
                } else if (player.trigScrub || pressed || hudNow != player.mpvLastHud || (hudNow && tn - player.mpvLastOv > 0.03)) {
                    // scrub: redesenha todo quadro (~30 fps) para a barra andar suave
                    if (mpv_player::renderToBgra(dst, W, H, W * 4, true)) present();
                }
            }
            sceKernelUsleep(4000);
            continue;
        }
        if (mode == Mode::Play) {
            // O = sair (ou fecha menu/info primeiro)
            if (pressed & B_CIRCLE) {
                if (player.showOpts) { player.showOpts = false; player.hudUntil = nowSec() + 8; }
                else if (player.showInfo) { player.showInfo = false; player.hudUntil = nowSec() + 8; }
                else {
                    player.stop(); player.quietWait = false;
                    player.epUrls.clear(); player.epTitles.clear(); player.epIndex = -1;
                    player.showOpts = false; player.showInfo = false;
                    mode = Mode::Menu; dirty = true; sceKernelUsleep(16000); continue;
                }
            }
            if (pressed) player.hudUntil = nowSec() + (player.showOpts ? 30 : 12);

            // TRIANGULO = menu Opcoes
            if (pressed & B_TRIANGLE) {
                player.showOpts = !player.showOpts;
                player.showInfo = false;
                if (player.showOpts) player.optCur = 0;
                player.hudUntil = nowSec() + 30;
            }

            // ---- Menu de opcoes (△) ----
            if (player.showOpts) {
                if (pressed & B_UP) { player.optCur = (player.optCur + 4) % 5; }
                if (pressed & B_DOWN) { player.optCur = (player.optCur + 1) % 5; }
                auto cycleOpt = [&](int dir) {
                    if (player.optCur == 0) g_quality = (g_quality.load() + (dir > 0 ? 1 : kQualityCount - 1)) % kQualityCount;
                    if (player.optCur == 1) {
                        player.aspectMode = (player.aspectMode + (dir > 0 ? 1 : 2)) % 3;
                        // aplica na hora: sai da pausa visual forcando redraw
                        player.hudUntil = nowSec() + 30;
                    }
                    if (player.optCur == 2) {
                        player.muted = !player.muted;
                        player.audio.muted = player.paused || player.muted;
                    }
                };
                if (pressed & B_LEFT) cycleOpt(-1);
                if (pressed & B_RIGHT) cycleOpt(+1);
                if (pressed & B_CROSS) {
                    if (player.optCur == 0) {
                        if (!player.isVod && !player.url.empty()) {
                            g_quality = (g_quality.load() + 1) % kQualityCount;
                            app.say(std::string("Qualidade: ") + kQualityName[g_quality.load() % kQualityCount]);
                            safeRestartLive("opts-quality");
                            sceKernelUsleep(30000);
                            continue;
                        } else g_quality = (g_quality.load() + 1) % kQualityCount;
                    } else if (player.optCur == 1) {
                        player.aspectMode = (player.aspectMode + 1) % 3;
                    } else if (player.optCur == 2) {
                        player.muted = !player.muted;
                        player.audio.muted = player.paused || player.muted;
                    } else if (player.optCur == 3) {
                        player.showInfo = true; player.showOpts = false;
                    } else if (player.optCur == 4) {
                        player.showOpts = false;
                    }
                }
                player.hudUntil = nowSec() + 30;
            } else if (player.showInfo) {
                // so O fecha (ja tratado)
            } else {
                // ---- Controles do player ----
                // Filme/serie: sem pausa, sem avanco e sem troca de episodio (nao funcionavam); a barra de tempo aparece sozinha.
                // Ao vivo (canais): L1/R1 trocam para o canal anterior/proximo da lista.
                if (!player.isVod && (pressed & B_L1) && player.canPrevEp()) {
                    player.epIndex--;
                    pf.pendingPlayUrl = player.epUrls[player.epIndex];
                    pf.pendingPlayTitle = player.epTitles[player.epIndex];
                    pf.pendingEpIndex = player.epIndex;
                    pf.pendingEpUrls = player.epUrls; pf.pendingEpTitles = player.epTitles;
                    player.quietWait = false;
                    player.waitLabel = "Trocando canal...";
                    player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
                }
                if (!player.isVod && (pressed & B_R1) && player.canNextEp()) {
                    player.epIndex++;
                    pf.pendingPlayUrl = player.epUrls[player.epIndex];
                    pf.pendingPlayTitle = player.epTitles[player.epIndex];
                    pf.pendingEpIndex = player.epIndex;
                    pf.pendingEpUrls = player.epUrls; pf.pendingEpTitles = player.epTitles;
                    player.quietWait = false;
                    player.waitLabel = "Trocando canal...";
                    player.stop(); mode = Mode::Menu; dirty = true; sceKernelUsleep(8000); continue;
                }
                // SQUARE = mute rapido
                if (pressed & B_SQUARE) {
                    player.muted = !player.muted;
                    player.audio.muted = player.paused || player.muted;
                }
            }

            // ---- audio-recover: se o som sumiu no VOD, reabre a bomba de audio ----
            if (player.isVod && player.active && !player.paused && !player.muted && player.h) {
                static double lastAudCheck = 0;
                static unsigned lastChunks = 0;
                double tn = nowSec();
                if (tn - lastAudCheck > 3.0) {
                    unsigned c = player.audio.chunks.load();
                    // video andando mas audio parado ha 3s
                    if (player.frames > 90 && c == lastChunks) {
                        try {
                            player.audio.start(player.h);
                            player.audio.muted = false;
                            trace("audio-recover");
                        } catch (...) {}
                    }
                    lastChunks = player.audio.chunks.load();
                    lastAudCheck = tn;
                }
            }

            // ---- Live (Kick/IPTV): reconecta sozinho se congelar ----
            if (!player.isVod && player.active && !player.failed && !player.paused
                && !player.kickSlug.empty() && player.frames > 120) {
                double tn = nowSec();
                if (player.kickUrlAt < 1.0) player.kickUrlAt = tn;
                if (tn - player.kickUrlAt >= 210.0) {
                    player.kickUrlAt = tn;
                    std::string err, hls = app.resolveKickHls(player.kickSlug, &err);
                    if (!hls.empty()) {
                        player.nextKickUrl = hls;
                        trace("kick-url-refreshed");
                        bool weak = g_feed.failed.load()
                            || (player.lastFeedSegWall > 1.0 && tn - player.lastFeedSegWall > 6.0);
                        if (weak) {
                            g_reconnectNote = "renovacao do link Kick";
                            safeRestartLive("kick-url-proactive");
                            sceKernelUsleep(40000);
                            continue;
                        }
                    }
                }
            }
            if (!player.isVod && player.active && !player.failed && !player.paused) {
                std::string why;
                if (player.wantReconnect(why)) {
                    if (!allowAuto())
                        app.say("Live instavel — reconectando...");
                    g_autoRestarts++;
                    g_stallT[g_stallI++ % 8] = monoNow();                 // alimenta o ajuste automatico da proxima abertura
                    g_reconnectNote = why;
                    safeRestartLive(why);
                    sceKernelUsleep(50000);
                    continue;
                }
            }

            player.serviceJump();       // salto pendente (espera o download chegar ao ponto) + trava de seguranca

            // ---- Desenho ----
            if (player.active && !player.failed && g_tearing.load() == 0) {
                bool hud = player.hudActive();
                // CORRECAO DO "CONGELA APOS ~5 s": filme/serie SEMPRE desenha no rascunho (shadow) e o flip() copia para a tela.
                // Antes, com o HUD apagado, o quadro ia direto para a tela (bufs[back]) e logo em seguida o flip() copiava o
                // rascunho (com a ultima imagem do HUD) POR CIMA dele = imagem parada com o som seguindo. Mexer no analogico
                // religava o HUD (que usava o rascunho) e o video "voltava".
                player.bufferGuard();                       // internet lenta: pausa sozinho e retoma quando houver colchao
                FbRenderer r((hud || player.isVod) ? shadow : (uint32_t *)bufs[back], W, H);
                // CORRECAO DAS "SOMBRAS PRETAS PISCANDO": o overlay (faixas escuras de cima/baixo, translucidas) era redesenhado varias vezes
                // por cima do MESMO quadro (pausado, ou menu de opcoes aberto sem quadro novo) e escurecia cada vez mais, ate chegar um quadro
                // novo e limpar. Agora guardamos uma copia do quadro puro e restauramos antes de cada redesenho do overlay.
                static std::vector<uint32_t> cleanFrame; static bool cleanValid = false;
                const bool useShadow = (hud || player.isVod);
                auto saveClean = [&]() {
                    if (!useShadow || !shadow) return;
                    size_t n = (size_t)W * (size_t)H;
                    if (cleanFrame.size() != n) cleanFrame.resize(n);
                    memcpy(cleanFrame.data(), shadow, n * sizeof(uint32_t)); cleanValid = true;
                };
                auto prepClean = [&]() {                    // chamado antes de redesenhar o overlay SEM quadro novo
                    if (!useShadow || !shadow || !gotFrame) return;
                    size_t n = (size_t)W * (size_t)H;
                    if (!cleanValid || cleanFrame.size() != n) saveClean();         // shadow ainda esta sem overlay: ele e o quadro puro
                    else memcpy(shadow, cleanFrame.data(), n * sizeof(uint32_t));
                };
                if (player.paused) {
                    // pausado: so HUD (proporcao aplica no proximo quadro ao despausar)
                    prepClean();
                    player.overlay(r); flip();
                    sceKernelUsleep(12000);
                    continue;
                }
                if (player.poll(r)) {
                    gotFrame = true;
                    if (hud || player.showOpts || player.scrubMode) { saveClean(); player.overlay(r); flip(); }
                    else if (player.isVod) {
                        cleanValid = false;         // quadro puro no shadow, sem overlay
                        flip();                     // sem barra verde: a barra (vermelha) so aparece ao mexer no analogico/apertar botoes
                    } else {
                        cleanValid = false;
                        sceVideoOutSubmitFlip(vh, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0); back = (back + 1) % 3;
                    }
                } else if (player.showOpts || (hud && pressed)) {
                    prepClean();
                    player.overlay(r); flip();
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
            for (int i = 0; i < nU; i++) { ui.handle(Input::Up); }
            for (int i = 0; i < nD; i++) { ui.handle(Input::Down); }
            for (int i = 0; i < nL; i++) ui.handle(Input::Left);
            for (int i = 0; i < nR; i++) ui.handle(Input::Right);
            if (nU + nD + nL + nR > 0) { dirty = true; lastPressT = nowSec(); }
        }
        lap("direcional");
        if (mode == Mode::Menu && app.prefetchTick()) dirty = true;      // catalogo do painel chegando: a leitura pesada e feita na thread de rede; aqui so fatias de ~10 ms
        if (mode == Mode::Menu && ui.autoOpenTick()) dirty = true;      // painel Xtream: abre a categoria quando o cursor para nela
        lap("prefetch");
        if (pressed & B_L1)     { ui.handle(Input::PrevTab);  dirty = true; }
        if (pressed & B_R1)     { ui.handle(Input::NextTab);  dirty = true; }
        if (pressed & B_L2)     { ui.handle(Input::PageUp);   dirty = true; }
        if (pressed & B_R2)     { ui.handle(Input::PageDown); dirty = true; }
        if (pressed & B_CROSS)  { ui.handle(Input::Confirm);  dirty = true; }
        if (pressed & B_CIRCLE) { ui.handle(Input::Back);     dirty = true; }
        if ((pressed & B_SQUARE) && mode == Mode::Menu) { ui.handle(Input::Fav); dirty = true; }   // quadrado: favoritar (tela Kick)
        if (pressed & (B_TRIANGLE | B_TOUCH)) { ui.handle(Input::Extra);  dirty = true; }       // lupa: buscar (triangulo ou clique no touchpad)
        lap("botoes");
        if (updateClock()) dirty = true;
        lap("relogio");
        {   // andamento do lote de capas (TRIANGULO)
            static bool wasBulk = false; static double nextSay = 0;
            bool act = g_covers.backgroundActive();
            if (act && nowSec() >= nextSay) {
                nextSay = nowSec() + 0.5;
                app.say("Baixando capas... " + std::to_string(g_covers.backgroundDone()) + "/" + std::to_string(g_covers.backgroundTotal()) +
                        "  |  novas: " + std::to_string(g_covers.backgroundNew()) + "  |  TRIANGULO cancela");
                dirty = true;
            }
            if (wasBulk && !act) {
                size_t nw = g_covers.backgroundNew();
                app.say(nw >= 15000 ? "Capas: " + std::to_string(nw) + " novas salvas (limite por vez) - aperte TRIANGULO de novo para continuar"
                                    : "Capas prontas: " + std::to_string(nw) + " novas salvas no PS4 - nao precisam baixar de novo");
                dirty = true;
            }
            wasBulk = act;
        }
        {   // capa nova ficou pronta (baixada em segundo plano): redesenha sozinho, sem esperar um botao
            static uint32_t covSeen = 0; static double covNext = 0;
            uint32_t cv = g_covers.version();
            if (cv != covSeen && nowSec() >= covNext) { covSeen = cv; covNext = nowSec() + 0.12; dirty = true; }
        }
        if (loader.running()) {                   // andamento: atualiza o texto so quando muda (e no maximo ~3x por segundo)
            double tn = nowSec();
            size_t it = loader.items();
            if (tn - loaderSay >= 0.35 && it != loaderSayItems) {
                loaderSay = tn; loaderSayItems = it;
                { static size_t lastMB = 0; size_t mb = loader.bytes() / (16u << 20); if (mb != lastMB) { lastMB = mb; char b[40]; snprintf(b, sizeof b, "lista-%uMB", (unsigned)(mb * 16)); trace(b); } }
                app.say(std::string(loader.fromNet() ? "Baixando a lista M3U... " : "Carregando a lista salva... ") + App::fmtMB(loader.bytes()) + "  |  " + std::to_string(it) + " itens");
                dirty = true;
            }
        }
        {
            std::shared_ptr<Playlist> pl; ListLoader::State lst = ListLoader::Idle;
            if (loader.take(pl, lst)) {
                bgloadClearAt = nowSec() + 4.0;           // o menu ja vai desenhar a lista nova (capas etc.): so depois disso e que "deu certo"
                if (pl) {
                    app.list = std::move(*pl);
                    for (int i = 0; i < 4; i++) ui.cursor[i] = ui.scroll[i] = ui.catCur[i] = ui.catScroll[i] = 0;
                    ui.level = 0; ui.inItems = false;
                    std::string src = loader.fromNet() ? "  |  baixada e salva" : "  |  lista salva";
                    if (pl->truncated) app.say("Aviso: lista enorme - so os primeiros itens cabem na memoria do PS4 - " + app.summary());
                    else if (lst == ListLoader::Partial) app.say("Aviso: lista incompleta (" + std::string(loader.outOfMemory() ? "acabou a memoria" : "a conexao caiu") + ") - " + app.summary());
                    else app.say(app.summary() + src);
                    trace("lista-bg-ok");
                } else if (lst == ListLoader::Failed) {
                    {
                        std::string why = httpReason(pf.lastHttpStatus()), pv = bodyPreview(pf.lastBodyHead());
                        bool isM3u = pv.compare(0, 7, "#EXTM3U") == 0;
                        app.say("Falha ao carregar a lista: " + why + (!pv.empty() && !isM3u ? " | resposta: \"" + pv + "\"" : std::string()) +
                                "  (abra LISTA M3U e aperte X)");
                    }
                    trace("lista-bg-falhou");
                }
                dirty = true;
            }
        }

        if (bgloadClearAt > 0 && nowSec() >= bgloadClearAt) { pf.saveSetting("bgload", ""); bgloadClearAt = 0; trace("bgload-ok"); }

        // Lista salva: carrega do arquivo guardado no console (nao baixa), mas so depois de o menu ja ter aparecido uns quadros.
        // Marca "bgload" antes: se o app cair aqui, a proxima abertura NAO repete (abre normal e avisa).
        if (autoLoadList && mode == Mode::Menu && ++autoLoadFrames >= 20) {
            autoLoadList = false;
            presentMenu();
            pf.saveSetting("bgload", "1");
            trace("lista-auto-inicio");
            ui.loadSavedList();                      // copia salva; so baixa se nao houver copia desta lista
            bgloadClearAt = nowSec() + 4.0;
            trace("lista-auto-fim");
            dirty = true;
        }

        lap("capas-lista");
        {   // lista de lives da Kick baixada com o menu parado (so uma vez): depois, entrar na Kick e instantaneo
            static bool kickPre = false; static int kickPreFrames = 0;
            if (!kickPre && mode == Mode::Menu && !autoLoadList && ++kickPreFrames >= 250 && ui.screen == Screen::Home) { kickPre = true; try { ui.kickPrefetch(); } catch (...) {} dirty = true; trace("kick-prefetch"); }
        }
        if (ui.reloadRequested) {                 // botao ATUALIZAR (menu): baixa a lista de novo, pelo mesmo caminho do download manual
            ui.reloadRequested = false;
            if (mode == Mode::Menu && !ui.m3uUrl.empty()) {
                autoLoadList = false;             // se o carregamento automatico do inicio ainda nao rodou, este o substitui
                pf.pendingText = ui.m3uUrl; pf.hasPendingText = true; ui.textKind = 2;
                trace("lista-atualizar");
            }
        }
        if (pf.kbdRequested) {                    // X em "Digitar @streamer": abre o teclado na tela
            pf.kbdRequested = false;
            kbd.open(ui.textInitial(), ui.textKind == 2, ui.textKind == 3 ? 2 : (ui.textKind == 2 ? 1 : 0));
            mode = Mode::Kbd; dirty = true;
            continue;
        }
        if (pf.hasPendingText) {                  // texto confirmado no teclado: busca o canal ou baixa a lista M3U
            std::string typed = pf.pendingText; pf.pendingText.clear(); pf.hasPendingText = false;
            if (ui.textKind == 2 && loader.stillAlive() && !loader.cancel(4.0)) {
                app.say("A lista anterior ainda esta terminando - tente de novo em instantes");
                ui.textKind = 0; dirty = true; continue;
            }
            if (ui.textKind == 2) app.say(typed.empty() ? "Removendo a lista M3U..." : "Baixando a lista M3U...");
            else app.say("Buscando @" + kickSlug(typed) + " na Kick...");
            presentMenu();
            bool listLoad = ui.textKind == 2;
            if (listLoad) pf.saveSetting("bgload", typed.empty() ? "" : "1");     // download manual da lista tambem e protegido
            ui.onText(typed);                    // httpGet + (se ao vivo) play()
            if (listLoad && !typed.empty()) bgloadClearAt = nowSec() + 4.0;
            trace("texto-processado");
            dirty = true;
        }
        // Kick: resolve URL fresca antes de abrir o player
        if (!pf.pendingKickSlug.empty()) {
            std::string slug = pf.pendingKickSlug;
            pf.pendingKickSlug.clear();
            presentWait();
            std::string err, hls;
            for (int attempt = 0; attempt < 3 && hls.empty(); attempt++) {
                if (attempt) sceKernelUsleep(400000);
                err.clear();
                hls = app.resolveKickHls(slug, &err);
            }
            if (hls.empty()) {
                app.say("Reconexao falhou: " + (err.empty() ? "sem stream" : err));
                player.failed = true;
                player.log.clear();
                player.log.push_back("Reconexao @" + slug + " falhou: " + err);
                player.log.push_back("Aperte O e tente assistir de novo.");
                mode = Mode::Play; // mostra erro no cartao
                // força tela de erro minima
                for (int i = 0; i < 60; i++) {
                    presentWait();
                    sceKernelUsleep(100000);
                    pin.poll();
                    if (pin.take() & (B_CIRCLE | B_CROSS)) break;
                }
                mode = Mode::Menu; dirty = true; continue;
            }
            pf.pendingPlayUrl = hls;
            pf.pendingPlayTitle = "@" + slug;
            // cai no bloco abaixo
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
            g_sfx.suspend();                     // fecha a porta de som da interface: o audio do video fica com a porta so dele
            mode = Mode::Play; gotFrame = false; lastWaitDraw = 0;
            g_sys.reset();                       // abrir video pode acender o aviso do sistema por engano: so vale depois de 1,5 s calmo
            presentWait();                       // mostra "Player" antes de bloquear
            player.presentCb = presentWait;
            player.cancelCb = [&]() -> bool {        // O cancela o carregamento; PS tambem (para tudo na hora)
                if (sysWantsStop()) return true;
                pin.poll();
                if (pin.pending & B_CIRCLE) { pin.pending &= ~B_CIRCLE; return true; }
                return false;
            };
            trace("player-start");
            player.start(url, title);
            player.quietWait = false; player.waitLabel.clear();   // so vale para esta abertura
            if (player.cancelled) { player.cancelled = false; mode = Mode::Menu; dirty = true; trace("player-cancelado"); continue; }
            trace(player.failed ? "player-falhou" : "player-aberto");
            if (player.failed) {
                // mostra o erro e VOLTA ao menu (nao fica em Play com handle invalido)
                for (int i = 0; i < 1200; i++) {          // fica ate apertar O (ate 2 min) para dar tempo de ler/fotografar o erro
                    presentWait();
                    sceKernelUsleep(100000);
                    pin.poll();
                    if (pin.take() & (B_CIRCLE | B_CROSS)) break;
                }
                player.stop();
                autoMode = false;
                mode = Mode::Menu;
                dirty = true;
                continue;
            }
            // sucesso: toca normalmente (reconexao automatica so vale para live)
            autoMode = false;
            continue;
        }
        if (mode == Mode::Menu) {
            g_sfx.resume();
            // So redesenha quando ha algo a mostrar: animacao em andamento (~30 quadros/s), banner que passa sozinho,
            // ou dados que chegaram de threads (backdrop do banner). Tela parada = 0% de CPU.
            static double lastAnim = 0, lastPoll = 0;
            double tn = nowSec();
            if (!g_quitting && tn - lastPoll >= 0.25) { lastPoll = tn; if (ui.asyncPoll()) dirty = true; }
            lap("asyncPoll");
            if (!g_quitting && ui.needsFrame(tn) && tn - lastAnim >= 0.033) { lastAnim = tn; dirty = true; }
        }
        // limite de ~60 desenhos por segundo (nao enche a fila de flips do video)
        lap("antes-desenho");
        if (dirty && nowSec() - lastMenuPresent >= 0.0165) { presentMenu(); lap("desenho"); dirty = false; static bool once = false; if (!once) { once = true; trace("menu-ok"); } }
        sceKernelUsleep(4000);                     // 4 ms: o controle e lido ~200x por segundo (antes 16 ms)
    }
    // ---- encerramento limpo (OPTIONS, background ou saida normal) ----
    trace("saindo");
    g_quitting = true;
    // seguranca: se alguma thread nao largar a tempo, encerra de qualquer jeito em 2,5 s (o sistema nao espera mais que isso)
    try { const unsigned fu = sysExit ? 1500000u : 3500000u; std::thread([fu] { sceKernelUsleep(fu); ps4Exit("saida-forcada"); }).detach(); } catch (...) {}
    try { pf.saveSetting("bgload", ""); } catch (...) {}
    try { g_sfx.stop(); } catch (...) {}
    try { if (mode == Mode::Play || player.h) player.stop(); } catch (...) {}
    try { g_covers.setPaused(true); } catch (...) {}
    try { g_covers.cancelBackground(); } catch (...) {}
    try { if (loader.stillAlive()) loader.cancel(sysExit ? 0.1 : 0.4); } catch (...) {}
    try { g_feed.abort(); abortHf(); abortPf(); } catch (...) {}
    try { app.stopPrefetch(sysExit ? 0.1 : 0.3); } catch (...) {}          // thread do catalogo Xtream: tem que estar parada antes de sair
    try { app.stopHeroInfo(sysExit ? 0.1 : 0.3); } catch (...) {}
    Player::waitTeardown(sysExit ? 0.3 : 0.8);
    if (g_avClosedAt > 0) {                   // depois de fechar o AvPlayer, da tempo das threads internas dele (decodificador/audio) terminarem
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        double since = ts.tv_sec + ts.tv_nsec / 1e9 - g_avClosedAt;
        const double settle = sysExit ? 0.3 : 1.2;          // fechando pelo sistema: o PS4 tem pressa (matava o app aqui, com o erro CE-34878-0)
        if (since < settle) { trace("espera-avplayer-assentar"); sceKernelUsleep((unsigned)((settle - since) * 1e6)); }
    }
    if (g_tearing.load() == 0) unloadAvModules();
    try { if (!g_covers.shutdownFor(sysExit ? 0.3 : 1.0)) trace("fim-capas-timeout"); } catch (...) {}
    try { g_feed.join(); } catch (...) {}
    try { auto r = takeHf(); if (r) r->join(); } catch (...) {}
    try { auto r = takePf(); if (r) { r->join(); r.reset(); } } catch (...) {}
    try { removeVodFiles(); } catch (...) {}
    try { blit::pool().shutdown(); } catch (...) {}
    // fecha pad / video se ainda abertos
    // Fechado pelo menu do PS: NAO fecha controle nem saida de video (o sistema ja tomou a tela; fechar agora pode derrubar o app)
    if (!sysExit) {
        try { if (pin.h >= 0) scePadClose(pin.h); } catch (...) {}
        try { if (vh >= 0) sceVideoOutClose(vh); } catch (...) {}
    }
    // Sai PELO SISTEMA (nunca return/_Exit: CE-34878-0). Nao roda destrutores C++ de threads soltas (evita std::terminate).
    ps4Exit("fim");
}
