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
    double weight = 1;
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

// Weighted median of values (value, weight)
double weightedMedian(std::vector<std::pair<double, double>> v)
{
    std::sort(v.begin(), v.end());
    double total = 0;
    for (const auto& [x, w] : v) total += w;
    double sum = 0;
    for (const auto& [x, w] : v)
        if ((sum += w) >= total / 2) return x;
    return v.empty() ? 0.5 : v.back().first;
}

// Weighted median over the points with frames in [from, to], plus the middle of the picture with weight `prior` per
// point: sure measurements decide, unsure ones (a game's busy picture) leave the middle (the crosshair)
QPointF weightedOver(const std::vector<Point>& pts, int from, int to, double prior, double cx, double cy)
{
    std::vector<std::pair<double, double>> xs, ys;
    for (const Point& p : pts)
        if (p.frame >= from && p.frame <= to) {
            xs.push_back({p.x, p.weight});
            ys.push_back({p.y, p.weight});
            xs.push_back({cx, prior});
            ys.push_back({cy, prior});
        }
    if (xs.empty()) return {cx, cy};
    return {weightedMedian(xs), weightedMedian(ys)};
}

// One shot (no cut inside): holds and eased moves. previous = the camera before a cut: a new shot whose subject is
// within the dead zone of it keeps it (a flash or a menu counted as a cut must not jolt the picture)
void segmentPath(const QVector<Target>& seg, const PathParams& p, QVector<Key>& keys, const Key* previous)
{
    const double s = std::clamp(p.smoothness, 0.0, 1.0);
    const double fps = std::max(1.0, p.fps);
    const double ww = std::clamp(p.windowW, 0.01, 1.0), wh = std::clamp(p.windowH, 0.01, 1.0);
    const bool cropX = ww < 0.999, cropY = wh < 0.999;
    const auto keep = [](double v, double w, bool crop) { return crop ? std::clamp(v, w / 2, 1 - w / 2) : 0.5; };

    // Each side kept where the window can go
    std::vector<Point> raw;
    raw.reserve(size_t(seg.size()));
    for (const Target& t : seg)
        raw.push_back({t.frame, keep(t.x, ww, cropX), keep(t.y, wh, cropY), std::clamp(t.weight, 0.0, 1.0)});
    // Weighted median over a short time with a pull to the middle: single wrong measurements (a flash, a passing
    // object) do not move the camera, unsure ones lean towards the middle (game footage: the crosshair)
    const int half = std::max(1, int(std::lround((0.25 + 0.5 * s) * fps)));
    constexpr double kPrior = 0.1;
    std::vector<Point> f(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const QPointF m = weightedOver(raw, raw[i].frame - half, raw[i].frame + half, kPrior, 0.5, 0.5);
        f[i] = {raw[i].frame, keep(m.x(), ww, cropX), keep(m.y(), wh, cropY)};
    }

    const double dzX = ww * (0.08 + 0.22 * s), dzY = wh * (0.08 + 0.22 * s);
    const int minHold = std::max(1, int(std::lround((0.15 + 0.6 * s) * fps)));
    const int settle = std::max(1, int(std::lround((0.3 + 0.5 * s) * fps)));
    // Fastest move: window widths (heights) per second
    const double speed = 2.5 - 2.0 * s;
    const int first = f.front().frame, last = f.back().frame;

    QPointF cam = medianOver(f, first, first + settle);
    if (previous && std::abs(previous->x - cam.x()) <= dzX && std::abs(previous->y - cam.y()) <= dzY)
        cam = QPointF(previous->x, previous->y);
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
        // Arriving just before the shot ends: a quicker move that still holds a while (a short clip, the subject
        // moving late), else none - a jolt for nothing (the next shot starts anew)
        if (end + minHold > last && last - minHold - start >= std::max(1, int(0.25 * fps))) end = last - minHold;
        if (end + minHold > last && start > first) {
            outside = -1;
            busyUntil = last + 1;
            continue;
        }
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
        segmentPath(targets.mid(i0, i1 - i0), p, seg, keys.isEmpty() ? nullptr : &keys.back());
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

namespace {

bool whole(const QRectF& c)
{
    return c.left() <= 1e-4 && c.top() <= 1e-4 && c.right() >= 1 - 1e-4 && c.bottom() >= 1 - 1e-4;
}

QRectF valid(const QRectF& c)
{
    const QRectF r = c.normalized().intersected(QRectF(0, 0, 1, 1));
    return r.width() >= 0.01 && r.height() >= 0.01 ? r : QRectF(0, 0, 1, 1);
}

// Fit mode: the content fitted into the frame, centred
Fill fitIn(double mediaAspect, QSizeF frame, const QRectF& content)
{
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    const QSizeF size(fitted.width() * content.width(), fitted.height() * content.height());
    const double zoom = std::min(frame.width() / size.width(), frame.height() / size.height());
    return Fill{zoom, fitted * zoom, content, 1, 1};
}

// The part of the picture the static crop leaves (fractions of the picture fitted as `fitted`); the whole picture
// with the crop off or animated
QRectF cropContent(const Clip& c, QSizeF fitted)
{
    const ClipTransform& t = c.transform;
    if (!t.cropOn || fitted.isEmpty()) return QRectF(0, 0, 1, 1);
    for (AnimParam a : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom})
        if (c.keys.contains(a)) return QRectF(0, 0, 1, 1);
    const double l = t.cropLeft / fitted.width(), r = t.cropRight / fitted.width();
    const double top = t.cropTop / fitted.height(), b = t.cropBottom / fitted.height();
    return valid(QRectF(l, top, 1 - l - r, 1 - top - b));
}

bool same(double a, double b) { return std::abs(a - b) <= 1e-6 * std::max(1.0, std::abs(b)); }

} // namespace

