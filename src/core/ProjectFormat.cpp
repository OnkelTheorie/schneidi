#include "core/ProjectFormat.h"

#include "core/Keyframes.h"
#include "core/MediaAspect.h"
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

QString resolutionLabel(int width, int height)
{
    const QString size = QString("%1 × %2").arg(width).arg(height);
    for (const auto& p : kResolutionPresets)
        if (p.width == width && p.height == height) return size + " " + T(p.name);
    return size;
}

void scaleTimeline(Timeline& tl, QSize from, QSize to)
{
    if (from == to || from.isEmpty() || to.isEmpty()) return;
    const double sx = double(to.width()) / from.width();
    const double sy = double(to.height()) / from.height();
    const double s = std::min(sx, sy);
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
                    if (std::abs(sx - sy) > 1e-9 && c.kind == ClipKind::Media) {
                        double aspect = MediaAspect::of(c.mediaPath);
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
                TitleStyle& ti = c.title;
                ti.posX *= sx;
                ti.posY *= sy;
                ti.size *= s;
                ti.outlineWidth *= s;
                ti.boxPad *= s;
                c.transInStyle.border *= s;
                c.transOutStyle.border *= s;
                // Convert keyframe values in pixels likewise
                for (auto it = c.keys.begin(); it != c.keys.end(); ++it) {
                    double f = 1;
                    switch (it.key()) {
                    case AnimParam::PosX: case AnimParam::TitlePosX: f = sx; break;
                    case AnimParam::PosY: case AnimParam::TitlePosY: f = sy; break;
                    case AnimParam::TitleSize: f = s; break;
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
    if (tl.nested) {
        auto nested = std::make_shared<NestedTimelines>(*tl.nested);
        for (Timeline& n : *nested) scaleTimeline(n, from, to);
        tl.nested = std::move(nested);
    }
}
