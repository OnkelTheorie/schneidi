// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Reframe.h"

#include "core/Keyframes.h"
#include "core/MediaAspect.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Reframe {

const char* modeId(Mode m)
{
    switch (m) {
    case Mode::Center: return "center";
    case Mode::Auto: return "auto";
    case Mode::Fit: return "fit";
    }
    return "auto";
}

bool modeFromId(const QString& id, Mode* m)
{
    for (Mode x : {Mode::Center, Mode::Auto, Mode::Fit})
        if (id.trimmed().compare(QLatin1String(modeId(x)), Qt::CaseInsensitive) == 0) {
            *m = x;
            return true;
        }
    return false;
}

namespace {

double median(std::vector<double> v)
{
    if (v.empty()) return 0.5;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) m = (m + *std::max_element(v.begin(), v.begin() + mid)) / 2;
    return m;
}

struct Point {
    int frame;
    double x, y;
};

// Median of x and y over the points with frames in [from, to]
QPointF medianOver(const std::vector<Point>& pts, int from, int to)
{
    std::vector<double> xs, ys;
    for (const Point& p : pts)
        if (p.frame >= from && p.frame <= to) xs.push_back(p.x), ys.push_back(p.y);
    if (xs.empty()) return {0.5, 0.5};
    return {median(xs), median(ys)};
}

// One shot (no cut inside): holds and eased moves
void segmentPath(const QVector<Target>& seg, const PathParams& p, QVector<Key>& keys)
{
    const double s = std::clamp(p.smoothness, 0.0, 1.0);
    const double fps = std::max(1.0, p.fps);
    const double ww = std::clamp(p.windowW, 0.01, 1.0), wh = std::clamp(p.windowH, 0.01, 1.0);
    const bool cropX = ww < 0.999, cropY = wh < 0.999;
    const auto keep = [](double v, double w, bool crop) { return crop ? std::clamp(v, w / 2, 1 - w / 2) : 0.5; };

    // Unsure measurements lean towards the middle (game footage: the crosshair), then each side is kept where the
    // window can go
    std::vector<Point> raw;
    raw.reserve(size_t(seg.size()));
    for (const Target& t : seg) {
        const double w = std::clamp(t.weight, 0.0, 1.0);
        raw.push_back({t.frame, keep(0.5 + (t.x - 0.5) * w, ww, cropX), keep(0.5 + (t.y - 0.5) * w, wh, cropY)});
    }
    // Median over a short time: single wrong measurements (a flash, a passing object) do not move the camera
    const int half = std::max(1, int(std::lround((0.25 + 0.5 * s) * fps)));
    std::vector<Point> f(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const QPointF m = medianOver(raw, raw[i].frame - half, raw[i].frame + half);
        f[i] = {raw[i].frame, m.x(), m.y()};
    }

    const double dzX = ww * (0.08 + 0.22 * s), dzY = wh * (0.08 + 0.22 * s);
    const int minHold = std::max(1, int(std::lround((0.15 + 0.6 * s) * fps)));
    const int settle = std::max(1, int(std::lround((0.3 + 0.5 * s) * fps)));
    // Fastest move: window widths (heights) per second
    const double speed = 2.5 - 2.0 * s;
    const int first = f.front().frame, last = f.back().frame;

    QPointF cam = medianOver(f, first, first + settle);
    keys << Key{first, cam.x(), cam.y()};
    int outside = -1;     // index where the subject left the dead zone
    int busyUntil = first; // end of the running move
    for (size_t k = 0; k < f.size(); ++k) {
        if (f[k].frame < busyUntil) continue;
        const bool away = std::abs(f[k].x - cam.x()) > dzX || std::abs(f[k].y - cam.y()) > dzY;
        if (!away) {
            outside = -1;
            continue;
        }
        if (outside < 0) outside = int(k);
        if (f[k].frame - f[size_t(outside)].frame < minHold && k + 1 < f.size()) continue;
        // Where the subject stays now
        const QPointF m = medianOver(f, f[k].frame, f[k].frame + settle);
        const QPointF to(keep(m.x(), ww, cropX), keep(m.y(), wh, cropY));
        if (std::abs(to.x() - cam.x()) <= dzX && std::abs(to.y() - cam.y()) <= dzY) {
            outside = -1; // it came back
            continue;
        }
        // Offline: the move may start when the subject left
        const int start = std::max(keys.back().frame, f[size_t(outside)].frame);
        const double dist = std::max(cropX ? std::abs(to.x() - cam.x()) / ww : 0.0,
                                     cropY ? std::abs(to.y() - cam.y()) / wh : 0.0);
        const int length = std::clamp(int(std::lround(dist / speed * fps)), std::max(1, int(0.25 * fps)),
                                      std::max(1, int(2.5 * fps)));
        int end = std::min(start + length, std::max(last, start + 1));
        if (start > keys.back().frame) keys << Key{start, cam.x(), cam.y()};
        if (end <= keys.back().frame) end = keys.back().frame + 1;
        keys << Key{end, to.x(), to.y()};
        cam = to;
        busyUntil = end;
        outside = -1;
    }
}

} // namespace

