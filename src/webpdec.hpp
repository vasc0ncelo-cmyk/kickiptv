// webpdec.hpp - self-contained, dependency-free WebP decoder (C++17, single header).
//
// Supported:
//   * RIFF/WEBP container, simple ('VP8 ' / 'VP8L') and extended (VP8X) layouts.
//   * Lossy VP8 key frames (RFC 6386): boolean decoder, segmentation, per-segment
//     quantizers, multiple token partitions, intra 16x16 / 4x4 / chroma prediction,
//     inverse WHT + DCT, normal and simple loop filter. YUV420 -> RGBA uses libwebp's
//     fixed-point formulas and its "fancy" (bilinear) chroma upsampler; output is
//     meant to be bit-identical to libwebp / Pillow.
//   * ALPH chunk: raw and lossless-compressed (VP8L) alpha, with all four prediction
//     filters (none / horizontal / vertical / gradient).
//   * Lossless VP8L images (also used for compressed alpha).
// Not supported: animation (VP8X animation flag -> decode fails), VP8 scaling bits and
// cropping are ignored, ICC/EXIF are skipped.
//
// Robustness: every read is bounds checked, no exceptions are thrown by this code (only
// std::vector allocation can fail), loops are bounded by the image size / input size.
// Images with w*h > 6,000,000 pixels are refused.
//
// API (global namespace):
//   bool webpIsWebp(const uint8_t* d, size_t n);
//   bool webpInfo(const uint8_t* d, size_t n, int& w, int& h);
//   bool webpDecode(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint8_t>& rgba);
//
// Alpha handling note: a damaged/unsupported ALPH chunk does not fail the decode, the image
// is then returned fully opaque.
#ifndef WEBPDEC_HPP_INCLUDED
#define WEBPDEC_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace webpdec_impl {

constexpr int64_t kMaxPixels = 6000000;

// ---------------------------------------------------------------------------------------
// Constant tables (VP8 default probabilities / quantizer tables, RFC 6386)
// ---------------------------------------------------------------------------------------
inline constexpr uint8_t kCoeffsProba0[1056] = {
  128,128,128,128,128,128,128,128,128,128,128,
  128,128,128,128,128,128,128,128,128,128,128,
  128,128,128,128,128,128,128,128,128,128,128,
  253,136,254,255,228,219,128,128,128,128,128,
  189,129,242,255,227,213,255,219,128,128,128,
  106,126,227,252,214,209,255,255,128,128,128,
  1,98,248,255,236,226,255,255,128,128,128,
  181,133,238,254,221,234,255,154,128,128,128,
  78,134,202,247,198,180,255,219,128,128,128,
  1,185,249,255,243,255,128,128,128,128,128,
  184,150,247,255,236,224,128,128,128,128,128,
  77,110,216,255,236,230,128,128,128,128,128,
  1,101,251,255,241,255,128,128,128,128,128,
  170,139,241,252,236,209,255,255,128,128,128,
  37,116,196,243,228,255,255,255,128,128,128,
  1,204,254,255,245,255,128,128,128,128,128,
  207,160,250,255,238,128,128,128,128,128,128,
  102,103,231,255,211,171,128,128,128,128,128,
  1,152,252,255,240,255,128,128,128,128,128,
  177,135,243,255,234,225,128,128,128,128,128,
  80,129,211,255,194,224,128,128,128,128,128,
  1,1,255,128,128,128,128,128,128,128,128,
  246,1,255,128,128,128,128,128,128,128,128,
  255,128,128,128,128,128,128,128,128,128,128,
  198,35,237,223,193,187,162,160,145,155,62,
  131,45,198,221,172,176,220,157,252,221,1,
  68,47,146,208,149,167,221,162,255,223,128,
  1,149,241,255,221,224,255,255,128,128,128,
  184,141,234,253,222,220,255,199,128,128,128,
  81,99,181,242,176,190,249,202,255,255,128,
  1,129,232,253,214,197,242,196,255,255,128,
  99,121,210,250,201,198,255,202,128,128,128,
  23,91,163,242,170,187,247,210,255,255,128,
  1,200,246,255,234,255,128,128,128,128,128,
  109,178,241,255,231,245,255,255,128,128,128,
  44,130,201,253,205,192,255,255,128,128,128,
  1,132,239,251,219,209,255,165,128,128,128,
  94,136,225,251,218,190,255,255,128,128,128,
  22,100,174,245,186,161,255,199,128,128,128,
  1,182,249,255,232,235,128,128,128,128,128,
  124,143,241,255,227,234,128,128,128,128,128,
  35,77,181,251,193,211,255,205,128,128,128,
  1,157,247,255,236,231,255,255,128,128,128,
  121,141,235,255,225,227,255,255,128,128,128,
  45,99,188,251,195,217,255,224,128,128,128,
  1,1,251,255,213,255,128,128,128,128,128,
  203,1,248,255,255,128,128,128,128,128,128,
  137,1,177,255,224,255,128,128,128,128,128,
  253,9,248,251,207,208,255,192,128,128,128,
  175,13,224,243,193,185,249,198,255,255,128,
  73,17,171,221,161,179,236,167,255,234,128,
  1,95,247,253,212,183,255,255,128,128,128,
  239,90,244,250,211,209,255,255,128,128,128,
  155,77,195,248,188,195,255,255,128,128,128,
  1,24,239,251,218,219,255,205,128,128,128,
  201,51,219,255,196,186,128,128,128,128,128,
  69,46,190,239,201,218,255,228,128,128,128,
  1,191,251,255,255,128,128,128,128,128,128,
  223,165,249,255,213,255,128,128,128,128,128,
  141,124,248,255,255,128,128,128,128,128,128,
  1,16,248,255,255,128,128,128,128,128,128,
  190,36,230,255,236,255,128,128,128,128,128,
  149,1,255,128,128,128,128,128,128,128,128,
  1,226,255,128,128,128,128,128,128,128,128,
  247,192,255,128,128,128,128,128,128,128,128,
  240,128,255,128,128,128,128,128,128,128,128,
  1,134,252,255,255,128,128,128,128,128,128,
  213,62,250,255,255,128,128,128,128,128,128,
  55,93,255,128,128,128,128,128,128,128,128,
  128,128,128,128,128,128,128,128,128,128,128,
  128,128,128,128,128,128,128,128,128,128,128,
  128,128,128,128,128,128,128,128,128,128,128,
  202,24,213,235,186,191,220,160,240,175,255,
  126,38,182,232,169,184,228,174,255,187,128,
  61,46,138,219,151,178,240,170,255,216,128,
  1,112,230,250,199,191,247,159,255,255,128,
  166,109,228,252,211,215,255,174,128,128,128,
  39,77,162,232,172,180,245,178,255,255,128,
  1,52,220,246,198,199,249,220,255,255,128,
  124,74,191,243,183,193,250,221,255,255,128,
  24,71,130,219,154,170,243,182,255,255,128,
  1,182,225,249,219,240,255,224,128,128,128,
  149,150,226,252,216,205,255,171,128,128,128,
  28,108,170,242,183,194,254,223,255,255,128,
  1,81,230,252,204,203,255,192,128,128,128,
  123,102,209,247,188,196,255,233,128,128,128,
  20,95,153,243,164,173,255,203,128,128,128,
  1,222,248,255,216,213,128,128,128,128,128,
  168,175,246,252,235,205,255,255,128,128,128,
  47,116,215,255,211,212,255,255,128,128,128,
  1,121,236,253,212,214,255,255,128,128,128,
  141,84,213,252,201,202,255,219,128,128,128,
  42,80,160,240,162,185,255,205,128,128,128,
  1,1,255,128,128,128,128,128,128,128,128,
  244,1,255,128,128,128,128,128,128,128,128,
  238,1,255,128,128,128,128,128,128,128,128,
};
inline constexpr uint8_t kCoeffsUpdateProba[1056] = {
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  176,246,255,255,255,255,255,255,255,255,255,
  223,241,252,255,255,255,255,255,255,255,255,
  249,253,253,255,255,255,255,255,255,255,255,
  255,244,252,255,255,255,255,255,255,255,255,
  234,254,254,255,255,255,255,255,255,255,255,
  253,255,255,255,255,255,255,255,255,255,255,
  255,246,254,255,255,255,255,255,255,255,255,
  239,253,254,255,255,255,255,255,255,255,255,
  254,255,254,255,255,255,255,255,255,255,255,
  255,248,254,255,255,255,255,255,255,255,255,
  251,255,254,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,253,254,255,255,255,255,255,255,255,255,
  251,254,254,255,255,255,255,255,255,255,255,
  254,255,254,255,255,255,255,255,255,255,255,
  255,254,253,255,254,255,255,255,255,255,255,
  250,255,254,255,254,255,255,255,255,255,255,
  254,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  217,255,255,255,255,255,255,255,255,255,255,
  225,252,241,253,255,255,254,255,255,255,255,
  234,250,241,250,253,255,253,254,255,255,255,
  255,254,255,255,255,255,255,255,255,255,255,
  223,254,254,255,255,255,255,255,255,255,255,
  238,253,254,254,255,255,255,255,255,255,255,
  255,248,254,255,255,255,255,255,255,255,255,
  249,254,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,253,255,255,255,255,255,255,255,255,255,
  247,254,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,253,254,255,255,255,255,255,255,255,255,
  252,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,254,254,255,255,255,255,255,255,255,255,
  253,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,254,253,255,255,255,255,255,255,255,255,
  250,255,255,255,255,255,255,255,255,255,255,
  254,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  186,251,250,255,255,255,255,255,255,255,255,
  234,251,244,254,255,255,255,255,255,255,255,
  251,251,243,253,254,255,254,255,255,255,255,
  255,253,254,255,255,255,255,255,255,255,255,
  236,253,254,255,255,255,255,255,255,255,255,
  251,253,253,254,254,255,255,255,255,255,255,
  255,254,254,255,255,255,255,255,255,255,255,
  254,254,254,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,254,255,255,255,255,255,255,255,255,255,
  254,254,255,255,255,255,255,255,255,255,255,
  254,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  254,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  248,255,255,255,255,255,255,255,255,255,255,
  250,254,252,254,255,255,255,255,255,255,255,
  248,254,249,253,255,255,255,255,255,255,255,
  255,253,253,255,255,255,255,255,255,255,255,
  246,253,253,255,255,255,255,255,255,255,255,
  252,254,251,254,254,255,255,255,255,255,255,
  255,254,252,255,255,255,255,255,255,255,255,
  248,254,253,255,255,255,255,255,255,255,255,
  253,255,254,254,255,255,255,255,255,255,255,
  255,251,254,255,255,255,255,255,255,255,255,
  245,251,254,255,255,255,255,255,255,255,255,
  253,253,254,255,255,255,255,255,255,255,255,
  255,251,253,255,255,255,255,255,255,255,255,
  252,253,254,255,255,255,255,255,255,255,255,
  255,254,255,255,255,255,255,255,255,255,255,
  255,252,255,255,255,255,255,255,255,255,255,
  249,255,254,255,255,255,255,255,255,255,255,
  255,255,254,255,255,255,255,255,255,255,255,
  255,255,253,255,255,255,255,255,255,255,255,
  250,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
  254,255,255,255,255,255,255,255,255,255,255,
  255,255,255,255,255,255,255,255,255,255,255,
};
inline constexpr uint8_t kBModesProba[900] = {
  231,120,48,89,115,113,120,152,112,
  152,179,64,126,170,118,46,70,95,
  175,69,143,80,85,82,72,155,103,
  56,58,10,171,218,189,17,13,152,
  114,26,17,163,44,195,21,10,173,
  121,24,80,195,26,62,44,64,85,
  144,71,10,38,171,213,144,34,26,
  170,46,55,19,136,160,33,206,71,
  63,20,8,114,114,208,12,9,226,
  81,40,11,96,182,84,29,16,36,
  134,183,89,137,98,101,106,165,148,
  72,187,100,130,157,111,32,75,80,
  66,102,167,99,74,62,40,234,128,
  41,53,9,178,241,141,26,8,107,
  74,43,26,146,73,166,49,23,157,
  65,38,105,160,51,52,31,115,128,
  104,79,12,27,217,255,87,17,7,
  87,68,71,44,114,51,15,186,23,
  47,41,14,110,182,183,21,17,194,
  66,45,25,102,197,189,23,18,22,
  88,88,147,150,42,46,45,196,205,
  43,97,183,117,85,38,35,179,61,
  39,53,200,87,26,21,43,232,171,
  56,34,51,104,114,102,29,93,77,
  39,28,85,171,58,165,90,98,64,
  34,22,116,206,23,34,43,166,73,
  107,54,32,26,51,1,81,43,31,
  68,25,106,22,64,171,36,225,114,
  34,19,21,102,132,188,16,76,124,
  62,18,78,95,85,57,50,48,51,
  193,101,35,159,215,111,89,46,111,
  60,148,31,172,219,228,21,18,111,
  112,113,77,85,179,255,38,120,114,
  40,42,1,196,245,209,10,25,109,
  88,43,29,140,166,213,37,43,154,
  61,63,30,155,67,45,68,1,209,
  100,80,8,43,154,1,51,26,71,
  142,78,78,16,255,128,34,197,171,
  41,40,5,102,211,183,4,1,221,
  51,50,17,168,209,192,23,25,82,
  138,31,36,171,27,166,38,44,229,
  67,87,58,169,82,115,26,59,179,
  63,59,90,180,59,166,93,73,154,
  40,40,21,116,143,209,34,39,175,
  47,15,16,183,34,223,49,45,183,
  46,17,33,183,6,98,15,32,183,
  57,46,22,24,128,1,54,17,37,
  65,32,73,115,28,128,23,128,205,
  40,3,9,115,51,192,18,6,223,
  87,37,9,115,59,77,64,21,47,
  104,55,44,218,9,54,53,130,226,
  64,90,70,205,40,41,23,26,57,
  54,57,112,184,5,41,38,166,213,
  30,34,26,133,152,116,10,32,134,
  39,19,53,221,26,114,32,73,255,
  31,9,65,234,2,15,1,118,73,
  75,32,12,51,192,255,160,43,51,
  88,31,35,67,102,85,55,186,85,
  56,21,23,111,59,205,45,37,192,
  55,38,70,124,73,102,1,34,98,
  125,98,42,88,104,85,117,175,82,
  95,84,53,89,128,100,113,101,45,
  75,79,123,47,51,128,81,171,1,
  57,17,5,71,102,57,53,41,49,
  38,33,13,121,57,73,26,1,85,
  41,10,67,138,77,110,90,47,114,
  115,21,2,10,102,255,166,23,6,
  101,29,16,10,85,128,101,196,26,
  57,18,10,102,102,213,34,20,43,
  117,20,15,36,163,128,68,1,26,
  102,61,71,37,34,53,31,243,192,
  69,60,71,38,73,119,28,222,37,
  68,45,128,34,1,47,11,245,171,
  62,17,19,70,146,85,55,62,70,
  37,43,37,154,100,163,85,160,1,
  63,9,92,136,28,64,32,201,85,
  75,15,9,9,64,255,184,119,16,
  86,6,28,5,64,255,25,248,1,
  56,8,17,132,137,255,55,116,128,
  58,15,20,82,135,57,26,121,40,
  164,50,31,137,154,133,25,35,218,
  51,103,44,131,131,123,31,6,158,
  86,40,64,135,148,224,45,183,128,
  22,26,17,131,240,154,14,1,209,
  45,16,21,91,64,222,7,1,197,
  56,21,39,155,60,138,23,102,213,
  83,12,13,54,192,255,68,47,28,
  85,26,85,85,128,128,32,146,171,
  18,11,7,63,144,171,4,4,246,
  35,27,10,146,174,171,12,26,128,
  190,80,35,99,180,80,126,54,45,
  85,126,47,87,176,51,41,20,32,
  101,75,128,139,118,146,116,128,85,
  56,41,15,176,236,85,37,9,62,
  71,30,17,119,118,255,17,18,138,
  101,38,60,138,55,70,43,26,142,
  146,36,19,30,171,255,97,27,20,
  138,45,61,62,219,1,81,188,64,
  32,41,20,117,151,142,20,21,163,
  112,19,12,61,195,128,48,4,24,
};
inline constexpr uint8_t kDcTable[128] = {
  4,5,6,7,8,9,10,10,11,12,13,14,15,16,17,17,
  18,19,20,20,21,21,22,22,23,23,24,25,25,26,27,28,
  29,30,31,32,33,34,35,36,37,37,38,39,40,41,42,43,
  44,45,46,46,47,48,49,50,51,52,53,54,55,56,57,58,
  59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,
  75,76,76,77,78,79,80,81,82,83,84,85,86,87,88,89,
  91,93,95,96,98,100,101,102,104,106,108,110,112,114,116,118,
  122,124,126,128,130,132,134,136,138,140,143,145,148,151,154,157,
};
inline constexpr uint16_t kAcTable[128] = {
  4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,
  20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,
  36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,
  52,53,54,55,56,57,58,60,62,64,66,68,70,72,74,76,
  78,80,82,84,86,88,90,92,94,96,98,100,102,104,106,108,
  110,112,114,116,119,122,125,128,131,134,137,140,143,146,149,152,
  155,158,161,164,167,170,173,177,181,185,189,193,197,201,205,209,
  213,217,221,225,229,234,239,245,249,254,259,264,269,274,279,284,
};

