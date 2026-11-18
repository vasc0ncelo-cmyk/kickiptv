// hwplayer.cpp — implementação do protótipo (fase 1)
#include "hwplayer.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Orbis / OpenOrbis
extern "C" {
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags,
                             void *opt, int *res);
int sceKernelStopUnloadModule(int handle, size_t args, const void *argp, uint32_t flags,
                              void *opt, int *res);
int sceKernelDlsym(int handle, const char *name, void **addr);
int sceKernelAllocateDirectMemory(int64_t searchStart, int64_t searchEnd, size_t length, size_t alignment, int type, int64_t *physOut);
int sceKernelMapDirectMemory(void **addr, size_t length, int prot, int flags, int64_t physStart, size_t alignment);
int64_t sceKernelGetDirectMemorySize(void);
}

namespace hwplayer {
void (*traceHook)(const char *) = nullptr;
namespace {
void T(const char *s) { if (traceHook) traceHook(s); }
const char *kGuardFile = "/data/kickiptv_hwguard";   // existe = a ultima tentativa de decodificar derrubou o app


vdec::Api g_api;

// Memoria CPU/GPU do decoder: o hardware exige alinhamento (>= 64 KB) e memoria que a GPU enxerga.
// Um std::vector comum nao serve. Pegamos memoria direta uma vez (e reaproveitamos nas proximas aberturas).
struct DirectBlock { void *ptr = nullptr; size_t size = 0; };
DirectBlock g_gpuBlock, g_fbBlock;
void *directAlloc(DirectBlock &blk, size_t size, size_t align) {
    if (blk.ptr && blk.size >= size) return blk.ptr;
    int64_t phys = 0; void *addr = nullptr;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, 0, &phys) < 0) return nullptr;
    if (sceKernelMapDirectMemory(&addr, size, 0x33, 0, phys, align) < 0) return nullptr;
    blk.ptr = addr; blk.size = size;
    return addr;
}
bool sane(uint64_t v, uint64_t max) { return v <= max; }
bool pow2(uint64_t v) { return v && !(v & (v - 1)); }
size_t roundUp(size_t v, size_t a) { return (v + a - 1) / a * a; }

bool resolveOne(int mod, const char *name, void **out) {
    *out = nullptr;
    if (sceKernelDlsym(mod, name, out) != 0 || !*out) return false;
    return true;
}

// Nomes de módulo usados em firmwares diferentes
const char *kModuleCandidates[] = {
    "/system/common/lib/libSceVideodec.sprx",
    "/system/common/lib/libSceVideodec2.sprx",
    "libSceVideodec.sprx",
    "libSceVideodec2.sprx",
    nullptr,
};

} // namespace

bool Player::tryLoadApi() {
    if (g_api.loaded) return true;

    for (int i = 0; kModuleCandidates[i]; i++) {
        int mod = -1;
        int res = 0;
        { char b[160]; snprintf(b, sizeof b, "hw-loadmodule %s", kModuleCandidates[i]); T(b); }
        int rc = sceKernelLoadStartModule(kModuleCandidates[i], 0, nullptr, 0, nullptr, &res);
        if (rc < 0) continue;
        mod = rc;

        void *p = nullptr;
        bool ok = true;
        ok = resolveOne(mod, "sceVideodecQueryResourceInfo", &p) && (g_api.queryResourceInfo = (vdec::FnQueryResourceInfo)p, true);
        ok = ok && resolveOne(mod, "sceVideodecCreateDecoder", &p) && (g_api.createDecoder = (vdec::FnCreateDecoder)p, true);
        ok = ok && resolveOne(mod, "sceVideodecDecode", &p) && (g_api.decode = (vdec::FnDecode)p, true);
        ok = ok && resolveOne(mod, "sceVideodecDeleteDecoder", &p) && (g_api.deleteDecoder = (vdec::FnDeleteDecoder)p, true);

        // opcionais
        if (resolveOne(mod, "sceVideodecFlush", &p)) g_api.flush = (vdec::FnFlush)p;
        if (resolveOne(mod, "sceVideodecReset", &p)) g_api.reset = (vdec::FnReset)p;
        if (resolveOne(mod, "sceVideodecMapMemory", &p)) g_api.mapMemory = (vdec::FnMapMemory)p;

        if (!ok) {
            sceKernelStopUnloadModule(mod, 0, nullptr, 0, nullptr, nullptr);
            g_api = vdec::Api{};
            continue;
        }

        T("hw-simbolos-ok");
        g_api.module = mod;
        g_api.loaded = true;
        return true;
    }

    // Tenta símbolos já linkados (caso o stub exista no toolchain do usuário)
    // — se não existirem, o linker do protótipo não os referencia de forma hard.
    return false;
}