Fill fill(double mediaAspect, QSizeF frame, const QRectF& contentRect)
{
    Fill f;
    f.picture = frame;
    f.content = valid(contentRect);
    if (mediaAspect <= 0 || frame.isEmpty()) return f;
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    const QSizeF content(fitted.width() * f.content.width(), fitted.height() * f.content.height());
    f.zoom = std::max(frame.width() / content.width(), frame.height() / content.height());
    f.picture = fitted * f.zoom;
    f.windowW = std::min(1.0, frame.width() / (content.width() * f.zoom));
    f.windowH = std::min(1.0, frame.height() / (content.height() * f.zoom));
    return f;
}

QPointF position(const Fill& f, QSizeF frame, double x, double y)
{
    Q_UNUSED(frame);
    // Window centre kept inside the content, then as a share of the whole picture
    const double cx = f.windowW >= 0.999 ? 0.5 : std::clamp(x, f.windowW / 2, 1 - f.windowW / 2);
    const double cy = f.windowH >= 0.999 ? 0.5 : std::clamp(y, f.windowH / 2, 1 - f.windowH / 2);
    const double px = f.content.left() + cx * f.content.width(), py = f.content.top() + cy * f.content.height();
    // To see a part right of the middle the picture moves left; y counts upward
    const double ox = (0.5 - px) * f.picture.width(), oy = (py - 0.5) * f.picture.height();
    return {std::abs(ox) < 1e-6 ? 0.0 : ox, std::abs(oy) < 1e-6 ? 0.0 : oy};
}

bool cropBars(Clip& c, const QRectF& contentRect, double mediaAspect, QSizeF frame)
{
    if (c.kind != ClipKind::Media || whole(contentRect)) return false;
    const QRectF content = valid(contentRect);
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    ClipTransform& t = c.transform;
    const ClipTransform before = t;
    const auto keysBefore = c.keys;
    for (AnimParam a : {AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom}) c.keys.remove(a);
    t.cropOn = true;
    t.cropLeft = content.left() * fitted.width();
    t.cropRight = (1 - content.right()) * fitted.width();
    t.cropTop = content.top() * fitted.height();
    t.cropBottom = (1 - content.bottom()) * fitted.height();
    for (double* v : {&t.cropLeft, &t.cropRight, &t.cropTop, &t.cropBottom})
        if (std::abs(*v) < 1e-6) *v = 0;
    return !(t == before) || c.keys != keysBefore;
}

