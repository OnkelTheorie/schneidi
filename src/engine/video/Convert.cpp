// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Convert.h"
#include "engine/video/Compose.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QByteArray>

#include <algorithm>
#include <atomic>
#include <vector>

namespace Video {

namespace {

bool yuvj(AVPixelFormat f)
{
    return f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ411P || f == AV_PIX_FMT_YUVJ422P || f == AV_PIX_FMT_YUVJ444P
           || f == AV_PIX_FMT_YUVJ440P;
}

// The range is part of the swscale format. A yuvj format is full range even without the tag (MJPEG in AVI/MOV: the
// demuxer does not know the range, only the decoder's format tells)
AVPixelFormat withRange(AVPixelFormat f, bool full)
{
    if (yuvj(f)) return f;
    switch (f) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P: return full ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUV411P:
    case AV_PIX_FMT_YUVJ411P: return full ? AV_PIX_FMT_YUVJ411P : AV_PIX_FMT_YUV411P;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P: return full ? AV_PIX_FMT_YUVJ422P : AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P: return full ? AV_PIX_FMT_YUVJ444P : AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUV440P:
    case AV_PIX_FMT_YUVJ440P: return full ? AV_PIX_FMT_YUVJ440P : AV_PIX_FMT_YUV440P;
    default: return f;
    }
}

// swscale flags by conversion kind (same size: point sampling where no interpolation is needed)
int swsFlags(int sw, int sh, AVPixelFormat src, int dw, int dh, AVPixelFormat dst)
{
    int flags = SWS_BICUBIC | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND;
    if (sw != dw || sh != dh) return flags;
    const AVPixFmtDescriptor* s = av_pix_fmt_desc_get(src);
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(dst);
    if (!s || !d) return flags;
    const bool srgb = s->flags & AV_PIX_FMT_FLAG_RGB, drgb = d->flags & AV_PIX_FMT_FLAG_RGB;
    if (srgb && !drgb) return SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (!srgb && drgb) return SWS_POINT | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;
    if (!srgb && !drgb) {
        if (s->log2_chroma_w == d->log2_chroma_w && s->log2_chroma_h == d->log2_chroma_h)
            return SWS_POINT | SWS_ACCURATE_RND;
        return SWS_BILINEAR | SWS_ACCURATE_RND;
    }
    return flags;
}

// The matrix of a colorspace number. Never the context's current table: the contexts are cached per thread, so that
// table is whatever the previous conversion set (a BT.2020 picture decoded with 601 or 709 depending on the order).
const int* coefficients(int colorspace)
{
    return sws_getCoefficients(swsColorspace(colorspace));
}

// The matrices of both sides by colorspace number and the source's range, set for every conversion (like the
// matrix, a range set on a cached context stays for the next picture). srcRange: 0/1 for a YUV source, -1 = the
// context's (RGB sources). The destination's range is the context's (RGB: full).
int lumaTransfer(SwsContext* ctx, int srcColorspace, int dstColorspace, int srcRange)
{
    int* src = nullptr;
    int* dst = nullptr;
    int range = 0, dstRange = 0, brightness = 0, contrast = 1 << 16, saturation = 1 << 16;
    sws_getColorspaceDetails(ctx, &src, &range, &dst, &dstRange, &brightness, &contrast, &saturation);
    return sws_setColorspaceDetails(ctx, coefficients(srcColorspace), srcRange < 0 ? range : srcRange,
                                    coefficients(dstColorspace), dstRange, brightness, contrast, saturation);
}

// lumaTransfer's srcRange for a source format: YUV tagged full range or yuvj -> 1 (formats without a yuvj twin, such
// as 10 bit, were read as video range), other YUV 0, RGB/grey -1
int sourceRange(AVPixelFormat f, bool tagged)
{
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
    if (!d || (d->flags & AV_PIX_FMT_FLAG_RGB) || d->nb_components < 3) return -1;
    return tagged || yuvj(f) ? 1 : 0;
}

// Per thread: a context per conversion (sws_getCachedContext reuses it while the parameters stay the same); slot =
// another context for a second pass in the same thread
SwsContext* cachedContext(int w, int h, AVPixelFormat src, AVPixelFormat dst, int flags, int dw = 0, int dh = 0,
                          int slot = 0)
{
    struct Holder {
        SwsContext* ctx = nullptr;
        ~Holder() { sws_freeContext(ctx); }
    };
    thread_local Holder holders[2];
    Holder& holder = holders[slot];
    holder.ctx = sws_getCachedContext(holder.ctx, w, h, src, dw ? dw : w, dh ? dh : h, dst, flags, nullptr, nullptr, nullptr);
    return holder.ctx;
}

// Scaling: bilinear in good quality
constexpr int kScaleFlags = SWS_BILINEAR | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND;

// swscale's unscaled YUV -> RGB converters (SIMD on aarch64 and x86) take these, and only without SWS_ACCURATE_RND
bool fastRgbSource(AVPixelFormat f)
{
    return f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV422P || f == AV_PIX_FMT_YUVJ422P
           || f == AV_PIX_FMT_NV12;
}

// Preview: YUV -> RGB(A), first the planes scaled (YUV -> YUV, SIMD), then converted unscaled without accurate
// rounding (the SIMD converter: nearest chroma, ±1 step). With SWS_ACCURATE_RND and SWS_FULL_CHR_H_INT the combined
// pass runs in C code in one thread: 80 ms for 1080p -> 540p on a Raspberry Pi Zero 2 W, 3 ms so.
bool convertFast(const AVFrame* frame, AVPixelFormat src, uint8_t** outData, int* outStride, AVPixelFormat dst,
                 int width, int height, int dw, int dh, int flags, int colorspace, bool fullRange)
{
    const uint8_t* const* inData = frame->data;
    const int* inStride = frame->linesize;
    uint8_t* midData[4] = {};
    int midStride[4] = {};
    if (dw != width || dh != height) {
        thread_local std::vector<uint8_t> mid;
        const int size = av_image_get_buffer_size(src, dw, dh, 32);
        if (size <= 0) return false;
        mid.resize(size_t(size));
        av_image_fill_arrays(midData, midStride, mid.data(), src, dw, dh, 32);
        SwsContext* scale = cachedContext(width, height, src, src, flags & ~SWS_ACCURATE_RND, dw, dh, 1);
        if (!scale) return false;
        sws_scale(scale, frame->data, frame->linesize, 0, height, midData, midStride);
        inData = midData;
        inStride = midStride;
    }
    SwsContext* ctx = cachedContext(dw, dh, src, dst, SWS_POINT);
    if (!ctx) return false;
    lumaTransfer(ctx, colorspace, 601, sourceRange(src, fullRange)); // swscale's RGB side is SWS_CS_DEFAULT (601)
    // The SIMD converter writes whole blocks of pixels per row (8-32): with a width that is no multiple, the last
    // row runs past the end of a tightly packed picture (heap corruption at preview sizes like 476 px). Then into a
    // padded scratch picture and copied.
    if (dw % 32 != 0) {
        const int pixelBytes = av_get_padded_bits_per_pixel(av_pix_fmt_desc_get(dst)) / 8;
        const int bytes = dw * pixelBytes;
        const int stride = FFALIGN(dw, 32) * pixelBytes + 64;
        thread_local std::vector<uint8_t> scratch;
        scratch.resize(size_t(stride) * (dh + 1));
        uint8_t* tmp[4] = {scratch.data()};
        const int tmpStride[4] = {stride};
        sws_scale(ctx, inData, inStride, 0, dh, tmp, tmpStride);
        av_image_copy_plane(outData[0], outStride[0], scratch.data(), stride, bytes, dh);
        return true;
    }
    sws_scale(ctx, inData, inStride, 0, dh, outData, outStride);
    return true;
}

// Converted and scaled to RGBA in one pass (progressive); false = not possible
bool convertScaled(const Picture& pic, uint8_t* buffer, int width, int height, int dw, int dh, const Info& info,
                   bool fast)
{
    const AVFrame* frame = pic.frame;
    const AVPixelFormat src = withRange(AVPixelFormat(frame->format), info.fullRange);
    uint8_t* outData[4] = {};
    int outStride[4] = {};
    av_image_fill_arrays(outData, outStride, buffer, AV_PIX_FMT_RGBA, dw, dh, 1);
    if (fast && fastRgbSource(src))
        return convertFast(frame, src, outData, outStride, AV_PIX_FMT_RGBA, width, height, dw, dh, kScaleFlags,
                           info.colorspace, info.fullRange);
    SwsContext* ctx = cachedContext(width, height, src, AV_PIX_FMT_RGBA, kScaleFlags, dw, dh);
    if (!ctx) return false;
    // swscale's RGB side is SWS_CS_DEFAULT (601). RGB to RGB goes through YUV inside swscale: the same matrix both
    // ways, or the colors shift
    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(src);
    const bool rgbSource = srcDesc && (srcDesc->flags & AV_PIX_FMT_FLAG_RGB);
    lumaTransfer(ctx, rgbSource ? 601 : info.colorspace, 601, sourceRange(src, info.fullRange));
    sws_scale(ctx, frame->data, frame->linesize, 0, height, outData, outStride);
    return true;
}

// Unscaled conversion to RGBA (interlaced 4:2:0 field by field)
void convert(const Picture& pic, uint8_t* buffer, int width, int height, const Info& info, bool fast)
{
    const AVFrame* frame = pic.frame;
    const auto pixFmt = AVPixelFormat(frame->format);
    const bool interlaced = !pic.progressive;
    const AVPixelFormat src = withRange(pixFmt, info.fullRange);
    const AVPixelFormat dst = AV_PIX_FMT_RGBA;
    uint8_t* outData[4] = {};
    int outStride[4] = {};
    const int flags = swsFlags(width, height, src, width, height, dst);
    av_image_fill_arrays(outData, outStride, buffer, dst, width, height, 1);
    // Field by field (limited or full range: withRange makes full-range yuv420p yuvj420p)
    if ((src == AV_PIX_FMT_YUV420P || src == AV_PIX_FMT_YUVJ420P) && interlaced) {
        const int fieldHeight = height / 2;
        SwsContext* ctx = cachedContext(width, fieldHeight, src, dst, flags);
        if (!ctx) return;
        lumaTransfer(ctx, info.colorspace, 601, sourceRange(src, info.fullRange)); // RGB side: SWS_CS_DEFAULT (601)
        const uint8_t* inData[4];
        int inStride[4];
        for (int i = 0; i < 4; ++i) {
            inData[i] = frame->data[i];
            inStride[i] = frame->linesize[i] * 2;
            outStride[i] *= 2;
        }
        sws_scale(ctx, inData, inStride, 0, fieldHeight, outData, outStride);
        for (int i = 0; i < 4; ++i) {
            inData[i] += inStride[i] / 2;
            outData[i] += outStride[i] / 2;
        }
        sws_scale(ctx, inData, inStride, 0, fieldHeight, outData, outStride);
    } else if (!fast || !fastRgbSource(src)
               || !convertFast(frame, src, outData, outStride, dst, width, height, width, height, flags,
                               info.colorspace, info.fullRange)) {
        SwsContext* ctx = cachedContext(width, height, src, dst, flags);
        if (!ctx) return;
        lumaTransfer(ctx, info.colorspace, 601, sourceRange(src, info.fullRange));
        sws_scale(ctx, frame->data, frame->linesize, 0, height, outData, outStride);
    }
}

} // namespace

