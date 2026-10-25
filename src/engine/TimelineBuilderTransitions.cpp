// TimelineBuilder: video and audio transitions (fades, dissolves, wipes).
#include "engine/TimelineBuilderDetail.h"
#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/Limiter.h"
#include "core/Loudness.h"
#include "core/TruePeak.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Denoise.h"
#include "engine/Lumas.h"
#include "engine/Profiles.h"
#include "engine/PitchLatency.h"
#include "engine/RampProducer.h"
#include "engine/Stabilizer.h"

#include <Mlt.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <QColor>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QStandardPaths>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>


namespace TimelineBuilderDetail {

// Fade video in/out over `len` frames via the alpha channel (on V1 = from black, above = to the
// track below, like DaVinci). Keyframes via anim_set instead of text: "0.5" would otherwise be read as 0 depending on LC_NUMERIC.
void applyFade(Mlt::Profile& profile, Mlt::Producer& cut, int len, bool fadeIn)
{
    auto value = [&](int i) { return fadeIn ? double(i) / len : double(len - 1 - i) / len; };
    Mlt::Filter f(profile, "brightness");
    if (!f.is_valid()) return;
    f.set("level", 1.0);
    f.anim_set("alpha", value(0), 0, len);
    f.anim_set("alpha", value(len - 1), len - 1, len);
    f.set_in_and_out(cut.get_in(), cut.get_out()); // keyframes count from filter-in
    cut.attach(f);
}

// Audio transition: level of clip clipId in the excerpt of the transition as keyframes in dB per frame
// (curve from TimelineOps, the same one draws the waveform)
void applyAudioTransition(Mlt::Profile& profile, Mlt::Producer& cut, const TimelineOps::TransitionSpan& s, int clipId)
{
    Mlt::Filter f(profile, "volume");
    if (!f.is_valid()) return;
    const int len = s.length();
    for (int i = 0; i < len; ++i) {
        const double v = TimelineOps::audioTransitionGain(s, clipId, s.start + i);
        f.anim_set("level", v > 0.001 ? 20.0 * std::log10(v) : -200.0, i, len);
    }
    f.set_in_and_out(cut.get_in(), cut.get_out()); // keyframes count from filter-in
    cut.attach(f);
}

// Opacity keyframes (brightness/alpha) over an excerpt of length len: (frame, value)
void applyAlphaKeys(Mlt::Profile& profile, Mlt::Producer& cut, int len, std::initializer_list<std::pair<int, double>> keys)
{
    Mlt::Filter f(profile, "brightness");
    if (!f.is_valid()) return;
    f.set("level", 1.0);
    for (const auto& [pos, v] : keys) f.anim_set("alpha", v, std::clamp(pos, 0, len - 1), len);
    f.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(f);
}

// Gradient image for luma (grayscale PNG, created once): dark spots switch first.
// PNG instead of PGM: luma loads it via an image producer and scales it cleanly to the preview size.
QString wipeLuma(TransitionType type, int w, int h)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/lumas";
    const QString path = QString("%1/%2_%3x%4.png").arg(dir, transitionTypeInfo(type).id).arg(w).arg(h);
    if (QFile::exists(path)) return path;
    QDir().mkpath(dir);
    QImage img(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            double t = 0;
            switch (type) {
            case TransitionType::WipeRight: t = double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeLeft: t = 1.0 - double(x) / std::max(1, w - 1); break;
            case TransitionType::WipeDown: t = double(y) / std::max(1, h - 1); break;
            case TransitionType::WipeUp: t = 1.0 - double(y) / std::max(1, h - 1); break;
            default: break;
            }
            line[x] = uchar(std::lround(t * 255));
        }
    }
    // First write under a different name so that a second instance never reads a half image (own name per writer:
    // preview and export build in different threads)
    const QString tmp = path + "." + QUuid::createUuid().toString(QUuid::Id128) + ".tmp.png";
    if (!img.save(tmp)) return {};
    QFile::remove(path);
    QFile::rename(tmp, path);
    return path;
}

