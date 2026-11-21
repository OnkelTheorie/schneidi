// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Reframe.h"

#include "core/Keyframes.h"
#include "core/GameHud.h"
#include "core/MediaAspect.h"

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
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

// One shot (no cut inside): holds and eased moves. previous = the camera before a cut: a new shot whose subject is
// within the dead zone of it keeps it (a flash or a menu counted as a cut must not jolt the picture)
//
// The camera commits to a place and holds it: the holds are chosen for the whole shot at once (offline: the future is
// known) as the cheapest sequence of places, where every sample costs by how far its subject lies from the window
// centre beyond a comfortable zone (up to leaving the window) and every move costs as much as a subject a good bit off
// for a second or so. An excursion that turns back soon, or whose subject does not clearly win, is dropped. Two
// subjects that take turns (two people, a cyclist and a tree, flickering measurements) thus get one place for both
// (or the one that matters most), not a camera swinging between them; a subject that goes somewhere else and stays
// there gets one move. Moves are calm eased pans; turning back needs a hold first; pans in the same direction close
// after each other become one.
void segmentPath(const QVector<Target>& seg, const PathParams& p, QVector<Key>& keys, const Key* previous)
{
    const double s = std::clamp(p.smoothness, 0.0, 1.0);
    const double fps = std::max(1.0, p.fps);
    const double ww = std::clamp(p.windowW, 0.01, 1.0), wh = std::clamp(p.windowH, 0.01, 1.0);
    const bool cropX = ww < 0.999, cropY = wh < 0.999;
    const int first = seg.front().frame, last = seg.back().frame;
    // Fill crops one side only (the other fills the frame); both = the wider crop decides, the other stays centred
    if (!cropX && !cropY) {
        keys << Key{first, 0.5, 0.5};
        return;
    }
    const bool alongX = cropX && (!cropY || ww <= wh);
    const double w = alongX ? ww : wh;
    const double lo = w / 2, hi = 1 - w / 2;
    const auto keyAt = [&](int frame, double v) { return alongX ? Key{frame, v, 0.5} : Key{frame, 0.5, v}; };

    struct Sample {
        int frame;
        double v;      // subject centre on the cropped side, kept where the window can go
        double weight; // how much it counts: the square of how sure the measurement is (sure ones decide clearly)
        double dt;     // seconds this sample stands for
    };
    // Unsure measurements (a busy picture) lean to the middle: every sample pulls there with this weight too (as much
    // as a measurement of weight 0.1)
    constexpr double kPrior = 0.01;
    std::vector<Sample> pts;
    pts.reserve(size_t(seg.size()));
    double scale = 0; // the mean pull of a sample (subject and middle): a move costs in proportion
    for (const Target& t : seg) {
        const double weight = std::clamp(t.weight, 0.0, 1.0);
        pts.push_back({t.frame, std::clamp(alongX ? t.x : t.y, lo, hi), weight * weight, 0});
        scale += weight * weight + kPrior;
    }
    scale /= double(pts.size());
    for (size_t k = 0; k < pts.size(); ++k) {
        const int a = k > 0 ? pts[k - 1].frame : pts[k].frame, b = k + 1 < pts.size() ? pts[k + 1].frame : pts[k].frame;
        pts[k].dt = std::max(1.0, (b - a) / 2.0) / fps;
    }

    // Cost per second of a subject `d` window widths from the window centre: none in a comfortable zone, then
    // growing until it is out of the window (further away is not worse: lost is lost - a subject far off for a moment
    // must not buy a move)
    const double zone = 0.08 + 0.22 * s;
    const auto off = [zone](double d) { return std::max(0.0, std::min(d, 0.6) - zone); };
    const auto cost = [&](const Sample& q, double at) {
        return q.dt * (q.weight * off(std::abs(q.v - at) / w) + kPrior * off(std::abs(0.5 - at) / w));
    };
    // A move costs as much as an average subject a quarter window beyond the comfortable zone for this long: subjects
    // taking turns for shorter do not pay a move there and back
    const double commit = 0.5 + 1.0 * s; // seconds
    const double moveCost = 0.25 * commit * scale;

    // Places the camera can hold (a twentieth of the window apart)
    std::vector<double> places;
    const int n = std::clamp(int(std::ceil((hi - lo) / (0.05 * w))) + 1, 1, 201);
    for (int j = 0; j < n; ++j) places.push_back(n == 1 ? 0.5 : lo + (hi - lo) * j / (n - 1));
    if (n > 1) places.push_back(0.5); // the middle exactly
    const int m = int(places.size());
    // Viterbi: cheapest sequence of places through the samples
    std::vector<double> best(static_cast<size_t>(m)), next(static_cast<size_t>(m));
    std::vector<std::vector<int>> from(pts.size(), std::vector<int>(static_cast<size_t>(m), 0));
    for (int j = 0; j < m; ++j) best[size_t(j)] = cost(pts[0], places[size_t(j)]);
    for (size_t k = 1; k < pts.size(); ++k) {
        int arg = 0;
        for (int j = 1; j < m; ++j)
            if (best[size_t(j)] < best[size_t(arg)]) arg = j;
        for (int j = 0; j < m; ++j) {
            double c = best[size_t(j)];
            int src = j;
            const double moved = best[size_t(arg)] + moveCost;
            if (moved < c) c = moved, src = arg;
            next[size_t(j)] = c + cost(pts[k], places[size_t(j)]);
            from[k][size_t(j)] = src;
        }
        std::swap(best, next);
    }
    std::vector<int> state(pts.size());
    {
        int j = 0;
        for (int i = 1; i < m; ++i)
            if (best[size_t(i)] < best[size_t(j)]) j = i;
        for (size_t k = pts.size(); k-- > 0;) {
            state[k] = j;
            j = from[k][size_t(j)];
        }
    }
    // Weight of the samples [a, b] that a window at `at` shows (their pull to the middle counts as a subject there)
    const auto inView = [&](size_t a, size_t b, double at) {
        const bool middle = std::abs(0.5 - at) < 0.45 * w;
        double sum = 0;
        for (size_t i = a; i <= b; ++i)
            sum += pts[i].dt * (pts[i].weight * (std::abs(pts[i].v - at) < 0.45 * w) + kPrior * middle);
        return sum;
    };
    // Runs of one place. An excursion that turns back (to one subject and back to the other) is no run of its own
    // when it is short or its samples do not clearly show its subject rather than the one before (subjects taking
    // turns: the cheapest path may still pay for those when they are sure and far apart): its holds become one
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t k = 0; k < pts.size();) {
        size_t e = k;
        while (e + 1 < pts.size() && state[e + 1] == state[k]) ++e;
        runs.push_back({k, e});
        k = e + 1;
    }
    const double excursion = (1.0 + 2.0 * s) * fps; // frames
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t r = 1; r + 1 < runs.size(); ++r) {
            const double a = places[size_t(state[runs[r - 1].first])], b = places[size_t(state[runs[r].first])],
                         c = places[size_t(state[runs[r + 1].first])];
            const int length = pts[runs[r + 1].first].frame - pts[runs[r].first].frame;
            double all = 0;
            for (size_t i = runs[r].first; i <= runs[r].second; ++i) all += pts[i].dt * (pts[i].weight + kPrior);
            const bool clear = inView(runs[r].first, runs[r].second, b) - inView(runs[r].first, runs[r].second, a)
                               >= 0.5 * all;
            if ((b - a) * (c - b) < 0 && std::abs(c - b) >= 0.5 * std::abs(b - a) && (length < excursion || !clear)) {
                for (size_t i = runs[r].first; i <= runs[r + 1].second; ++i) state[i] = state[runs[r - 1].first];
                runs[r - 1].second = runs[r + 1].second;
                runs.erase(runs.begin() + std::ptrdiff_t(r), runs.begin() + std::ptrdiff_t(r) + 2);
                merged = true;
                break;
            }
        }
    }
    // Holds: runs of one place. Each is held on the weighted median of its samples (with the pull to the middle),
    // unless the place found keeps clearly more of them in the window (two subjects taking turns: between them)
    struct Hold {
        size_t a, b; // samples [a, b]
        double at;
    };
    std::vector<Hold> holds;
    for (const auto& [k, e] : runs) {
        std::vector<std::pair<double, double>> values;
        double all = 0;
        for (size_t i = k; i <= e; ++i) {
            values.push_back({pts[i].v, pts[i].dt * pts[i].weight});
            values.push_back({0.5, pts[i].dt * kPrior});
            all += pts[i].dt * (pts[i].weight + kPrior);
        }
        std::sort(values.begin(), values.end());
        double total = 0, sum = 0, at = 0.5;
        for (const auto& [v, x] : values) total += x;
        for (const auto& [v, x] : values)
            if ((sum += x) >= total / 2) {
                at = v;
                break;
            }
        // The place showing the most (two subjects that both fit: between them), the nearest to the median of those
        double place = at, most = inView(k, e, at);
        for (double c : places) {
            const double v = inView(k, e, c);
            if (v > most + 1e-12 || (std::abs(v - most) <= 1e-12 && std::abs(c - at) < std::abs(place - at))) {
                most = v;
                place = c;
            }
        }
        if (most > inView(k, e, at) + 0.2 * all) at = place;
        holds.push_back({k, e, at});
    }
    // A new shot whose place is close to the camera before the cut keeps that
    if (previous) {
        const double was = alongX ? previous->x : previous->y;
        if (std::abs(was - holds.front().at) <= w * zone) holds.front().at = was;
    }

    // Moves between the holds: eased, starting when the subject went (offline); turning back only after a hold
    const int minHold = std::max(1, int(std::lround((0.6 + 1.2 * s) * fps)));  // before a move back
    const int endHold = std::max(1, int(std::lround((0.15 + 0.6 * s) * fps))); // before the shot ends
    const double speed = 1.5 - 1.0 * s; // fastest move: window widths per second (a calm pan, not a jump)
    QVector<Key> out;
    out << keyAt(first, holds.front().at);
    double cam = holds.front().at, lastDir = 0;
    int lastMoveEnd = std::numeric_limits<int>::min() / 2;
    for (size_t h = 1; h < holds.size(); ++h) {
        const double to = holds[h].at;
        if (std::abs(to - cam) <= 0.1 * w) continue; // (a nudge is no move)
        const double dir = to > cam ? 1 : -1;
        const int leave = pts[holds[h].a].frame;
        const int length = std::clamp(int(std::lround(std::abs(to - cam) / w / speed * fps)),
                                      std::max(1, int(std::lround((0.4 + 0.8 * s) * fps))),
                                      std::max(1, int(std::lround(2.5 * fps))));
        int start = std::max({leave, out.back().frame, dir == lastDir ? lastMoveEnd : lastMoveEnd + minHold});
        int end = start + length;
        // Arriving just before the shot ends: a quicker move that still holds a while, else none (the next shot
        // starts anew)
        if (end + endHold > last && last - endHold - start >= std::max(1, int(std::lround(0.25 * fps))))
            end = last - endHold;
        if (end + endHold > last && start > first) continue;
        if (start > out.back().frame) out << keyAt(start, cam);
        if (end <= out.back().frame) end = out.back().frame + 1;
        out << keyAt(end, to);
        cam = to;
        lastDir = dir;
        lastMoveEnd = end;
    }
    // Two pans the same way with a short hold between: one pan (the hold's keys go)
    const int merge = std::max(1, int(std::lround((1.0 + 1.0 * s) * fps)));
    const auto value = [alongX](const Key& k) { return alongX ? k.x : k.y; };
    for (int i = 1; i + 2 < out.size();) {
        // out[i-1] -> out[i] a move, out[i] -> out[i+1] a hold, out[i+1] -> out[i+2] a move
        const double d1 = value(out[i]) - value(out[i - 1]), hold = value(out[i + 1]) - value(out[i]),
                     d2 = value(out[i + 2]) - value(out[i + 1]);
        if (std::abs(hold) < 1e-9 && std::abs(d1) > 1e-9 && d1 * d2 > 0 && out[i + 1].frame - out[i].frame < merge) {
            out.remove(i, 2);
            continue;
        }
        ++i;
    }
    keys += out;
}

} // namespace

