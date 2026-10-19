// TimelineBuilder: per-clip filters (effects, color, transform, fades, titles, subtitles).
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
#include <QColor>
#include <QDir>
#include <QFile>
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

// Keyframes (core/Keyframes.h) as MLT animation of a filter value over an excerpt: a = clip frame at the
// start of the excerpt, len = its length. Set at the excerpt boundaries and at all keyframes of the
// parameters (MLT interpolates linearly in between); sections with an ease curve (or bake) frame by frame, so that
// preview, export and Inspector have exactly the same values. false = none of the parameters animated.
bool animate(const Clip& c, std::initializer_list<AnimParam> params, int a, int len,
             const std::function<void(int pos, double t)>& set, bool bake)
{
    if (len <= 0 || std::none_of(params.begin(), params.end(), [&](AnimParam p) { return Keys::animated(c, p); }))
        return false;
    QVector<int> pos{0, len - 1};
    for (int t : Keys::keyTimes(c, QVector<AnimParam>(params)))
        if (t - a > 0 && t - a < len - 1) pos << t - a;
    std::sort(pos.begin(), pos.end());
    pos.erase(std::unique(pos.begin(), pos.end()), pos.end());
    for (int i = 0; i < pos.size(); ++i) {
        set(pos[i], a + pos[i]);
        if (i + 1 == pos.size()) break;
        bool linear = !bake;
        for (AnimParam p : params) linear = linear && Keys::linearBetween(c, p, a + pos[i], a + pos[i + 1]);
        if (!linear)
            for (int q = pos[i] + 1; q < pos[i + 1]; ++q) set(q, a + q);
    }
    return true;
}

// Attach a filter to an excerpt; keyframes count from filter-in (see docs/dev/mlt.md)
void attachTo(Mlt::Producer& cut, Mlt::Filter& f)
{
    f.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(f);
}

// ---- Effects (Open FX) ----

// Color correction as a 3x3 matrix (avfilter.colorchannelmixer): white balance like DaVinci Temp/Tint as
// channel gain, normalized to equal brightness (Rec. 709), then saturation (mix with the brightness).
// Temperature +100 = warm (more red, less blue), tint +100 = magenta (less green), saturation -100 = gray.
struct Matrix { double m[3][3]; };
Matrix colorMatrix(double temp, double tint, double saturation)
{
    const double k = temp / 100.0 * 0.3, n = tint / 100.0 * 0.25;
    double g[3] = {1.0 + k, 1.0 - n, 1.0 - k};
    const double w[3] = {0.2126, 0.7152, 0.0722};
    const double l = w[0] * g[0] + w[1] * g[1] + w[2] * g[2];
    for (double& x : g) x /= l;
    const double s = std::max(0.0, 1.0 + saturation / 100.0);
    Matrix out;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out.m[i][j] = std::clamp(((i == j ? s : 0.0) + (1.0 - s) * w[j]) * g[j], -2.0, 2.0);
    return out;
}

// Brightness/contrast as tone correction (avfilter.colorlevels): out = 0.5 + (in - 0.5)·c + b, limited to 0..1.
// colorlevels allows input -1..1 and output 0..1 -> cut the line where it reaches 0 resp. 1.
struct Levels { double imin, imax, omin, omax; };
Levels levels(double brightness, double contrast)
{
    const double c = std::max(0.0, 1.0 + contrast / 100.0), b = brightness / 200.0;
    const double d = 0.5 - 0.5 * c + b; // out = c·in + d
    Levels lv{0.0, 1.0, d, c + d};
    if (lv.omin < 0 && c > 0) {
        lv.imin = std::min(-d / c, 1.0);
        lv.omin = 0;
    }
    if (lv.omax > 1 && c > 0) {
        lv.imax = std::max((1.0 - d) / c, -1.0);
        lv.omax = 1;
    }
    if (lv.imax - lv.imin < 1e-4) lv.imin = lv.imax - 1e-4; // completely dark/bright: jump instead of division by 0
    lv.omin = std::clamp(lv.omin, 0.0, 1.0);
    lv.omax = std::clamp(lv.omax, 0.0, 1.0);
    return lv;
}

