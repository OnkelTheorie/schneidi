#include "engine/video/Producer.h"
#include "engine/video/Reader.h"

#include <Mlt.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QByteArray>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace Video {

namespace {

constexpr int kImageAlign = 1; // MLT's IMAGE_ALIGN

struct Data {
    std::unique_ptr<Reader> reader;
    Info info;
    double sourceFps = 0;
    int turns = 0;                 // quarter turns applied by the reader
    mlt_image_format native = mlt_image_yuv422;
    bool fast = false;             // preview: ProducerOptions::fastConvert
    std::atomic<int> progressive{-1}, topFieldFirst{-1}; // last values written to the producer
};

// ---- Image conversion: the same swscale calls as MLT's producer_avformat (7.30), so the pixels are identical ----

// MLT's pick_av_pixel_format: the range is part of the swscale format
AVPixelFormat withRange(AVPixelFormat f, bool full)
{
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

// MLT's pick_image_format: which MLT format the producer hands out for a request
mlt_image_format pickImageFormat(AVPixelFormat pix, mlt_image_format current)
{
    const bool rgbaSource = pix == AV_PIX_FMT_ARGB || pix == AV_PIX_FMT_RGBA || pix == AV_PIX_FMT_ABGR
                            || pix == AV_PIX_FMT_BGRA;
    if (current == mlt_image_none || current == mlt_image_movit || rgbaSource) {
        switch (pix) {
        case AV_PIX_FMT_ARGB:
        case AV_PIX_FMT_RGBA:
        case AV_PIX_FMT_ABGR:
        case AV_PIX_FMT_BGRA: return mlt_image_rgba;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVA420P: return mlt_image_yuv420p;
        case AV_PIX_FMT_RGB24:
        case AV_PIX_FMT_BGR24:
        case AV_PIX_FMT_GRAY8:
        case AV_PIX_FMT_MONOWHITE:
        case AV_PIX_FMT_MONOBLACK:
        case AV_PIX_FMT_RGB8:
        case AV_PIX_FMT_BGR8:
        case AV_PIX_FMT_BAYER_RGGB16LE: return mlt_image_rgb;
        case AV_PIX_FMT_YUV420P10LE: return mlt_image_yuv420p10;
        case AV_PIX_FMT_YUV422P10LE:
        case AV_PIX_FMT_YUV444P10LE: return mlt_image_yuv444p10;
        case AV_PIX_FMT_YUV422P16LE: return mlt_image_yuv422p16;
        default: current = mlt_image_yuv422;
        }
    }
    if (pix == AV_PIX_FMT_BAYER_RGGB16LE) return mlt_image_rgb;
    if (pix == AV_PIX_FMT_YUVA444P10LE || pix == AV_PIX_FMT_GBRAP10LE || pix == AV_PIX_FMT_GBRAP12LE)
        return mlt_image_rgba;
    return current;
}

// MLT's mlt_get_sws_flags (avformat/common.c)
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

const int* coefficients(int colorspace, const int* fallback)
{
    switch (colorspace) {
    case 170:
    case 470:
    case 601:
    case 624: return sws_getCoefficients(SWS_CS_ITU601);
    case 240: return sws_getCoefficients(SWS_CS_SMPTE240M);
    case 709: return sws_getCoefficients(SWS_CS_ITU709);
    default: return fallback;
    }
}

// MLT's mlt_set_luma_transfer. As there, the ranges come from the context (the pixel formats), not the arguments.
int lumaTransfer(SwsContext* ctx, int srcColorspace, int dstColorspace)
{
    int* src = nullptr;
    int* dst = nullptr;
    int srcRange = 0, dstRange = 0, brightness = 0, contrast = 1 << 16, saturation = 1 << 16;
    sws_getColorspaceDetails(ctx, &src, &srcRange, &dst, &dstRange, &brightness, &contrast, &saturation);
    return sws_setColorspaceDetails(ctx, coefficients(srcColorspace, src), srcRange, coefficients(dstColorspace, dst),
                                    dstRange, brightness, contrast, saturation);
}

AVPixelFormat avFormatOf(mlt_image_format f)
{
    switch (f) {
    case mlt_image_yuv420p: return AV_PIX_FMT_YUV420P;
    case mlt_image_yuv420p10: return AV_PIX_FMT_YUV420P10LE;
    case mlt_image_yuv444p10: return AV_PIX_FMT_YUV444P10LE;
    case mlt_image_yuv422p16: return AV_PIX_FMT_YUV422P16LE;
    case mlt_image_rgb: return AV_PIX_FMT_RGB24;
    case mlt_image_rgba: return AV_PIX_FMT_RGBA;
    default: return AV_PIX_FMT_NONE;
    }
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

// MLT's swscale filter (filter_scale): the interpolation the consumer asked for, in good quality
int scaleFlags(const char* interp)
{
    int flags = SWS_BILINEAR;
    const QByteArray i(interp ? interp : "");
    if (i == "nearest" || i == "neighbor") flags = SWS_POINT;
    else if (i == "tiles" || i == "fast_bilinear") flags = SWS_FAST_BILINEAR;
    else if (i == "bicubic") flags = SWS_BICUBIC;
    else if (i == "bicublin") flags = SWS_BICUBLIN;
    else if (i == "gauss") flags = SWS_GAUSS;
    else if (i == "sinc") flags = SWS_SINC;
    else if (i == "hyper" || i == "lanczos") flags = SWS_LANCZOS;
    else if (i == "spline") flags = SWS_SPLINE;
    return flags | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND;
}

// swscale's unscaled YUV -> RGB converters (SIMD on aarch64 and x86) take these, and only without SWS_ACCURATE_RND
bool fastRgbSource(AVPixelFormat f)
{
    return f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUV422P || f == AV_PIX_FMT_YUVJ422P
           || f == AV_PIX_FMT_NV12;
}

// Preview: YUV -> RGB(A), first the planes scaled (YUV -> YUV, SIMD), then converted unscaled without accurate
// rounding (the SIMD converter: nearest chroma, ±1 step). With SWS_ACCURATE_RND and SWS_FULL_CHR_H_INT (MLT's flags)
// the combined pass runs in C code in one thread: 80 ms for 1080p -> 540p on a Raspberry Pi Zero 2 W, 3 ms so.
bool convertFast(const AVFrame* frame, AVPixelFormat src, uint8_t** outData, int* outStride, AVPixelFormat dst,
                 int width, int height, int dw, int dh, int flags, int colorspace)
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
    lumaTransfer(ctx, colorspace, 601); // swscale's RGB side is SWS_CS_DEFAULT (601)
    sws_scale(ctx, inData, inStride, 0, dh, outData, outStride);
    return true;
}

// Converted and scaled in one pass (rgb/rgba or planar YUV, progressive, no separate alpha); false = not possible
bool convertScaled(const Picture& pic, uint8_t* buffer, mlt_image_format format, int width, int height, int dw, int dh,
                   int flags, const Info& info, int profileColorspace, int* colorspace, bool fast)
{
    const AVFrame* frame = pic.frame;
    const AVPixelFormat src = withRange(AVPixelFormat(frame->format), info.fullRange);
    const AVPixelFormat dst = avFormatOf(format);
    uint8_t* outData[4] = {};
    int outStride[4] = {};
    const bool rgb = format == mlt_image_rgb || format == mlt_image_rgba;
    if (rgb)
        av_image_fill_arrays(outData, outStride, buffer, dst, dw, dh, kImageAlign);
    else
        mlt_image_format_planes(format, dw, dh, buffer, outData, outStride);
    *colorspace = info.colorspace;
    if (fast && rgb && fastRgbSource(src))
        return convertFast(frame, src, outData, outStride, dst, width, height, dw, dh, flags, info.colorspace);
    SwsContext* ctx = cachedContext(width, height, src, dst, flags, dw, dh);
    if (!ctx) return false;
    // swscale's RGB side is SWS_CS_DEFAULT (601). RGB to RGB goes through YUV inside swscale: the same matrix both
    // ways, or the colors shift (MLT's swscale filter shifted them so: a 601-tagged RGBA picture in a 709 profile)
    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(src);
    const bool rgbSource = srcDesc && (srcDesc->flags & AV_PIX_FMT_FLAG_RGB);
    if (rgb)
        lumaTransfer(ctx, rgbSource ? 601 : info.colorspace, 601);
    else if (!lumaTransfer(ctx, info.colorspace, profileColorspace))
        *colorspace = profileColorspace;
    sws_scale(ctx, frame->data, frame->linesize, 0, height, outData, outStride);
    return true;
}

struct Sliced {
    int width = 0, height = 0, sliceW = 0;
    const AVFrame* frame = nullptr;
    bool interlaced = false;
    uint8_t* outData[4] = {};
    int outStride[4] = {};
    AVPixelFormat srcFormat = AV_PIX_FMT_NONE, dstFormat = AV_PIX_FMT_NONE;
    const AVPixFmtDescriptor* srcDesc = nullptr;
    const AVPixFmtDescriptor* dstDesc = nullptr;
    int flags = 0, srcColorspace = 0, dstColorspace = 0;
};

// MLT's sliced_h_pix_fmt_conv_proc: packed 4:2:2 in vertical stripes (and fields)
int slicedProc(int /*id*/, int idx, int jobs, void* cookie)
{
    const auto* c = static_cast<const Sliced*>(cookie);
    const bool interlaced = c->interlaced;
    const int field = interlaced ? (idx & 1) : 0;
    idx = interlaced ? idx / 2 : idx;
    (void)jobs;
    const int mul = interlaced ? 2 : 1;
    const int h = c->height >> int(interlaced);
    int sliceW = c->sliceW;
    const int sliceX = sliceW * idx;
    sliceW = std::min(sliceW, c->width - sliceX);
    int srcChrPos = -513, dstChrPos = -513;
    if (c->srcFormat == AV_PIX_FMT_YUV420P) srcChrPos = !interlaced ? 128 : !field ? 64 : 192;
    if (c->dstFormat == AV_PIX_FMT_YUV420P) dstChrPos = !interlaced ? 128 : !field ? 64 : 192;
    if (sliceW <= 0) return 0;
    SwsContext* sws = sws_alloc_context();
    av_opt_set_int(sws, "srcw", sliceW, 0);
    av_opt_set_int(sws, "srch", h, 0);
    av_opt_set_int(sws, "src_format", c->srcFormat, 0);
    av_opt_set_int(sws, "dstw", sliceW, 0);
    av_opt_set_int(sws, "dsth", h, 0);
    av_opt_set_int(sws, "dst_format", c->dstFormat, 0);
    av_opt_set_int(sws, "sws_flags", c->flags, 0);
    av_opt_set_int(sws, "src_h_chr_pos", -513, 0);
    av_opt_set_int(sws, "src_v_chr_pos", srcChrPos, 0);
    av_opt_set_int(sws, "dst_h_chr_pos", -513, 0);
    av_opt_set_int(sws, "dst_v_chr_pos", dstChrPos, 0);
    if (sws_init_context(sws, nullptr, nullptr) < 0) {
        sws_freeContext(sws);
        return 0;
    }
    lumaTransfer(sws, c->srcColorspace, c->dstColorspace);
    const uint8_t* in[4];
    uint8_t* out[4];
    int inStride[4], outStride[4];
    for (int i = 0; i < 4; ++i) {
        int inOffset = (c->srcDesc->flags & AV_PIX_FMT_FLAG_PLANAR)
                           ? ((i == 1 || i == 2) ? (sliceX >> c->srcDesc->log2_chroma_w) : sliceX)
                           : (i == 0 ? sliceX : 0);
        int outOffset = (c->dstDesc->flags & AV_PIX_FMT_FLAG_PLANAR)
                            ? ((i == 1 || i == 2) ? (sliceX >> c->dstDesc->log2_chroma_w) : sliceX)
                            : (i == 0 ? sliceX : 0);
        inOffset *= c->srcDesc->comp[i].step;
        outOffset *= c->dstDesc->comp[i].step;
        inStride[i] = c->frame->linesize[i] * mul;
        outStride[i] = c->outStride[i] * mul;
        in[i] = c->frame->data[i] + c->frame->linesize[i] * field + inOffset;
        out[i] = c->outData[i] + c->outStride[i] * field + outOffset;
    }
    sws_scale(sws, in, inStride, 0, h, out, outStride);
    sws_freeContext(sws);
    return 0;
}

// MLT's convert_image; returns the YUV colorspace of the result
int convert(const Picture& pic, uint8_t* buffer, mlt_image_format format, int width, int height, uint8_t** alpha,
            const Info& info, int profileColorspace, bool fast)
{
    const AVFrame* frame = pic.frame;
    const auto pixFmt = AVPixelFormat(frame->format);
    const bool interlaced = !pic.progressive;
    int result = info.colorspace;

    if ((pixFmt == AV_PIX_FMT_YUVA420P || pixFmt == AV_PIX_FMT_YUVA444P) && format != mlt_image_rgba && frame->data[3]
        && frame->linesize[3]) {
        uint8_t* dst = *alpha = static_cast<uint8_t*>(mlt_pool_alloc(width * height));
        const uint8_t* src = frame->data[3];
        for (int i = 0; i < height; dst += width, src += frame->linesize[3], ++i)
            std::memcpy(dst, src, std::min(width, frame->linesize[3]));
    }
    const AVPixelFormat src = withRange(pixFmt, info.fullRange);
    const AVPixelFormat dst = avFormatOf(format);
    uint8_t* outData[4] = {};
    int outStride[4] = {};

    if (format == mlt_image_rgb || format == mlt_image_rgba) {
        const int flags = swsFlags(width, height, src, width, height, dst);
        av_image_fill_arrays(outData, outStride, buffer, dst, width, height, kImageAlign);
        if (src == AV_PIX_FMT_YUV420P && interlaced) { // field by field
            const int fieldHeight = height / 2;
            SwsContext* ctx = cachedContext(width, fieldHeight, src, dst, flags);
            if (!ctx) return result;
            lumaTransfer(ctx, info.colorspace, 601); // swscale's RGB side is SWS_CS_DEFAULT (601)
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
                                   info.colorspace)) {
            SwsContext* ctx = cachedContext(width, height, src, dst, flags);
            if (!ctx) return result;
            lumaTransfer(ctx, info.colorspace, 601);
            sws_scale(ctx, frame->data, frame->linesize, 0, height, outData, outStride);
        }
    } else if (dst != AV_PIX_FMT_NONE) {
        mlt_image_format_planes(format, width, height, buffer, outData, outStride);
        const int flags = swsFlags(width, height, src, width, height, dst);
        SwsContext* ctx = cachedContext(width, height, src, dst, flags);
        if (!ctx) return result;
        // As MLT: the frame is tagged with the profile's colorspace when swscale accepts the details (it has no
        // YUV -> YUV matrix, the pixels stay as they are)
        if (!lumaTransfer(ctx, info.colorspace, profileColorspace)) result = profileColorspace;
        const int* srcTable = coefficients(info.colorspace, nullptr);
        if (src == dst && srcTable && srcTable == coefficients(profileColorspace, nullptr)) {
            // swscale copies a format onto itself unchanged (same matrix, same range): plain copy
            const int planes = av_pix_fmt_count_planes(src);
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(src);
            for (int p = 0; p < planes; ++p) {
                const bool sub = p == 1 || p == 2;
                const int h = sub ? AV_CEIL_RSHIFT(height, desc->log2_chroma_h) : height;
                const int bytes = av_image_get_linesize(src, width, p);
                av_image_copy_plane(outData[p], outStride[p], frame->data[p], frame->linesize[p], bytes, h);
            }
        } else {
            sws_scale(ctx, frame->data, frame->linesize, 0, height, outData, outStride);
        }
    } else {
        Sliced c;
        c.width = width;
        c.height = height;
        c.frame = frame;
        c.interlaced = interlaced;
        c.dstFormat = AV_PIX_FMT_YUYV422;
        c.srcColorspace = info.colorspace;
        c.dstColorspace = profileColorspace;
        c.srcFormat = info.fullRange && src == AV_PIX_FMT_YUV422P ? AV_PIX_FMT_YUVJ422P : src;
        c.srcDesc = av_pix_fmt_desc_get(c.srcFormat);
        c.dstDesc = av_pix_fmt_desc_get(c.dstFormat);
        c.flags = swsFlags(width, height, c.srcFormat, width, height, c.dstFormat);
        av_image_fill_arrays(c.outData, c.outStride, buffer, c.dstFormat, width, height, kImageAlign);
        const bool sliced = !qEnvironmentVariableIsSet("MLT_AVFORMAT_SLICED_PIXFMT_DISABLE") && src != c.dstFormat;
        c.sliceW = sliced ? (width < 1000 ? 256 >> int(interlaced) : 512 >> int(interlaced)) : width;
        int jobs = (width + c.sliceW - 1) / c.sliceW;
        const int lastSliceW = width - c.sliceW * (jobs - 1);
        if (sliced && lastSliceW % 8 == 0 && !(c.srcFormat == AV_PIX_FMT_YUV422P && lastSliceW % 16)) {
            jobs *= interlaced ? 2 : 1;
            mlt_slices_run_normal(jobs, slicedProc, &c);
        } else {
            jobs = interlaced ? 2 : 1;
            c.sliceW = width;
            for (int i = 0; i < jobs; ++i) slicedProc(i, i, jobs, &c);
        }
        result = profileColorspace;
    }
    return result;
}

// ---- The producer ----

// Picture size handed out (MLT's set_image_size / producer_set_up_video): upright; 1088-line encodes without
// cropping info show 1080 lines in a 16:9 profile
void outputSize(const Data& d, mlt_profile profile, int* width, int* height)
{
    const bool sideways = d.turns == 1 || d.turns == 3;
    int w = d.info.width, h = d.info.height;
    if (!sideways && h == 1088 && mlt_profile_dar(profile) == 16.0 / 9.0) h = 1080;
    *width = sideways ? h : w;
    *height = sideways ? w : h;
}

int getImage(mlt_frame frame, uint8_t** buffer, mlt_image_format* format, int* width, int* height, int /*writable*/)
{
    auto producer = static_cast<mlt_producer>(mlt_frame_pop_service(frame));
    auto* d = static_cast<Data*>(producer->child);
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    mlt_profile profile = mlt_service_profile(MLT_PRODUCER_SERVICE(producer));

    const mlt_position position = mlt_frame_original_position(frame);
    const int64_t request = int64_t(position / mlt_producer_get_fps(producer) * d->sourceFps + 0.5);
    const std::shared_ptr<const Picture> pic = d->reader->picture(request);
    if (!pic || !pic->frame) return 1;

    int fullRange = mlt_image_full_range(mlt_properties_get(fp, "consumer.color_range"));
    // Above 8 bits swscale only changes the range when scaling: the source range stays
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(d->info.format);
    if (desc && desc->nb_components > 0 && desc->comp[0].depth > 8) fullRange = d->info.fullRange;

    *format = pickImageFormat(d->info.format, *format);
    outputSize(*d, profile, width, height);
    // The picture must hold the size (a stream whose size changes midway: MLT's test image instead)
    if (pic->frame->width < *width || pic->frame->height < *height) return 1;

    auto tag = [&](int colorspace) {
        mlt_properties_set_int(fp, "colorspace", colorspace);
        mlt_properties_set_int(fp, "full_range", fullRange);
        mlt_properties_set_int(fp, "format", *format);
        mlt_properties_set_int(fp, "progressive", pic->progressive);
        mlt_properties_set_int(fp, "top_field_first", pic->topFieldFirst);
        // The source's scan type on the producer (MLT updates it per picture; written only when it changes)
        mlt_properties pp = MLT_PRODUCER_PROPERTIES(producer);
        if (d->progressive.exchange(pic->progressive) != int(pic->progressive))
            mlt_properties_set_int(pp, "meta.media.progressive", pic->progressive);
        if (d->topFieldFirst.exchange(pic->topFieldFirst) != int(pic->topFieldFirst))
            mlt_properties_set_int(pp, "meta.media.top_field_first", pic->topFieldFirst);
    };

    // The rescaler (MLT's swscale filter above) tells the size it wants. Scaled right here, in one swscale pass:
    // MLT converted at full size and the filter then scaled that again with a new context per picture – a 1080p
    // preview spent more than half its time there, in swscale's C paths. Not with crop or interlacing (the crop and
    // deinterlace filters in between need the full picture) or a separate alpha plane.
    const int dw = mlt_properties_get_int(fp, "rescale_width"), dh = mlt_properties_get_int(fp, "rescale_height");
    const char* interp = mlt_properties_get(fp, "consumer.rescale");
    const auto pixFmt = AVPixelFormat(pic->frame->format);
    const bool separateAlpha = (pixFmt == AV_PIX_FMT_YUVA420P || pixFmt == AV_PIX_FMT_YUVA444P) && *format != mlt_image_rgba;
    const bool cropped = mlt_properties_get_int(fp, "crop.left") || mlt_properties_get_int(fp, "crop.right")
                         || mlt_properties_get_int(fp, "crop.top") || mlt_properties_get_int(fp, "crop.bottom");
    if (dw >= 6 && dh >= 6 && (dw != *width || dh != *height) && interp && std::strcmp(interp, "none") != 0
        && avFormatOf(*format) != AV_PIX_FMT_NONE && pic->progressive && !cropped && !separateAlpha
        && !qEnvironmentVariableIsSet("SCHNEIDI_SCALE_LATE")) { // as MLT, for comparisons
        const int size = mlt_image_format_size(*format, dw, dh, nullptr);
        auto* image = static_cast<uint8_t*>(mlt_pool_alloc(size));
        int colorspace = d->info.colorspace;
        if (image && convertScaled(*pic, image, *format, *width, *height, dw, dh, scaleFlags(interp), d->info,
                                   profile->colorspace, &colorspace, d->fast)) {
            mlt_frame_set_image(frame, image, size, mlt_pool_release);
            *width = dw;
            *height = dh;
            *buffer = image;
            tag(colorspace);
            return 0;
        }
        mlt_pool_release(image);
    }

    const int size = mlt_image_format_size(*format, *width, *height, nullptr);
    auto* image = static_cast<uint8_t*>(mlt_pool_alloc(size));
    if (!image) return 1;
    mlt_frame_set_image(frame, image, size, mlt_pool_release);
    uint8_t* alpha = nullptr;
    const int colorspace = convert(*pic, image, *format, *width, *height, &alpha, d->info, profile->colorspace, d->fast);
    if (alpha) mlt_frame_set_alpha(frame, alpha, *width * *height, mlt_pool_release);
    *buffer = image;
    tag(colorspace);
    return 0;
}

int getFrame(mlt_producer self, mlt_frame_ptr frame, int /*index*/)
{
    auto* d = static_cast<Data*>(self->child);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(self));
    if (!*frame) return 1;
    mlt_properties fp = MLT_FRAME_PROPERTIES(*frame);
    mlt_frame_set_position(*frame, mlt_producer_position(self));
    int w = 0, h = 0;
    outputSize(*d, mlt_service_profile(MLT_PRODUCER_SERVICE(self)), &w, &h);
    mlt_properties_set_int(fp, "width", w);
    mlt_properties_set_int(fp, "height", h);
    mlt_properties_set_double(fp, "aspect_ratio", av_q2d(d->info.sar));
    mlt_properties_set_int(fp, "colorspace", d->info.colorspace);
    mlt_properties_set_int(fp, "color_trc", d->info.colorTrc);
    mlt_properties_set_int(fp, "color_primaries", d->info.colorPrimaries);
    mlt_properties_set_int(fp, "full_range", d->info.fullRange);
    mlt_properties_set_int(fp, "full_luma", d->info.fullRange);
    mlt_properties_set_int(fp, "format", d->native);
    mlt_properties_set_position(fp, "original_position", mlt_producer_frame(self));
    mlt_frame_push_service(*frame, self);
    mlt_frame_push_get_image(*frame, getImage);
    mlt_producer_prepare_next(self);
    return 0;
}