inline constexpr uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
inline constexpr uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
inline constexpr uint8_t kCat3[] = {173, 148, 140, 0};
inline constexpr uint8_t kCat4[] = {176, 155, 140, 135, 0};
inline constexpr uint8_t kCat5[] = {180, 157, 141, 134, 130, 0};
inline constexpr uint8_t kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
inline constexpr const uint8_t* kCat3456[4] = {kCat3, kCat4, kCat5, kCat6};
// 4x4 intra mode tree (libwebp numbering: DC,TM,VE,HE,RD,VR,LD,VL,HD,HU)
inline constexpr int8_t kYModesIntra4[18] = {-0, 1, -1, 2, -2, 3, 4, 6, -3, 5, -4, -5, -6, 7, -7, 8, -8, -9};

enum { B_DC_PRED = 0, B_TM_PRED, B_VE_PRED, B_HE_PRED, B_RD_PRED, B_VR_PRED, B_LD_PRED,
       B_VL_PRED, B_HD_PRED, B_HU_PRED };
enum { DC_PRED = B_DC_PRED, V_PRED = B_VE_PRED, H_PRED = B_HE_PRED, TM_PRED = B_TM_PRED };

inline int clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// ---------------------------------------------------------------------------------------
// Boolean entropy decoder (RFC 6386 section 7), byte-wise refill, libwebp-compatible.
// ---------------------------------------------------------------------------------------
struct BitReader {
  const uint8_t* buf = nullptr;
  const uint8_t* end = nullptr;
  uint32_t value = 0;
  uint32_t range = 254;  // range - 1
  int bits = -8;
  bool eof = false;

  void init(const uint8_t* b, size_t sz) {
    buf = b; end = b + sz; value = 0; range = 254; bits = -8; eof = false;
    load();
  }
  void load() {
    if (buf < end) {
      value = (value << 8) | *buf++;
      bits += 8;
    } else if (!eof) {
      value <<= 8;
      bits += 8;
      eof = true;
    } else {
      bits = 0;  // keep going with garbage, but safely
    }
  }
  int getBit(int prob) {
    uint32_t r = range;
    if (bits < 0) load();
    const int pos = bits;
    const uint32_t split = (r * (uint32_t)prob) >> 8;
    const uint32_t v = value >> pos;
    int bit;
    if (v > split) {
      r -= split;
      value -= (split + 1) << pos;
      bit = 1;
    } else {
      r = split + 1;
      bit = 0;
    }
#if defined(__GNUC__) || defined(__clang__)
    const int shift = __builtin_clz(r) - 24;
#else
    int shift = 0;
    { uint32_t t = r; while (t < 128) { t <<= 1; ++shift; } }
#endif
    r <<= shift;
    bits -= shift;
    range = r - 1;
    return bit;
  }
  uint32_t getValue(int nbits) {
    uint32_t v = 0;
    while (nbits-- > 0) v |= (uint32_t)getBit(0x80) << nbits;
    return v;
  }
  int getSigned(int nbits) {
    const int v = (int)getValue(nbits);
    return getBit(0x80) ? -v : v;
  }
};

// ---------------------------------------------------------------------------------------
// VP8 decoder
// ---------------------------------------------------------------------------------------
struct Vp8Decoder {
  struct Quant { int y1[2], y2[2], uv[2]; };
  struct FStrength { int limit, ilevel, hev, inner; };

  int w = 0, h = 0, mbw = 0, mbh = 0;
  BitReader br;
  BitReader parts[8];
  int numPartsM1 = 0;

  bool useSegment = false, updateMap = false, absDelta = true;
  int segQuant[4] = {0, 0, 0, 0}, segFilter[4] = {0, 0, 0, 0};
  uint8_t segProb[3] = {255, 255, 255};

  bool simple = false;
  int level = 0, sharp = 0;
  bool useLfDelta = false;
  int refLf[4] = {0, 0, 0, 0}, modeLf[4] = {0, 0, 0, 0};
  int filterType = 0;

  Quant dqm[4];
  uint8_t proba[4][8][3][11];
  bool useSkip = false;
  int skipP = 0;
  FStrength fstr[4][2];

  // frame planes (with 1 pixel border on top/left, 4 extra on the right of luma)
  std::vector<uint8_t> Y, U, V;
  int ys = 0, uvs = 0;
  uint8_t* yo = nullptr;  // pointer to luma pixel (0,0)
  uint8_t* uo = nullptr;
  uint8_t* vo = nullptr;

