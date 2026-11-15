// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Automation.h"

#include "core/VolumeDraw.h"

#include <algorithm>
#include <cmath>

namespace Automation {

double valueAt(const AutoCurve& curve, double frame)
{
    if (curve.isEmpty()) return 0.0;
    if (frame <= curve.first().frame) return curve.first().db;
    if (frame >= curve.last().frame) return curve.last().db;
    // first point after `frame` (there is one: frame < last)
    const auto it = std::upper_bound(curve.cbegin(), curve.cend(), frame,
                                     [](double f, const AutoPoint& p) { return f < p.frame; });
    const AutoPoint& b = *it;
    const AutoPoint& a = *(it - 1);
    if (b.frame == a.frame) return b.db;
    return a.db + (b.db - a.db) * (frame - a.frame) / (b.frame - a.frame);
}

double levelAt(const Track& track, double frame)
{
    return std::clamp(track.volumeDb + valueAt(track.volumeAuto, frame), kMinVolumeDb, kMaxVolumeDb);
}

double panAt(const Track& track, double frame)
{
    return std::clamp(track.pan + valueAt(track.panAuto, frame), -100.0, 100.0);
}

double heardAt(const Track& track, AutoTarget t, double frame)
{
    return t == AutoTarget::Pan ? panAt(track, frame) : levelAt(track, frame);
}

namespace {

// Sorted, one point per frame, frames >= 0, values clamped to [lo, hi]
AutoCurve sortedUnique(AutoCurve curve, double lo, double hi)
{
    curve.removeIf([](const AutoPoint& p) { return p.frame < 0 || !std::isfinite(p.db); });
    for (AutoPoint& p : curve) p.db = std::clamp(p.db, lo, hi);
    std::stable_sort(curve.begin(), curve.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.frame < b.frame; });
    AutoCurve out;
    out.reserve(curve.size());
    for (const AutoPoint& p : std::as_const(curve)) {
        if (!out.isEmpty() && out.last().frame == p.frame) out.last() = p; // the last one given wins
        else out << p;
    }
    return out;
}

// Ripple and retime only move points: values stay whatever they are (a pan value is beyond the dB range)
AutoCurve sortedOnly(AutoCurve curve) { return sortedUnique(std::move(curve), -HUGE_VAL, HUGE_VAL); }

} // namespace

AutoCurve normalized(AutoCurve curve, AutoTarget target)
{
    return sortedUnique(std::move(curve), minValue(target), maxValue(target));
}

void replaceRange(AutoCurve& curve, int from, int to, const AutoCurve& inside, AutoTarget target)
{
    if (to < from) std::swap(from, to);
    from = std::max(0, from);
    to = std::max(0, to);
    const double before = valueAt(curve, from - 1), after = valueAt(curve, to + 1);
    AutoCurve next;
    for (const AutoPoint& p : std::as_const(curve))
        if (p.frame < from || p.frame > to) next << p;
    if (from > 0) next << AutoPoint{from - 1, before};
    next << AutoPoint{to + 1, after};
    for (const AutoPoint& p : inside)
        if (p.frame >= from && p.frame <= to) next << p;
    curve = normalized(next, target);
}

void ripple(AutoCurve& curve, const QVector<QPair<int, int>>& shifts)
{
    if (curve.isEmpty() || shifts.isEmpty()) return;
    const AutoCurve old = curve;
    const auto offsetAt = [&](int f, bool strict) {
        int d = 0;
        for (const auto& s : shifts)
            if (strict ? s.first < f : s.first <= f) d += s.second;
        return d;
    };
    struct Item {
        AutoPoint p;     // before the shift (moved) or already at its place (fixed)
        bool helper;     // added here to keep the values at an edge (dropped again when it changes nothing)
        bool fixed;      // already placed (the start of time put in)
    };
    QVector<Item> items;
    QVector<QPair<int, int>> cut; // time taken out [a, b), before the shift
    for (const auto& s : shifts) {
        if (s.second < 0) {
            const int a = s.first + s.second, b = s.first;
            cut << qMakePair(a, b);
            if (a > 0) items << Item{{a - 1, valueAt(old, a - 1)}, true, false};
            items << Item{{b, valueAt(old, b)}, true, false};
        } else if (s.second > 0) {
            const double v = valueAt(old, s.first);
            items << Item{{s.first, v}, true, false}; // moves to the end of the time put in
            items << Item{{s.first + offsetAt(s.first, true), v}, true, true};
        }
    }
    for (const AutoPoint& p : old) items << Item{p, false, false}; // after the helpers: an original point wins
    const auto inCut = [&](int f) {
        return std::any_of(cut.cbegin(), cut.cend(), [f](const QPair<int, int>& c) { return f >= c.first && f < c.second; });
    };
    QVector<Item> placed;
    for (Item it : std::as_const(items)) {
        if (it.fixed) {
            placed << it;
            continue;
        }
        if (inCut(it.p.frame)) continue;
        it.p.frame += offsetAt(it.p.frame, false);
        if (it.p.frame >= 0) placed << it;
    }
    std::stable_sort(placed.begin(), placed.end(), [](const Item& a, const Item& b) { return a.p.frame < b.p.frame; });
    QVector<Item> unique;
    for (const Item& it : std::as_const(placed)) {
        if (!unique.isEmpty() && unique.last().p.frame == it.p.frame) {
            if (!it.helper || unique.last().helper) unique.last() = it;
        } else {
            unique << it;
        }
    }
    // Helpers that only repeat what the line through their neighbours says go again
    AutoCurve out;
    for (int i = 0; i < unique.size(); ++i) {
        const Item& it = unique[i];
        if (it.helper) {
            const AutoPoint* prev = out.isEmpty() ? nullptr : &out.last();
            const AutoPoint* next = i + 1 < unique.size() ? &unique[i + 1].p : nullptr;
            double line;
            if (prev && next) line = prev->db + (next->db - prev->db) * double(it.p.frame - prev->frame) / (next->frame - prev->frame);
            else if (prev) line = prev->db;
            else if (next) line = next->db;
            else line = it.p.db + 1; // the only point: keep
            if (std::abs(line - it.p.db) < 1e-9) continue;
        }
        out << it.p;
    }
    curve = sortedOnly(out);
}