bool Player::hwAvailable() { return g_api.loaded; }

size_t Player::findStartCode(const uint8_t *p, size_t n, size_t from, size_t *scLen) {
    for (size_t i = from; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) {
                if (scLen) *scLen = 3;
                return i;
            }
            if (i + 4 <= n && p[i + 2] == 0 && p[i + 3] == 1) {
                if (scLen) *scLen = 4;
                return i;
            }
        }
    }
    return n;
}

bool Player::nextAccessUnit(std::vector<uint8_t> &au) {
    au.clear();
    if (pos_ >= file_.size()) return false;

    size_t sc0 = 0;
    size_t pos = findStartCode(file_.data(), file_.size(), pos_, &sc0);
    if (pos >= file_.size()) return false;

    size_t nalStart = pos + sc0;
    size_t sc1 = 0;
    size_t next = findStartCode(file_.data(), file_.size(), nalStart, &sc1);
    size_t nalEnd = next;

    // Junta NALs de um frame: SPS/PPS + slice (heurística simples).
    // NAL type = byte & 0x1F. 1-5 = VCL (slice). 7=SPS 8=PPS 9=AUD.
    auto nalType = [&](size_t off) -> int {
        if (off >= file_.size()) return -1;
        return file_[off] & 0x1F;
    };

    // Se é SPS/PPS, inclui o próximo VCL no mesmo AU (muitos decoders gostam assim no 1º frame).
    int t = nalType(nalStart);
    if (t == 7 || t == 8) {
        while (nalEnd < file_.size()) {
            size_t scx = 0;
            size_t n2 = findStartCode(file_.data(), file_.size(), nalEnd, &scx);
            if (n2 >= file_.size()) break;
            int t2 = nalType(n2 + scx);
            nalEnd = n2;
            if (t2 >= 1 && t2 <= 5) {
                size_t scy = 0;
                size_t n3 = findStartCode(file_.data(), file_.size(), n2 + scx, &scy);
                nalEnd = n3;
                break;
            }
            if (t2 != 7 && t2 != 8 && t2 != 9 && t2 != 6) break;
            // continua agregando headers
            nalEnd = n2 + scx; // will re-loop - fix:
            // advance properly
            size_t after = n2 + scx;
            size_t scz = 0;
            size_t nx = findStartCode(file_.data(), file_.size(), after, &scz);
            nalEnd = nx;
            if (t2 >= 1 && t2 <= 5) break;
        }
    }

    au.assign(file_.begin() + pos, file_.begin() + nalEnd);
    pos_ = nalEnd;
    return !au.empty();
}