void closeProducer(mlt_producer self)
{
    delete static_cast<Data*>(self->child);
    self->child = nullptr;
    self->close = nullptr;
    mlt_producer_close(self);
    free(self);
}

// The loader's normalizing filters (producer_loader.c, loader.ini) in the same order: deinterlace, fieldorder,
// crop, rescaler (swscale), resizer, then the image converter. No sound filters (no sound here).
void attachNormalizers(mlt_profile profile, mlt_producer producer)
{
    const char* const filters[][2] = {{"deinterlace", nullptr}, {"fieldorder", nullptr}, {"crop", "1"},
                                      {"swscale", nullptr},     {"resize", nullptr},     {"avcolor_space", nullptr}};
    for (const auto& [name, arg] : filters) {
        mlt_filter f = mlt_factory_filter(profile, name, const_cast<char*>(arg));
        if (!f && std::strcmp(name, "avcolor_space") == 0) f = mlt_factory_filter(profile, "imageconvert", nullptr);
        if (!f && std::strcmp(name, "deinterlace") == 0) f = mlt_factory_filter(profile, "avdeinterlace", nullptr);
        if (!f && std::strcmp(name, "swscale") == 0) f = mlt_factory_filter(profile, "rescale", nullptr);
        if (!f) continue;
        mlt_properties_set_int(MLT_FILTER_PROPERTIES(f), "_loader", 1);
        mlt_producer_attach(producer, f);
        mlt_filter_close(f);
    }
}