QVector<Key> path(const QVector<Target>& targets, const PathParams& p)
{
    QVector<Key> keys;
    if (targets.isEmpty() || p.game) return {Key{}};
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

namespace {

// The crop and placement of a copy showing region `r` (fractions of the picture fitted as `fitted`) at `place` in
// `frame`; lineHeight > 0 (part of the picture height): its text at most place.maxLine of the frame height
// maxZoom > 0: at most that zoom (the text size limit when the line height is not known)
void placeRegion(ClipTransform& t, const QRectF& r, QSizeF fitted, QSizeF frame, const KeepPlace& place,
                 double lineHeight = 0, double maxZoom = 0)
{
    t.cropLeft = r.left() * fitted.width();
    t.cropRight = (1 - r.right()) * fitted.width();
    t.cropTop = r.top() * fitted.height();
    t.cropBottom = (1 - r.bottom()) * fitted.height();
    for (double* v : {&t.cropLeft, &t.cropRight, &t.cropTop, &t.cropBottom})
        if (std::abs(*v) < 1e-6) *v = 0;
    const double w = r.width() * fitted.width(), h = r.height() * fitted.height();
    if (w > 0 && h > 0 && !frame.isEmpty()) {
        double z = std::min(place.width * frame.width() / w, place.maxHeight * frame.height() / h);
        if (lineHeight > 0 && place.maxLine > 0)
            z = std::min(z, place.maxLine * frame.height() / (lineHeight * fitted.height()));
        if (maxZoom > 0) z = std::min(z, maxZoom);
        t.zoomX = t.zoomY = z;
        // Zoom and position act on the whole picture around its centre: move the region's centre to the place
        const double dx = (r.center().x() - 0.5) * fitted.width(), dy = (r.center().y() - 0.5) * fitted.height();
        t.posX = -z * dx;
        t.posY = frame.height() / 2 - place.top * frame.height() - z * h / 2 + z * dy;
        if (std::abs(t.posX) < 1e-6) t.posX = 0;
    }
}

constexpr double kFeedFade = 0.15;  // seconds: the copy fades in before its first line, out after its last
constexpr double kFeedMove = 0.2;   // seconds: its edges move to the lines there are
constexpr double kFeedGap = 0.5;    // seconds: lines missing for a shorter moment are still there (flash, hit tint)
constexpr double kFeedBlip = 0.25;  // seconds: lines seen for a shorter moment are not there (a sign in the picture)
constexpr double kFeedPadX = 0.35;  // left of the widest line, part of a line's height
constexpr double kFeedPadY = 0.3;   // below the last line, part of a line's height

constexpr int kFeedSteady = 3;     // samples an edge is seen in (fewer samples: all of them)
constexpr double kFeedSame = 0.004; // ... within this much (part of the picture)

// The smallest value that `steady` of the values come close to (all of them when there are fewer)
double steadyLeast(std::vector<double> v, int steady, double same)
{
    std::sort(v.begin(), v.end());
    const size_t need = std::min(v.size(), size_t(std::max(1, steady)));
    for (size_t i = 0; i < v.size(); ++i) {
        size_t n = 0;
        for (size_t k = i; k < v.size() && v[k] <= v[i] + same; ++k) ++n;
        if (n >= need) return v[i];
    }
    return v.back();
}

// The region a state shows: from its widest line to the region's right edge, the region's top to below its last line
QRectF stateRegion(const FeedState& s, const QRectF& region, double lineHeight, double mediaAspect)
{
    const double padY = kFeedPadY * lineHeight, padX = kFeedPadX * lineHeight / std::max(0.1, mediaAspect);
    const double left = std::clamp(s.left - padX, 0.0, region.right() - 0.01);
    const double bottom = std::clamp(s.bottom + padY, region.top() + 0.01, 1.0);
    return QRectF(QPointF(left, region.top()), QPointF(region.right(), bottom));
}

} // namespace

FeedUse feedUse(const Feed& f)
{
    if (!f.measured) return FeedUse::Whole;
    // A line in a stretch after the blips and gaps are filtered (a single sample is none)
    double first = 0, last = -1;
    for (const FeedSample& s : f.samples) {
        first = std::min(first, s.frame);
        last = std::max(last, s.frame);
    }
    const QVector<FeedState> states = feedStates(f, int(std::ceil(last)) + 1);
    if (std::any_of(states.begin(), states.end(), [](const FeedState& s) { return s.lines > 0; })) return FeedUse::Follow;
    // None over a long stretch of footage: the analysis does not know this HUD, the whole region stays
    double seconds = 0;
    const bool files = !f.samples.isEmpty()
                       && std::all_of(f.samples.begin(), f.samples.end(), [](const FeedSample& s) { return s.file >= 0; });
    if (files) {
        const auto [lo, hi] = std::minmax_element(f.samples.begin(), f.samples.end(),
                                                  [](const FeedSample& a, const FeedSample& b) { return a.file < b.file; });
        seconds = hi->file - lo->file;
    } else if (!f.samples.isEmpty()) {
        seconds = (last - first) / (f.fps > 0 ? f.fps : 25);
    }
    return seconds >= kFeedFallback - 0.5 ? FeedUse::Whole : FeedUse::None; // (samples a tenth of a second apart)
}

bool feedLineless(const Feed& f) { return f.measured && feedUse(f) != FeedUse::Follow; }

QVector<FeedState> feedStates(const Feed& f, int length)
{
    if (!f.measured || length <= 0) return {};
    QVector<FeedSample> in;
    for (const FeedSample& s : f.samples)
        if (s.frame > -1 && s.frame < length) in << s;
    if (in.isEmpty()) return {FeedState{0, length, 0, 1, 0}};
    const int n = int(in.size());
    const double fps = f.fps > 0 ? f.fps : 25;
    // Closing (fills dips shorter than kFeedGap), then opening (drops peaks shorter than kFeedBlip): per sample the
    // largest (smallest) count within half the time either side, then the smallest (largest) of those. Time in the
    // clip or in the file, whichever reaches further: a fast clip shows a moment of the file shorter (it must not
    // flicker), a slowed one longer (its samples lie further apart than the moment: a blip is one sample still)
    const auto pass = [&](const std::vector<int>& v, double seconds, bool largest) {
        std::vector<int> out(v.size());
        const double half = seconds / 2;
        const auto within = [&](int i, int j) {
            return std::abs(in[i].frame - in[j].frame) <= half * fps
                   || (in[i].file >= 0 && in[j].file >= 0 && std::abs(in[i].file - in[j].file) <= half + 1e-9);
        };
        for (int i = 0; i < n; ++i) {
            int x = v[size_t(i)];
            for (int j = i - 1; j >= 0 && within(i, j); --j)
                x = largest ? std::max(x, v[size_t(j)]) : std::min(x, v[size_t(j)]);
            for (int j = i + 1; j < n && within(i, j); ++j)
                x = largest ? std::max(x, v[size_t(j)]) : std::min(x, v[size_t(j)]);
            out[size_t(i)] = x;
        }
        return out;
    };
    std::vector<int> count(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) count[size_t(i)] = std::max(0, in[i].lines);
    // A filled dip takes the larger count next to it, not the smaller: a new line slides in for a few frames that
    // read none (or a flash hides the feed just as a line comes), and the copy must not stay a line short while it
    // shows (its last line cut off)
    {
        const std::vector<int> raw = count, grown = pass(count, kFeedGap, true);
        count = pass(grown, kFeedGap, false);
        for (size_t i = 0; i < count.size(); ++i)
            if (count[i] > raw[i]) count[i] = grown[i];
    }
    count = pass(pass(count, kFeedBlip, false), kFeedBlip, true);

    QVector<FeedState> out;
    for (int i = 0; i < n;) {
        int j = i;
        while (j + 1 < n && count[size_t(j + 1)] == count[size_t(i)]) ++j;
        FeedState s;
        // Boundaries halfway between the samples (the first and last stretch reach the clip's ends)
        s.from = i == 0 ? 0 : int(std::lround((in[i - 1].frame + in[i].frame) / 2));
        s.to = j == n - 1 ? length : int(std::lround((in[j].frame + in[j + 1].frame) / 2));
        s.lines = count[size_t(i)];
        if (s.lines > 0) {
            // The widest line and the lowest bottom of the samples that saw this many lines (else of all that saw
            // some: a stretch held over a moment without), each seen in a few samples: one sample that took some
            // game picture next to a line for its box does not widen the whole stretch
            for (int pickSame : {1, 0}) {
                std::vector<double> lefts, bottoms;
                for (int k = i; k <= j; ++k)
                    if (in[k].lines > 0 && (!pickSame || in[k].lines == s.lines)) {
                        lefts.push_back(in[k].left);
                        bottoms.push_back(-in[k].bottom);
                    }
                if (lefts.empty()) continue;
                s.left = steadyLeast(lefts, kFeedSteady, kFeedSame);
                s.bottom = -steadyLeast(bottoms, kFeedSteady, kFeedSame);
                break;
            }
            if (s.bottom <= 0) s.lines = 0; // nothing measured to show
        }
        if (s.to > s.from) {
            if (!out.isEmpty() && out.last().lines == s.lines && (s.lines == 0 || (out.last().left == s.left
                                                                                    && out.last().bottom == s.bottom)))
                out.last().to = s.to;
            else
                out << s;
        }
        i = j + 1;
    }
    if (!out.isEmpty()) out.last().to = length;
    return out;
}

Clip keepCopy(const Clip& base, const Keep& k, double mediaAspect, QSizeF frame, const KeepPlace& place)
{
    Clip c = base;
    c.id = 0;
    c.linkId = 0;
    c.renderCache = false;
    c.keepRegion = k.id.isEmpty() ? QStringLiteral("custom") : k.id;
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::Rotation,
                        AnimParam::TiltX, AnimParam::TiltY, AnimParam::Fov, AnimParam::CropLeft, AnimParam::CropRight,
                        AnimParam::CropTop, AnimParam::CropBottom})
        c.keys.remove(a);
    const QRectF r = valid(k.region);
    if (mediaAspect <= 0 && !frame.isEmpty()) mediaAspect = frame.width() / frame.height();
    const QSizeF fitted = MediaAspect::fit(mediaAspect, frame);
    ClipTransform t; // opacity and fades stay as the base's: the copy appears and goes with it
    t.opacity = base.transform.opacity;
    t.compositeOn = base.transform.compositeOn;
    placeRegion(t, r, fitted, frame, place);
    c.transform = t;
    const FeedUse use = feedUse(k.feed);
    if (use == FeedUse::None) {
        // No line: hidden all along (the callers leave such a copy out)
        c.keys.remove(AnimParam::Opacity);
        c.transform.opacity = 0;
        c.transform.compositeOn = true;
        return c;
    }
    // Whole: the region all the time, as the base shows (its own opacity keys stay)
    const QVector<FeedState> states = use == FeedUse::Follow ? feedStates(k.feed, c.length()) : QVector<FeedState>{};
    if (states.isEmpty() || frame.isEmpty()) return c;

    // Follows the feed: per state its crop and place, keys where they change
    const double lineHeight = k.feed.lineHeight > 0 ? k.feed.lineHeight : r.height() / 5;
    const auto shape = [&](const FeedState& s) {
        ClipTransform x = t;
        placeRegion(x, stateRegion(s, r, lineHeight, mediaAspect), fitted, frame, place, k.feed.lineHeight);
        return x;
    };
    const double fps = k.feed.fps > 0 ? k.feed.fps : 25;
    const int fade = std::max(1, int(std::lround(kFeedFade * fps))), move = std::max(1, int(std::lround(kFeedMove * fps)));
    const int last = std::max(0, c.length() - 1);
    // Keys per parameter as (clip frame, value); a later key on the same frame replaces an earlier one
    struct Key {
        int frame;
        double value;
    };
    QHash<AnimParam, QVector<Key>> keys;
    const QVector<AnimParam> shapeParams{AnimParam::CropLeft, AnimParam::CropBottom, AnimParam::ZoomX, AnimParam::ZoomY,
                                         AnimParam::PosX, AnimParam::PosY};
    const auto valueOf = [](const ClipTransform& x, AnimParam p) {
        switch (p) {
        case AnimParam::CropLeft: return x.cropLeft;
        case AnimParam::CropBottom: return x.cropBottom;
        case AnimParam::ZoomX: return x.zoomX;
        case AnimParam::ZoomY: return x.zoomY;
        case AnimParam::PosX: return x.posX;
        default: return x.posY;
        }
    };
    const auto shapeKey = [&](int frame, const ClipTransform& x) {
        frame = std::clamp(frame, 0, last);
        for (AnimParam p : shapeParams) keys[p] << Key{frame, valueOf(x, p)};
    };
    // Shown (1) or hidden (0); the opacity is the base's times that (below)
    QVector<Key> shown;
    const auto opacityKey = [&](int frame, double on) { shown << Key{std::clamp(frame, 0, last), on}; };
    // The first shape shown is the static one too (a copy without keys looks like that)
    const FeedState* firstShown = nullptr;
    for (const FeedState& s : states)
        if (s.lines > 0) {
            firstShown = &s;
            break;
        }
    if (firstShown) {
        const ClipTransform x = shape(*firstShown);
        for (AnimParam p : shapeParams) {
            ClipTransform& y = c.transform;
            switch (p) {
            case AnimParam::CropLeft: y.cropLeft = x.cropLeft; break;
            case AnimParam::CropBottom: y.cropBottom = x.cropBottom; break;
            case AnimParam::ZoomX: y.zoomX = x.zoomX; break;
            case AnimParam::ZoomY: y.zoomY = x.zoomY; break;
            case AnimParam::PosX: y.posX = x.posX; break;
            default: y.posY = x.posY; break;
            }
        }
    }
    opacityKey(0, states.first().lines > 0 ? 1 : 0);
    shapeKey(0, firstShown ? shape(states.first().lines > 0 ? states.first() : *firstShown) : t);
    for (int i = 1; i < states.size(); ++i) {
        const FeedState& a = states[i - 1];
        const FeedState& b = states[i];
        const int at = b.from;
        if (a.lines == 0 && b.lines > 0) {
            // Fade in, already in its shape
            shapeKey(at - fade, shape(b));
            opacityKey(at - fade, 0);
            opacityKey(at, 1);
        } else if (a.lines > 0 && b.lines == 0) {
            opacityKey(at, 1);
            opacityKey(at + fade, 0);
            shapeKey(at + fade, shape(a)); // the shape stays while it fades
        } else if (a.lines > 0 && b.lines > 0) {
            // Growing (a new line, a wider one) is done when the line shows; shrinking starts when it went
            const bool grows = b.lines > a.lines || b.left < a.left - 1e-9 || b.bottom > a.bottom + 1e-9;
            shapeKey(grows ? at - move : at, shape(a));
            shapeKey(grows ? at : at + move, shape(b));
        }
    }
    // Opacity: the base's (with its own keys; 100 with compositing off) times shown, at the keys of both
    {
        std::stable_sort(shown.begin(), shown.end(), [](const Key& x, const Key& y) { return x.frame < y.frame; });
        Clip on; // the shown curve, eased as the copy's keys are
        on.in = 0;
        KeyTrack& track = on.keys[AnimParam::Opacity];
        for (const Key& key : shown) {
            if (!track.isEmpty() && track.last().frame == key.frame) track.removeLast();
            track << Keyframe{key.frame, key.value, KeyEase::EaseInOut};
        }
        QVector<int> frames;
        for (const Keyframe& key : track) frames << key.frame;
        if (base.transform.compositeOn && base.keys.contains(AnimParam::Opacity)) {
            frames << 0 << last; // (its keys before and after the clip: their values at its ends)
            for (const Keyframe& key : base.keys.value(AnimParam::Opacity))
                if (key.frame - base.in >= 0 && key.frame - base.in <= last) frames << key.frame - base.in;
        }
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        for (int f : frames) {
            const double opacity = base.transform.compositeOn ? Keys::valueAt(base, AnimParam::Opacity, f) : 100.0;
            keys[AnimParam::Opacity] << Key{f, opacity * Keys::valueAt(on, AnimParam::Opacity, f)};
        }
    }
    // Into the clip's keys (source frames), sorted, a key per frame (the later one), eased; a track that never
    // changes is no track (the static value holds)
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        QVector<Key> list = it.value();
        std::stable_sort(list.begin(), list.end(), [](const Key& x, const Key& y) { return x.frame < y.frame; });
        KeyTrack track;
        for (const Key& key : list) {
            if (!track.isEmpty() && track.last().frame == c.in + key.frame) track.removeLast();
            track << Keyframe{c.in + key.frame, key.value, KeyEase::EaseInOut};
        }
        // A key between two of the same value adds nothing
        for (int i = int(track.size()) - 2; i >= 1; --i)
            if (track[i].value == track[i - 1].value && track[i].value == track[i + 1].value) track.removeAt(i);
        const bool flat = std::all_of(track.begin(), track.end(), [&](const Keyframe& x) {
            return std::abs(x.value - track.first().value) < 1e-9;
        });
        if (flat) {
            c.keys.remove(it.key()); // (the base's own opacity keys too)
            if (track.isEmpty()) continue;
            Keys::setStaticValue(c, it.key(), track.first().value);
            if (it.key() == AnimParam::Opacity && std::abs(track.first().value - 100) > 1e-9)
                c.transform.compositeOn = true; // else the opacity does not apply
            continue;
        }
        c.keys[it.key()] = track;
        if (it.key() == AnimParam::Opacity) c.transform.compositeOn = true; // else the opacity does not apply
    }
    return c;
}