bool Player::initDecoder(int maxW, int maxH) {
    if (!g_api.loaded) {
        err_ = "libSceVideodec nao carregou (sem HW neste firmware/toolchain)";
        return false;
    }

    // Tenta cada codecType candidato (ver videodec.hpp). Guarda o diagnostico de cada tentativa.
    // Protecao contra queda repetida: se a tentativa anterior derrubou o app (marcador ainda no disco), nao tenta de novo.
    if (FILE *g = fopen(kGuardFile, "rb")) {
        fclose(g);
        remove(kGuardFile);        // so bloqueia UMA vez: o aviso aparece, e a proxima tentativa ja roda de novo
        err_ = "o decoder nativo derrubou o app na ultima tentativa (veja a etapa no menu inicial). Tente de novo para repetir.";
        T("hw-guard-bloqueou");
        return false;
    }
    static const uint32_t kCodecs[] = {vdec::kCodecAvc, vdec::kCodecAvcAlt};
    std::string diag;
    for (uint32_t codec : kCodecs) {
        vdec::ConfigInfo cfg{};
        cfg.thisSize = sizeof(cfg);
        cfg.codecType = codec;
        cfg.profile = 100;
        cfg.maxLevel = 51;
        cfg.maxFrameWidth = maxW;
        cfg.maxFrameHeight = maxH;
        cfg.maxDpbFrameCount = 6;
        cfg.videodecFlags = 0;

        res_ = {};
        res_.thisSize = sizeof(res_);
        T("hw-query");
        int q = g_api.queryResourceInfo(&cfg, &res_);
        char b[200];
        snprintf(b, sizeof b, "[codec=%u q=0x%08X cpu=%llu gpu=%llu fb=%llu al=%u] ", (unsigned)codec, (unsigned)q,
                 (unsigned long long)res_.cpuMemorySize, (unsigned long long)res_.cpuGpuMemorySize,
                 (unsigned long long)res_.maxFrameBufferSize, (unsigned)res_.frameBufferAlignment);
        diag += b;
        T(b);
        if (q != 0) continue;                          // sem os tamanhos reais nao adianta chutar: tenta o proximo codec
        // Layout errado de ResourceInfo = lixo nos tamanhos. Nao aloca nem chama o decoder com isso.
        if (!sane(res_.cpuMemorySize, 256ull << 20) || !sane(res_.cpuGpuMemorySize, 512ull << 20) || !sane(res_.maxFrameBufferSize, 64ull << 20) ||
            (res_.frameBufferAlignment && (!pow2(res_.frameBufferAlignment) || res_.frameBufferAlignment > (16u << 20)))) {
            diag += "[tamanhos absurdos: layout de ResourceInfo nao bate com o da biblioteca] ";
            T("hw-resinfo-absurdo");
            continue;
        }

        size_t align = res_.frameBufferAlignment > 0x10000 ? res_.frameBufferAlignment : 0x10000;
        size_t cpuSz = roundUp((size_t)res_.cpuMemorySize, 0x10000);
        size_t gpuSz = roundUp((size_t)res_.cpuGpuMemorySize, 0x10000);
        T("hw-alloc");
        cpuMem_.assign(cpuSz + align, 0);               // alinhado "na mao" dentro do vetor
        uintptr_t cp = ((uintptr_t)cpuMem_.data() + align - 1) & ~(uintptr_t)(align - 1);
        res_.pCpuMemory = (void *)cp;
        res_.pCpuGpuMemory = gpuSz ? directAlloc(g_gpuBlock, gpuSz, align) : nullptr;
        if (gpuSz && !res_.pCpuGpuMemory) { diag += "[sem memoria direta p/ GPU] "; continue; }

        // Frame buffer: o hardware escreve o NV12 nele, entao tambem precisa ser memoria direta (visivel para a GPU).
        size_t fbSize = res_.maxFrameBufferSize ? (size_t)res_.maxFrameBufferSize : (size_t)maxW * maxH * 3 / 2 + 65536;
        fbSize = roundUp(fbSize, 0x10000);
        fbAligned_ = (uint8_t *)directAlloc(g_fbBlock, fbSize, align);
        if (!fbAligned_) { diag += "[sem memoria direta p/ frame buffer] "; continue; }
        fbSize_ = fbSize;

        if (g_api.mapMemory) { T("hw-mapmemory"); g_api.mapMemory(); }

        ctrl_ = {};
        ctrl_.thisSize = sizeof(ctrl_);
        if (FILE *g = fopen(kGuardFile, "wb")) { fwrite("x", 1, 1, g); fclose(g); }   // apaga depois do 1o decode bem-sucedido
        T("hw-create");
        int rc = g_api.createDecoder(&cfg, &res_, &ctrl_);
        if (rc != 0) {
            remove(kGuardFile);
            snprintf(b, sizeof b, "create=0x%08X ", (unsigned)rc);
            diag += b;
            T(b);
            continue;
        }

        T("hw-create-ok");
        created_ = true;
        codecUsed_ = codec;
        return true;
    }
    err_ = "sceVideodecCreateDecoder falhou " + diag;
    return false;
}

