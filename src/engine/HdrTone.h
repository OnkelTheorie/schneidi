// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// HDR -> SDR: pictures with an HDR transfer (HLG, PQ; mostly BT.2020 primaries, as phones record them) shown and
// exported as BT.709 SDR. Our own calculation (no zscale/libplacebo: the bundled LGPL FFmpeg has neither): the CPU
// applies it per pixel (Video::toRgba, 16-bit input, tables), the card in its layer shader (shaders/layer.frag
// hdrToSdr, the same steps in floating point; keep both alike). A 3D LUT was tried first: trilinear between 65 nodes
// it missed saturated colours by up to 120 code values (the SDR gamma is steep near black).
//
// Steps: signal -> display light in nits (HLG: inverse OETF + OOTF for a 1000-nit display, PQ: EOTF) -> BT.2020 to
// BT.709 primaries in linear light -> relative to HDR reference white (203 nits, BT.2408; HLG 75 % lands there) ->
// tone curve on the luminance (linear up to a knee, then a soft shoulder up to the peak) -> out-of-gamut colours
// pulled towards their luminance -> BT.1886 (gamma 2.4) inverse, the SDR display's code values.

#include <cstdint>

namespace HdrTone {

enum class Transfer : uint8_t { None = 0, Hlg = 1, Pq = 2 };

// The HDR transfer of an AVColorTransferCharacteristic (Video::Info::colorTrc): ARIB STD-B67 -> Hlg, SMPTE ST 2084 ->
// Pq, anything else None (SDR)
Transfer ofColorTrc(int colorTrc);
// The tone of a stream or frame by its FFmpeg tags (color_trc, color_primaries, colorspace numbers); primaries not
// stated count as BT.2020 when the matrix is
struct Tone;
Tone ofFrame(int colorTrc, int colorPrimaries, int colorSpace);
// "HLG", "PQ" or empty (for probe/info/the Media Pool)
const char* name(Transfer t);

// The tone mapping of a source: transfer, and whether its primaries are BT.2020 (else BT.709 is assumed)
struct Tone {
    Transfer transfer = Transfer::None;
    bool bt2020 = true;
    bool active() const { return transfer != Transfer::None; }
    bool operator==(const Tone&) const = default;
};

inline constexpr double kReferenceWhite = 203; // nits: HDR reference white = SDR 100 %
inline constexpr double kPeak = 1000;          // nits: HLG's nominal display; PQ without metadata is assumed mastered so

// One colour: signal R'G'B' 0..1 (after the Y'CbCr matrix, in the source's primaries) -> SDR BT.709 R'G'B' 0..1
void toSdr(const Tone& tone, const float in[3], float out[3]);

// Rows of RGBA64 (16 bits per channel, native endianness, as swscale's AV_PIX_FMT_RGBA64) -> RGBA8, alpha scaled.
// Tables are made once per transfer; threads by Compose::forRows (`threads` 0 = a few for big pictures).
void toSdrRgba64(const Tone& tone, const uint16_t* in, int inStride, uint8_t* out, int outStride, int width, int height,
                 int threads = 0);

// The same on an RGBA8 picture in place (8-bit signal, coarser; used where only 8 bits are at hand: stills of a
// thumbnail path, a picture the CPU made from an 8-bit proxy)
// bgra: blue first (QImage::Format_RGB32/ARGB32 on little-endian machines, AV_PIX_FMT_RGB32); stride 0 = width * 4
void toSdrRgba8(const Tone& tone, uint8_t* rgba, int width, int height, int threads = 0, bool bgra = false,
                int stride = 0);

} // namespace HdrTone