namespace {

// reshapeKeep of a copy with keys (one that follows its kill feed's lines): per key its region from the crop there,
// placed for the old shape as the keys show it (else placed by hand: false) and anew for the new one. The line height
// is not stored: the largest zoom of the keys is taken as the text size limit (exact when a key was held by it; the
// limit grows or shrinks with the picture's height)
bool reshapeKeyedKeep(Clip& c, double oldAspect, double newAspect, QSizeF frame)
{
    const QSizeF a = MediaAspect::fit(oldAspect, frame), b = MediaAspect::fit(newAspect, frame);
    if (a.isEmpty() || b.isEmpty()) return false;
    const QVector<AnimParam> crops{AnimParam::CropLeft, AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom};
    const QVector<AnimParam> places{AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY};
    QVector<int> frames = Keys::keyTimes(c, crops + places);
    if (frames.isEmpty()) frames << 0;
    // The region at a clip frame (fractions of the picture; the crop is in the new shape's pixels already)
    const auto regionAt = [&](const std::function<double(AnimParam)>& value) {
        const double l = value(AnimParam::CropLeft) / b.width(), r = value(AnimParam::CropRight) / b.width();
        const double top = value(AnimParam::CropTop) / b.height(), bottom = value(AnimParam::CropBottom) / b.height();
        return valid(QRectF(l, top, 1 - l - r, 1 - top - bottom));
    };
    const auto keyed = [&](int f) { return [&c, f](AnimParam p) { return Keys::valueAt(c, p, f); }; };
    double largest = 0;
    for (int f : frames) largest = std::max(largest, Keys::valueAt(c, AnimParam::ZoomX, f));
    if (largest <= 0) return false;
    const KeepPlace place;
    const auto valueOf = [](const ClipTransform& x, AnimParam p) {
        return p == AnimParam::ZoomX ? x.zoomX : p == AnimParam::ZoomY ? x.zoomY : p == AnimParam::PosX ? x.posX : x.posY;
    };
    // Placed as before?
    for (int f : frames) {
        ClipTransform was;
        placeRegion(was, regionAt(keyed(f)), a, frame, place, 0, largest);
        for (AnimParam p : places)
            if (std::abs(valueOf(was, p) - Keys::valueAt(c, p, f)) > 1e-4 * std::max(1.0, std::abs(valueOf(was, p))))
                return false; // placed by hand
    }
    const double limit = largest * a.height() / b.height();
    QHash<AnimParam, KeyTrack> tracks;
    for (int f : frames) {
        ClipTransform now;
        placeRegion(now, regionAt(keyed(f)), b, frame, place, 0, limit);
        for (AnimParam p : places) tracks[p] << Keyframe{c.in + f, valueOf(now, p), KeyEase::EaseInOut};
    }
    // The static values: the static crop's region
    ClipTransform still;
    placeRegion(still, regionAt([&c](AnimParam p) { return Keys::staticValue(c, p); }), b, frame, place, 0, limit);
    for (AnimParam p : places) {
        Keys::setStaticValue(c, p, valueOf(still, p));
        KeyTrack track = tracks.value(p);
        for (int i = int(track.size()) - 2; i >= 1; --i)
            if (same(track[i].value, track[i - 1].value) && same(track[i].value, track[i + 1].value)) track.removeAt(i);
        const bool flat = std::all_of(track.begin(), track.end(), [&](const Keyframe& x) { return same(x.value, track.first().value); });
        if (flat) {
            c.keys.remove(p);
            if (!track.isEmpty()) Keys::setStaticValue(c, p, track.first().value);
        } else {
            c.keys[p] = track;
        }
    }
    return true;
}

} // namespace