// Value of an effect parameter at clip frame t (static from the instance or from the keyframes)
double fxValue(const Clip& c, AnimParam p, double t) { return Keys::valueAt(c, p, t); }

// Parameter not neutral resp. animated?
bool fxActive(const Clip& c, std::initializer_list<AnimParam> params, double neutral = 0.0)
{
    return std::any_of(params.begin(), params.end(),
                       [&](AnimParam p) { return Keys::animated(c, p) || Keys::staticValue(c, p) != neutral; });
}

// avfilter values always via anim_set (also static: one value at the start): properties set as a number are converted
// to text by MLT when passing them on to FFmpeg – with the current number format. Only filters with number options
// work (avfilter.eq has text options for expressions -> anim_set has no effect there).
void fxSet(const Clip& c, std::initializer_list<AnimParam> params, int a, int len,
           const std::function<void(int pos, double t)>& set)
{
    if (!animate(c, params, a, len, set)) set(0, a);
}

void applyColor(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, int a, int len)
{
    using P = AnimParam;
    if (fxActive(c, {P::FxTemp, P::FxTint, P::FxSaturation})) {
        Mlt::Filter f(profile, "avfilter.colorchannelmixer");
        if (f.is_valid()) {
            static const char* names[3][3] = {{"av.rr", "av.rg", "av.rb"}, {"av.gr", "av.gg", "av.gb"},
                                              {"av.br", "av.bg", "av.bb"}};
            fxSet(c, {P::FxTemp, P::FxTint, P::FxSaturation}, a, len, [&](int pos, double t) {
                const Matrix m = colorMatrix(fxValue(c, P::FxTemp, t), fxValue(c, P::FxTint, t),
                                             fxValue(c, P::FxSaturation, t));
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) f.anim_set(names[i][j], m.m[i][j], pos, len);
            });
            attachTo(cut, f);
        }
    }
    if (fxActive(c, {P::FxBrightness, P::FxContrast})) {
        Mlt::Filter f(profile, "avfilter.colorlevels");
        if (f.is_valid()) {
            fxSet(c, {P::FxBrightness, P::FxContrast}, a, len, [&](int pos, double t) {
                const Levels lv = levels(fxValue(c, P::FxBrightness, t), fxValue(c, P::FxContrast, t));
                for (const char* ch : {"r", "g", "b"}) {
                    f.anim_set(QByteArray("av.") + ch + "imin", lv.imin, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "imax", lv.imax, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "omin", lv.omin, pos, len);
                    f.anim_set(QByteArray("av.") + ch + "omax", lv.omax, pos, len);
                }
            });
            attachTo(cut, f);
        }
    }
}

void applyBlur(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, int a, int len)
{
    if (!fxActive(c, {AnimParam::FxBlur})) return;
    Mlt::Filter f(profile, "avfilter.gblur");
    if (!f.is_valid()) return;
    // Sigma relative to the profile height (strength 100 = 1/20 of the image height); MLT scales a smaller preview itself
    // (avformat/resolution_scale.yml), a smaller export has a smaller profile
    const double scale = profile.height() / 2000.0;
    fxSet(c, {AnimParam::FxBlur}, a, len, [&](int pos, double t) {
        const double sigma = std::max(0.0, fxValue(c, AnimParam::FxBlur, t)) * scale;
        f.anim_set("av.sigma", sigma, pos, len);
        // sigmaV = -1 (like sigma) is only resolved by gblur on creation -> otherwise the vertical would stay almost sharp
        f.anim_set("av.sigmaV", sigma, pos, len);
    });
    attachTo(cut, f);
}

// Before/After switch of the currently building builder (preview only), see TimelineBuilder::setGradeBypass
thread_local std::shared_ptr<std::atomic<bool>> t_gradeBypass;

// Generic effect: parameters straight onto the MLT filter of the descriptor
void applyGeneric(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, const EffectInstance& inst,
                  const EffectDescriptor& d, int a, int len);

