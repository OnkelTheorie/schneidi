// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ShortPick.h"

#include <algorithm>
#include <cmath>

namespace ShortPick {

QVector<Moment> joined(QVector<Moment> list, double gap)
{
    std::sort(list.begin(), list.end(),
              [](const Moment& a, const Moment& b) { return a.file != b.file ? a.file < b.file : a.from < b.from; });
    QVector<Moment> out;
    for (const Moment& m : list) {
        // Not across a death screen: the earlier one ended by a death (its hi) before where the later one may begin
        // (lo, else from). Another hi is only "a little after the last hit": no reason to keep two clips apart
        const bool apart = !out.isEmpty() && out.last().died && out.last().hi >= 0
                        && out.last().hi < (m.lo >= 0 ? m.lo : m.from);
        if (!out.isEmpty() && out.last().file == m.file && m.from - out.last().to < gap && !apart) {
            // The earlier one's lo; the end's hi of the one reaching further (no limit when either has none)
            if (m.to > out.last().to) out.last().hi = m.hi, out.last().died = m.died;
            else if (m.hi < 0) out.last().hi = -1, out.last().died = false;
            if (m.lo < 0) out.last().lo = -1;
            out.last().to = std::max(out.last().to, m.to);
            out.last().score = std::max(out.last().score, m.score);
        } else {
            out << m;
        }
    }
    return out;
}

void widen(Moment& m, double by, double duration)
{
    double before = by * 0.45, after = by - before;
    const double first = m.lo >= 0 ? std::min(m.lo, m.from) : 0.0;
    const double last = m.hi >= 0 ? std::max(m.to, std::min(m.hi, duration)) : duration;
    const double roomBefore = std::max(0.0, m.from - first), roomAfter = std::max(0.0, last - m.to);
    if (before > roomBefore) after += before - roomBefore, before = roomBefore;
    if (after > roomAfter) before = std::min(roomBefore, before + after - roomAfter), after = roomAfter;
    m.from -= before;
    m.to += after;
}

double total(const QVector<Moment>& moments)
{
    double s = 0;
    for (const Moment& m : moments) s += m.to - m.from;
    return s;
}

QVector<Moment> pick(const QVector<Moment>& candidates, const QVector<double>& durations, const Options& o)
{
    const double maxS = std::max(1.0, o.maxS), minS = std::clamp(o.minS, 0.0, maxS);
    const auto duration = [&](int file) { return file >= 0 && file < durations.size() ? durations[file] : 0.0; };
    double material = 0;
    for (double d : durations) material += std::max(0.0, d);
    if (material <= 0) return {};

    // All recordings together are short: everything
    if (material <= minS) {
        QVector<Moment> all;
        for (int i = 0; i < durations.size(); ++i)
            if (durations[i] > 0) all << Moment{i, 0, durations[i], 0};
        return all;
    }

    QVector<Moment> picked;
    for (Moment m : candidates) {
        const double d = duration(m.file);
        m.from = std::clamp(m.from, 0.0, d);
        m.to = std::clamp(m.to, 0.0, d);
        if (m.to <= m.from) continue;
        if (m.to - m.from < o.minMomentS) widen(m, std::min(o.minMomentS, d) - (m.to - m.from), d);
        QVector<Moment> trial = joined(picked + QVector<Moment>{m}, o.joinGapS);
        if (total(trial) <= maxS + 1e-6) {
            picked = trial;
            continue;
        }
        if (picked.isEmpty()) { // one long moment alone: its start
            m.to = m.from + maxS;
            picked << m;
        }
        // Too long with the others: a shorter one further down may still fit
    }

    if (picked.isEmpty()) {
        // Nothing found: the end of every recording, together fallbackS (within minS..maxS)
        const double want = std::clamp(o.fallbackS, minS, maxS);
        int count = 0;
        for (double d : durations) count += d > 0;
        for (int i = 0; i < durations.size(); ++i)
            if (durations[i] > 0) picked << Moment{i, std::max(0.0, durations[i] - want / count), durations[i], 0};
    }

    // Too short: every moment grows evenly until minS (moments that reach each other join)
    for (int round = 0; round < 30 && total(picked) < minS - 0.01; ++round) {
        const double before = total(picked);
        const double each = (minS - before) / picked.size();
        for (Moment& m : picked) widen(m, each, duration(m.file));
        picked = joined(picked, o.joinGapS);
        if (total(picked) <= before + 0.001) break; // the recordings are used up
    }
    // Joining can add the gaps: trim the end of the last moment back to maxS
    if (const double over = total(picked) - maxS; over > 0.001 && !picked.isEmpty())
        picked.last().to = std::max(picked.last().from + 0.5, picked.last().to - over);
    return picked;
}

QVector<QVector<Moment>> variants(const QVector<Moment>& candidates, const QVector<double>& durations, int count,
                                  const Options& o)
{
    QVector<QVector<Moment>> out;
    QVector<Moment> used; // moments of the variants so far
    const auto overlaps = [&used](const Moment& c) {
        return std::any_of(used.begin(), used.end(),
                           [&c](const Moment& u) { return u.file == c.file && c.from < u.to && u.from < c.to; });
    };
    for (int v = 0; v < std::max(1, count); ++v) {
        QVector<Moment> left;
        for (const Moment& c : candidates)
            if (!overlaps(c)) left << c;
        if (v > 0 && left.isEmpty()) break;
        QVector<Moment> one = pick(v == 0 ? candidates : left, durations, o);
        if (v > 0) {
            // Only the fallback (no candidate fit): no variant of its own
            if (std::all_of(one.begin(), one.end(), [](const Moment& m) { return m.score <= 0; })) break;
            // Grown up to the minimum length into an earlier variant's moment: back to its edge
            for (Moment& m : one)
                for (const Moment& u : used) {
                    if (u.file != m.file || !(m.from < u.to && u.from < m.to)) continue;
                    if ((m.from + m.to) / 2 < (u.from + u.to) / 2) m.to = std::min(m.to, u.from);
                    else m.from = std::max(m.from, u.to);
                }
            one.erase(std::remove_if(one.begin(), one.end(), [](const Moment& m) { return m.to - m.from < 0.5; }), one.end());
        }
        if (one.isEmpty()) break;
        out << one;
        used += one;
    }
    return out;
}

} // namespace ShortPick
