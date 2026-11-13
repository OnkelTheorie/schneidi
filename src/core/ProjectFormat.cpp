// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ProjectFormat.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/Generators.h"
#include "core/GradeWindows.h"
#include "core/Drawing.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Shapes.h"
#include "core/TimelineStory.h"
#include "core/Types.h"


QString FrameRate::label() const
{
    // 23.976 / 29.97 / 59.94 like DaVinci; decimal comma in German and the other non-English languages
    QString s = QString::number(fps(), 'f', 3);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    s.replace('.', I18n::decimalPoint());
    return s;
}

FrameRate nearestFrameRate(double fps, bool integerOnly)
{
    FrameRate best;
    double bestDist = 1e9;
    for (const FrameRate& r : kFrameRates) {
        if (integerOnly && !r.isInteger()) continue;
        const double d = std::abs(r.fps() - fps);
        if (d < bestDist) {
            bestDist = d;
            best = r;
        }
    }
    return best;
}

ProjectFormat withinLimits(ProjectFormat f)
{
    f.width = std::clamp(f.width, kMinFrameSize, kMaxFrameSize);
    f.height = std::clamp(f.height, kMinFrameSize, kMaxFrameSize);
    if (f.rate.num <= 0 || f.rate.den <= 0 || f.rate.fps() < 1 || f.rate.fps() > 1000)
        f.rate = nearestFrameRate(f.rate.num > 0 && f.rate.den > 0 ? f.rate.fps() : 25);
    return f;
}

QString resolutionLabel(int width, int height)
{
    const QString size = QString("%1 × %2").arg(width).arg(height);
    for (const auto& p : kResolutionPresets)
        if (p.width == width && p.height == height) return size + " " + T(p.name);
    return size;
}

namespace {

// Width of a title's widest line at `size` px in project pixels: a font-independent estimate from the characters'
// typical share of the size, set to DejaVu Sans (one of the wider sans serif fonts; Inter, Segoe UI, Liberation are
// narrower), so a fitted title fits on real fonts too.
// Deliberately not measured with QFontMetrics: the result is stored in the project, and the CLI (no GUI, no fonts)
// and the app (and two systems with other fonts) must scale a timeline the same way.
double charWidth(QChar c)
{
    const ushort u = c.unicode();
    if (u >= 0x2E80) return 1.0; // CJK and other full-width scripts
    if (c == ' ') return 0.32;
    if (QStringLiteral(".,:;!|'`ijlI").contains(c)) return 0.3;
    if (QStringLiteral("ftr()[]-\"/").contains(c)) return 0.4;
    if (QStringLiteral("mwMW@%").contains(c)) return 0.95;
    if (c.isDigit()) return 0.64;
    if (c.isUpper()) return 0.72;
    if (c.isLower()) return 0.6;
    return 0.65;
}

double textWidth(const TitleStyle& st, const QString& line, double size)
{
    double em = 0;
    for (const QChar c : line) em += charWidth(c);
    return em * size * (st.bold ? 1.08 : 1.0);
}

} // namespace

double titleWidth(const TitleStyle& st, double size)
{
    size = std::max(1.0, size);
    double w = 1;
    const auto width = [&](const QString& line) { return textWidth(st, line, size); };
    for (const QString& line : (st.isCounter() ? Generators::counterWidest(st, width) : st.text).split('\n'))
        w = std::max(w, width(line));
    return w;
}