bool Player::openFile(const std::string &path, int maxW, int maxH) {
    close();
    err_.clear();

    if (!tryLoadApi()) {
        err_ = "HW decoder indisponivel — use AvPlayer";
        return false;
    }

    // stdio em vez de std::ifstream: o libc++ da toolchain do GMCA (musl) nao traz <fstream> completo
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        err_ = "nao abriu arquivo: " + path;
        return false;
    }
    fseek(f, 0, SEEK_END);
    long long sz = (long long)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512ll * 1024 * 1024) {
        fclose(f);
        err_ = "arquivo vazio ou grande demais para o prototipo";
        return false;
    }
    file_.resize((size_t)sz);
    size_t got = fread((char *)file_.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        err_ = "falha ao ler arquivo";
        file_.clear();
        return false;
    }

    // precisa ter start code Annex-B
    size_t sc = 0;
    if (findStartCode(file_.data(), file_.size(), 0, &sc) >= file_.size()) {
        err_ = "nao e H.264 Annex-B (sem start code). Converta com: ffmpeg -i in.mp4 -c copy -bsf h264_mp4toannexb out.h264";
        file_.clear();
        return false;
    }

    if (!initDecoder(maxW, maxH)) {
        file_.clear();
        return false;
    }

    pos_ = 0;
    ptsTick_ = 0;
    open_ = true;
    return true;
}

bool Player::open(int maxW, int maxH) {
    close();
    err_.clear();
    if (!tryLoadApi() || !g_api.loaded) {
        err_ = "HW decoder indisponivel";
        return false;
    }
    if (!initDecoder(maxW, maxH)) return false;
    pos_ = 0;
    ptsTick_ = 0;
    open_ = true;
    return true;
}

bool Player::feedAu(const uint8_t *data, size_t size, Frame &out) {
    out = Frame{};
    if (!open_ || !g_api.loaded || !data || size < 4) return false;

    vdec::InputData in{};
    in.thisSize = sizeof(in);
    in.pAuData = (uint8_t *)data;
    in.auSize = size;
    in.ptsData = ptsTick_++;
    in.dtsData = in.ptsData;
    in.attachedData = 0;

    vdec::FrameBuffer fb{};
    fb.thisSize = sizeof(fb);
    fb.pFrameBuffer = fbAligned_;
    fb.frameBufferSize = fbSize_;

    vdec::PictureInfo pic{};
    pic.thisSize = sizeof(pic);

    if (decodes_ < 3) { char b[64]; snprintf(b, sizeof b, "hw-decode-%d au=%u", decodes_, (unsigned)size); T(b); }
    int rc = g_api.decode(&ctrl_, &in, &fb, &pic);
    if (decodes_ < 3) { char b[64]; snprintf(b, sizeof b, "hw-decode-ret rc=0x%08X valid=%u", (unsigned)rc, (unsigned)pic.isValid); T(b); }
    decodes_++;
    if (rc == 0) remove(kGuardFile);                 // sobreviveu ao 1o decode: nao e mais suspeito de derrubar o app
    if (rc != 0 || !pic.isValid) return false;

    out.nv12 = (const uint8_t *)fb.pFrameBuffer;
    out.width = (int)pic.frameWidth;
    out.height = (int)pic.frameHeight;
    out.pitch = (int)(pic.framePitch ? pic.framePitch : pic.frameWidth);
    out.yRows = (int)pic.frameHeight;
    out.ptsMs = pic.ptsData;
    out.valid = true;
    return true;
}

bool Player::pump(Frame &out) {
    out = Frame{};
    if (!open_ || !g_api.loaded) return false;

    std::vector<uint8_t> au;
    if (!nextAccessUnit(au)) {
        err_ = "fim do arquivo";
        return false;
    }

    vdec::InputData in{};
    in.thisSize = sizeof(in);
    in.pAuData = au.data();
    in.auSize = au.size();
    in.ptsData = ptsTick_++;
    in.dtsData = in.ptsData;
    in.attachedData = 0;

    vdec::FrameBuffer fb{};
    fb.thisSize = sizeof(fb);
    fb.pFrameBuffer = fbAligned_;
    fb.frameBufferSize = fbSize_;

    vdec::PictureInfo pic{};
    pic.thisSize = sizeof(pic);

    int rc = g_api.decode(&ctrl_, &in, &fb, &pic);
    if (rc != 0 || !pic.isValid) {
        // alguns frames (só SPS) não produzem picture — não é erro fatal
        return false;
    }

    out.nv12 = (const uint8_t *)fb.pFrameBuffer;
    out.width = (int)pic.frameWidth;
    out.height = (int)pic.frameHeight;
    out.pitch = (int)(pic.framePitch ? pic.framePitch : pic.frameWidth);
    out.yRows = (int)pic.frameHeight;
    out.ptsMs = pic.ptsData;
    out.valid = true;
    return true;
}

