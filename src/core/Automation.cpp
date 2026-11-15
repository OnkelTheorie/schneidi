// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Automation.h"

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

AutoCurve normalized(AutoCurve curve)
{
    curve.removeIf([](const AutoPoint& p) { return p.frame < 0 || !std::isfinite(p.db); });
    for (AutoPoint& p : curve) p.db = std::clamp(p.db, kMinDb, kMaxDb);
    std::stable_sort(curve.begin(), curve.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.frame < b.frame; });
    AutoCurve out;
    out.reserve(curve.size());
    for (const AutoPoint& p : std::as_const(curve)) {
        if (!out.isEmpty() && out.last().frame == p.frame) out.last() = p; // the last one given wins
        else out << p;
    }
    return out;
}

void replaceRange(AutoCurve& curve, int from, int to, const AutoCurve& inside)
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
    curve = normalized(next);
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
    curve = normalized(out);
}

void retime(AutoCurve& curve, const std::function<int(int)>& f)
{
    for (AutoPoint& p : curve) p.frame = f(p.frame);
    curve = normalized(curve);
}

QJsonArray toJson(const AutoCurve& curve)
{
    QJsonArray a;
    for (const AutoPoint& p : curve) a << QJsonArray{p.frame, std::round(p.db * 1000) / 1000};
    return a;
}

AutoCurve fromJson(const QJsonArray& a)
{
    AutoCurve c;
    for (const QJsonValue& v : a) {
        const QJsonArray p = v.toArray();
        if (p.size() >= 2 && p[0].isDouble() && p[1].isDouble()) c << AutoPoint{p[0].toInt(), p[1].toDouble()};
    }
    return normalized(c);
}

} // namespace Automation
