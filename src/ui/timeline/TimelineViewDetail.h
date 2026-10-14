#pragma once
// Internal helpers of TimelineView, shared by its implementation files (not part of the API).

#include "ui/timeline/TimelineView.h"
#include "app/InputBindings.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/EffectFolders.h"
#include "core/I18n.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QContextMenuEvent>
#include <QHash>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <array>
#include <tuple>
#include <cmath>

namespace TimelineViewDetail {

// Clip name, with changed speed with a note like DaVinci (e.g. "clip.mp4 (50 %)");
// audio from files with several audio streams (OBS) with stream name ("aufnahme.mkv · Mikro")
inline QString clipLabel(const Project* project, const Clip& c, TrackKind kind)
{
    QString name = project->clipName(c); // compound clips: name of the sequence
    if (kind == TrackKind::Audio && !c.mediaPath.isEmpty())
        if (const MediaInfo* m = project->mediaInfo(c.mediaPath); m && m->audioStreamCount() > 1)
            name += QString(" · %1").arg(m->audioStreamName(c.audioStream));
    if (!c.isRetimed()) return name;
    if (!c.freeze && !c.reverse && c.speed == 1.0
        && std::all_of(c.ramp.begin(), c.ramp.end(), [](const SpeedPoint& p) { return p.speed == 1.0; }))
        return name; // speed points without speed change
    const QString speed = c.freeze     ? T("Standbild")
                          : c.hasRamp() ? T("Speed Ramp")
                                        : QString("%1%2 %").arg(c.reverse ? "-" : "").arg(QLocale().toString(c.speed * 100, 'g', 4));
    return QString("%1 (%2)").arg(name, speed);
}

// Compound clip in the clip body: stacked layers (like the DaVinci icon) instead of filmstrip/waveform
inline void drawCompoundBody(QPainter& p, const QRect& body, const QColor& base)
{
    if (body.height() < 8 || body.width() < 12) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double h = std::min(18.0, body.height() - 4.0), w = h * 1.4;
    const double x = body.left() + 6, y = body.center().y() - h / 2;
    for (int i = 2; i >= 0; --i) {
        const QRectF r(x + i * h * 0.22, y + (2 - i) * h * 0.22, w * 0.7, h * 0.56);
        p.setPen(QPen(base.lighter(170), 1));
        p.setBrush(base.darker(110 + 20 * i));
        p.drawRoundedRect(r, 1.5, 1.5);
    }
    p.restore();
}

// Clip color of the track: own track color, otherwise invalid (= default per clip kind)
inline QColor trackColor(const Track& t)
{
    const TrackColorInfo* info = trackColorInfo(t.color);
    return info ? QColor::fromRgba(info->rgb) : QColor();
}

// Color of a clip like DaVinci: clip color of the Media Pool clip before the track color, otherwise invalid (= default)
inline QColor clipColor(const Project* project, const Clip& c, const Track* t)
{
    if (!c.isTitle())
        if (const MediaInfo* m = project->mediaInfo(c.mediaPath))
            if (const TrackColorInfo* info = trackColorInfo(m->clipColor)) return QColor::fromRgba(info->rgb);
    return t ? trackColor(*t) : QColor();
}

// Lock icon (padlock) centered in r
inline void drawLock(QPainter& p, const QRectF& r, const QColor& color)
{
    const double cx = r.center().x(), cy = r.center().y();
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.4));
    p.setBrush(Qt::NoBrush);
    QPainterPath shackle;
    shackle.moveTo(cx - 2.6, cy);
    shackle.lineTo(cx - 2.6, cy - 2.2);
    shackle.arcTo(QRectF(cx - 2.6, cy - 5.0, 5.2, 5.6), 180, -180);
    shackle.lineTo(cx + 2.6, cy);
    p.drawPath(shackle);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(cx - 4, cy - 0.5, 8, 6), 1, 1);
    p.restore();
}

constexpr int kSnapPx = 8;
constexpr double kFinePxPerFrame = 12; // from here audio clips move finer than 1 frame
constexpr int kDragStartPx = 4;
constexpr int kAutoScrollZone = 24; // edge zone (px) in which dragging scrolls the timeline along
constexpr int kEdgeGrabPx = 6; // this close to the clip edge it is trimmed instead of moved
constexpr int kVolumeGrabPx = 4;
constexpr int kClipBarH = 16;  // title bar in the clip
constexpr int kLaneH = 16;     // expanded keyframe lane under the clip
constexpr int kCurveH = 120;   // expanded curve editor under the clip
constexpr int kKeyGrabPx = 5;  // this close to a diamond it is grabbed