// Border of a wipe: color area, limited to a stripe around the edge by an animated qtcrop.
// edge0/edge1 = position of the edge (fraction 0..1 in wipe direction) in the first/last frame, linear in between.
void addWipeBorder(Mlt::Profile& profile, Mlt::Tractor& mix, int track, int len, const TransitionStyle& st,
                   double edge0, double edge1)
{
    QColor c = st.borderColor;
    c.setAlpha(255);
    Mlt::Producer strip(profile, ("color:" + c.name(QColor::HexArgb)).toUtf8().constData());
    if (!strip.is_valid()) return;
    strip.set("length", len);
    strip.set_in_and_out(0, len - 1);
    Mlt::Filter crop(profile, "qtcrop");
    if (!crop.is_valid()) return;
    const double W = profile.width(), H = profile.height();
    const bool horizontal = st.type == TransitionType::WipeRight || st.type == TransitionType::WipeLeft;
    // to the left/top: edge runs from the end to the start
    const bool reverse = st.type == TransitionType::WipeLeft || st.type == TransitionType::WipeUp;
    const double full = horizontal ? W : H;
    for (const auto& [i, e] : {std::pair{0, edge0}, std::pair{len - 1, edge1}}) {
        const double pos = (reverse ? 1.0 - e : e) * full;
        mlt_rect r{0, 0, W, H, 1.0};
        (horizontal ? r.x : r.y) = pos - st.border / 2;
        (horizontal ? r.w : r.h) = st.border;
        crop.anim_set("rect", r, i, len);
    }
    crop.set("color", "#00000000");
    crop.set_in_and_out(0, len - 1);
    strip.attach(crop);
    mix.set_track(strip, track);
    Mlt::Transition t(profile, "qtblend");
    t.set("always_active", 1);
    mix.plant_transition(t, 0, track);
}

// ---- Both sides of a transition at the requested size ----
// luma (Cross Dissolve, wipes) composites at the size its first input (a) comes back with and lays the second (b) on
// top at the size it asked for, without scaling. MLT's qtblend filter passes a picture whose rectangle is the whole
// frame through unchanged in YUV, at its own (media) size instead of the requested one: zoom exactly 1, e.g. where
// zoom keyframes end. The incoming clip then sat as a small rectangle in the top left corner of the preview (AI
// test run 2026-10-25), or only the corner of a larger b showed; with sources larger than the export, the export too.
namespace {

AVPixelFormat avFormatOf(mlt_image_format f)
{
    switch (f) {
    case mlt_image_rgb: return AV_PIX_FMT_RGB24;
    case mlt_image_rgba: return AV_PIX_FMT_RGBA;
    case mlt_image_yuv422: return AV_PIX_FMT_YUYV422;
    case mlt_image_yuv420p: return AV_PIX_FMT_YUV420P;
    case mlt_image_yuv422p16: return AV_PIX_FMT_YUV422P16LE;
    case mlt_image_yuv420p10: return AV_PIX_FMT_YUV420P10LE;
    case mlt_image_yuv444p10: return AV_PIX_FMT_YUV444P10LE;
    default: return AV_PIX_FMT_NONE;
    }
}

// Scale an image (MLT layout = planes packed one after the other) from w x h to ow x oh; nullptr = not possible
uint8_t* scaled(const uint8_t* src, AVPixelFormat format, int w, int h, int ow, int oh, int* size)
{
    *size = av_image_get_buffer_size(format, ow, oh, 1);
    if (*size <= 0) return nullptr;
    const bool rgb = format == AV_PIX_FMT_RGBA || format == AV_PIX_FMT_RGB24;
    // Same format in and out: scaling only, no color conversion (RGB keeps full chroma)
    SwsContext* ctx = sws_getContext(w, h, format, ow, oh, format,
                                     SWS_BILINEAR | (rgb ? SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT : 0), nullptr,
                                     nullptr, nullptr);
    if (!ctx) return nullptr;
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(*size));
    uint8_t* in[4];
    uint8_t* to[4];
    int inStride[4], toStride[4];
    av_image_fill_arrays(in, inStride, src, format, w, h, 1);
    av_image_fill_arrays(to, toStride, out, format, ow, oh, 1);
    sws_scale(ctx, in, inStride, 0, h, to, toStride);
    sws_freeContext(ctx);
    return out;
}

int requestedSizeGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height,
                          int writable)
{
    mlt_frame_pop_service(frame);
    const int ow = *width, oh = *height;
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    if (error || !*image || ow <= 0 || oh <= 0 || (*width == ow && *height == oh)) return error;
    const AVPixelFormat av = avFormatOf(*format);
    int size = 0;
    uint8_t* out = av == AV_PIX_FMT_NONE ? nullptr : scaled(*image, av, *width, *height, ow, oh, &size);
    if (!out) return error; // other formats (GPU) stay as they are
    int alphaSize = 0;
    if (const uint8_t* alpha = mlt_frame_get_alpha_size(frame, &alphaSize); alpha) {
        int scaledSize = 0;
        uint8_t* a = alphaSize >= *width * *height ? scaled(alpha, AV_PIX_FMT_GRAY8, *width, *height, ow, oh, &scaledSize)
                                                  : nullptr;
        mlt_frame_set_alpha(frame, a, a ? scaledSize : 0, a ? mlt_pool_release : nullptr); // unknown layout: dropped
    }
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    *image = out;
    *width = ow;
    *height = oh;
    return 0;
}

mlt_frame requestedSizeProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, requestedSizeGetImage);
    return frame;
}

} // namespace

void attachRequestedSize(Mlt::Producer& cut)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return;
    f->process = requestedSizeProcess;
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    cut.attach(filter);
}