// Effects in the order on the clip; a/len: section for keyframes. Video: stabilization first (it has to see the
// camera image), then the Color page grade (like DaVinci: grade before the edit effects), regardless of their
// position. Audio: the audio effects (before clip volume/pan); noise reduction is attached per clip instead.
void applyEffects(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, TrackKind kind, int a, int len)
{
    const bool video = kind == TrackKind::Video;
    if (video) {
        Stabilizer::attach(profile, clip, c);
        ColorGrade::attach(clip, c, a, t_gradeBypass);
    }
    for (const auto& inst : c.effects) {
        if (!inst.enabled || inst.effectId == ColorGrade::EffectId || inst.effectId == Stabilizer::EffectId) continue;
        const EffectDescriptor* d = EffectRegistry::find(inst.effectId);
        if (!d || d->video != video) continue;
        if (inst.effectId == "color") applyColor(profile, clip, c, a, len);
        else if (inst.effectId == "blur") applyBlur(profile, clip, c, a, len);
        else if (inst.effectId == Denoise::EffectId) continue; // one filter per clip (buildTimeline)
        else applyGeneric(profile, clip, c, inst, *d, a, len);
    }
}

void applyGeneric(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, const EffectInstance& inst,
                  const EffectDescriptor& d, int a, int len)
{
    if (d.mltService.isEmpty()) return;
    Mlt::Filter f(profile, d.mltService.toUtf8().constData());
    if (!f.is_valid()) return;
    for (const auto& p : d.params) {
        const QVariant v = inst.params.value(p.key, p.defaultValue);
        const QByteArray prop = p.mltProperty.toUtf8();
        if (p.type == EffectParam::Color) {
            const QColor col = v.value<QColor>();
            f.set(prop.constData(), col.name(QColor::HexRgb).toUtf8().constData()); // "#rrggbb"
        } else if (p.type == EffectParam::Choice) {
            f.set(prop.constData(), v.toString().toUtf8().constData());
        } else if (p.type == EffectParam::Bool) {
            f.set(prop.constData(), v.toBool() ? 1.0 : 0.0);
        } else {
            // keyframes (frei0r): MLT animation via anim_set (numbers, not text -> independent of LC_NUMERIC)
            const bool animated = p.anim != AnimParam::Count && animate(c, {p.anim}, a, len, [&](int pos, double t) {
                f.anim_set(prop.constData(), Keys::valueAt(c, p.anim, t), pos, len);
            });
            if (!animated) f.set(prop.constData(), v.toDouble());
        }
    }
    attachTo(clip, f); // keyframes count from the cut's in point
}

// Aspect ratio of the source image as displayed (pixel aspect ratio and rotation from the metadata), 0 = unknown
double sourceAspect(Mlt::Producer& cut)
{
    Mlt::Producer& p = cut.is_cut() ? cut.parent() : cut;
    double w = p.get_int("width"), h = p.get_int("height");
    if (w <= 0 || h <= 0) {
        w = p.get_int("meta.media.width");
        h = p.get_int("meta.media.height");
    }
    if (w <= 0 || h <= 0) return 0;
    if (const double sar = p.get_double("aspect_ratio"); sar > 0) w *= sar;
    const int vi = p.get_int("video_index");
    const int rotate = std::abs(p.get_int(QString("meta.media.%1.codec.rotate").arg(vi).toUtf8().constData())) % 180;
    return rotate == 90 ? h / w : w / h;
}

// qtblend with distort=1 always asks for the source in its full media size, even when the consumer renders a small
// preview -> every effect below it ran in full resolution (1080p/4K) and the preview stuttered. This filter sits
// above qtblend and lowers meta.media.width/height to what the requested output needs (requested size times the
// largest zoom of the clip, never above the media size). Export requests the full size and is unchanged.
// Compared with the media size (the profile is the project size; the preview consumer has its own, see Engine).
// Measured (tests/bench_inspector, 8 effects + keyframes, 960x540 preview): 125 -> 70 ms per frame.
int sourceCapGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
    const int mw = mlt_properties_get_int(fp, "meta.media.width"), mh = mlt_properties_get_int(fp, "meta.media.height");
    if (mw > 0 && mh > 0 && *width > 0 && *height > 0) {
        const double zoom = mlt_properties_get_double(MLT_FILTER_PROPERTIES(filter), "max_zoom");
        // A source of another aspect ratio is fitted into the frame: the larger ratio keeps enough pixels
        const double scale = std::max(double(*width) / mw, double(*height) / mh) * zoom;
        if (scale < 1.0) {
            mlt_properties_set_int(fp, "meta.media.width", std::max(2, int(std::ceil(mw * scale))));
            mlt_properties_set_int(fp, "meta.media.height", std::max(2, int(std::ceil(mh * scale))));
        }
    }
    return mlt_frame_get_image(frame, image, format, width, height, writable);
}