// Volume <-> height in the clip (0 = bottom, 1 = top), piecewise linear like a fader:
// lower quarter -∞..-20 dB, middle -20..0 dB, upper quarter 0..+12 dB
inline double volumeToPos(double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if (db < -20) return 0.25 * (db - kMinVolumeDb) / (-20 - kMinVolumeDb);
    if (db < 0) return 0.25 + 0.5 * (db + 20) / 20;
    return 0.75 + 0.25 * db / kMaxVolumeDb;
}

inline double posToVolume(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    if (t < 0.25) return kMinVolumeDb + t / 0.25 * (-20 - kMinVolumeDb);
    if (t < 0.75) return -20 + (t - 0.25) / 0.5 * 20;
    return (t - 0.75) / 0.25 * kMaxVolumeDb;
}

inline QRect clipBodyRect(const QRect& r)
{
    const int barH = std::min(kClipBarH, r.height());
    return QRect(r.left(), r.top() + barH, r.width(), r.height() - barH);
}

inline int volumeLineY(const QRect& body, double db)
{
    return body.bottom() - int(std::lround(volumeToPos(db) * (body.height() - 1)));
}

// Clip volume at clip frame t (with keyframes)
inline double volumeAt(const Clip& c, double t)
{
    return Keys::valueAt(c, AnimParam::Volume, std::clamp(t, 0.0, double(c.length() - 1)));
}

// Diamond (keyframe) around (x, y)
inline void drawDiamond(QPainter& p, double x, double y, double r)
{
    const QPointF pts[] = {{x, y - r}, {x + r, y}, {x, y + r}, {x - r, y}};
    p.drawPolygon(pts, 4);
}
// Mouse cursor for trim mode (like DaVinci): bracket(s) for Ripple/Roll, frame for Slip/Slide
inline QCursor trimCursor(TimelineOps::TrimKind kind, TimelineOps::Edge edge)
{
    using TimelineOps::TrimKind;
    static QHash<int, QCursor> cache;
    const int key = int(kind) * 2 + (edge == TimelineOps::Edge::Start);
    if (cache.contains(key)) return cache[key];
    constexpr int S = 32, M = S / 2;
    QPixmap pm(S, S);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    auto arrow = [&](double x1, double x2, double y) { // line with arrowhead at x2
        path.moveTo(x1, y);
        path.lineTo(x2, y);
        const double d = x2 > x1 ? -4 : 4;
        path.moveTo(x2 + d, y - 4);
        path.lineTo(x2, y);
        path.lineTo(x2 + d, y + 4);
    };
    auto bracket = [&](double x, bool open) { // "[" (open) resp. "]"
        const double w = open ? 4 : -4;
        path.moveTo(x + w, M - 9);
        path.lineTo(x, M - 9);
        path.lineTo(x, M + 9);
        path.lineTo(x + w, M + 9);
    };
    switch (kind) {
    case TrimKind::Ripple: {
        const bool start = edge == TimelineOps::Edge::Start;
        bracket(start ? M - 2 : M + 2, start);
        arrow(start ? M - 6 : M + 6, start ? 4 : S - 4, M);   // outwards
        arrow(start ? M + 1 : M - 1, start ? M + 10 : M - 10, M); // inwards
        break;
    }
    case TrimKind::Roll:
        bracket(M - 3, false);
        bracket(M + 3, true);
        arrow(M - 6, 3, M);
        arrow(M + 6, S - 3, M);
        break;
    case TrimKind::Slip: // arrows inside the frame
    case TrimKind::Slide: // arrows outside
        path.addRect(M - 7, M - 8, 14, 16);
        if (kind == TrimKind::Slip) {
            arrow(M, M - 5, M);
            arrow(M, M + 5, M);
        } else {
            arrow(M - 8, 2, M);
            arrow(M + 8, S - 2, M);
        }
        break;
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.end();
    return cache[key] = QCursor(pm, M, M);
}
} // namespace TimelineViewDetail