QVector<Key> path(const QVector<Target>& targets, const PathParams& p)
{
    QVector<Key> keys;
    if (targets.isEmpty()) return {Key{}};
    for (int i0 = 0; i0 < targets.size();) {
        int i1 = i0 + 1;
        while (i1 < targets.size() && !targets[i1].cut) ++i1;
        QVector<Key> seg;
        segmentPath(targets.mid(i0, i1 - i0), p, seg);
        if (!keys.isEmpty()) {
            // A cut: the old position up to the frame before, the new one from the cut on
            const Key held = keys.back();
            const int before = seg.front().frame - 1;
            if (before > held.frame) keys << Key{before, held.x, held.y};
            while (!keys.isEmpty() && keys.back().frame >= seg.front().frame) keys.removeLast();
        }
        keys += seg;
        i0 = i1;
    }
    // Keys in the middle of a hold add nothing
    QVector<Key> out;
    for (int i = 0; i < keys.size(); ++i) {
        const auto same = [](const Key& a, const Key& b) { return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9; };
        if (i > 0 && i + 1 < keys.size() && same(keys[i - 1], keys[i]) && same(keys[i], keys[i + 1])) continue;
        out << keys[i];
    }
    // A still camera: one key
    if (out.size() == 2 && std::abs(out[0].x - out[1].x) < 1e-9 && std::abs(out[0].y - out[1].y) < 1e-9)
        out.removeLast();
    return out;
}

Fill fill(double mediaAspect, QSizeF frame)
{
    Fill f;
    f.picture = frame;
    if (mediaAspect <= 0 || frame.isEmpty()) return f;
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    f.zoom = std::max(frame.width() / fitted.width(), frame.height() / fitted.height());
    f.picture = fitted * f.zoom;
    f.windowW = std::min(1.0, frame.width() / f.picture.width());
    f.windowH = std::min(1.0, frame.height() / f.picture.height());
    return f;
}

QPointF position(const Fill& f, QSizeF frame, double x, double y)
{
    const double maxX = std::max(0.0, (f.picture.width() - frame.width()) / 2);
    const double maxY = std::max(0.0, (f.picture.height() - frame.height()) / 2);
    // To see a part right of the middle the picture moves left; y counts upward
    const double px = std::clamp((0.5 - x) * f.picture.width(), -maxX, maxX);
    const double py = std::clamp((y - 0.5) * f.picture.height(), -maxY, maxY);
    return {std::abs(px) < 1e-6 ? 0.0 : px, std::abs(py) < 1e-6 ? 0.0 : py};
}

void apply(Clip& c, Mode mode, const QVector<Key>& keys, double mediaAspect, QSizeF frame)
{
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY}) c.keys.remove(a);
    ClipTransform& t = c.transform;
    t.transformOn = true;
    if (mode == Mode::Fit) {
        t.zoomX = t.zoomY = 1;
        t.posX = t.posY = 0;
        t.fill = FitFill::Blur;
        return;
    }
    t.fill = FitFill::None;
    const Fill f = fill(mediaAspect, frame);
    t.zoomX = t.zoomY = f.zoom;
    if (mode == Mode::Center || keys.isEmpty()) {
        t.posX = t.posY = 0;
        return;
    }
    KeyTrack px, py;
    for (const Key& k : keys) {
        if (k.frame < 0 || k.frame >= c.length()) continue;
        const QPointF pos = position(f, frame, k.x, k.y);
        px << Keyframe{c.in + k.frame, pos.x(), KeyEase::EaseInOut};
        py << Keyframe{c.in + k.frame, pos.y(), KeyEase::EaseInOut};
    }
    const QPointF first = position(f, frame, keys.front().x, keys.front().y);
    t.posX = first.x();
    t.posY = first.y();
    const auto moves = [](const KeyTrack& k) {
        return std::any_of(k.begin(), k.end(), [&](const Keyframe& x) { return std::abs(x.value - k.front().value) > 1e-6; });
    };
    if (px.size() > 1 && moves(px)) c.keys[AnimParam::PosX] = px;
    if (py.size() > 1 && moves(py)) c.keys[AnimParam::PosY] = py;
}

bool canReframe(const Clip& c) { return c.kind == ClipKind::Media && !c.mediaPath.isEmpty(); }

} // namespace Reframe