namespace {

// Factor (<= 1) that shrinks a title scaled by `s` from frame width `fromW` to `toW` just enough to fit the new frame
// with a margin (90 % of its width), but never below the share of the frame it took before (a title running off the
// old frame on purpose runs off the new one as far). Its largest size counts (an animated size).
double titleFit(const Clip& c, double s, double fromW, double toW)
{
    const TitleStyle& st = c.title;
    if (!c.isTitle() || st.isProgress() || (st.text.isEmpty() && !st.isCounter()) || s <= 0) return 1.0;
    double peak = st.size;
    for (const Keyframe& k : c.keys.value(AnimParam::TitleSize)) peak = std::max(peak, k.value);
    const double limit = std::max(0.9 * toW, titleWidth(st, peak) / fromW * toW);
    double q = 1.0;
    for (int i = 0; i < 4; ++i) { // measured again at the shrunk size
        const double w = titleWidth(st, peak * s * q);
        if (w <= limit) break;
        q *= limit / w;
    }
    return q;
}

// Node values of compositing graphs in project pixels (Comp::pixelKeys: a text node's size, a blur radius, a shadow
// offset ...) times k, static values and keyframes; positions as 0..1 of the picture stay. A value never set keeps
// its node default (the engine's, unknown here) - like a fresh node in the new resolution.
// Clip in a shape (0.22): its pixel values (stroke, soft edge, shadow softness), static and keyframed; size and place
// are percent of the picture and stay
void scaleClipShape(Clip& c, double k)
{
    const QString id = QString::fromLatin1(Shapes::kClipShapeEffect);
    if (!EffectRegistry::has(c, id)) return;
    for (const char* key : {"stroke_width", "feather", "shadow_soft"}) // static value (into the effect) and keyframes
        Keys::convert(c, Keys::effectParam(id, QString::fromLatin1(key)), [k](double v) { return v * k; }, k);
}

void scaleCompPixels(Clip& c, double k)
{
    QVector<AnimParam> scaled; // keyframes belong to the effect id: two comp instances share them
    for (EffectInstance& e : c.effects) {
        CompGraph g;
        if (!Comp::graphOf(e, &g)) continue;
        for (const CompNode& n : g.nodes)
            for (const QString& key : Comp::pixelKeys(n.type)) {
                const QString full = CompGraph::paramKey(n.id, key);
                if (auto it = e.params.find(full); it != e.params.end()) {
                    bool ok = false;
                    const double v = it->toDouble(&ok);
                    if (ok) *it = v * k;
                }
                const AnimParam p = Keys::effectParam(QString::fromLatin1(Comp::kEffectId), full);
                if (scaled.contains(p)) continue;
                scaled << p;
                if (auto kt = c.keys.find(p); kt != c.keys.end())
                    for (Keyframe& f : *kt) {
                        f.value *= k;
                        f.inDv *= k; // Bezier handles
                        f.outDv *= k;
                    }
            }
    }
}

} // namespace