  // ---- header ------------------------------------------------------------------------
  static bool readDims(const uint8_t* d, size_t n, int& w, int& h) {
    if (n < 10) return false;
    const uint32_t bits = d[0] | (d[1] << 8) | (d[2] << 16);
    if (bits & 1) return false;              // not a key frame
    if (((bits >> 1) & 7) > 3) return false;  // unknown profile
    if (!((bits >> 4) & 1)) return false;     // not shown
    if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) return false;
    w = (d[6] | (d[7] << 8)) & 0x3fff;
    h = (d[8] | (d[9] << 8)) & 0x3fff;
    return w > 0 && h > 0;
  }

  bool parseHeaders(const uint8_t* d, size_t n) {
    if (!readDims(d, n, w, h)) return false;
    if ((int64_t)w * h > kMaxPixels) return false;
    const uint32_t bits = d[0] | (d[1] << 8) | (d[2] << 16);
    const size_t partLen = bits >> 5;
    const uint8_t* buf = d + 10;
    size_t left = n - 10;
    if (partLen > left) return false;
    br.init(buf, partLen);
    buf += partLen;
    left -= partLen;
    mbw = (w + 15) >> 4;
    mbh = (h + 15) >> 4;

    br.getValue(1);  // colorspace
    br.getValue(1);  // clamp type

    // segment header
    useSegment = br.getValue(1) != 0;
    updateMap = false;
    absDelta = true;
    for (int i = 0; i < 4; ++i) segQuant[i] = segFilter[i] = 0;
    segProb[0] = segProb[1] = segProb[2] = 255;
    if (useSegment) {
      updateMap = br.getValue(1) != 0;
      if (br.getValue(1)) {  // update data
        absDelta = br.getValue(1) != 0;
        for (int s = 0; s < 4; ++s) segQuant[s] = br.getValue(1) ? br.getSigned(7) : 0;
        for (int s = 0; s < 4; ++s) segFilter[s] = br.getValue(1) ? br.getSigned(6) : 0;
      }
      if (updateMap) {
        for (int s = 0; s < 3; ++s) segProb[s] = br.getValue(1) ? (uint8_t)br.getValue(8) : 255;
      }
    }
    if (br.eof) return false;

    // filter header
    simple = br.getValue(1) != 0;
    level = (int)br.getValue(6);
    sharp = (int)br.getValue(3);
    useLfDelta = br.getValue(1) != 0;
    for (int i = 0; i < 4; ++i) refLf[i] = modeLf[i] = 0;
    if (useLfDelta) {
      if (br.getValue(1)) {
        for (int i = 0; i < 4; ++i) if (br.getValue(1)) refLf[i] = br.getSigned(6);
        for (int i = 0; i < 4; ++i) if (br.getValue(1)) modeLf[i] = br.getSigned(6);
      }
    }
    filterType = (level == 0) ? 0 : (simple ? 1 : 2);
    if (br.eof) return false;

    // partitions
    numPartsM1 = (1 << br.getValue(2)) - 1;
    {
      const int lastPart = numPartsM1;
      if (left < (size_t)3 * lastPart) return false;
      const uint8_t* sz = buf;
      const uint8_t* partStart = buf + 3 * lastPart;
      size_t sizeLeft = left - 3 * lastPart;
      for (int p = 0; p < lastPart; ++p) {
        size_t psize = sz[0] | (sz[1] << 8) | (sz[2] << 16);
        if (psize > sizeLeft) psize = sizeLeft;
        parts[p].init(partStart, psize);
        partStart += psize;
        sizeLeft -= psize;
        sz += 3;
      }
      parts[lastPart].init(partStart, sizeLeft);
      if (sizeLeft == 0) return false;  // no data in the last partition: truncated
    }

    // quantizers
    {
      const int baseQ0 = (int)br.getValue(7);
      const int dqy1dc = br.getValue(1) ? br.getSigned(4) : 0;
      const int dqy2dc = br.getValue(1) ? br.getSigned(4) : 0;
      const int dqy2ac = br.getValue(1) ? br.getSigned(4) : 0;
      const int dquvdc = br.getValue(1) ? br.getSigned(4) : 0;
      const int dquvac = br.getValue(1) ? br.getSigned(4) : 0;
      for (int i = 0; i < 4; ++i) {
        int q;
        if (useSegment) {
          q = segQuant[i];
          if (!absDelta) q += baseQ0;
        } else {
          if (i > 0) { dqm[i] = dqm[0]; continue; }
          q = baseQ0;
        }
        Quant& m = dqm[i];
        m.y1[0] = kDcTable[clipQ(q + dqy1dc, 127)];
        m.y1[1] = kAcTable[clipQ(q + 0, 127)];
        m.y2[0] = kDcTable[clipQ(q + dqy2dc, 127)] * 2;
        m.y2[1] = (kAcTable[clipQ(q + dqy2ac, 127)] * 101581) >> 16;
        if (m.y2[1] < 8) m.y2[1] = 8;
        m.uv[0] = kDcTable[clipQ(q + dquvdc, 117)];
        m.uv[1] = kAcTable[clipQ(q + dquvac, 127)];
      }
    }
    br.getValue(1);  // ignored (refresh entropy probs)

    // coefficient probabilities
    for (int t = 0; t < 4; ++t)
      for (int b = 0; b < 8; ++b)
        for (int c = 0; c < 3; ++c)
          for (int p = 0; p < 11; ++p) {
            const int idx = ((t * 8 + b) * 3 + c) * 11 + p;
            proba[t][b][c][p] = br.getBit(kCoeffsUpdateProba[idx]) ? (uint8_t)br.getValue(8)
                                                                   : kCoeffsProba0[idx];
          }
    useSkip = br.getValue(1) != 0;
    skipP = useSkip ? (int)br.getValue(8) : 0;
    if (br.eof) return false;

    precomputeFilterStrengths();
    return true;
  }

  static int clipQ(int v, int m) { return v < 0 ? 0 : (v > m ? m : v); }

  void precomputeFilterStrengths() {
    if (filterType == 0) return;
    for (int s = 0; s < 4; ++s) {
      int baseLevel;
      if (useSegment) {
        baseLevel = segFilter[s];
        if (!absDelta) baseLevel += level;
      } else {
        baseLevel = level;
      }
      for (int i4 = 0; i4 <= 1; ++i4) {
        FStrength& info = fstr[s][i4];
        int lv = baseLevel;
        if (useLfDelta) {
          lv += refLf[0];
          if (i4) lv += modeLf[0];
        }
        lv = (lv < 0) ? 0 : (lv > 63 ? 63 : lv);
        if (lv > 0) {
          int il = lv;
          if (sharp > 0) {
            il >>= (sharp > 4) ? 2 : 1;
            if (il > 9 - sharp) il = 9 - sharp;
          }
          if (il < 1) il = 1;
          info.ilevel = il;
          info.limit = 2 * lv + il;
          info.hev = (lv >= 40) ? 2 : (lv >= 15) ? 1 : 0;
        } else {
          info.limit = 0;
          info.ilevel = 0;
          info.hev = 0;
        }
        info.inner = i4;
      }
    }
  }

  // ---- residual parsing ------------------------------------------------------------------
  static int getLargeValue(BitReader& r, const uint8_t* p) {
    int v;
    if (!r.getBit(p[3])) {
      if (!r.getBit(p[4])) v = 2;
      else v = 3 + r.getBit(p[5]);
    } else {
      if (!r.getBit(p[6])) {
        if (!r.getBit(p[7])) {
          v = 5 + r.getBit(159);
        } else {
          v = 7 + 2 * r.getBit(165);
          v += r.getBit(145);
        }
      } else {
        const int bit1 = r.getBit(p[8]);
        const int bit0 = r.getBit(p[9 + bit1]);
        const int cat = 2 * bit1 + bit0;
        v = 0;
        for (const uint8_t* tab = kCat3456[cat]; *tab; ++tab) v += v + r.getBit(*tab);
        v += 3 + (8 << cat);
      }
    }
    return v;
  }

  // prob: [8][3][11] for one block type. Returns index of last non-zero coeff + 1 (or first).
  static int getCoeffs(BitReader& r, const uint8_t (*prob)[3][11], int ctx, const int* dq, int n,
                       int16_t* out) {
    const uint8_t* p = prob[kBands[n]][ctx];
    for (; n < 16; ++n) {
      if (!r.getBit(p[0])) return n;
      while (!r.getBit(p[1])) {
        ++n;
        p = prob[kBands[n]][0];
        if (n == 16) return 16;
      }
      const uint8_t(*pc)[11] = prob[kBands[n + 1]];
      int v;
      if (!r.getBit(p[2])) {
        v = 1;
        p = pc[1];
      } else {
        v = getLargeValue(r, p);
        p = pc[2];
      }
      if (r.getBit(0x80)) v = -v;
      out[kZigzag[n]] = (int16_t)(v * dq[n > 0]);
    }
    return 16;
  }

  struct NzCtx { uint8_t y[4], u[2], v[2], dc; };

  static int nzCode(int nz, bool dcNz) { return (nz > 3) ? 3 : (nz > 1) ? 2 : (dcNz ? 1 : 0); }

  // returns true if the macroblock has no non-zero coefficient
  bool parseResiduals(BitReader& tr, NzCtx& top, NzCtx& lft, const Quant& q, bool isI4, int16_t* coeffs,
                      uint8_t* ycode, uint8_t* uvcode) {
    int first;
    const uint8_t(*acProba)[3][11];
    std::memset(coeffs, 0, 384 * sizeof(int16_t));
    if (!isI4) {
      int16_t dc[16];
      std::memset(dc, 0, sizeof(dc));
      const int ctx = top.dc + lft.dc;
      const int nz = getCoeffs(tr, proba[1], ctx, q.y2, 0, dc);
      top.dc = lft.dc = (nz > 0);
      if (nz > 1) {
        transformWHT(dc, coeffs);
      } else {
        const int dc0 = (dc[0] + 3) >> 3;
        for (int i = 0; i < 16; ++i) coeffs[i * 16] = (int16_t)dc0;
      }
      first = 1;
      acProba = proba[0];
    } else {
      first = 0;
      acProba = proba[3];
    }
    bool any = false;
    int16_t* dst = coeffs;
    for (int y = 0; y < 4; ++y) {
      int l = lft.y[y];
      for (int x = 0; x < 4; ++x) {
        const int ctx = l + top.y[x];
        const int nz = getCoeffs(tr, acProba, ctx, q.y1, first, dst);
        l = (nz > first);
        top.y[x] = (uint8_t)l;
        const int code = nzCode(nz, dst[0] != 0);
        ycode[y * 4 + x] = (uint8_t)code;
        any |= (code != 0);
        dst += 16;
      }
      lft.y[y] = (uint8_t)l;
    }
    for (int ch = 0; ch < 2; ++ch) {
      uint8_t* tn = ch ? top.v : top.u;
      uint8_t* ln = ch ? lft.v : lft.u;
      for (int y = 0; y < 2; ++y) {
        int l = ln[y];
        for (int x = 0; x < 2; ++x) {
          const int ctx = l + tn[x];
          const int nz = getCoeffs(tr, proba[2], ctx, q.uv, 0, dst);
          l = (nz > 0);
          tn[x] = (uint8_t)l;
          const int code = nzCode(nz, dst[0] != 0);
          uvcode[ch * 4 + y * 2 + x] = (uint8_t)code;
          any |= (code != 0);
          dst += 16;
        }
        ln[y] = (uint8_t)l;
      }
    }
    return !any;
  }

  // ---- transforms ------------------------------------------------------------------------
  static void transformWHT(const int16_t* in, int16_t* out) {
    int tmp[16];
    for (int i = 0; i < 4; ++i) {
      const int a0 = in[0 + i] + in[12 + i];
      const int a1 = in[4 + i] + in[8 + i];
      const int a2 = in[4 + i] - in[8 + i];
      const int a3 = in[0 + i] - in[12 + i];
      tmp[0 + i] = a0 + a1;
      tmp[8 + i] = a0 - a1;
      tmp[4 + i] = a3 + a2;
      tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; ++i) {
      const int dc = tmp[0 + i * 4] + 3;
      const int a0 = dc + tmp[3 + i * 4];
      const int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
      const int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
      const int a3 = dc - tmp[3 + i * 4];
      out[0] = (int16_t)((a0 + a1) >> 3);
      out[16] = (int16_t)((a3 + a2) >> 3);
      out[32] = (int16_t)((a0 - a1) >> 3);
      out[48] = (int16_t)((a3 - a2) >> 3);
      out += 64;
    }
  }

  static int mul1(int a) { return ((a * 20091) >> 16) + a; }
  static int mul2(int a) { return (a * 35468) >> 16; }

  static void transformAdd(const int16_t* in, uint8_t* dst, int stride) {
    int C[16];
    int* tmp = C;
    for (int i = 0; i < 4; ++i) {
      const int a = in[0] + in[8];
      const int b = in[0] - in[8];
      const int c = mul2(in[4]) - mul1(in[12]);
      const int d = mul1(in[4]) + mul2(in[12]);
      tmp[0] = a + d;
      tmp[1] = b + c;
      tmp[2] = b - c;
      tmp[3] = a - d;
      tmp += 4;
      in++;
    }
    tmp = C;
    for (int i = 0; i < 4; ++i) {
      const int dc = tmp[0] + 4;
      const int a = dc + tmp[8];
      const int b = dc - tmp[8];
      const int c = mul2(tmp[4]) - mul1(tmp[12]);
      const int d = mul1(tmp[4]) + mul2(tmp[12]);
      dst[0] = (uint8_t)clip8(dst[0] + ((a + d) >> 3));
      dst[1] = (uint8_t)clip8(dst[1] + ((b + c) >> 3));
      dst[2] = (uint8_t)clip8(dst[2] + ((b - c) >> 3));
      dst[3] = (uint8_t)clip8(dst[3] + ((a - d) >> 3));
      tmp++;
      dst += stride;
    }
  }

  // ---- intra prediction ------------------------------------------------------------------
  static void predBig(uint8_t* dst, int stride, int size, int mode, int mbx, int mby) {
    const int lg = (size == 16) ? 4 : 3;
    switch (mode) {
      case DC_PRED: {
        int dc;
        if (mbx == 0 && mby == 0) {
          dc = 0x80;
        } else {
          int s = 0;
          if (mby > 0) for (int i = 0; i < size; ++i) s += dst[i - stride];
          if (mbx > 0) for (int i = 0; i < size; ++i) s += dst[-1 + i * stride];
          if (mbx > 0 && mby > 0) dc = (s + size) >> (lg + 1);
          else dc = (s + size / 2) >> lg;
        }
        for (int j = 0; j < size; ++j) std::memset(dst + j * stride, dc, size);
        break;
      }
      case V_PRED:
        for (int j = 0; j < size; ++j) std::memcpy(dst + j * stride, dst - stride, size);
        break;
      case H_PRED:
        for (int j = 0; j < size; ++j) std::memset(dst + j * stride, dst[-1 + j * stride], size);
        break;
      default: {  // TM_PRED
        const uint8_t* top = dst - stride;
        const int tl = top[-1];
        for (int j = 0; j < size; ++j) {
          const int l = dst[-1 + j * stride] - tl;
          for (int i = 0; i < size; ++i) dst[i + j * stride] = (uint8_t)clip8(top[i] + l);
        }
        break;
      }
    }
  }

  static int avg3(int a, int b, int c) { return (a + 2 * b + c + 2) >> 2; }
  static int avg2(int a, int b) { return (a + b + 1) >> 1; }

  // top8: the 8 pixels above (4 above + 4 above-right)
  static void pred4(uint8_t* dst, int stride, int mode, const uint8_t* top8) {
    const int X = dst[-stride - 1];
    const int I = dst[-1], J = dst[-1 + stride], K = dst[-1 + 2 * stride], L = dst[-1 + 3 * stride];
    const int A = top8[0], B = top8[1], C = top8[2], D = top8[3], E = top8[4], F = top8[5],
              G = top8[6], H = top8[7];
#define DST(x, y) dst[(x) + (y) * stride]
    switch (mode) {
      case B_DC_PRED: {
        int dc = 4;
        for (int i = 0; i < 4; ++i) dc += top8[i] + dst[-1 + i * stride];
        dc >>= 3;
        for (int j = 0; j < 4; ++j) std::memset(dst + j * stride, dc, 4);
        break;
      }
      case B_TM_PRED:
        for (int j = 0; j < 4; ++j) {
          const int l = dst[-1 + j * stride] - X;
          for (int i = 0; i < 4; ++i) DST(i, j) = (uint8_t)clip8(top8[i] + l);
        }
        break;
      case B_VE_PRED: {
        const uint8_t v0 = (uint8_t)avg3(X, A, B), v1 = (uint8_t)avg3(A, B, C),
                      v2 = (uint8_t)avg3(B, C, D), v3 = (uint8_t)avg3(C, D, E);
        for (int j = 0; j < 4; ++j) { DST(0, j) = v0; DST(1, j) = v1; DST(2, j) = v2; DST(3, j) = v3; }
        break;
      }
      case B_HE_PRED: {
        const uint8_t r0 = (uint8_t)avg3(X, I, J), r1 = (uint8_t)avg3(I, J, K),
                      r2 = (uint8_t)avg3(J, K, L), r3 = (uint8_t)avg3(K, L, L);
        std::memset(dst, r0, 4);
        std::memset(dst + stride, r1, 4);
        std::memset(dst + 2 * stride, r2, 4);
        std::memset(dst + 3 * stride, r3, 4);
        break;
      }
      case B_RD_PRED:
        DST(0, 3) = (uint8_t)avg3(J, K, L);
        DST(1, 3) = DST(0, 2) = (uint8_t)avg3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = (uint8_t)avg3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = (uint8_t)avg3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = (uint8_t)avg3(B, A, X);
        DST(3, 1) = DST(2, 0) = (uint8_t)avg3(C, B, A);
        DST(3, 0) = (uint8_t)avg3(D, C, B);
        break;
      case B_VR_PRED:
        DST(0, 0) = DST(1, 2) = (uint8_t)avg2(X, A);
        DST(1, 0) = DST(2, 2) = (uint8_t)avg2(A, B);
        DST(2, 0) = DST(3, 2) = (uint8_t)avg2(B, C);
        DST(3, 0) = (uint8_t)avg2(C, D);
        DST(0, 3) = (uint8_t)avg3(K, J, I);
        DST(0, 2) = (uint8_t)avg3(J, I, X);
        DST(0, 1) = DST(1, 3) = (uint8_t)avg3(I, X, A);
        DST(1, 1) = DST(2, 3) = (uint8_t)avg3(X, A, B);
        DST(2, 1) = DST(3, 3) = (uint8_t)avg3(A, B, C);
        DST(3, 1) = (uint8_t)avg3(B, C, D);
        break;
      case B_LD_PRED:
        DST(0, 0) = (uint8_t)avg3(A, B, C);
        DST(1, 0) = DST(0, 1) = (uint8_t)avg3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = (uint8_t)avg3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = (uint8_t)avg3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = (uint8_t)avg3(E, F, G);
        DST(3, 2) = DST(2, 3) = (uint8_t)avg3(F, G, H);
        DST(3, 3) = (uint8_t)avg3(G, H, H);
        break;
      case B_VL_PRED:
        DST(0, 0) = (uint8_t)avg2(A, B);
        DST(1, 0) = DST(0, 2) = (uint8_t)avg2(B, C);
        DST(2, 0) = DST(1, 2) = (uint8_t)avg2(C, D);
        DST(3, 0) = DST(2, 2) = (uint8_t)avg2(D, E);
        DST(0, 1) = (uint8_t)avg3(A, B, C);
        DST(1, 1) = DST(0, 3) = (uint8_t)avg3(B, C, D);
        DST(2, 1) = DST(1, 3) = (uint8_t)avg3(C, D, E);
        DST(3, 1) = DST(2, 3) = (uint8_t)avg3(D, E, F);
        DST(3, 2) = (uint8_t)avg3(E, F, G);
        DST(3, 3) = (uint8_t)avg3(F, G, H);
        break;
      case B_HD_PRED:
        DST(0, 0) = DST(2, 1) = (uint8_t)avg2(I, X);
        DST(0, 1) = DST(2, 2) = (uint8_t)avg2(J, I);
        DST(0, 2) = DST(2, 3) = (uint8_t)avg2(K, J);
        DST(0, 3) = (uint8_t)avg2(L, K);
        DST(3, 0) = (uint8_t)avg3(A, B, C);
        DST(2, 0) = (uint8_t)avg3(X, A, B);
        DST(1, 0) = DST(3, 1) = (uint8_t)avg3(I, X, A);
        DST(1, 1) = DST(3, 2) = (uint8_t)avg3(J, I, X);
        DST(1, 2) = DST(3, 3) = (uint8_t)avg3(K, J, I);
        DST(1, 3) = (uint8_t)avg3(L, K, J);
        break;
      default:  // B_HU_PRED
        DST(0, 0) = (uint8_t)avg2(I, J);
        DST(2, 0) = DST(0, 1) = (uint8_t)avg2(J, K);
        DST(2, 1) = DST(0, 2) = (uint8_t)avg2(K, L);
        DST(1, 0) = (uint8_t)avg3(I, J, K);
        DST(3, 0) = DST(1, 1) = (uint8_t)avg3(J, K, L);
        DST(3, 1) = DST(1, 2) = (uint8_t)avg3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = (uint8_t)L;
        break;
    }
#undef DST
  }

  // ---- loop filter -----------------------------------------------------------------------
  static int sclip1(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }
  static int sclip2(int v) { return v < -16 ? -16 : (v > 15 ? 15 : v); }
  static int iabs(int v) { return v < 0 ? -v : v; }

  static void doFilter2(uint8_t* p, int step) {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    const int a1 = sclip2((a + 4) >> 3);
    const int a2 = sclip2((a + 3) >> 3);
    p[-step] = (uint8_t)clip8(p0 + a2);
    p[0] = (uint8_t)clip8(q0 - a1);
  }
  static void doFilter4(uint8_t* p, int step) {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    const int a = 3 * (q0 - p0);
    const int a1 = sclip2((a + 4) >> 3);
    const int a2 = sclip2((a + 3) >> 3);
    const int a3 = (a1 + 1) >> 1;
    p[-2 * step] = (uint8_t)clip8(p1 + a3);
    p[-step] = (uint8_t)clip8(p0 + a2);
    p[0] = (uint8_t)clip8(q0 - a1);
    p[step] = (uint8_t)clip8(q1 - a3);
  }
  static void doFilter6(uint8_t* p, int step) {
    const int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    const int q0 = p[0], q1 = p[step], q2 = p[2 * step];
    const int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    const int a1 = (27 * a + 63) >> 7;
    const int a2 = (18 * a + 63) >> 7;
    const int a3 = (9 * a + 63) >> 7;
    p[-3 * step] = (uint8_t)clip8(p2 + a3);
    p[-2 * step] = (uint8_t)clip8(p1 + a2);
    p[-step] = (uint8_t)clip8(p0 + a1);
    p[0] = (uint8_t)clip8(q0 - a1);
    p[step] = (uint8_t)clip8(q1 - a2);
    p[2 * step] = (uint8_t)clip8(q2 - a3);
  }
  static bool hev(const uint8_t* p, int step, int thresh) {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return iabs(p1 - p0) > thresh || iabs(q1 - q0) > thresh;
  }
  static bool needsFilter(const uint8_t* p, int step, int t) {
    const int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return (4 * iabs(p0 - q0) + iabs(p1 - q1)) <= t;
  }
  static bool needsFilter2(const uint8_t* p, int step, int t, int it) {
    const int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    const int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    if ((4 * iabs(p0 - q0) + iabs(p1 - q1)) > t) return false;
    return iabs(p3 - p2) <= it && iabs(p2 - p1) <= it && iabs(p1 - p0) <= it &&
           iabs(q3 - q2) <= it && iabs(q2 - q1) <= it && iabs(q1 - q0) <= it;
  }
  static void simpleLoop(uint8_t* p, int hstride, int vstride, int thresh) {
    const int t2 = 2 * thresh + 1;
    for (int i = 0; i < 16; ++i) {
      if (needsFilter(p + i * vstride, hstride, t2)) doFilter2(p + i * vstride, hstride);
    }
  }
  static void loop26(uint8_t* p, int hstride, int vstride, int size, int thresh, int ithresh, int hevT) {
    const int t2 = 2 * thresh + 1;
    while (size-- > 0) {
      if (needsFilter2(p, hstride, t2, ithresh)) {
        if (hev(p, hstride, hevT)) doFilter2(p, hstride);
        else doFilter6(p, hstride);
      }
      p += vstride;
    }
  }
  static void loop24(uint8_t* p, int hstride, int vstride, int size, int thresh, int ithresh, int hevT) {
    const int t2 = 2 * thresh + 1;
    while (size-- > 0) {
      if (needsFilter2(p, hstride, t2, ithresh)) {
        if (hev(p, hstride, hevT)) doFilter2(p, hstride);
        else doFilter4(p, hstride);
      }
      p += vstride;
    }
  }

  void filterMB(int mbx, int mby, const FStrength& f, bool inner) {
    const int limit = f.limit;
    if (limit == 0) return;
    uint8_t* yd = yo + (size_t)mby * 16 * ys + mbx * 16;
    uint8_t* ud = uo + (size_t)mby * 8 * uvs + mbx * 8;
    uint8_t* vd = vo + (size_t)mby * 8 * uvs + mbx * 8;
    if (filterType == 1) {
      if (mbx > 0) simpleLoop(yd, 1, ys, limit + 4);
      if (inner) for (int k = 1; k < 4; ++k) simpleLoop(yd + 4 * k, 1, ys, limit);
      if (mby > 0) simpleLoop(yd, ys, 1, limit + 4);
      if (inner) for (int k = 1; k < 4; ++k) simpleLoop(yd + 4 * k * ys, ys, 1, limit);
    } else {
      const int il = f.ilevel, ht = f.hev;
      if (mbx > 0) {
        loop26(yd, 1, ys, 16, limit + 4, il, ht);
        loop26(ud, 1, uvs, 8, limit + 4, il, ht);
        loop26(vd, 1, uvs, 8, limit + 4, il, ht);
      }
      if (inner) {
        for (int k = 1; k < 4; ++k) loop24(yd + 4 * k, 1, ys, 16, limit, il, ht);
        loop24(ud + 4, 1, uvs, 8, limit, il, ht);
        loop24(vd + 4, 1, uvs, 8, limit, il, ht);
      }
      if (mby > 0) {
        loop26(yd, ys, 1, 16, limit + 4, il, ht);
        loop26(ud, uvs, 1, 8, limit + 4, il, ht);
        loop26(vd, uvs, 1, 8, limit + 4, il, ht);
      }
      if (inner) {
        for (int k = 1; k < 4; ++k) loop24(yd + 4 * k * ys, ys, 1, 16, limit, il, ht);
        loop24(ud + 4 * uvs, uvs, 1, 8, limit, il, ht);
        loop24(vd + 4 * uvs, uvs, 1, 8, limit, il, ht);
      }
    }
  }

  // ---- main decoding loop ------------------------------------------------------------------
  bool decodeFrame() {
    const int W = mbw * 16, H = mbh * 16;
    ys = W + 1 + 4;
    uvs = W / 2 + 1;
    Y.assign((size_t)(H + 1) * ys, 0);
    U.assign((size_t)(H / 2 + 1) * uvs, 0);
    V.assign((size_t)(H / 2 + 1) * uvs, 0);
    yo = Y.data() + ys + 1;
    uo = U.data() + uvs + 1;
    vo = V.data() + uvs + 1;
    // borders: row -1 = 127 (incl. corner and right extension), column -1 = 129
    std::memset(Y.data(), 127, ys);
    std::memset(U.data(), 127, uvs);
    std::memset(V.data(), 127, uvs);
    for (int j = 0; j < H; ++j) yo[(size_t)j * ys - 1] = 129;
    for (int j = 0; j < H / 2; ++j) { uo[(size_t)j * uvs - 1] = 129; vo[(size_t)j * uvs - 1] = 129; }

    std::vector<NzCtx> topNz((size_t)mbw);
    std::memset(topNz.data(), 0, topNz.size() * sizeof(NzCtx));
    std::vector<uint8_t> intraTop((size_t)mbw * 4, B_DC_PRED);
    std::vector<uint8_t> mbMeta((size_t)mbw * mbh, 0);  // seg | i4<<2 | inner<<3
    int16_t coeffs[384];
    uint8_t ycode[16], uvcode[8];
    uint8_t imodes[16];

    for (int mby = 0; mby < mbh; ++mby) {
      BitReader& tr = parts[mby & numPartsM1];
      NzCtx lft;
      std::memset(&lft, 0, sizeof(lft));
      uint8_t intraLeft[4] = {B_DC_PRED, B_DC_PRED, B_DC_PRED, B_DC_PRED};
      if (mby > 0) {
        // replicate last pixel of the row above into the above-right extension
        uint8_t* row = yo + (size_t)(mby * 16 - 1) * ys;
        std::memset(row + W, row[W - 1], 4);
      }
      for (int mbx = 0; mbx < mbw; ++mbx) {
        // ---- intra modes (partition 0)
        int segment = 0;
        if (updateMap) {
          segment = !br.getBit(segProb[0]) ? br.getBit(segProb[1]) : br.getBit(segProb[2]) + 2;
        }
        bool skip = useSkip ? (br.getBit(skipP) != 0) : false;
        const bool isI4 = !br.getBit(145);
        int ymode = 0;
        uint8_t* topm = &intraTop[(size_t)mbx * 4];
        if (!isI4) {
          ymode = br.getBit(156) ? (br.getBit(128) ? TM_PRED : H_PRED)
                                 : (br.getBit(163) ? V_PRED : DC_PRED);
          std::memset(topm, ymode, 4);
          std::memset(intraLeft, ymode, 4);
        } else {
          uint8_t* modes = imodes;
          for (int y = 0; y < 4; ++y) {
            int m = intraLeft[y];
            for (int x = 0; x < 4; ++x) {
              const uint8_t* prob = &kBModesProba[(topm[x] * 10 + m) * 9];
              int i = kYModesIntra4[br.getBit(prob[0])];
              while (i > 0) i = kYModesIntra4[2 * i + br.getBit(prob[i])];
              m = -i;
              topm[x] = (uint8_t)m;
            }
            std::memcpy(modes, topm, 4);
            modes += 4;
            intraLeft[y] = (uint8_t)m;
          }
        }
        const int uvmode = !br.getBit(142) ? DC_PRED
                         : !br.getBit(114) ? V_PRED
                         : br.getBit(183) ? TM_PRED : H_PRED;
        if (br.eof) return false;

        // ---- residuals
        NzCtx& top = topNz[(size_t)mbx];
        bool noCoeffs;
        if (!skip) {
          noCoeffs = parseResiduals(tr, top, lft, dqm[segment], isI4, coeffs, ycode, uvcode);
        } else {
          std::memset(top.y, 0, 4); std::memset(top.u, 0, 2); std::memset(top.v, 0, 2);
          std::memset(lft.y, 0, 4); std::memset(lft.u, 0, 2); std::memset(lft.v, 0, 2);
          if (!isI4) top.dc = lft.dc = 0;
          std::memset(ycode, 0, sizeof(ycode));
          std::memset(uvcode, 0, sizeof(uvcode));
          noCoeffs = true;
          std::memset(coeffs, 0, sizeof(coeffs));
        }
        if (tr.eof) return false;
        mbMeta[(size_t)mby * mbw + mbx] =
            (uint8_t)(segment | (isI4 ? 4 : 0) | ((isI4 || !noCoeffs) ? 8 : 0));

        // ---- reconstruction
        uint8_t* yd = yo + (size_t)mby * 16 * ys + mbx * 16;
        uint8_t* ud = uo + (size_t)mby * 8 * uvs + mbx * 8;
        uint8_t* vd = vo + (size_t)mby * 8 * uvs + mbx * 8;
        if (!isI4) {
          predBig(yd, ys, 16, ymode, mbx, mby);
          for (int n = 0; n < 16; ++n) {
            if (ycode[n]) transformAdd(coeffs + n * 16, yd + (n >> 2) * 4 * ys + (n & 3) * 4, ys);
          }
        } else {
          uint8_t mbTopRight[4];
          std::memcpy(mbTopRight, yd - ys + 16, 4);
          for (int n = 0; n < 16; ++n) {
            const int bx = n & 3, by = n >> 2;
            uint8_t* bd = yd + by * 4 * ys + bx * 4;
            uint8_t t8[8];
            std::memcpy(t8, bd - ys, 4);
            if (bx == 3 && by > 0) std::memcpy(t8 + 4, mbTopRight, 4);
            else std::memcpy(t8 + 4, bd - ys + 4, 4);
            pred4(bd, ys, imodes[n], t8);
            if (ycode[n]) transformAdd(coeffs + n * 16, bd, ys);
          }
        }
        predBig(ud, uvs, 8, uvmode, mbx, mby);
        predBig(vd, uvs, 8, uvmode, mbx, mby);
        for (int n = 0; n < 4; ++n) {
          const int off = (n >> 1) * 4 * uvs + (n & 1) * 4;
          if (uvcode[n]) transformAdd(coeffs + 256 + n * 16, ud + off, uvs);
          if (uvcode[4 + n]) transformAdd(coeffs + 320 + n * 16, vd + off, uvs);
        }
      }
    }

    if (filterType > 0) {
      for (int mby = 0; mby < mbh; ++mby)
        for (int mbx = 0; mbx < mbw; ++mbx) {
          const uint8_t m = mbMeta[(size_t)mby * mbw + mbx];
          filterMB(mbx, mby, fstr[m & 3][(m >> 2) & 1], (m & 8) != 0);
        }
    }
    return true;
  }

  bool decode(const uint8_t* d, size_t n) {
    if (!parseHeaders(d, n)) return false;
    return decodeFrame();
  }
};