void retime(AutoCurve& curve, const std::function<int(int)>& f)
{
    for (AutoPoint& p : curve) p.frame = f(p.frame);
    curve = sortedOnly(curve);
}

void rippleTrack(Track& t, const QVector<QPair<int, int>>& shifts)
{
    if (!std::as_const(t).volumeAuto.isEmpty()) ripple(t.volumeAuto, shifts);
    if (!std::as_const(t).panAuto.isEmpty()) ripple(t.panAuto, shifts);
}

void retimeTrack(Track& t, const std::function<int(int)>& f)
{
    if (!std::as_const(t).volumeAuto.isEmpty()) retime(t.volumeAuto, f);
    if (!std::as_const(t).panAuto.isEmpty()) retime(t.panAuto, f);
}

QJsonArray toJson(const AutoCurve& curve)
{
    QJsonArray a;
    for (const AutoPoint& p : curve) a << QJsonArray{p.frame, std::round(p.db * 1000) / 1000};
    return a;
}

AutoCurve fromJson(const QJsonArray& a, AutoTarget target)
{
    AutoCurve c;
    for (const QJsonValue& v : a) {
        const QJsonArray p = v.toArray();
        if (p.size() >= 2 && p[0].isDouble() && p[1].isDouble()) c << AutoPoint{p[0].toInt(), p[1].toDouble()};
    }
    return normalized(c, target);
}

// ---------- Write mode (touch) ----------

int glideBackFrames(double fps) { return std::max(1, int(std::lround(0.5 * fps))); }

Touch beginTouch(const Track& t, AutoTarget target)
{
    Touch touch;
    touch.target = target;
    touch.original = t.automation(target);
    touch.baseValue = base(t, target);
    return touch;
}

void record(Touch& touch, int frame, double heard)
{
    if (frame < 0 || !std::isfinite(heard)) return;
    if (!touch.heard.isEmpty()) {
        if (frame < touch.heard.last().first) return; // a jump back: the recording keeps going forward only
        if (frame == touch.heard.last().first) {
            touch.heard.last().second = heard;
            return;
        }
    } else {
        touch.start = frame;
    }
    touch.heard << qMakePair(frame, heard);
}

AutoCurve thinned(const Touch& touch, double fps)
{
    if (touch.heard.isEmpty()) return {};
    // One value per frame (frames skipped while playing get the straight line), on top of the fader / knob
    const int first = touch.heard.first().first, last = touch.heard.last().first;
    QVector<double> v(last - first + 1);
    for (int i = 0; i < touch.heard.size(); ++i) {
        const auto& [f, h] = touch.heard[i];
        v[f - first] = h - touch.baseValue;
        if (i + 1 < touch.heard.size()) {
            const auto& [g, k] = touch.heard[i + 1];
            for (int x = f + 1; x < g; ++x) v[x - first] = (h + (k - h) * double(x - f) / (g - f)) - touch.baseValue;
        }
    }
    const double step = touch.target == AutoTarget::Pan ? kWriteStepPan : kWriteStepDb;
    const QVector<int> keep = VolumeDraw::thin(v, step / 2);
    // At most kWriteKeysPerSecond keys: a key closer than that to the one before goes (the last one stays)
    const int gap = std::max(1, int(std::ceil(fps / kWriteKeysPerSecond - 1e-9)));
    AutoCurve out;
    for (int i : keep) {
        const AutoPoint p{first + i, std::round(v[i] * 100) / 100};
        if (!out.isEmpty() && p.frame - out.last().frame < gap) {
            if (i != keep.last()) continue;
            if (out.size() > 1) out.removeLast(); // the last value wins over the one too close before it
        }
        out << p;
    }
    return normalized(out, touch.target);
}

AutoCurve touchingCurve(const Touch& touch, double fps)
{
    if (touch.heard.isEmpty()) return touch.original;
    AutoCurve c;
    for (const AutoPoint& p : touch.original)
        if (p.frame < touch.start) c << p;
    if (touch.start > 0) c << AutoPoint{touch.start - 1, valueAt(touch.original, touch.start - 1)};
    c << thinned(touch, fps); // after the last key its value holds
    return normalized(c, touch.target);
}

AutoCurve releasedCurve(const Touch& touch, int frame, double fps)
{
    if (touch.heard.isEmpty()) return touch.original;
    AutoCurve rec = thinned(touch, fps);
    const int end = std::max(frame, touch.heard.last().first);
    if (rec.last().frame < end) rec << AutoPoint{end, rec.last().db}; // held until the release
    AutoCurve c = touch.original;
    // The glide back: from the release to the old curve glideBackFrames later (replaceRange's point after `to`)
    replaceRange(c, touch.start, end + glideBackFrames(fps) - 1, rec, touch.target);
    return c;
}

} // namespace Automation
