// SPDX-License-Identifier: AGPL-3.0-only
#include "core/VolumeDraw.h"

#include "core/Automation.h"
#include "core/Keyframes.h"

#include <algorithm>
#include <cmath>

namespace VolumeDraw {

double posOf(double db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if (db < -20) return 0.25 * (db - kMinVolumeDb) / (-20 - kMinVolumeDb);
    if (db < 0) return 0.25 + 0.5 * (db + 20) / 20;
    return 0.75 + 0.25 * db / kMaxVolumeDb;
}

double dbAt(double pos)
{
    pos = std::clamp(pos, 0.0, 1.0);
    if (pos < 0.25) return kMinVolumeDb + pos / 0.25 * (-20 - kMinVolumeDb);
    if (pos < 0.75) return -20 + (pos - 0.25) / 0.5 * 20;
    return (pos - 0.75) / 0.25 * kMaxVolumeDb;
}

QVector<double> resample(const QVector<Point>& stroke, int* first, double minValue, double maxValue)
{
    if (stroke.isEmpty()) return {};
    double lo = stroke.first().frame, hi = lo;
    for (const Point& p : stroke) {
        lo = std::min(lo, p.frame);
        hi = std::max(hi, p.frame);
    }
    const int from = int(std::lround(lo)), to = int(std::lround(hi));
    *first = from;
    QVector<double> v(to - from + 1, 0.0);
    QVector<bool> set(v.size(), false);
    auto put = [&](int f, double db) {
        const int i = f - from;
        if (i < 0 || i >= v.size()) return;
        v[i] = std::clamp(db, minValue, maxValue);
        set[i] = true;
    };
    put(int(std::lround(stroke.first().frame)), stroke.first().db);
    // Every frame between two pointer positions gets the straight line between them; the later segment wins
    for (int s = 1; s < stroke.size(); ++s) {
        const Point& a = stroke[s - 1];
        const Point& b = stroke[s];
        const int fa = int(std::lround(a.frame)), fb = int(std::lround(b.frame));
        const int step = fb >= fa ? 1 : -1;
        for (int f = fa;; f += step) {
            const double t = fa == fb ? 1.0 : double(f - fa) / (fb - fa);
            put(f, a.db + (b.db - a.db) * t);
            if (f == fb) break;
        }
    }
    // A path is continuous, so every frame is set; just in case, a gap takes its left neighbour
    for (int i = 1; i < v.size(); ++i)
        if (!set[i]) v[i] = v[i - 1];
    return v;
}

QVector<double> smooth(const QVector<double>& values, int radius)
{
    if (radius <= 0 || values.size() < 3) return values;
    QVector<double> out(values.size());
    // Prefix sums: O(n) for any radius
    QVector<double> sum(values.size() + 1, 0.0);
    for (int i = 0; i < values.size(); ++i) sum[i + 1] = sum[i] + values[i];
    for (int i = 0; i < values.size(); ++i) {
        const int a = std::max(0, i - radius), b = std::min(int(values.size()) - 1, i + radius);
        out[i] = (sum[b + 1] - sum[a]) / (b - a + 1);
    }
    return out;
}

QVector<int> thin(const QVector<double>& values, double tolerance)
{
    if (values.isEmpty()) return {};
    if (values.size() == 1) return {0};
    QVector<bool> keep(values.size(), false);
    keep.first() = keep.last() = true;
    // Ramer-Douglas-Peucker without recursion (long strokes)
    QVector<std::pair<int, int>> todo{{0, int(values.size()) - 1}};
    while (!todo.isEmpty()) {
        const auto [a, b] = todo.takeLast();
        if (b - a < 2) continue;
        int worst = -1;
        double worstErr = tolerance;
        for (int i = a + 1; i < b; ++i) {
            const double line = values[a] + (values[b] - values[a]) * double(i - a) / (b - a);
            const double err = std::abs(values[i] - line);
            if (err > worstErr) {
                worstErr = err;
                worst = i;
            }
        }
        if (worst < 0) continue;
        keep[worst] = true;
        todo.append({a, worst});
        todo.append({worst, b});
    }
    QVector<int> out;
    for (int i = 0; i < keep.size(); ++i)
        if (keep[i]) out << i;
    return out;
}

bool spansEnough(const QVector<Point>& stroke)
{
    if (stroke.isEmpty()) return false;
    double lo = stroke.first().frame, hi = lo;
    for (const Point& p : stroke) {
        lo = std::min(lo, p.frame);
        hi = std::max(hi, p.frame);
    }
    return std::lround(hi) - std::lround(lo) >= kMinSpan;
}

bool apply(Clip& c, const QVector<Point>& stroke, int smoothing)
{
    if (!spansEnough(stroke)) return false;
    int first = 0;
    const QVector<double> raw = resample(stroke, &first);
    if (raw.isEmpty()) return false;
    const int last = first + int(raw.size()) - 1;
    // The part of the stroke over this clip (timeline frames)
    const int lo = std::max(first, c.start), hi = std::min(last, c.end() - 1);
    if (lo > hi) return false;
    // Smoothed over the whole stroke (the clip's part looks the same as drawn over several clips), then cut to the clip
    const QVector<double> all = smooth(raw, smoothing);
    const QVector<double> part = all.mid(lo - first, hi - lo + 1);

    // The old curve just outside the stroke, before anything changes
    const int t0 = lo - c.start, t1 = hi - c.start; // clip frames
    const bool before = t0 > 0, after = t1 < c.length() - 1;
    const double oldBefore = before ? Keys::valueAt(c, AnimParam::Volume, t0 - 1) : 0;
    const double oldAfter = after ? Keys::valueAt(c, AnimParam::Volume, t1 + 1) : 0;

    // Keys under the stroke go; a clip without any keeps its static volume as the level outside the stroke
    KeyTrack& keys = c.keys[AnimParam::Volume];
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [&](const Keyframe& k) { return k.frame >= c.in + t0 && k.frame <= c.in + t1; }),
               keys.end());
    if (before) Keys::setKey(c, AnimParam::Volume, t0 - 1, oldBefore);
    if (after) Keys::setKey(c, AnimParam::Volume, t1 + 1, oldAfter);
    for (int i : thin(part)) Keys::setKey(c, AnimParam::Volume, t0 + i, std::round(part[i] * 100) / 100);
    return true;
}

bool applyTrack(Track& t, const QVector<Point>& stroke, int smoothing, AutoTarget target)
{
    if (!spansEnough(stroke)) return false;
    int first = 0;
    const bool pan = target == AutoTarget::Pan;
    const QVector<double> raw = pan ? resample(stroke, &first, -100, 100) : resample(stroke, &first);
    if (raw.isEmpty()) return false;
    const QVector<double> all = smooth(raw, smoothing);
    // Frames before 0 (a stroke started left of the timeline's start) are left out
    const int skip = std::max(0, -first);
    if (skip >= all.size()) return false;
    const QVector<double> part = all.mid(skip);
    const int from = first + skip;
    AutoCurve inside;
    // Pan: thinned to half a step (a pan curve has 200 steps where the volume has 0.25 dB)
    for (int i : thin(part, pan ? 0.5 : kTolerance))
        inside << AutoPoint{from + i, std::round(part[i] * 100) / 100 - Automation::base(t, target)};
    Automation::replaceRange(t.automation(target), from, from + int(part.size()) - 1, inside, target);
    return true;
}

} // namespace VolumeDraw