mlt_frame sourceCapProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, sourceCapGetImage);
    return frame;
}

void attachSourceCap(Mlt::Producer& clip, const Clip& c)
{
    // Without video effects the extra scaling step costs more than it saves
    if (std::none_of(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) {
            const EffectDescriptor* d = EffectRegistry::find(e.effectId);
            return e.enabled && d && d->video;
        }))
        return;
    double zoom = 0;
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
        zoom = std::max(zoom, std::abs(Keys::staticValue(c, p)));
        for (const Keyframe& k : c.keys.value(p)) zoom = std::max(zoom, std::abs(k.value));
    }
    attachSourceCap(clip, std::max(zoom, 1.0) * 1.1); // a little headroom for Bezier overshoot between keyframes
}

void attachSourceCap(Mlt::Producer& clip, double maxZoom)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return;
    f->process = sourceCapProcess;
    mlt_properties_set_double(MLT_FILTER_PROPERTIES(f), "max_zoom", maxZoom);
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    clip.attach(filter);
}

void applyTransform(Mlt::Profile& profile, Mlt::Producer& clip, const Clip& c, int a, int len)
{
    const double W = profile.width(), H = profile.height();
    auto val = [&](AnimParam p, double t) { return Keys::valueAt(c, p, t); };
    if (Keys::hasCrop(c)) {
        // Cropping = make the edge transparent, the image stays in its place (like DaVinci)
        Mlt::Filter crop(profile, "qtcrop");
        if (crop.is_valid()) {
            auto rect = [&](double t) {
                const double l = val(AnimParam::CropLeft, t), r = val(AnimParam::CropRight, t);
                const double top = val(AnimParam::CropTop, t), b = val(AnimParam::CropBottom, t);
                return mlt_rect{l, top, std::max(1.0, W - l - r), std::max(1.0, H - top - b), 1.0};
            };
            if (!animate(c, {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}, a, len,
                         [&](int pos, double t) { crop.anim_set("rect", rect(t), pos, len); }))
                crop.set("rect", rect(a));
            crop.set("color", "#00000000");
            attachTo(clip, crop);
        }
    }
    if (Keys::hasOpacity(c)) {
        // Opacity via the alpha channel (like Shotcut); the qtblend opacity only delivered black
        Mlt::Filter op(profile, "brightness");
        if (op.is_valid()) {
            op.set("level", 1.0);
            if (!animate(c, {AnimParam::Opacity}, a, len,
                         [&](int pos, double t) { op.anim_set("alpha", val(AnimParam::Opacity, t) / 100.0, pos, len); }))
                op.set("alpha", c.transform.opacity / 100.0);
            attachTo(clip, op);
        }
    }
    if (!Keys::hasTransform(c)) return;
    Mlt::Filter f(profile, "qtblend");
    if (!f.is_valid()) return;
    // distort=1 (below) stretches the source onto the rectangle -> fit the rectangle itself into the image preserving aspect ratio
    // (a portrait clip in a landscape project gets room at the sides, as without transform and as in DaVinci)
    double fitW = W, fitH = H;
    if (const double asp = c.isTitle() ? 0.0 : sourceAspect(clip); asp > 0) {
        if (asp > W / H) fitH = W / asp;
        else fitW = H * asp;
    }
    auto rect = [&](double t) {
        const double w = fitW * val(AnimParam::ZoomX, t), h = fitH * val(AnimParam::ZoomY, t);
        return mlt_rect{(W - w) / 2 + val(AnimParam::PosX, t), (H - h) / 2 - val(AnimParam::PosY, t), w, h, 1.0};
    };
    // Set as mlt_rect instead of text: MLT reads text rectangles with the system number format
    // (German: "940.8" -> 940 and ".8" slips into the next field). The 5th value (opacity) must be 1.
    if (!animate(c, {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY}, a, len,
                 [&](int pos, double t) { f.anim_set("rect", rect(t), pos, len); }))
        f.set("rect", rect(a));
    // DaVinci: positive = counterclockwise
    if (!animate(c, {AnimParam::Rotation}, a, len,
                 [&](int pos, double t) { f.anim_set("rotation", -val(AnimParam::Rotation, t), pos, len); }))
        f.set("rotation", -c.transform.rotation);
    f.set("rotate_center", 1);
    // distort=1: fetch the source at full size and only scale when drawing. Otherwise qtblend requests the image
    // at target height (at small zoom only a few pixels -> image mush) and keeps X/Y zoom in the aspect ratio.
    f.set("distort", 1);
    attachTo(clip, f);
    attachSourceCap(clip, c); // after qtblend: its get_image runs first
}