// ---------------------------------------------------------------------------------------
// YUV -> RGBA, libwebp fixed point + fancy upsampling
// ---------------------------------------------------------------------------------------
inline int multHi(int v, int coeff) { return (v * coeff) >> 8; }
inline int yuvClip8(int v) { return (v & ~((256 << 6) - 1)) == 0 ? (v >> 6) : (v < 0 ? 0 : 255); }
inline void yuvToRgba(int y, int u, int v, uint8_t* rgba) {
  rgba[0] = (uint8_t)yuvClip8(multHi(y, 19077) + multHi(v, 26149) - 14234);
  rgba[1] = (uint8_t)yuvClip8(multHi(y, 19077) - multHi(u, 6419) - multHi(v, 13320) + 8708);
  rgba[2] = (uint8_t)yuvClip8(multHi(y, 19077) + multHi(u, 33050) - 17685);
  rgba[3] = 255;
}

inline void upsampleLinePair(const uint8_t* topY, const uint8_t* botY, const uint8_t* topU,
                             const uint8_t* topV, const uint8_t* curU, const uint8_t* curV,
                             uint8_t* topDst, uint8_t* botDst, int len) {
#define LOAD_UV(u, v) ((uint32_t)(u) | ((uint32_t)(v) << 16))
  const int lastPixelPair = (len - 1) >> 1;
  uint32_t tl = LOAD_UV(topU[0], topV[0]);
  uint32_t l = LOAD_UV(curU[0], curV[0]);
  {
    const uint32_t uv0 = (3 * tl + l + 0x00020002u) >> 2;
    yuvToRgba(topY[0], uv0 & 0xff, (uv0 >> 16) & 0xff, topDst);
  }
  if (botY) {
    const uint32_t uv0 = (3 * l + tl + 0x00020002u) >> 2;
    yuvToRgba(botY[0], uv0 & 0xff, (uv0 >> 16) & 0xff, botDst);
  }
  for (int x = 1; x <= lastPixelPair; ++x) {
    const uint32_t t_uv = LOAD_UV(topU[x], topV[x]);
    const uint32_t uv = LOAD_UV(curU[x], curV[x]);
    const uint32_t avg = tl + t_uv + l + uv + 0x00080008u;
    const uint32_t diag12 = (avg + 2 * (t_uv + l)) >> 3;
    const uint32_t diag03 = (avg + 2 * (tl + uv)) >> 3;
    {
      const uint32_t uv0 = (diag12 + tl) >> 1;
      const uint32_t uv1 = (diag03 + t_uv) >> 1;
      yuvToRgba(topY[2 * x - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, topDst + (2 * x - 1) * 4);
      yuvToRgba(topY[2 * x - 0], uv1 & 0xff, (uv1 >> 16) & 0xff, topDst + (2 * x - 0) * 4);
    }
    if (botY) {
      const uint32_t uv0 = (diag03 + l) >> 1;
      const uint32_t uv1 = (diag12 + uv) >> 1;
      yuvToRgba(botY[2 * x - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, botDst + (2 * x - 1) * 4);
      yuvToRgba(botY[2 * x + 0], uv1 & 0xff, (uv1 >> 16) & 0xff, botDst + (2 * x + 0) * 4);
    }
    tl = t_uv;
    l = uv;
  }
  if (!(len & 1)) {
    {
      const uint32_t uv0 = (3 * tl + l + 0x00020002u) >> 2;
      yuvToRgba(topY[len - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, topDst + (len - 1) * 4);
    }
    if (botY) {
      const uint32_t uv0 = (3 * l + tl + 0x00020002u) >> 2;
      yuvToRgba(botY[len - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, botDst + (len - 1) * 4);
    }
  }
#undef LOAD_UV
}

inline void convertFrameToRgba(const Vp8Decoder& d, uint8_t* rgba) {
  const int w = d.w, h = d.h;
  const int uvh = (h + 1) >> 1;
  const size_t rs = (size_t)w * 4;
  const uint8_t* y0 = d.yo;
  upsampleLinePair(y0, nullptr, d.uo, d.vo, d.uo, d.vo, rgba, nullptr, w);
  for (int y = 1; y < h; y += 2) {
    const int k = (y + 1) >> 1;
    const uint8_t* tu = d.uo + (size_t)(k - 1) * d.uvs;
    const uint8_t* tv = d.vo + (size_t)(k - 1) * d.uvs;
    const uint8_t* cu = (k < uvh) ? d.uo + (size_t)k * d.uvs : tu;
    const uint8_t* cv = (k < uvh) ? d.vo + (size_t)k * d.uvs : tv;
    const bool hasBot = (y + 1) < h;
    upsampleLinePair(y0 + (size_t)y * d.ys, hasBot ? y0 + (size_t)(y + 1) * d.ys : nullptr, tu, tv, cu,
                     cv, rgba + (size_t)y * rs, hasBot ? rgba + (size_t)(y + 1) * rs : nullptr, w);
  }
}

// ---------------------------------------------------------------------------------------
// VP8L (lossless) decoder; also used for compressed alpha
// ---------------------------------------------------------------------------------------
inline constexpr uint8_t kCodeToPlane[120] = {
  24,7,23,25,40,6,39,41,22,26,38,42,56,5,55,57,
  21,27,54,58,37,43,72,4,71,73,20,28,53,59,70,74,
  36,44,88,69,75,52,60,3,87,89,19,29,86,90,35,45,
  68,76,85,91,51,61,104,2,103,105,18,30,102,106,34,46,
  84,92,67,77,101,107,50,62,120,1,119,121,83,93,17,31,
  100,108,66,78,118,122,33,47,117,123,49,63,99,109,82,94,
  0,116,124,65,79,16,32,98,110,48,115,125,81,95,64,114,
  126,97,111,80,113,127,96,112,
};

struct LBitReader {
  const uint8_t* data = nullptr;
  size_t size = 0, pos = 0;
  uint64_t acc = 0;
  int nbits = 0;
  bool eos = false;

  void init(const uint8_t* d, size_t n) { data = d; size = n; pos = 0; acc = 0; nbits = 0; eos = false; }
  void fill() {
    while (nbits <= 56 && pos < size) {
      acc |= (uint64_t)data[pos++] << nbits;
      nbits += 8;
    }
  }
  // peek up to 32 bits (zero padded past the end)
  uint32_t peek(int n) {
    fill();
    return (uint32_t)(acc & ((n >= 32) ? 0xffffffffull : ((1ull << n) - 1)));
  }
  void consume(int n) {
    if (n > nbits) { eos = true; acc = 0; nbits = 0; return; }
    acc >>= n;
    nbits -= n;
  }
  uint32_t read(int n) {
    if (n == 0) return 0;
    const uint32_t v = peek(n);
    consume(n);
    return v;
  }
};

struct HuffCode {
  std::vector<uint32_t> table;  // (len << 16) | symbol, indexed by reversed code prefix
  std::vector<uint16_t> sorted;
  uint16_t count[16];
  int root = 0, maxLen = 0;
  bool single = false;
  uint16_t singleSym = 0;

  // lengths: array of 'n' code lengths. Returns false for invalid (over/under-subscribed, empty).
  bool build(const uint8_t* lengths, int n) {
    std::memset(count, 0, sizeof(count));
    int nonzero = 0, last = 0;
    for (int i = 0; i < n; ++i) {
      if (lengths[i] > 15) return false;
      if (lengths[i]) { ++nonzero; last = i; }
      ++count[lengths[i]];
    }
    if (nonzero == 0) return false;
    table.clear();
    sorted.clear();
    if (nonzero == 1) {
      single = true;
      singleSym = (uint16_t)last;
      root = 0;
      maxLen = 0;
      return true;
    }
    single = false;
    count[0] = 0;
    int left = 1;
    maxLen = 0;
    for (int l = 1; l < 16; ++l) {
      left <<= 1;
      left -= count[l];
      if (left < 0) return false;
      if (count[l]) maxLen = l;
    }
    if (left != 0) return false;
    root = maxLen < 8 ? maxLen : 8;
    table.assign((size_t)1 << root, 0);
    uint16_t offs[17];
    offs[1] = 0;
    for (int l = 1; l < 16; ++l) offs[l + 1] = (uint16_t)(offs[l] + count[l]);
    if (maxLen > root) sorted.assign((size_t)nonzero, 0);
    uint32_t nextCode[16];
    {
      uint32_t code = 0;
      for (int l = 1; l < 16; ++l) {
        code = (code + count[l - 1]) << 1;
        nextCode[l] = code;
      }
    }
    for (int s = 0; s < n; ++s) {
      const int l = lengths[s];
      if (!l) continue;
      const uint32_t code = nextCode[l]++;
      if (maxLen > root) sorted[offs[l]] = (uint16_t)s;
      ++offs[l];
      if (l <= root) {
        uint32_t rev = 0;
        for (int b = 0; b < l; ++b) rev |= ((code >> b) & 1u) << (l - 1 - b);
        for (uint32_t idx = rev; idx < ((uint32_t)1 << root); idx += (1u << l))
          table[idx] = ((uint32_t)l << 16) | (uint32_t)s;
      }
    }
    return true;
  }

  int read(LBitReader& br) const {
    if (single) return singleSym;
    const uint32_t bits = br.peek(16);
    const uint32_t e = table[bits & ((1u << root) - 1)];
    if (e >> 16) {
      br.consume((int)(e >> 16));
      return (int)(e & 0xffff);
    }
    // slow path (codes longer than the root table)
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= maxLen; ++len) {
      code |= (int)((bits >> (len - 1)) & 1);
      const int cnt = count[len];
      if (code - cnt < first) {
        br.consume(len);
        return sorted[(size_t)(index + (code - first))];
      }
      index += cnt;
      first += cnt;
      first <<= 1;
      code <<= 1;
    }
    br.eos = true;
    return 0;
  }
};

struct HuffGroup { HuffCode c[5]; };  // green(+len+cache), red, blue, alpha, distance

struct LTransform {
  int type = 0;
  int bits = 0;
  int xsize = 0, ysize = 0;
  std::vector<uint32_t> data;
};

inline uint32_t lAddPixels(uint32_t a, uint32_t b) {
  return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) |
         (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}
inline uint32_t lAvg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }
inline int lClip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline uint32_t lSelect(uint32_t a, uint32_t b, uint32_t c) {
  // a = top, b = left, c = top-left
  int sum = 0;
  for (int s = 0; s < 32; s += 8) {
    const int pa = (int)((a >> s) & 0xff) - (int)((c >> s) & 0xff);
    const int pb = (int)((b >> s) & 0xff) - (int)((c >> s) & 0xff);
    sum += (pb < 0 ? -pb : pb) - (pa < 0 ? -pa : pa);
  }
  return sum <= 0 ? a : b;
}
inline uint32_t lClampAddSubFull(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t r = 0;
  for (int s = 0; s < 32; s += 8)
    r |= (uint32_t)lClip255((int)((a >> s) & 0xff) + (int)((b >> s) & 0xff) - (int)((c >> s) & 0xff)) << s;
  return r;
}
inline uint32_t lClampAddSubHalf(uint32_t a, uint32_t b) {
  uint32_t r = 0;
  for (int s = 0; s < 32; s += 8) {
    const int x = (int)((a >> s) & 0xff), y = (int)((b >> s) & 0xff);
    r |= (uint32_t)lClip255(x + (x - y) / 2) << s;
  }
  return r;
}
inline uint32_t lPredict(int mode, uint32_t L, const uint32_t* top) {
  // top[-1] = TL, top[0] = T, top[1] = TR
  switch (mode) {
    case 0: return 0xff000000u;
    case 1: return L;
    case 2: return top[0];
    case 3: return top[1];
    case 4: return top[-1];
    case 5: return lAvg2(lAvg2(L, top[1]), top[0]);
    case 6: return lAvg2(L, top[-1]);
    case 7: return lAvg2(L, top[0]);
    case 8: return lAvg2(top[-1], top[0]);
    case 9: return lAvg2(top[0], top[1]);
    case 10: return lAvg2(lAvg2(L, top[-1]), lAvg2(top[0], top[1]));
    case 11: return lSelect(top[0], L, top[-1]);
    case 12: return lClampAddSubFull(L, top[0], top[-1]);
    case 13: return lClampAddSubHalf(lAvg2(L, top[0]), top[-1]);
    default: return 0xff000000u;
  }
}

struct LosslessDecoder {
  LBitReader br;

  static int subSize(int size, int bits) { return (int)(((int64_t)size + (1 << bits) - 1) >> bits); }

  static int copyValue(LBitReader& r, int sym) {
    if (sym < 4) return sym + 1;
    const int extra = (sym - 2) >> 1;
    const int offset = (2 + (sym & 1)) << extra;
    return offset + (int)r.read(extra) + 1;
  }

  bool readHuffCode(HuffCode& hc, int alphabet) {
    std::vector<uint8_t> lengths((size_t)alphabet, 0);
    if (br.read(1)) {  // simple code
      const int numSym = (int)br.read(1) + 1;
      const int firstBits = (int)br.read(1);
      const int s0 = (int)br.read(firstBits ? 8 : 1);
      if (s0 >= alphabet) return false;
      lengths[(size_t)s0] = 1;
      if (numSym == 2) {
        const int s1 = (int)br.read(8);
        if (s1 >= alphabet) return false;
        lengths[(size_t)s1] = 1;
      }
    } else {
      static const uint8_t order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
      uint8_t cl[19];
      std::memset(cl, 0, sizeof(cl));
      const int numCodes = (int)br.read(4) + 4;
      for (int i = 0; i < numCodes; ++i) cl[order[i]] = (uint8_t)br.read(3);
      if (br.eos) return false;
      HuffCode lc;
      if (!lc.build(cl, 19)) return false;
      int maxSym;
      if (br.read(1)) {
        const int lenBits = 2 + 2 * (int)br.read(3);
        maxSym = 2 + (int)br.read(lenBits);
      } else {
        maxSym = alphabet;
      }
      int sym = 0, prev = 8;
      while (sym < alphabet) {
        if (maxSym-- == 0) break;
        if (br.eos) return false;
        const int cs = lc.read(br);
        if (cs < 16) {
          lengths[(size_t)sym++] = (uint8_t)cs;
          if (cs) prev = cs;
        } else {
          const int extra = (cs == 16) ? 2 : (cs == 17) ? 3 : 7;
          const int base = (cs == 16) ? 3 : (cs == 17) ? 3 : 11;
          const int rep = (int)br.read(extra) + base;
          if (sym + rep > alphabet) return false;
          const uint8_t v = (cs == 16) ? (uint8_t)prev : 0;
          for (int i = 0; i < rep; ++i) lengths[(size_t)sym++] = v;
        }
      }
    }
    if (br.eos) return false;
    return hc.build(lengths.data(), alphabet);
  }

  // Decode an entropy coded image of xs*ys pixels.
  bool decodeImageStream(int xs, int ys, bool level0, std::vector<uint32_t>& out) {
    std::vector<LTransform> transforms;
    int curX = xs;
    if (level0) {
      uint32_t seen = 0;
      while (br.read(1)) {
        if (br.eos) return false;
        const int type = (int)br.read(2);
        if (seen & (1u << type)) return false;
        seen |= 1u << type;
        LTransform t;
        t.type = type;
        t.xsize = curX;
        t.ysize = ys;
        if (type == 0 || type == 1) {
          t.bits = (int)br.read(3) + 2;
          if (!decodeImageStream(subSize(curX, t.bits), subSize(ys, t.bits), false, t.data)) return false;
        } else if (type == 3) {
          const int num = (int)br.read(8) + 1;
          t.bits = num > 16 ? 0 : num > 4 ? 1 : num > 2 ? 2 : 3;
          if (!decodeImageStream(num, 1, false, t.data)) return false;
          t.data.resize(256, 0);  // out-of-range index -> transparent black (rest stays zero)
          for (int i = num; i < 256; ++i) t.data[(size_t)i] = 0;
          for (int i = 1; i < num; ++i) t.data[(size_t)i] = lAddPixels(t.data[(size_t)i], t.data[(size_t)i - 1]);
          curX = subSize(curX, t.bits);
        }
        transforms.push_back(std::move(t));
      }
    }
    if (br.eos) return false;

    int cacheBits = 0;
    if (br.read(1)) {
      cacheBits = (int)br.read(4);
      if (cacheBits < 1 || cacheBits > 11) return false;
    }

    int metaBits = 0;
    std::vector<uint32_t> meta;
    int numGroups = 1;
    int metaW = 0;
    if (level0 && br.read(1)) {
      metaBits = (int)br.read(3) + 2;
      metaW = subSize(curX, metaBits);
      if (!decodeImageStream(metaW, subSize(ys, metaBits), false, meta)) return false;
      for (size_t i = 0; i < meta.size(); ++i) {
        meta[i] = (meta[i] >> 8) & 0xffff;
        if ((int)meta[i] + 1 > numGroups) numGroups = (int)meta[i] + 1;
      }
    }

    // Huffman groups; only the used ones are kept.
    std::vector<int> mapping;
    int usedGroups = numGroups;
    if (!meta.empty()) {
      mapping.assign((size_t)numGroups, -1);
      usedGroups = 0;
      for (size_t i = 0; i < meta.size(); ++i)
        if (mapping[meta[i]] < 0) mapping[meta[i]] = usedGroups++;
    }
    std::vector<HuffGroup> groups((size_t)usedGroups);
    {
      const int cacheSize = cacheBits ? (1 << cacheBits) : 0;
      const int alpha[5] = {256 + 24 + cacheSize, 256, 256, 256, 40};
      HuffGroup scratch;
      for (int g = 0; g < numGroups; ++g) {
        int slot = 0;
        bool keep = true;
        if (!mapping.empty()) {
          if (mapping[(size_t)g] < 0) keep = false;
          else slot = mapping[(size_t)g];
        }
        HuffGroup& hg = keep ? groups[(size_t)slot] : scratch;
        for (int k = 0; k < 5; ++k)
          if (!readHuffCode(hg.c[k], alpha[k])) return false;
      }
    }

    // pixels
    std::vector<uint32_t> cache;
    if (cacheBits) cache.assign((size_t)1 << cacheBits, 0);
    const size_t total = (size_t)curX * (size_t)ys;
    out.assign(total, 0);
    const int mask = metaBits ? ((1 << metaBits) - 1) : ~0;
    size_t pos = 0, lastCached = 0;
    int x = 0, y = 0;
    const HuffGroup* hg = &groups[0];
    auto groupAt = [&](int px, int py) -> const HuffGroup* {
      if (meta.empty()) return &groups[0];
      const uint32_t m = meta[(size_t)(py >> metaBits) * (size_t)metaW + (size_t)(px >> metaBits)];
      return &groups[(size_t)mapping[m]];
    };
    const int cacheShift = 32 - cacheBits;
    while (pos < total) {
      if ((x & mask) == 0) hg = groupAt(x, y);
      if (br.eos) return false;
      const int code = hg->c[0].read(br);
      if (code < 256) {
        const uint32_t r = (uint32_t)hg->c[1].read(br);
        const uint32_t b = (uint32_t)hg->c[2].read(br);
        const uint32_t a = (uint32_t)hg->c[3].read(br);
        out[pos++] = (a << 24) | (r << 16) | ((uint32_t)code << 8) | b;
        if (++x >= curX) { x = 0; ++y; }
      } else if (code < 256 + 24) {
        const int len = copyValue(br, code - 256);
        const int dsym = hg->c[4].read(br);
        const int dcode = copyValue(br, dsym);
        int64_t dist;
        if (dcode > 120) {
          dist = dcode - 120;
        } else {
          const int dc = kCodeToPlane[dcode - 1];
          const int yoff = dc >> 4, xoff = 8 - (dc & 0xf);
          dist = (int64_t)yoff * curX + xoff;
          if (dist < 1) dist = 1;
        }
        if (br.eos || (int64_t)pos < dist || (int64_t)(total - pos) < (int64_t)len) return false;
        for (int i = 0; i < len; ++i) { out[pos] = out[pos - (size_t)dist]; ++pos; }
        x += len;
        while (x >= curX) { x -= curX; ++y; }
        if (pos < total && (x & mask)) hg = groupAt(x, y);
      } else if (cacheBits && code < 256 + 24 + (1 << cacheBits)) {
        while (lastCached < pos) {
          const uint32_t v = out[lastCached++];
          cache[(size_t)((v * 0x1e35a7bdu) >> cacheShift)] = v;
        }
        out[pos++] = cache[(size_t)(code - 256 - 24)];
        if (++x >= curX) { x = 0; ++y; }
      } else {
        return false;
      }
    }
    if (br.eos) return false;

    if (level0) return applyTransforms(transforms, xs, ys, out);
    return true;
  }

  static bool applyTransforms(std::vector<LTransform>& tr, int xs, int ys, std::vector<uint32_t>& img) {
    for (size_t ti = tr.size(); ti-- > 0;) {
      LTransform& t = tr[ti];
      const int w = t.xsize, h = t.ysize;
      switch (t.type) {
        case 2:  // subtract green
          for (size_t i = 0; i < img.size(); ++i) {
            const uint32_t p = img[i];
            const uint32_t g = (p >> 8) & 0xff;
            const uint32_t rb = (((p & 0x00ff00ffu) + ((g << 16) | g)) & 0x00ff00ffu);
            img[i] = (p & 0xff00ff00u) | rb;
          }
          break;
        case 0: {  // predictor
          const int tw = subSize(w, t.bits);
          for (int y = 0; y < h; ++y) {
            uint32_t* row = img.data() + (size_t)y * w;
            if (y == 0) {
              row[0] = lAddPixels(row[0], 0xff000000u);
              for (int x = 1; x < w; ++x) row[x] = lAddPixels(row[x], row[x - 1]);
              continue;
            }
            const uint32_t* up = row - w;
            row[0] = lAddPixels(row[0], up[0]);
            for (int x = 1; x < w;) {
              const int mode = (int)((t.data[(size_t)(y >> t.bits) * tw + (size_t)(x >> t.bits)] >> 8) & 0xf);
              int end = ((x >> t.bits) + 1) << t.bits;
              if (end > w) end = w;
              for (; x < end; ++x) {
                // top-right of the last pixel in a row is the first pixel of the current row
                row[x] = lAddPixels(row[x], lPredict(mode, row[x - 1], up + x));
              }
            }
          }
          break;
        }
        case 1: {  // cross color
          const int tw = subSize(w, t.bits);
          for (int y = 0; y < h; ++y) {
            uint32_t* row = img.data() + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
              const uint32_t cp = t.data[(size_t)(y >> t.bits) * tw + (size_t)(x >> t.bits)];
              const int g2r = (int8_t)(cp & 0xff), g2b = (int8_t)((cp >> 8) & 0xff),
                        r2b = (int8_t)((cp >> 16) & 0xff);
              const uint32_t p = row[x];
              const int green = (int8_t)((p >> 8) & 0xff);
              int red = (int)((p >> 16) & 0xff), blue = (int)(p & 0xff);
              red += (g2r * green) >> 5;
              red &= 0xff;
              blue += (g2b * green) >> 5;
              blue += (r2b * (int8_t)red) >> 5;
              blue &= 0xff;
              row[x] = (p & 0xff00ff00u) | ((uint32_t)red << 16) | (uint32_t)blue;
            }
          }
          break;
        }
        case 3: {  // color indexing
          std::vector<uint32_t> out((size_t)w * h);
          const int pw = subSize(w, t.bits);
          const int bpp = 8 >> t.bits;
          const uint32_t m = (1u << bpp) - 1;
          for (int y = 0; y < h; ++y) {
            const uint32_t* src = img.data() + (size_t)y * pw;
            uint32_t* dst = out.data() + (size_t)y * w;
            if (t.bits == 0) {
              for (int x = 0; x < w; ++x) dst[x] = t.data[(src[x] >> 8) & 0xff];
            } else {
              for (int x = 0; x < w; ++x) {
                const uint32_t packed = (src[x >> t.bits] >> 8) & 0xff;
                dst[x] = t.data[(packed >> (bpp * (x & ((1 << t.bits) - 1)))) & m];
              }
            }
          }
          img.swap(out);
          break;
        }
        default: return false;
      }
    }
    (void)xs; (void)ys;
    return true;
  }
};

// Decode a full 'VP8L' chunk payload into ARGB.
inline bool decodeVp8l(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint32_t>& argb) {
  if (n < 5 || d[0] != 0x2f) return false;
  const uint32_t bits = d[1] | (d[2] << 8) | (d[3] << 16) | ((uint32_t)d[4] << 24);
  w = (int)(bits & 0x3fff) + 1;
  h = (int)((bits >> 14) & 0x3fff) + 1;
  if ((bits >> 29) != 0) return false;  // version
  if ((int64_t)w * h > kMaxPixels) return false;
  LosslessDecoder dec;
  dec.br.init(d + 5, n - 5);
  return dec.decodeImageStream(w, h, true, argb);
}

inline bool vp8lDims(const uint8_t* d, size_t n, int& w, int& h) {
  if (n < 5 || d[0] != 0x2f) return false;
  const uint32_t bits = d[1] | (d[2] << 8) | (d[3] << 16) | ((uint32_t)d[4] << 24);
  if ((bits >> 29) != 0) return false;
  w = (int)(bits & 0x3fff) + 1;
  h = (int)((bits >> 14) & 0x3fff) + 1;
  return true;
}

// ---------------------------------------------------------------------------------------
// Alpha plane
// ---------------------------------------------------------------------------------------
inline void alphaUnfilter(int filter, uint8_t* a, int w, int h) {
  if (filter == 0) return;
  for (int y = 0; y < h; ++y) {
    uint8_t* row = a + (size_t)y * w;
    const uint8_t* prev = y ? row - w : nullptr;
    if (!prev || filter == 1) {
      uint8_t pred = prev ? prev[0] : 0;
      for (int i = 0; i < w; ++i) { row[i] = (uint8_t)(row[i] + pred); pred = row[i]; }
    } else if (filter == 2) {
      for (int i = 0; i < w; ++i) row[i] = (uint8_t)(row[i] + prev[i]);
    } else {  // gradient
      int top = prev[0], topLeft = top, left = top;
      for (int i = 0; i < w; ++i) {
        top = prev[i];
        left = (uint8_t)(row[i] + lClip255(left + top - topLeft));
        topLeft = top;
        row[i] = (uint8_t)left;
      }
    }
  }
}

// Returns false if the ALPH chunk is unusable.
inline bool decodeAlpha(const uint8_t* d, size_t n, int w, int h, std::vector<uint8_t>& alpha) {
  if (n < 1) return false;
  const int comp = d[0] & 3, filter = (d[0] >> 2) & 3;
  const size_t count = (size_t)w * h;
  alpha.assign(count, 0);
  if (comp == 0) {
    if (n - 1 < count) return false;
    std::memcpy(alpha.data(), d + 1, count);
  } else if (comp == 1) {
    LosslessDecoder dec;
    dec.br.init(d + 1, n - 1);
    std::vector<uint32_t> argb;
    if (!dec.decodeImageStream(w, h, true, argb) || argb.size() != count) return false;
    for (size_t i = 0; i < count; ++i) alpha[i] = (uint8_t)(argb[i] >> 8);
  } else {
    return false;
  }
  alphaUnfilter(filter, alpha.data(), w, h);
  return true;
}

// ---------------------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------------------
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline bool tagIs(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }

struct Parsed {
  bool vp8x = false, lossless = false;
  int canvasW = 0, canvasH = 0;
  const uint8_t* img = nullptr;
  size_t imgSize = 0;
  const uint8_t* alph = nullptr;
  size_t alphSize = 0;
};

inline bool parseContainer(const uint8_t* d, size_t n, Parsed& P) {
  if (n < 12 || !tagIs(d, "RIFF") || !tagIs(d + 8, "WEBP")) return false;
  const uint32_t riffSize = rd32(d + 4);
  if (riffSize >= 4 && (uint64_t)riffSize + 8 < n) n = (size_t)riffSize + 8;
  size_t pos = 12;
  while (n - pos >= 8) {
    const uint8_t* tag = d + pos;
    const uint32_t sz = rd32(d + pos + 4);
    const size_t payload = pos + 8;
    const size_t avail = n - payload;
    const size_t csz = sz < avail ? (size_t)sz : avail;
    if (tagIs(tag, "VP8X")) {
      if (csz < 10) return false;
      if (d[payload] & 0x02) return false;  // animation not supported
      P.vp8x = true;
      P.canvasW = (int)((d[payload + 4] | (d[payload + 5] << 8) | (d[payload + 6] << 16)) + 1);
      P.canvasH = (int)((d[payload + 7] | (d[payload + 8] << 8) | (d[payload + 9] << 16)) + 1);
    } else if (tagIs(tag, "ALPH")) {
      P.alph = d + payload;
      P.alphSize = csz;
    } else if (tagIs(tag, "VP8 ") || tagIs(tag, "VP8L")) {
      P.lossless = tagIs(tag, "VP8L");
      P.img = d + payload;
      P.imgSize = csz;
      return true;
    }
    const uint64_t next = (uint64_t)payload + sz + (sz & 1);
    if (next >= n) break;
    pos = (size_t)next;
  }
  return false;
}

}  // namespace webpdec_impl

