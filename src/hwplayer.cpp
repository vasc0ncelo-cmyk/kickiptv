// hwplayer.cpp — implementação do protótipo (fase 1)
#include "hwplayer.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

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
namespace {

vdec::Api g_api;

// Memoria CPU/GPU do decoder: o hardware exige alinhamento (>= 64 KB) e memoria que a GPU enxerga.
// Um std::vector comum nao serve. Pegamos memoria direta uma vez (e reaproveitamos nas proximas aberturas).
struct DirectBlock { void *ptr = nullptr; size_t size = 0; };
DirectBlock g_gpuBlock;
void *directAlloc(size_t size, size_t align) {
    if (g_gpuBlock.ptr && g_gpuBlock.size >= size) return g_gpuBlock.ptr;
    int64_t phys = 0; void *addr = nullptr;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size, align, 0, &phys) < 0) return nullptr;
    if (sceKernelMapDirectMemory(&addr, size, 0x33, 0, phys, align) < 0) return nullptr;
    g_gpuBlock.ptr = addr; g_gpuBlock.size = size;
    return addr;
}
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
        int q = g_api.queryResourceInfo(&cfg, &res_);
        char b[200];
        snprintf(b, sizeof b, "[codec=%u q=0x%08X cpu=%llu gpu=%llu fb=%llu al=%u] ", (unsigned)codec, (unsigned)q,
                 (unsigned long long)res_.cpuMemorySize, (unsigned long long)res_.cpuGpuMemorySize,
                 (unsigned long long)res_.maxFrameBufferSize, (unsigned)res_.frameBufferAlignment);
        diag += b;
        if (q != 0) continue;                          // sem os tamanhos reais nao adianta chutar: tenta o proximo codec

        size_t align = res_.frameBufferAlignment > 0x10000 ? res_.frameBufferAlignment : 0x10000;
        size_t cpuSz = roundUp((size_t)res_.cpuMemorySize, 0x10000);
        size_t gpuSz = roundUp((size_t)res_.cpuGpuMemorySize, 0x10000);
        cpuMem_.assign(cpuSz + align, 0);               // alinhado "na mao" dentro do vetor
        uintptr_t cp = ((uintptr_t)cpuMem_.data() + align - 1) & ~(uintptr_t)(align - 1);
        res_.pCpuMemory = (void *)cp;
        res_.pCpuGpuMemory = gpuSz ? directAlloc(gpuSz, align) : nullptr;
        if (gpuSz && !res_.pCpuGpuMemory) { diag += "[sem memoria direta p/ GPU] "; continue; }

        if (g_api.mapMemory) g_api.mapMemory();

        ctrl_ = {};
        ctrl_.thisSize = sizeof(ctrl_);
        int rc = g_api.createDecoder(&cfg, &res_, &ctrl_);
        if (rc != 0) {
            snprintf(b, sizeof b, "create=0x%08X ", (unsigned)rc);
            diag += b;
            continue;
        }

        created_ = true;
        size_t fbSize = res_.maxFrameBufferSize ? (size_t)res_.maxFrameBufferSize : (size_t)maxW * maxH * 3 / 2 + 65536;
        frameBuf_.assign(fbSize + align, 0);
        fbAligned_ = (uint8_t *)(((uintptr_t)frameBuf_.data() + align - 1) & ~(uintptr_t)(align - 1));
        fbSize_ = fbSize;
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

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err_ = "nao abriu arquivo: " + path;
        return false;
    }
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz <= 0 || sz > 512ll * 1024 * 1024) {
        err_ = "arquivo vazio ou grande demais para o prototipo";
        return false;
    }
    file_.resize((size_t)sz);
    f.read((char *)file_.data(), sz);
    if (!f) {
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

    int rc = g_api.decode(&ctrl_, &in, &fb, &pic);
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

void Player::close() {
    if (created_ && g_api.loaded && g_api.deleteDecoder) g_api.deleteDecoder(&ctrl_);   // nao depende do layout do campo "handle"
    created_ = false; fbAligned_ = nullptr; fbSize_ = 0;
    ctrl_ = {};
    file_.clear();
    cpuMem_.clear();
    cpuGpuMem_.clear();
    frameBuf_.clear();
    pos_ = 0;
    open_ = false;
}

} // namespace hwplayer