void scaleTimeline(Timeline& tl, QSize from, QSize to)
{
    if (from == to || from.isEmpty() || to.isEmpty()) return;
    const double sx = double(to.width()) / from.width();
    const double sy = double(to.height()) / from.height();
    // Sizes by the frame's short side (like Subtitles::defaultStyle): a round trip 16:9 -> 9:16 -> 16:9 gives the
    // sizes back (the smaller of sx and sy shrank them both ways, a title of 80 px came back with 25)
    const double s = double(std::min(to.width(), to.height())) / std::min(from.width(), from.height());
    for (SubtitleTrack& t : tl.subtitles) {
        TitleStyle& st = t.style;
        st.posX *= sx;
        st.posY *= sy;
        st.size *= s;
        st.outlineWidth *= s;
        st.boxPad *= s;
    }
    const QSizeF before(from), after(to);
    for (auto* tracks : {&tl.video, &tl.audio})
        for (Track& t : *tracks)
            for (Clip& c : t.clips) {
                ClipTransform& tr = c.transform;
                tr.posX *= sx;
                tr.posY *= sy;
                // Crop: timeline pixels on the picture fitted into the frame -> it scales with that picture. With the
                // frame's shape kept that is the frame's scale; otherwise the picture keeps its own shape (needs it:
                // MediaAspect, a file of unknown shape counts as shaped like the old frame), while titles and
                // compound clips are the frame itself.
                constexpr AnimParam crops[] = {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop,
                                               AnimParam::CropBottom};
                if (tracks == &tl.video && std::any_of(std::begin(crops), std::end(crops), [&](AnimParam p) {
                        return Keys::staticValue(c, p) != 0 || Keys::animated(c, p);
                    })) {
                    double kx = sx, ky = sy;
                    if (std::abs(sx - sy) > 1e-9 && c.kind == ClipKind::Media && !c.cropOnFrame) { // else on the frame
                        double aspect = MediaAspect::ofClip(c);
                        if (aspect <= 0) aspect = before.width() / before.height();
                        const QSizeF a = MediaAspect::fit(aspect, before), b = MediaAspect::fit(aspect, after);
                        kx = b.width() / a.width();
                        ky = b.height() / a.height();
                    }
                    for (AnimParam p : crops) {
                        const double k = p == AnimParam::CropLeft || p == AnimParam::CropRight ? kx : ky;
                        Keys::convert(c, p, [k](double v) { return v * k; }, k);
                    }
                }
                // Color page windows (a blurred name, a tracked face) lie on the picture fitted into the frame: a
                // frame of another shape fits it differently (a 16:9 edit copied to a 9:16 short left its name masks
                // above the picture band, the names readable)
                if (tracks == &tl.video && std::abs(sx - sy) > 1e-9 && c.kind == ClipKind::Media && !c.cropOnFrame
                    && !GradeWindows::list(c).isEmpty()) {
                    double aspect = MediaAspect::ofClip(c);
                    if (aspect <= 0) aspect = before.width() / before.height();
                    const QSizeF a = MediaAspect::fit(aspect, before), b = MediaAspect::fit(aspect, after);
                    GradeWindows::refit(c, (b.width() / after.width()) / (a.width() / before.width()),
                                        (b.height() / after.height()) / (a.height() / before.height()));
                }
                // A title nearly as wide as the old frame would run off a narrower one (a 16:9 caption in a 9:16
                // copy): shrunk to fit, outline and box margin with it; one that fits keeps its size
                const double ts = tracks == &tl.video ? s * titleFit(c, s, from.width(), to.width()) : s;
                TitleStyle& ti = c.title;
                ti.posX *= sx;
                ti.posY *= sy;
                ti.size *= ts;
                ti.outlineWidth *= ts;
                ti.boxPad *= ts;
                for (double* v : {&ti.look.edgeDepth, &ti.look.shadowX, &ti.look.shadowY, &ti.look.shadowSoft,
                                  &ti.look.glowRadius, &ti.look.texX, &ti.look.texY, &ti.look.solidDepth,
                                  &ti.look.solidBevel})
                    *v *= ts;
                ti.barThickness *= sy;
                ti.barMargin *= sy;
                for (double* v : {&ti.shape.width, &ti.shape.height, &ti.shape.corner, &ti.shape.feather}) *v *= s;
                if (ti.isDrawing()) // strokes in 0..1 of the frame: only a frame of another shape moves them
                    Drawing::scaleFormat(ti.drawing, from.width(), from.height(), to.width(), to.height(), s);
                c.transInStyle.border *= s;
                c.transOutStyle.border *= s;
                if (tracks == &tl.video) scaleCompPixels(c, s);
                if (tracks == &tl.video) scaleClipShape(c, s);
                // Convert keyframe values in pixels likewise
                for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
                    double f = 1;
                    switch (it.key()) {
                    case AnimParam::PosX: case AnimParam::TitlePosX: f = sx; break;
                    case AnimParam::PosY: case AnimParam::TitlePosY: f = sy; break;
                    case AnimParam::TitleSize: f = ts; break;
                    case AnimParam::ShapeWidth: case AnimParam::ShapeHeight: f = s; break;
                    default: continue;
                    }
                    for (Keyframe& k : it.value()) {
                        k.value *= f;
                        k.inDv *= f; // Bezier handles
                        k.outDv *= f;
                    }
                }
            }
    // Content of the compound clips (appended only when rendering) likewise
    // (a sequence of its own resolution: that one scaled by the same factors, so it keeps its shape inside)
    if (tl.nested) {
        auto nested = std::make_shared<NestedTimelines>(*tl.nested);
        for (Timeline& n : *nested) {
            if (n.frameSize.isEmpty() || n.frameSize == from) {
                scaleTimeline(n, from, to);
                if (!n.frameSize.isEmpty()) n.frameSize = to;
                continue;
            }
            const QSize own(std::max(2, int(std::lround(double(n.frameSize.width()) * to.width() / from.width()))),
                            std::max(2, int(std::lround(double(n.frameSize.height()) * to.height() / from.height()))));
            const QSize was = n.frameSize;
            scaleTimeline(n, was, own);
            n.frameSize = own;
        }
        tl.nested = std::move(nested);
    }
    if (!tl.frameSize.isEmpty()) tl.frameSize = to;
}

void retimeTimeline(Timeline& tl, FrameRate from, FrameRate to)
{
    if (from == to || from.fps() <= 0 || to.fps() <= 0) return;
    const double k = to.fps() / from.fps();
    const auto at = [k](int frame) { return frame < 0 ? frame : int(std::lround(frame * k)); };
    for (SubtitleTrack& t : tl.subtitles) {
        int prevEnd = 0;
        for (SubtitleCue& c : t.cues) { // sorted without overlap: stays so, every cue keeps at least one frame
            const int start = c.start;
            c.start = std::max(prevEnd, at(c.start));
            c.end = std::max(c.start + 1, at(c.end));
            prevEnd = c.end;
            for (CueWord& w : c.words) { // word captions: their times from the cue start in the new rate
                w.from = std::max(0, at(start + w.from) - c.start);
                w.to = std::max(w.from + 1, at(start + w.to) - c.start);
            }
        }
    }
    for (int& m : tl.markers) m = at(m);
    tl.markers.erase(std::unique(tl.markers.begin(), tl.markers.end()), tl.markers.end());
    tl.markIn = at(tl.markIn);
    tl.markOut = at(tl.markOut);
    if (tl.markIn >= 0 && tl.markOut >= 0) tl.markOut = std::max(tl.markOut, tl.markIn);
    TimelineStory::retime(tl.sections, tl.notes, k);
}