// ---------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------
inline bool webpIsWebp(const uint8_t* d, size_t n) {
  return d && n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0;
}

inline bool webpInfo(const uint8_t* d, size_t n, int& w, int& h) {
  using namespace webpdec_impl;
  if (!webpIsWebp(d, n)) return false;
  Parsed P;
  if (!parseContainer(d, n, P)) return false;
  int iw = 0, ih = 0;
  if (P.lossless) {
    if (!vp8lDims(P.img, P.imgSize, iw, ih)) return false;
  } else {
    if (!Vp8Decoder::readDims(P.img, P.imgSize, iw, ih)) return false;
  }
  if (P.vp8x && (P.canvasW != iw || P.canvasH != ih)) return false;
  w = iw;
  h = ih;
  return true;
}

inline bool webpDecode(const uint8_t* d, size_t n, int& w, int& h, std::vector<uint8_t>& rgba) {
  using namespace webpdec_impl;
  if (!webpIsWebp(d, n)) return false;
  Parsed P;
  if (!parseContainer(d, n, P)) return false;
  int iw = 0, ih = 0;
  if (P.lossless) {
    if (!vp8lDims(P.img, P.imgSize, iw, ih)) return false;
  } else {
    if (!Vp8Decoder::readDims(P.img, P.imgSize, iw, ih)) return false;
  }
  if (P.vp8x && (P.canvasW != iw || P.canvasH != ih)) return false;
  if ((int64_t)iw * ih > kMaxPixels) return false;

  std::vector<uint8_t> out;
  if (P.lossless) {
    std::vector<uint32_t> argb;
    int lw, lh;
    if (!decodeVp8l(P.img, P.imgSize, lw, lh, argb)) return false;
    out.resize((size_t)lw * lh * 4);
    for (size_t i = 0; i < argb.size(); ++i) {
      const uint32_t p = argb[i];
      out[i * 4 + 0] = (uint8_t)(p >> 16);
      out[i * 4 + 1] = (uint8_t)(p >> 8);
      out[i * 4 + 2] = (uint8_t)p;
      out[i * 4 + 3] = (uint8_t)(p >> 24);
    }
  } else {
    Vp8Decoder dec;
    if (!dec.decode(P.img, P.imgSize)) return false;
    out.resize((size_t)iw * ih * 4);
    convertFrameToRgba(dec, out.data());
    if (P.alph) {
      std::vector<uint8_t> alpha;
      if (decodeAlpha(P.alph, P.alphSize, iw, ih, alpha)) {
        const size_t cnt = (size_t)iw * ih;
        for (size_t i = 0; i < cnt; ++i) out[i * 4 + 3] = alpha[i];
      }
    }
  }
  w = iw;
  h = ih;
  rgba.swap(out);
  return true;
}

#endif  // WEBPDEC_HPP_INCLUDED