// Descarrega os modulos do AvPlayer (AvPlayer 0xA5 e AvPlayerStreaming 0x80000050) procurando as funcoes de libSceSysmodule em tempo
// de execucao (o stub do toolchain nao as exporta). -1 = nao achou/nao chamou; senao o retorno da funcao do sistema.
void unloadSysmodules(int *rcInternal, int *rcAv) {
    *rcInternal = -1; *rcAv = -1;
    static const char *paths[] = {"/system/common/lib/libSceSysmodule.sprx", "libSceSysmodule.sprx", nullptr};
    for (int i = 0; paths[i]; i++) {
        int res = 0;
        int mod = sceKernelLoadStartModule(paths[i], 0, nullptr, 0, nullptr, &res);
        { char b[120]; snprintf(b, sizeof b, "hw-sysmodule %s mod=%d", paths[i], mod); T(b); }
        if (mod < 0) continue;
        void *p = nullptr;
        if (resolveOne(mod, "sceSysmoduleUnloadModuleInternal", &p)) *rcInternal = ((int (*)(uint32_t))p)(0x80000050u);
        p = nullptr;
        if (resolveOne(mod, "sceSysmoduleUnloadModule", &p)) *rcAv = ((int (*)(uint16_t))p)((uint16_t)0x00A5);
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// Sonda de diagnostico (so roda se existir /data/kickiptv_x_decprobe). Nao decodifica nada: so descobre o que este PS4 oferece.
// ---------------------------------------------------------------------------------------------------------------------------
namespace {
struct ProbeLog {
    FILE *f = nullptr;
    void line(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (!f) return;
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fflush(f);
    }
};
}

void runProbe(const char *path) {
    ProbeLog L; L.f = fopen(path, "w");
    if (!L.f) return;
    const char *guard = "/data/kickiptv_probe_guard";
    bool crashedBefore = false;
    { FILE *g = fopen(guard, "rb"); if (g) { crashedBefore = true; fclose(g); } }
    L.line("kickiptv decprobe v1");
    if (crashedBefore) L.line("AVISO: a sonda anterior nao terminou (o app caiu). Passos de risco serao pulados.");
    { FILE *g = fopen(guard, "wb"); if (g) { fputs("probe", g); fclose(g); } }

    struct Mod { const char *name; int h; };
    Mod mods[] = {{"libSceVideodec.sprx", -1}, {"libSceVideodec2.sprx", -1}, {"libSceAudiodec.sprx", -1}, {"libSceAjm.sprx", -1}, {"libSceAvPlayer.sprx", -1}};
    static const char *dirs[] = {"/system/common/lib/", "/system/priv/lib/", ""};
    for (auto &m : mods) {
        for (const char *d : dirs) {
            char full[160]; snprintf(full, sizeof full, "%s%s", d, m.name);
            int res = 0;
            int rc = sceKernelLoadStartModule(full, 0, nullptr, 0, nullptr, &res);
            L.line("modulo %-40s rc=0x%08x res=%d", full, (unsigned)rc, res);
            if (rc >= 0) { m.h = rc; break; }
        }
    }
    struct Syms { int mod; const char *const *names; };
    static const char *kVideodec[] = {"sceVideodecQueryResourceInfo", "sceVideodecCreateDecoder", "sceVideodecDecode", "sceVideodecFlush", "sceVideodecReset", "sceVideodecDeleteDecoder", "sceVideodecMapMemory", "sceVideodecInitLibrary", "sceVideodecTermLibrary", nullptr};
    static const char *kVideodec2[] = {"sceVideodec2QueryComputeMemoryInfo", "sceVideodec2AllocateComputeQueue", "sceVideodec2ReleaseComputeQueue", "sceVideodec2QueryDecoderMemoryInfo", "sceVideodec2CreateDecoder", "sceVideodec2DeleteDecoder", "sceVideodec2Decode", "sceVideodec2Flush", "sceVideodec2Reset", "sceVideodec2GetPictureInfo", nullptr};
    static const char *kAudiodec[] = {"sceAudiodecInitLibrary", "sceAudiodecTermLibrary", "sceAudiodecCreateDecoder", "sceAudiodecDecode", "sceAudiodecDeleteDecoder", "sceAudiodecClearContext", "sceAudiodecDecode2", nullptr};
    static const char *kAjm[] = {"sceAjmInitialize", "sceAjmFinalize", "sceAjmModuleRegister", "sceAjmModuleUnregister", "sceAjmInstanceCreate", "sceAjmInstanceDestroy", "sceAjmBatchInitialize", "sceAjmBatchStart", "sceAjmBatchWait", "sceAjmBatchJobRun", "sceAjmBatchJobInlineBuffer", "sceAjmBatchJobInputBuffer", "sceAjmBatchJobOutputBuffer", "sceAjmBatchErrorDump", nullptr};
    Syms table[] = {{mods[0].h, kVideodec}, {mods[1].h, kVideodec2}, {mods[2].h, kAudiodec}, {mods[3].h, kAjm}};
    void *fnVideoQuery = nullptr, *fnAudInit = nullptr, *fnAudTerm = nullptr;
    for (auto &t : table) {
        if (t.mod < 0) continue;
        for (int i = 0; t.names[i]; i++) {
            void *p = nullptr; bool ok = resolveOne(t.mod, t.names[i], &p);
            L.line("simbolo %-40s %s", t.names[i], ok ? "OK" : "nao existe");
            if (ok && !strcmp(t.names[i], "sceVideodecQueryResourceInfo")) fnVideoQuery = p;
            if (ok && !strcmp(t.names[i], "sceAudiodecInitLibrary")) fnAudInit = p;
            if (ok && !strcmp(t.names[i], "sceAudiodecTermLibrary")) fnAudTerm = p;
        }
    }
    if (crashedBefore) { L.line("fim (passos de risco pulados)"); fclose(L.f); return; }

    // Audio: so inicializa a biblioteca para cada codec (1 = AT9, 2 = MP3, 3 = AAC) e devolve.
    if (fnAudInit) for (uint32_t codec = 1; codec <= 3; codec++) {
        L.line("audiodec InitLibrary(%u)...", codec);
        int rc = ((int (*)(uint32_t))fnAudInit)(codec);
        L.line("audiodec InitLibrary(%u) rc=0x%08x", codec, (unsigned)rc);
        if (rc >= 0 && fnAudTerm) { int rt = ((int (*)(uint32_t))fnAudTerm)(codec); L.line("audiodec TermLibrary(%u) rc=0x%08x", codec, (unsigned)rt); }
    }

    // Video: QueryResourceInfo (so consulta, nao cria decoder) com varios codecType e tamanhos de estrutura.
    if (fnVideoQuery) {
        const uint64_t cfgSizes[] = {sizeof(vdec::ConfigInfo), 0x30, 0x38, 0x40};
        const uint64_t resSizes[] = {sizeof(vdec::ResourceInfo), 0x38, 0x40};
        L.line("sizeof ConfigInfo=%zu ResourceInfo=%zu", sizeof(vdec::ConfigInfo), sizeof(vdec::ResourceInfo));
        for (uint32_t codec = 0; codec <= 3; codec++) for (uint64_t cs : cfgSizes) for (uint64_t rs : resSizes) {
            vdec::ConfigInfo c{}; vdec::ResourceInfo r{};
            c.thisSize = cs; c.codecType = codec; c.profile = 100; c.maxLevel = 41; c.maxFrameWidth = 1920; c.maxFrameHeight = 1080; c.maxDpbFrameCount = 16;
            r.thisSize = rs;
            L.line("videodec Query codec=%u cfgSize=0x%llx resSize=0x%llx ...", codec, (unsigned long long)cs, (unsigned long long)rs);
            int rc = ((vdec::FnQueryResourceInfo)fnVideoQuery)(&c, &r);
            if (rc >= 0) L.line("  rc=0x%08x cpuMem=%llu cpuGpuMem=%llu fbSize=%llu fbAlign=%u", (unsigned)rc, (unsigned long long)r.cpuMemorySize, (unsigned long long)r.cpuGpuMemorySize, (unsigned long long)r.maxFrameBufferSize, r.frameBufferAlignment);
            else L.line("  rc=0x%08x", (unsigned)rc);
        }
    }
    L.line("fim");
    fclose(L.f);
    remove(guard);
}

void Player::close() {
    if (created_ && g_api.loaded && g_api.deleteDecoder) g_api.deleteDecoder(&ctrl_);   // nao depende do layout do campo "handle"
    created_ = false; fbAligned_ = nullptr; fbSize_ = 0; decodes_ = 0;
    ctrl_ = {};
    file_.clear();
    cpuMem_.clear();
    cpuGpuMem_.clear();
    pos_ = 0;
    open_ = false;
}

} // namespace hwplayer
