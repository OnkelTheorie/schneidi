#include "core/ProjectFormat.h"

#include "core/Types.h"

QString FrameRate::label() const
{
    // 23.976 / 29.97 / 59.94 wie DaVinci; im Deutschen mit Komma
    QString s = QString::number(fps(), 'f', 3);
    while (s.endsWith('0')) s.chop(1);
    if (s.endsWith('.')) s.chop(1);
    if (I18n::language() == "de") s.replace('.', ',');
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
    for (auto* tracks : {&tl.video, &tl.audio})
        for (Track& t : *tracks)
            for (Clip& c : t.clips) {
                ClipTransform& tr = c.transform;
                tr.posX *= sx;
                tr.posY *= sy;
                tr.cropLeft *= sx;
                tr.cropRight *= sx;
                tr.cropTop *= sy;
                tr.cropBottom *= sy;
                TitleStyle& ti = c.title;
                ti.posX *= sx;
                ti.posY *= sy;
                ti.size *= s;
                ti.outlineWidth *= s;
                ti.boxPad *= s;
                c.transInStyle.border *= s;
                c.transOutStyle.border *= s;
            }
}