bool reshapeKeep(Clip& c, double oldAspect, double newAspect, QSizeF frame)
{
    if (c.keepRegion.isEmpty() || oldAspect <= 0 || newAspect <= 0 || frame.isEmpty()) return false;
    for (AnimParam a : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::PosX, AnimParam::PosY, AnimParam::CropLeft,
                        AnimParam::CropRight, AnimParam::CropTop, AnimParam::CropBottom})
        if (c.keys.contains(a)) return reshapeKeyedKeep(c, oldAspect, newAspect, frame);
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

QVector<Keep> keepsOf(const Clip& c, Mode mode, QSizeF frame, const QRectF& content, const KeepChoice& choice,
                      bool* noGame)
{
    if (noGame) *noGame = false;
    if (choice.kind == KeepChoice::None) return {};
    if (choice.kind == KeepChoice::Custom) return {Keep{QStringLiteral("custom"), choice.region, {}}};
    const GameHud::Game* game = choice.game.isEmpty() ? GameHud::guess(c.mediaPath) : GameHud::find(choice.game);
    if (!game) {
        if (choice.kind == KeepChoice::KillFeed && noGame) *noGame = true;
        return {};
    }
    if (choice.kind == KeepChoice::KillFeed || cropsSides(mode, MediaAspect::ofClip(c), frame, content))
        return {Keep{QStringLiteral("killfeed"), inContent(game->feed, content), {}}};
    return {};
}

bool cropsSides(Mode mode, double mediaAspect, QSizeF frame, const QRectF& content)
{
    return mode != Mode::Fit && fill(mediaAspect, frame, content).windowW < 0.95;
}

} // namespace Reframe