void apply(Clip& c, Mode mode, const QVector<Key>& keys, double mediaAspect, QSizeF frame, const QRectF& contentRect)
{
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY}) c.keys.remove(a);
    const bool bars = !whole(contentRect) && mediaAspect > 0;
    if (bars) cropBars(c, contentRect, mediaAspect, frame);
    ClipTransform& t = c.transform;
    t.transformOn = true;
    const Fill f = fill(mediaAspect, frame, bars ? contentRect : QRectF(0, 0, 1, 1));
    if (mode == Mode::Fit) {
        t.zoomX = t.zoomY = 1;
        t.posX = t.posY = 0;
        t.fill = FitFill::Blur;
        if (bars) {
            // The content fitted in instead of the picture with its bars
            const Fill fz = fitIn(mediaAspect, frame, f.content);
            t.zoomX = t.zoomY = fz.zoom;
            const QPointF p = position(fz, frame, 0.5, 0.5);
            t.posX = p.x();
            t.posY = p.y();
        }
        return;
    }
    t.fill = FitFill::None;
    t.zoomX = t.zoomY = f.zoom;
    if (mode == Mode::Center || keys.isEmpty()) {
        const QPointF p = position(f, frame, 0.5, 0.5);
        t.posX = p.x();
        t.posY = p.y();
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

bool reshape(Clip& c, double oldAspect, double newAspect, QSizeF frame)
{
    ClipTransform& t = c.transform;
    if (oldAspect <= 0 || newAspect <= 0 || frame.isEmpty() || c.keys.contains(AnimParam::ZoomX)
        || c.keys.contains(AnimParam::ZoomY))
        return false;
    const QRectF content = cropContent(c, MediaAspect::fit(newAspect, frame));
    if (t.fill != FitFill::None) {
        // Fit: centred, the content fitted in (zoom 1 without bars: nothing to change)
        const Fill a = fitIn(oldAspect, frame, content), b = fitIn(newAspect, frame, content);
        const QPointF was = position(a, frame, 0.5, 0.5);
        if (!same(t.zoomX, a.zoom) || !same(t.zoomY, a.zoom) || !same(t.posX, was.x()) || !same(t.posY, was.y())
            || c.keys.contains(AnimParam::PosX) || c.keys.contains(AnimParam::PosY))
            return false;
        const QPointF now = position(b, frame, 0.5, 0.5);
        t.zoomX = t.zoomY = b.zoom;
        t.posX = now.x();
        t.posY = now.y();
        return true;
    }
    // Filling the frame: zoomed past the fitted picture (zoom 1 is a picture fitted in, which the new shape keeps)
    const Fill a = fill(oldAspect, frame, content), b = fill(newAspect, frame, content);
    if (a.zoom <= 1 + 1e-6 || !same(t.zoomX, a.zoom) || !same(t.zoomY, a.zoom)) return false;
    t.zoomX = t.zoomY = b.zoom;
    // Each position: the window centre on the old picture (a fraction of the content), put there on the new one
    const auto x = [&](double v) {
        const double px = 0.5 - v / a.picture.width();
        return position(b, frame, (px - content.left()) / content.width(), 0.5).x();
    };
    const auto y = [&](double v) {
        const double py = 0.5 + v / a.picture.height();
        return position(b, frame, 0.5, (py - content.top()) / content.height()).y();
    };
    Keys::convert(c, AnimParam::PosX, x, b.picture.width() / a.picture.width());
    Keys::convert(c, AnimParam::PosY, y, b.picture.height() / a.picture.height());
    return true;
}

bool canReframe(const Clip& c) { return c.kind == ClipKind::Media && !c.mediaPath.isEmpty() && c.keepRegion.isEmpty(); }

bool isOverlay(const Clip& c, int track, double mediaAspect, QSizeF frame)
{
    if (track <= 0 || !canReframe(c) || frame.isEmpty()) return false;
    const ClipTransform& t = c.transform;
    if (t.transformOn) {
        // The largest zoom it reaches (an overlay zooming to full screen on a keyframe is a picture of its own)
        const auto largest = [&](AnimParam p, double v) {
            double z = std::abs(v);
            for (const Keyframe& k : c.keys.value(p)) z = std::max(z, std::abs(k.value));
            return z;
        };
        if (std::max(largest(AnimParam::ZoomX, t.zoomX), largest(AnimParam::ZoomY, t.zoomY)) < 0.95) return true;
    }
    if (t.cropOn) {
        if (mediaAspect <= 0) mediaAspect = frame.width() / frame.height();
        const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
        if (fitted.width() > 0 && fitted.height() > 0) {
            const double w = 1 - (Keys::staticValue(c, AnimParam::CropLeft) + Keys::staticValue(c, AnimParam::CropRight))
                                     / fitted.width();
            const double h = 1 - (Keys::staticValue(c, AnimParam::CropTop) + Keys::staticValue(c, AnimParam::CropBottom))
                                     / fitted.height();
            if (std::clamp(w, 0.0, 1.0) * std::clamp(h, 0.0, 1.0) < 0.7) return true;
        }
    }
    return false;
}

Clip keepCopy(const Clip& base, const Keep& k, double mediaAspect, QSizeF frame, const KeepPlace& place)
{
    Clip c = base;
    c.id = 0;
    c.linkId = 0;
    c.renderCache = false;
    c.keepRegion = k.id.isEmpty() ? QStringLiteral("custom") : k.id;
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation,
                        AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom})
        c.keys.remove(a);
    const QRectF r = valid(k.region);
    if (mediaAspect <= 0 && !frame.isEmpty()) mediaAspect = frame.width() / frame.height();
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    ClipTransform t; // opacity and fades stay as the base's: the copy appears and goes with it
    t.opacity = base.transform.opacity;
    t.compositeOn = base.transform.compositeOn;
    t.cropLeft = r.left() * fitted.width();
    t.cropRight = (1 - r.right()) * fitted.width();
    t.cropTop = r.top() * fitted.height();
    t.cropBottom = (1 - r.bottom()) * fitted.height();
    for (double* v : {&t.cropLeft, &t.cropRight, &t.cropTop, &t.cropBottom})
        if (std::abs(*v) < 1e-6) *v = 0;
    const double w = r.width() * fitted.width(), h = r.height() * fitted.height();
    if (w > 0 && h > 0 && !frame.isEmpty()) {
        const double z = std::min(place.width * frame.width() / w, place.maxHeight * frame.height() / h);
        t.zoomX = t.zoomY = z;
        // Zoom and position act on the whole picture around its centre: move the region's centre to the place
        const double dx = (r.center().x() - 0.5) * fitted.width(), dy = (r.center().y() - 0.5) * fitted.height();
        t.posX = -z * dx;
        t.posY = frame.height() / 2 - place.top * frame.height() - z * h / 2 + z * dy;
        if (std::abs(t.posX) < 1e-6) t.posX = 0;
    }
    c.transform = t;
    return c;
}