// a/len: excerpt (clip frame at the start, length) for keyframes
void decorate(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int a, int len)
{
    applyEffects(profile, cut, c, kind, a, len);
    if (kind == TrackKind::Video) applyTransform(profile, cut, c, a, len);
    if (c.isTitle() && !Keys::hasTransform(c) && !Keys::animated(c, AnimParam::TitleSize)) {
        // qtext draws at the requested size but does not scale the outline along (preview 960 px:
        // outline three times as thick). qtblend with distort=1 always fetches the image at full project size.
        Mlt::Filter f(profile, "qtblend");
        if (f.is_valid()) {
            f.set("rect", mlt_rect{0, 0, double(profile.width()), double(profile.height()), 1.0});
            f.set("distort", 1);
            cut.attach(f);
        }
    }
    if (kind == TrackKind::Audio) {
        applyVolume(profile, cut, c, a, len);
        applyPan(profile, cut, c, a, len);
    }
}

// Title: transparent full image ("color" with alpha) + qtext filter (Qt: font, color, outline, box).
// The filter's rectangle is set to the width of the text block and positioned around its center,
// then halign only aligns the lines within the block (like DaVinci) instead of pushing the block to the image edge.
// a/len: excerpt for keyframes (position, color, size).
Mlt::Producer* titleCut(Mlt::Profile& profile, const Clip& c, int a, int len)
{
    const TitleStyle& t = c.title;
    // With blur: transparent area in text color instead of black, otherwise gblur (non-premultiplied
    // alpha) mixes in the black neighbor pixels and the text becomes dark and gray
    QByteArray base = "color:#00000000";
    if (const EffectInstance* fx = EffectRegistry::instance(c, "blur"); fx && fx->enabled)
        base = "color:#00" + t.color.name(QColor::HexRgb).mid(1).toLatin1();
    Mlt::Producer src(profile, base.constData());
    src.set("length", len);
    src.set("out", len - 1);
    Mlt::Producer* cut = src.cut(0, len - 1); // cut holds a reference to src
    const double W = profile.width(), H = profile.height();

    // Size animated: qtext cannot animate "size" -> draw at the largest size (sharp)
    // and shrink via qtblend around the text center
    const bool sizeAnim = Keys::animated(c, AnimParam::TitleSize);
    double size = sizeAnim ? 0.0 : t.size;
    if (sizeAnim) // Ease curves stay between the keyframe values -> largest keyframe = largest size
        for (const Keyframe& k : c.keys.value(AnimParam::TitleSize)) size = std::max(size, k.value);
    size = std::max(1.0, size);

    QFont font(t.fontFamily());
    font.setPixelSize(std::max(1, int(std::lround(size))));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    const QFontMetricsF fm(font);
    double w = 1;
    for (const QString& line : t.text.split('\n')) w = std::max(w, fm.horizontalAdvance(line));

    // qtext strokes the outline centred on the glyph edge, so half of it covers the letters (bold text looked
    // thin). Like DaVinci the outline goes outside: a layer behind in the outline colour with twice the width,
    // the text without outline on top.
    auto mltColor = [](const QColor& col) {
        return mlt_color{uint8_t(col.red()), uint8_t(col.green()), uint8_t(col.blue()), uint8_t(col.alpha())};
    };
    const bool outline = t.outlineOn && t.outlineWidth > 0;
    auto textFilter = [&](bool outlineLayer) {
        Mlt::Filter f(profile, "qtext");
        if (!f.is_valid()) return false;
        f.set("argument", t.text.toUtf8().constData());
        // Rectangle as mlt_rect (number format, see applyTransform)
        auto geometry = [&](double tt) {
            return mlt_rect{(W - w) / 2 + Keys::valueAt(c, AnimParam::TitlePosX, tt), -Keys::valueAt(c, AnimParam::TitlePosY, tt),
                            w, H, 1.0};
        };
        if (!animate(c, {AnimParam::TitlePosX, AnimParam::TitlePosY}, a, len,
                     [&](int pos, double tt) { f.anim_set("geometry", geometry(tt), pos, len); }))
            f.set("geometry", geometry(a));
        f.set("family", t.fontFamily().toUtf8().constData());
        f.set("size", size);
        f.set("weight", t.bold ? 700 : 400);
        f.set("style", t.italic ? "italic" : "normal");
        f.set("halign", t.align == 0 ? "left" : t.align == 2 ? "right" : "center");
        f.set("valign", "middle");
        // Colours as "#aarrggbb"; bgcolour otherwise defaults to a light grey
        if (outlineLayer) {
            f.set("fgcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
        } else if (!animate(c, {AnimParam::TitleColor}, a, len, [&](int pos, double tt) {
                       f.anim_set("fgcolour", mltColor(Keys::toColor(Keys::valueAt(c, AnimParam::TitleColor, tt))), pos, len);
                   })) {
            f.set("fgcolour", t.color.name(QColor::HexArgb).toUtf8().constData());
        }
        // The box belongs to the bottom layer (the text layer keeps the same pad so both sit in the same place)
        const bool box = t.boxOn && (outlineLayer || !outline);
        f.set("bgcolour", box ? t.boxColor.name(QColor::HexArgb).toUtf8().constData() : "#00000000");
        f.set("pad", t.boxOn ? std::max(0.0, t.boxPad) : 0.0);
        f.set("olcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
        f.set("outline", outlineLayer ? 2 * t.outlineWidth : 0.0);
        attachTo(*cut, f);
        return true;
    };
    if (outline && !textFilter(true)) return cut;
    if (!textFilter(false)) return cut;

    if (sizeAnim) {
        // Scale s = size/largest size around the text center; product of size and position -> frame by frame.
        // qtblend (distort=1) also always fetches the image at project size (outline, see decorate).
        Mlt::Filter scale(profile, "qtblend");
        if (scale.is_valid()) {
            animate(c, {AnimParam::TitleSize, AnimParam::TitlePosX, AnimParam::TitlePosY}, a, len, [&](int pos, double tt) {
                const double k = std::max(0.001, Keys::valueAt(c, AnimParam::TitleSize, tt) / size);
                const double cx = W / 2 + Keys::valueAt(c, AnimParam::TitlePosX, tt);
                const double cy = H / 2 - Keys::valueAt(c, AnimParam::TitlePosY, tt);
                scale.anim_set("rect", mlt_rect{cx * (1 - k), cy * (1 - k), W * k, H * k, 1.0}, pos, len);
            }, true);
            scale.set("distort", 1);
            attachTo(*cut, scale);
        }
    }
    return cut;
}

// Apply the clip's fade handles to the excerpt [from, to) (timeline frames). Keyframes only in the
// fade ranges and at their boundaries, so that long clips do not get thousands of keyframes.
void applyClipFades(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c, TrackKind kind, int from, int to)
{
    const int length = c.length();
    const int fi = std::min(c.fadeIn, length);
    const int fo = std::min(c.fadeOut, length - fi);
    const int a = from - c.start, b = to - c.start; // clip-local, b exclusive
    const int len = b - a;
    if ((fi <= 0 || a >= fi) && (fo <= 0 || b <= length - fo)) return; // excerpt touches no fade
    auto isKey = [&](int t) { return t == a || t == b - 1 || t <= fi || t >= length - fo - 1; };
    const bool video = kind == TrackKind::Video;
    Mlt::Filter f(profile, video ? "brightness" : "volume");
    if (!f.is_valid()) return;
    if (video) f.set("level", 1.0);
    for (int t = a; t < b; ++t) {
        if (!isKey(t)) continue;
        // the timeline draws the same curve (waveform, fade line)
        if (video) {
            f.anim_set("alpha", TimelineOps::fadeRamp(c, t), t - a, len);
        } else {
            const double g = TimelineOps::audioFadeGain(c, t);
            f.anim_set("level", g > 0.001 ? 20.0 * std::log10(g) : -200.0, t - a, len);
        }
    }
    f.set_in_and_out(cut.get_in(), cut.get_out()); // keyframes count from filter-in
    cut.attach(f);
}

// Subtitle: wrap lines to maxWidth (existing line breaks stay), too long words stay whole
QStringList wrapSubtitle(const QString& text, const QFontMetricsF& fm, double maxWidth)
{
    QStringList out;
    for (const QString& para : text.split('\n')) {
        QString line;
        for (const QString& word : para.split(' ', Qt::SkipEmptyParts)) {
            const QString candidate = line.isEmpty() ? word : line + ' ' + word;
            if (!line.isEmpty() && fm.horizontalAdvance(candidate) > maxWidth) {
                out << line;
                line = word;
            } else {
                line = candidate;
            }
        }
        out << line;
    }
    return out;
}

// One subtitle (track style like a title): text block at the bottom, lower edge posY pixels above the image edge,
// lines wrapped at 90 % of the image width. len frames long.
Mlt::Producer* subtitleCut(Mlt::Profile& profile, const TitleStyle& t, const QString& text, int len)
{
    Mlt::Producer src(profile, "color:#00000000");
    src.set("length", len);
    src.set("out", len - 1);
    Mlt::Producer* cut = src.cut(0, len - 1);
    const double W = profile.width(), H = profile.height();
    QFont font(t.fontFamily());
    font.setPixelSize(std::max(1, int(std::lround(t.size))));
    font.setBold(t.bold);
    font.setItalic(t.italic);
    const QFontMetricsF fm(font);
    const QStringList lines = wrapSubtitle(text.trimmed(), fm, W * 0.9);
    double w = 1;
    for (const QString& line : lines) w = std::max(w, fm.horizontalAdvance(line));
    const double bottom = std::clamp(H - t.posY, 1.0, H);
    const bool outline = t.outlineOn && t.outlineWidth > 0;
    // Outline outside the letters like titles (titleCut): a layer behind with twice the width, the text on top
    auto textFilter = [&](bool outlineLayer) {
        Mlt::Filter f(profile, "qtext");
        if (!f.is_valid()) return false;
        f.set("argument", lines.join('\n').toUtf8().constData());
        f.set("geometry", mlt_rect{(W - w) / 2 + t.posX, 0, w, bottom, 1.0});
        f.set("family", t.fontFamily().toUtf8().constData());
        f.set("size", std::max(1.0, t.size));
        f.set("weight", t.bold ? 700 : 400);
        f.set("style", t.italic ? "italic" : "normal");
        f.set("halign", t.align == 0 ? "left" : t.align == 2 ? "right" : "center");
        f.set("valign", "bottom");
        f.set("fgcolour", (outlineLayer ? t.outlineColor : t.color).name(QColor::HexArgb).toUtf8().constData());
        const bool box = t.boxOn && (outlineLayer || !outline);
        f.set("bgcolour", box ? t.boxColor.name(QColor::HexArgb).toUtf8().constData() : "#00000000");
        f.set("pad", t.boxOn ? std::max(0.0, t.boxPad) : 0.0);
        f.set("olcolour", t.outlineColor.name(QColor::HexArgb).toUtf8().constData());
        f.set("outline", outlineLayer ? 2 * t.outlineWidth : 0.0);
        attachTo(*cut, f);
        return true;
    };
    if (outline && !textFilter(true)) return cut;
    if (!textFilter(false)) return cut;
    // as with titles: always draw at full project size (otherwise the outline does not scale in the preview)
    Mlt::Filter full(profile, "qtblend");
    if (full.is_valid()) {
        full.set("rect", mlt_rect{0, 0, W, H, 1.0});
        full.set("distort", 1);
        attachTo(*cut, full);
    }
    return cut;
}

} // namespace TimelineBuilderDetail