int swsColorspace(int colorspace)
{
    switch (colorspace) {
    case 170:
    case 470:
    case 601:
    case 624: return SWS_CS_ITU601;
    case 240: return SWS_CS_SMPTE240M;
    case 709: return SWS_CS_ITU709;
    case 2020:
    case 2021: return SWS_CS_BT2020;
    default: return SWS_CS_DEFAULT;
    }
}

bool toRgba(const Picture& pic, const Info& info, uint8_t* out, int dw, int dh, bool fast)
{
    if (!pic.frame || dw <= 0 || dh <= 0) return false;
    const int w = pic.frame->width, h = pic.frame->height;
    if (dw == w && dh == h) {
        // Planar YUV, progressive, exact conversion: in horizontal bands over threads (point sampling at the same
        // size, so every band converts on its own). The preview's SIMD path is fast already.
        const AVPixelFormat src = withRange(AVPixelFormat(pic.frame->format), info.fullRange);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src);
        const bool planarYuv = desc && (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && !(desc->flags & AV_PIX_FMT_FLAG_RGB)
                               && desc->nb_components == 3 && desc->comp[0].depth == 8;
        if (planarYuv && pic.progressive && !(fast && fastRgbSource(src)) && h >= 128) {
            const int flags = swsFlags(w, h, src, w, h, AV_PIX_FMT_RGBA);
            const int bands = std::min(8, h / 64);
            const int step = ((h + bands - 1) / bands + 1) & ~1; // even: chroma rows stay whole
            std::atomic<bool> ok{true};
            Compose::forRows(bands, 1, [&](int first, int last) {
                for (int b = first; b < last; ++b) {
                    const int y0 = b * step, y1 = std::min(h, y0 + step);
                    if (y0 >= y1) continue;
                    SwsContext* ctx = cachedContext(w, y1 - y0, src, AV_PIX_FMT_RGBA, flags, 0, 0, y1 - y0 == step ? 0 : 1);
                    if (!ctx) {
                        ok = false;
                        continue;
                    }
                    lumaTransfer(ctx, info.colorspace, 601, sourceRange(src, info.fullRange));
                    const uint8_t* in[4] = {};
                    int inStride[4] = {};
                    for (int i = 0; i < 3; ++i) {
                        const int sub = i == 0 ? 0 : desc->log2_chroma_h;
                        in[i] = pic.frame->data[i] + (y0 >> sub) * pic.frame->linesize[i];
                        inStride[i] = pic.frame->linesize[i];
                    }
                    uint8_t* o[4] = {out + size_t(y0) * w * 4};
                    const int oStride[4] = {w * 4};
                    sws_scale(ctx, in, inStride, 0, y1 - y0, o, oStride);
                }
            });
            if (ok) return true;
        }
        convert(pic, out, w, h, info, fast);
        return true;
    }
    if (!pic.progressive) { // field-aware conversion at full size, then scaled
        std::vector<uint8_t> full(size_t(w) * h * 4);
        convert(pic, full.data(), w, h, info, fast);
        SwsContext* ctx = cachedContext(w, h, AV_PIX_FMT_RGBA, AV_PIX_FMT_RGBA, kScaleFlags, dw, dh, 1);
        if (!ctx) return false;
        const uint8_t* in[4] = {full.data()};
        const int inStride[4] = {w * 4};
        uint8_t* o[4] = {out};
        const int oStride[4] = {dw * 4};
        sws_scale(ctx, in, inStride, 0, h, o, oStride);
        return true;
    }
    return convertScaled(pic, out, w, h, dw, dh, info, fast);
}

} // namespace Video