bool reshapeKeep(Clip& c, double oldAspect, double newAspect, QSizeF frame)
{
    if (c.keepRegion.isEmpty() || oldAspect <= 0 || newAspect <= 0 || frame.isEmpty()) return false;
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY})
        if (c.keys.contains(a)) return false;
    const Keep k{c.keepRegion, cropContent(c, MediaAspect::fit(newAspect, frame))};
    const ClipTransform was = keepCopy(c, k, oldAspect, frame).transform;
    ClipTransform& t = c.transform;
    if (!same(t.zoomX, was.zoomX) || !same(t.zoomY, was.zoomY) || !same(t.posX, was.posX) || !same(t.posY, was.posY))
        return false; // placed by hand
    const ClipTransform now = keepCopy(c, k, newAspect, frame).transform;
    t.zoomX = now.zoomX;
    t.zoomY = now.zoomY;
    t.posX = now.posX;
    t.posY = now.posY;
    return true;
}

bool isKeepOf(const Clip& c, const Clip& base)
{
    return !c.keepRegion.isEmpty() && c.mediaPath == base.mediaPath && c.start == base.start && c.in == base.in
           && c.out == base.out && c.speed == base.speed && c.reverse == base.reverse && c.ramp == base.ramp;
}

bool isKeepAlong(const Clip& c, const Clip& base)
{
    if (isKeepOf(c, base)) return true;
    // Trimmed, moved or bladed since (the copy is not linked to it): the same file in sync where they overlap
    return !c.keepRegion.isEmpty() && base.keepRegion.isEmpty() && c.mediaPath == base.mediaPath && !c.isRetimed()
           && !base.isRetimed() && !c.freeze && !base.freeze && c.start - c.in == base.start - base.in
           && c.start < base.end() && c.end() > base.start;
}

QRectF inContent(const QRectF& region, const QRectF& content)
{
    const QRectF in = valid(content);
    return QRectF(in.x() + region.x() * in.width(), in.y() + region.y() * in.height(), region.width() * in.width(),
                  region.height() * in.height());
}

bool cropsSides(Mode mode, double mediaAspect, QSizeF frame, const QRectF& content)
{
    return mode != Mode::Fit && fill(mediaAspect, frame, content).windowW < 0.95;
}

} // namespace Reframe
