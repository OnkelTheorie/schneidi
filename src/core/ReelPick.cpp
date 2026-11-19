// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ReelPick.h"

#include <algorithm>
#include <cmath>

namespace ReelPick {

QVector<Moment> pick(const QVector<Moment>& candidates, const QVector<double>& durations, const Options& o)
{
    const auto duration = [&](int file) { return file >= 0 && file < durations.size() ? durations[file] : 0.0; };
    // Clamped, grown to the minimum, close ones of a recording joined
    QVector<Moment> list;
    for (Moment m : candidates) {
        const double d = duration(m.file);
        m.from = std::clamp(m.from, 0.0, d);
        m.to = std::clamp(m.to, 0.0, d);
        if (m.to <= m.from) continue;
        if (m.to - m.from < o.minMomentS) ShortPick::widen(m, std::min(o.minMomentS, d) - (m.to - m.from), d);
        list << m;
    }
    list = ShortPick::joined(list, o.joinGapS);
    // Best first; a moment starting within spacingS of a better one of the same recording is left out
    std::stable_sort(list.begin(), list.end(), [](const Moment& a, const Moment& b) { return a.score > b.score; });
    QVector<Moment> kept;
    for (const Moment& m : list) {
        const bool near = std::any_of(kept.begin(), kept.end(), [&](const Moment& k) {
            return k.file == m.file && std::abs(k.from - m.from) < o.spacingS;
        });
        if (!near) kept << m;
    }
    // Up to the target length: the best ones that still fit (a shorter one further down may)
    QVector<Moment> picked;
    if (o.targetS <= 0) {
        picked = kept;
    } else {
        double total = 0;
        for (const Moment& m : kept) {
            const double len = m.to - m.from;
            if (total + len > o.targetS + 1e-6) {
                if (picked.isEmpty()) { // one long moment alone: its start
                    Moment start = m;
                    start.to = start.from + o.targetS;
                    picked << start;
                    total = o.targetS;
                }
                continue;
            }
            picked << m;
            total += len;
        }
    }
    if (o.order == Order::Time)
        std::stable_sort(picked.begin(), picked.end(), [](const Moment& a, const Moment& b) {
            return a.file != b.file ? a.file < b.file : a.from < b.from;
        });
    // Cuts on the beat: each moment's end moves so the cut after it lands on the nearest beat (within snapS)
    if (o.beats && o.beats->found && o.beats->period > 0) {
        double at = 0;
        for (Moment& m : picked) {
            const double end = at + (m.to - m.from);
            const double snapped = Beats::snapToGrid({end}, *o.beats, o.snapS).front();
            const double to = std::clamp(m.to + snapped - end, m.from + 0.5, duration(m.file));
            m.to = to; // no room in the recording: as close as it gets
            at += m.to - m.from;
        }
    }
    return picked;
}

} // namespace ReelPick
