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
}

namespace hwplayer {
namespace {

vdec::Api g_api;

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

    vdec::ConfigInfo cfg{};
    cfg.thisSize = sizeof(cfg);
    cfg.codecType = vdec::kCodecAvc;
    cfg.profile = 100;
    cfg.maxLevel = 51;
    cfg.maxFrameWidth = maxW;
    cfg.maxFrameHeight = maxH;
    cfg.maxDpbFrameCount = 6;
    cfg.videodecFlags = 0;

    res_ = {};
    res_.thisSize = sizeof(res_);
    int q = g_api.queryResourceInfo(&cfg, &res_);
    if (q != 0) {
        // fallback: tamanhos razoáveis se Query falhar
        res_.cpuMemorySize = 8ull * 1024 * 1024;
        res_.cpuGpuMemorySize = 32ull * 1024 * 1024;
        res_.maxFrameBufferSize = (uint64_t)maxW * maxH * 3 / 2 + 65536;
        res_.frameBufferAlignment = 0x10000;
    }

    cpuMem_.assign((size_t)res_.cpuMemorySize, 0);
    cpuGpuMem_.assign((size_t)res_.cpuGpuMemorySize, 0);
    res_.pCpuMemory = cpuMem_.data();
    res_.pCpuGpuMemory = cpuGpuMem_.data();

    if (g_api.mapMemory) g_api.mapMemory();

    ctrl_ = {};
    ctrl_.thisSize = sizeof(ctrl_);
    int rc = g_api.createDecoder(&cfg, &res_, &ctrl_);
    if (rc != 0) {
        char b[96];
        snprintf(b, sizeof b, "sceVideodecCreateDecoder falhou: 0x%08X", (unsigned)rc);
        err_ = b;
        return false;
    }

    frameBuf_.assign((size_t)(res_.maxFrameBufferSize ? res_.maxFrameBufferSize
                                                      : (size_t)maxW * maxH * 3 / 2 + 65536),
                     0);
    return true;
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
    fb.pFrameBuffer = frameBuf_.data();
    fb.frameBufferSize = frameBuf_.size();

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
    if (open_ && g_api.loaded && g_api.deleteDecoder && ctrl_.handle) {
        g_api.deleteDecoder(&ctrl_);
    }
    ctrl_ = {};
    file_.clear();
    cpuMem_.clear();
    cpuGpuMem_.clear();
    frameBuf_.clear();
    pos_ = 0;
    open_ = false;
}

} // namespace hwplayer