void setStreamMetadata(mlt_properties p, const AVFormatContext* fmt, const Info& info)
{
    char key[200];
    mlt_properties_set_int(p, "meta.media.nb_streams", int(fmt->nb_streams));
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        const AVCodecParameters* par = st->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) continue;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            std::snprintf(key, sizeof key, "meta.media.%u.stream.type", i);
            mlt_properties_set(p, key, "video");
            std::snprintf(key, sizeof key, "meta.media.%u.stream.frame_rate", i);
            mlt_properties_set_double(p, key, int(i) == info.index ? av_q2d(info.frameRate)
                                                                    : av_q2d(av_guess_frame_rate(nullptr, const_cast<AVStream*>(st), nullptr)));
            std::snprintf(key, sizeof key, "meta.media.%u.stream.sample_aspect_ratio", i);
            mlt_properties_set_double(p, key, av_q2d(st->sample_aspect_ratio));
            std::snprintf(key, sizeof key, "meta.media.%u.codec.width", i);
            mlt_properties_set_int(p, key, par->width);
            std::snprintf(key, sizeof key, "meta.media.%u.codec.height", i);
            mlt_properties_set_int(p, key, par->height);
            std::snprintf(key, sizeof key, "meta.media.%u.codec.rotate", i);
            mlt_properties_set_int(p, key, int(i) == info.index ? int(info.rotation) : 0);
            std::snprintf(key, sizeof key, "meta.media.%u.codec.pix_fmt", i);
            mlt_properties_set(p, key, av_get_pix_fmt_name(AVPixelFormat(par->format)));
            std::snprintf(key, sizeof key, "meta.media.%u.codec.sample_aspect_ratio", i);
            mlt_properties_set_double(p, key, av_q2d(par->sample_aspect_ratio));
            std::snprintf(key, sizeof key, "meta.media.%u.codec.colorspace", i);
            int cs = par->color_space;
            switch (par->color_space) {
            case AVCOL_SPC_SMPTE240M: cs = 240; break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M: cs = 601; break;
            case AVCOL_SPC_BT709: cs = 709; break;
            case AVCOL_SPC_UNSPECIFIED:
            case AVCOL_SPC_RESERVED: cs = par->width * par->height > 750000 ? 709 : 601; break;
            default: break;
            }
            mlt_properties_set_int(p, key, cs);
            if (par->color_trc && par->color_trc != AVCOL_TRC_UNSPECIFIED) {
                std::snprintf(key, sizeof key, "meta.media.%u.codec.color_trc", i);
                mlt_properties_set_double(p, key, par->color_trc);
            }
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!par->ch_layout.nb_channels) continue;
            std::snprintf(key, sizeof key, "meta.media.%u.stream.type", i);
            mlt_properties_set(p, key, "audio");
            std::snprintf(key, sizeof key, "meta.media.%u.codec.sample_fmt", i);
            mlt_properties_set(p, key, av_get_sample_fmt_name(AVSampleFormat(par->format)));
            std::snprintf(key, sizeof key, "meta.media.%u.codec.sample_rate", i);
            mlt_properties_set_int(p, key, par->sample_rate);
            std::snprintf(key, sizeof key, "meta.media.%u.codec.channels", i);
            mlt_properties_set_int(p, key, par->ch_layout.nb_channels);
        } else {
            continue;
        }
        std::snprintf(key, sizeof key, "meta.media.%u.codec.name", i);
        mlt_properties_set(p, key, codec->name);
        std::snprintf(key, sizeof key, "meta.media.%u.codec.long_name", i);
        mlt_properties_set(p, key, codec->long_name);
        std::snprintf(key, sizeof key, "meta.media.%u.codec.bit_rate", i);
        mlt_properties_set_int64(p, key, par->bit_rate);
        const AVDictionaryEntry* tag = nullptr;
        while ((tag = av_dict_get(st->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            if (!*tag->value || std::strcmp(tag->value, "und") == 0) continue;
            std::snprintf(key, sizeof key, "meta.attr.%u.stream.%s.markup", i, tag->key);
            mlt_properties_set(p, key, tag->value);
        }
    }
    const AVDictionaryEntry* tag = nullptr;
    while ((tag = av_dict_get(fmt->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
        if (!*tag->value || std::strcmp(tag->value, "und") == 0) continue;
        std::snprintf(key, sizeof key, "meta.attr.%s.markup", tag->key);
        mlt_properties_set(p, key, tag->value);
    }
}

} // namespace

bool ownDecoder()
{
    static const bool on = qEnvironmentVariable("SCHNEIDI_DECODER").trimmed().toLower() != "mlt";
    return on;
}

std::unique_ptr<Mlt::Producer> openProducer(Mlt::Profile& profile, const QString& path, const ProducerOptions& options)
{
    Reader::Options ro;
    ro.threads = options.threads;
    // Export workers ask for neighboring frames out of order: keep that many decoded behind the newest request
    ro.behind = std::max(ro.behind, options.parallelFrames + 1);
    std::unique_ptr<Reader> reader = Reader::open(path, ro);
    if (!reader) return nullptr;
    const AVFormatContext* fmt = reader->container();
    const Info& info = reader->info();
    if (info.duration <= 0) return nullptr; // live streams, still images (MLT handles those)
    // Image sequences and single pictures go to MLT's qimage/pixbuf anyway; only demuxed video here
    if (fmt->iformat && (std::strstr(fmt->iformat->name, "image2") || std::strstr(fmt->iformat->name, "_pipe")))
        return nullptr;
    // Formats swscale cannot read are no use
    if (!sws_isSupportedInput(withRange(info.format, info.fullRange))) return nullptr;
    const int turns = std::abs(info.rotation - 90) < 1.0 ? 1 : std::abs(info.rotation - 180) < 1.0 ? 2
                    : std::abs(info.rotation - 270) < 1.0 ? 3 : 0;
    // A quarter turn of 4:2:2 (iPhone ProRes in portrait) leaves chroma the reader's plane-wise turn cannot hold: MLT
    // converts the picture first
    if (turns % 2) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(info.format);
        if (!desc || desc->log2_chroma_w != desc->log2_chroma_h) return nullptr;
    }
    if (info.frameRate.num <= 0 || info.frameRate.den <= 0) reader->setFrameRate({profile.frame_rate_num(), profile.frame_rate_den()});

    mlt_producer raw = mlt_producer_new(profile.get_profile());
    if (!raw) return nullptr;
    auto* d = new Data;
    d->reader = std::move(reader);
    d->info = d->reader->info();
    d->sourceFps = av_q2d(d->info.frameRate);
    d->turns = turns;
    d->fast = options.fastConvert && !qEnvironmentVariableIsSet("SCHNEIDI_EXACT_PREVIEW"); // exact: comparisons
    d->native = pickImageFormat(withRange(d->info.format, d->info.fullRange), mlt_image_yuv422);
    raw->child = d;
    raw->get_frame = getFrame;
    raw->close = reinterpret_cast<mlt_destructor>(closeProducer);
    auto p = std::make_unique<Mlt::Producer>(raw); // own reference
    mlt_producer_close(raw);

    mlt_properties props = p->get_properties();
    const QByteArray resource = path.toUtf8();
    p->set("resource", resource.constData());
    p->set("mlt_service", "schneidi_video");
    // Length in profile frames, from the container's duration (as MLT's get_basic_info)
    const int frames = int(std::lrint(double(info.duration) * profile.fps() / AV_TIME_BASE));
    p->set("length", std::max(1, frames));
    p->set("out", std::max(0, frames - 1));
    p->set("in", 0);
    p->set("seekable", 1);
    p->set("video_index", info.index);
    p->set("audio_index", -1);
    p->set("format", int(d->native));
    // Stored size before rotation (sourceAspect turns it by meta.media.N.codec.rotate itself)
    p->set("width", info.width);
    p->set("height", info.height);
    p->set("meta.media.sample_aspect_num", info.sar.num);
    p->set("meta.media.sample_aspect_den", info.sar.den);
    p->set("aspect_ratio", av_q2d(info.sar));
    p->set("meta.media.frame_rate_num", d->info.frameRate.num);
    p->set("meta.media.frame_rate_den", d->info.frameRate.den);
    p->set("meta.media.colorspace", info.colorspace);
    p->set("meta.media.color_trc", info.colorTrc);
    p->set("meta.media.has_b_frames", info.hasBFrames ? 1 : 0);
    p->set("meta.media.color_range", info.fullRange ? "full" : "mpeg");
    int w = 0, h = 0;
    outputSize(*d, profile.get_profile(), &w, &h);
    p->set("meta.media.width", w);
    p->set("meta.media.height", h);
    if (d->turns != 1 && d->turns != 3) p->set("meta.media.aspect_ratio", av_q2d(info.sar));
    const AVStream* st = fmt->streams[info.index];
    const bool progressive = st->codecpar->field_order == AV_FIELD_PROGRESSIVE || st->codecpar->field_order == AV_FIELD_UNKNOWN;
    p->set("meta.media.progressive", progressive ? 1 : 0);
    p->set("meta.media.top_field_first", st->codecpar->field_order == AV_FIELD_TT || st->codecpar->field_order == AV_FIELD_TB ? 1 : 0);
    {
        const int den = st->avg_frame_rate.den;
        if (den != 0 && den != 1 && den != 2 && den != 125 && den != 1001 && av_cmp_q(st->avg_frame_rate, st->r_frame_rate))
            p->set("meta.media.variable_frame_rate", 1);
    }
    setStreamMetadata(props, fmt, d->info);
    attachNormalizers(profile.get_profile(), raw);
    return p;
}

} // namespace Video
