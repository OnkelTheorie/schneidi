// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ShortPick.h"

#include <algorithm>
#include <cmath>

namespace ShortPick {

namespace {

// Sorted by file and time, overlapping or close ones (gap below `gap`) of the same file joined
QVector<Moment> joined(QVector<Moment> list, double gap)
{
    std::sort(list.begin(), list.end(),
              [](const Moment& a, const Moment& b) { return a.file != b.file ? a.file < b.file : a.from < b.from; });
    QVector<Moment> out;
    for (const Moment& m : list) {
        if (!out.isEmpty() && out.last().file == m.file && m.from - out.last().to < gap) {
            out.last().to = std::max(out.last().to, m.to);
            out.last().score = std::max(out.last().score, m.score);
        } else {
            out << m;
        }
    }
    return out;
}

// Grows [from, to) by `by` seconds, a bit more after than before (the reaction), within [0, duration); what one
// side cannot take goes to the other
void widen(Moment& m, double by, double duration)
{
    double before = by * 0.45, after = by - before;
    const double roomBefore = m.from, roomAfter = duration - m.to;
    if (before > roomBefore) after += before - roomBefore, before = roomBefore;
    if (after > roomAfter) before = std::min(roomBefore, before + after - roomAfter), after = roomAfter;
    m.from -= before;
    m.to += after;
}

} // namespace

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

} // namespace ShortPick
