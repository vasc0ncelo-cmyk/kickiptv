// videodec.hpp — tipos de libSceVideodec (PS4) baseados no reverso do shadPS4.
// Uso: carregar o módulo em runtime (sceKernelLoadStartModule + sceKernelDlsym).
// NÃO depende de stubs oficiais do OpenOrbis (ainda não existem de forma estável).
#pragma once

#include <cstdint>
#include <cstddef>

namespace vdec {

// codecType: 1 = AVC (H.264) — o mais comum em IPTV/VOD
enum : uint32_t {
    kCodecAvc = 1,
};

struct ConfigInfo {
    uint64_t thisSize;
    uint32_t codecType;
    uint32_t profile;          // 100 = High
    uint32_t maxLevel;         // 41 = 4.1 (1080p), 51 = 5.1
    int32_t  maxFrameWidth;
    int32_t  maxFrameHeight;
    int32_t  maxDpbFrameCount;
    uint64_t videodecFlags;
};

struct ResourceInfo {
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void    *pCpuMemory;
    uint64_t cpuGpuMemorySize;
    void    *pCpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
};

struct Ctrl {
    uint64_t thisSize;
    void    *handle;
    uint64_t version;
};

struct FrameBuffer {
    uint64_t thisSize;
    void    *pFrameBuffer;
    uint64_t frameBufferSize;
};

struct AvcInfo {
    uint32_t numUnitsInTick;
    uint32_t timeScale;
    uint8_t  fixedFrameRateFlag;
    uint8_t  aspectRatioIdc;
    uint16_t sarWidth;
    uint16_t sarHeight;
    uint8_t  colourPrimaries;
    uint8_t  transferCharacteristics;
    uint8_t  matrixCoefficients;
    uint8_t  videoFullRangeFlag;
    uint32_t frameCropLeftOffset;
    uint32_t frameCropRightOffset;
    uint32_t frameCropTopOffset;
    uint32_t frameCropBottomOffset;
};

union CodecInfo {
    uint8_t reserved[64];
    AvcInfo avc;
};

struct PictureInfo {
    uint64_t thisSize;
    uint32_t isValid;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    uint32_t isErrorPic;
    uint64_t ptsData;
    uint64_t attachedData;
    CodecInfo codec;
};

struct InputData {
    uint64_t thisSize;
    void    *pAuData;   // um access unit (NAL ou conjunto de NALs de um frame)
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
};

// Ponteiros resolvidos em runtime
using FnQueryResourceInfo = int (*)(const ConfigInfo *, ResourceInfo *);
using FnCreateDecoder     = int (*)(const ConfigInfo *, const ResourceInfo *, Ctrl *);
using FnDecode            = int (*)(Ctrl *, const InputData *, FrameBuffer *, PictureInfo *);
using FnFlush             = int (*)(Ctrl *, FrameBuffer *, PictureInfo *);
using FnDeleteDecoder     = int (*)(Ctrl *);
using FnReset             = int (*)(Ctrl *);
using FnMapMemory         = int (*)();

struct Api {
    bool loaded = false;
    int  module = -1;
    FnQueryResourceInfo queryResourceInfo = nullptr;
    FnCreateDecoder     createDecoder     = nullptr;
    FnDecode            decode            = nullptr;
    FnFlush             flush             = nullptr;
    FnDeleteDecoder     deleteDecoder     = nullptr;
    FnReset             reset             = nullptr;
    FnMapMemory         mapMemory         = nullptr;
};

} // namespace vdec