// Video transition other than Cross Dissolve as a small tractor: a leaves, b arrives; if a side is missing, it is empty there.
std::unique_ptr<Mlt::Tractor> styledTransition(Mlt::Profile& profile, Mlt::Producer* a, Mlt::Producer* b, int len,
                                               const TransitionStyle& st)
{
    const TransitionType type = st.type;
    auto mix = std::make_unique<Mlt::Tractor>(profile);
    if (type == TransitionType::DipToColor) {
        // Color area at the bottom, a fades out in the first half, b in the second (without opposite side: whole length)
        QColor c = st.color;
        c.setAlpha(255); // "#aarrggbb" like with the title
        Mlt::Producer color(profile, ("color:" + c.name(QColor::HexArgb)).toUtf8().constData());
        color.set("length", len);
        color.set_in_and_out(0, len - 1);
        mix->set_track(color, 0);
        const int half = a && b ? len / 2 : 0;
        int track = 1;
        for (Mlt::Producer* p : {a, b}) {
            if (!p) continue;
            if (p == a) applyAlphaKeys(profile, *p, len, {{0, 1.0}, {(b ? half : len) - 1, 0.0}, {len - 1, 0.0}});
            else applyAlphaKeys(profile, *p, len, {{0, 0.0}, {half, 0.0}, {len - 1, 1.0}});
            mix->set_track(*p, track);
            Mlt::Transition t(profile, "qtblend");
            t.set("always_active", 1);
            mix->plant_transition(t, 0, track);
            ++track;
        }
        return mix;
    }
    if (st.isLuma() && a && b) {
        // Gradient wipe: luma with the gradient image (from/to nothing the caller fades instead)
        mix->set_track(*a, 0);
        mix->set_track(*b, 1);
        Mlt::Transition t(profile, "luma");
        const QString luma = Lumas::file(st.luma, profile.width(), profile.height(), st.invert);
        if (!luma.isEmpty()) t.set("resource", luma.toUtf8().constData()); // missing: Cross Dissolve
        t.set("softness", 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0);
        t.set_in_and_out(0, len - 1);
        mix->plant_transition(t, 0, 1);
        return mix;
    }
    const bool border = st.border >= 0.5;
    if (!a || !b) {
        // Wipe from/to nothing: luma ignores transparency -> animate the visible area via qtcrop
        // (hard edge, softness has no effect here)
        Mlt::Producer& one = a ? *a : *b;
        // The new side comes from the start direction: to the right = from the left etc.
        const bool fromStart = (type == TransitionType::WipeRight || type == TransitionType::WipeDown) == !a;
        const bool horizontal = type == TransitionType::WipeRight || type == TransitionType::WipeLeft;
        double edges[2] = {0, 0}; // Position of the edge for the border (fraction in wipe direction)
        Mlt::Filter crop(profile, "qtcrop");
        if (crop.is_valid()) {
            const double W = profile.width(), H = profile.height();
            for (int k = 0; k < 2; ++k) {
                const int i = k ? len - 1 : 0;
                // p = fraction already taken by the new side (fade in: the clip itself, fade out: the empty part)
                const double p = len > 1 ? double(i) / (len - 1) : 1.0;
                const double shown = a ? 1.0 - p : p; // visible fraction of the clip
                mlt_rect r{0, 0, W, H, 1.0};
                double& pos = horizontal ? r.x : r.y;
                double& size = horizontal ? r.w : r.h;
                const double full = horizontal ? W : H;
                size = full * shown;
                pos = fromStart ? 0.0 : full - size;
                crop.anim_set("rect", r, i, len);
                edges[k] = p; // the edge moves in wipe direction from 0 to 1
            }
            crop.set("color", "#00000000");
            crop.set_in_and_out(one.get_in(), one.get_out());
            one.attach(crop);
        }
        mix->set_track(one, 0);
        if (border) addWipeBorder(profile, *mix, 1, len, st, edges[0], edges[1]);
        return mix;
    }
    // Wipe between two clips: luma with gradient image
    mix->set_track(*a, 0);
    mix->set_track(*b, 1);
    Mlt::Transition t(profile, "luma");
    const QString luma = wipeLuma(type, profile.width(), profile.height());
    if (!luma.isEmpty()) t.set("resource", luma.toUtf8().constData());
    // Softness: fraction of the gradient over which to crossfade (always some softness, against stair-stepping)
    const double soft = 0.02 + 0.98 * std::clamp(st.softness, 0.0, 100.0) / 100.0;
    t.set("softness", soft);
    t.set_in_and_out(0, len - 1);
    mix->plant_transition(t, 0, 1);
    if (border) {
        // luma: progress p = i/len, transition zone ends at p*(1+soft) -> middle of the zone as edge
        auto edge = [&](int i) { return double(i) / len * (1.0 + soft) - soft / 2; };
        addWipeBorder(profile, *mix, 2, len, st, edge(0), edge(len - 1));
    }
    return mix;
}

} // namespace TimelineBuilderDetail
