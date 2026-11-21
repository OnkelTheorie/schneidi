// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ReelPick.h"

#include <algorithm>
#include <cmath>

namespace ReelPick {

namespace {
constexpr double kEqualShare = 0.8; // a moment this strong next to a better one counts as a scene of its own
}

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
    QVector<Moment> kept, close; // close: left out for a better one near it (its score in `nearScore`)
    QVector<double> nearScore;
    for (const Moment& m : list) {
        double best = -1;
        for (const Moment& k : kept)
            if (k.file == m.file && std::abs(k.from - m.from) < o.spacingS) best = std::max(best, k.score);
        if (best < 0) {
            kept << m;
        } else {
            close << m;
            nearScore << best;
        }
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
        // Still short of the target: a moment as strong as the one near it is a scene of its own (a second multi
        // kill a few seconds later), not the same one again - it comes in where it overlaps nothing picked. A weaker
        // one near a better one (an echo of the same scene) stays out (videotest 21.11.: a 2-kill moment was left out
        // while 1-kill moments filled the video)
        for (int i = 0; i < close.size() && total < o.targetS - 1e-6; ++i) {
            const Moment& m = close[i];
            const double len = m.to - m.from;
            if (m.score < kEqualShare * nearScore[i] || total + len > o.targetS + 1e-6) continue;
            const bool overlaps = std::any_of(picked.begin(), picked.end(), [&](const Moment& p) {
                return p.file == m.file && m.from < p.to && p.from < m.to;
            });
            if (overlaps) continue;
            picked << m;
            total += len;
        }
        std::stable_sort(picked.begin(), picked.end(), [](const Moment& a, const Moment& b) { return a.score > b.score; });
    }
    // Short of the target: every moment grows into its room (up to growBeforeS before, growAfterS after, within its
    // recording, half the gap to a neighbour of the same recording), all by the same share of that room, just enough
    if (o.targetS > 0 && !picked.isEmpty()) {
        double total = 0;
        for (const Moment& m : picked) total += m.to - m.from;
        QVector<double> before(picked.size()), after(picked.size());
        double room = 0;
        for (int i = 0; i < picked.size(); ++i) {
            const Moment& m = picked[i];
            double lo = 0, hi = duration(m.file);
            for (int j = 0; j < picked.size(); ++j) {
                const Moment& n = picked[j];
                if (j == i || n.file != m.file) continue;
                if (n.to <= m.from) lo = std::max(lo, (n.to + m.from) / 2);
                if (n.from >= m.to) hi = std::min(hi, (m.to + n.from) / 2);
            }
            before[i] = std::clamp(m.from - lo, 0.0, std::max(0.0, o.growBeforeS));
            after[i] = std::clamp(hi - m.to, 0.0, std::max(0.0, o.growAfterS));
            room += before[i] + after[i];
        }
        const double missing = o.targetS - total;
        if (missing > 1e-6 && room > 1e-6) {
            const double share = std::min(1.0, missing / room);
            for (int i = 0; i < picked.size(); ++i) {
                picked[i].from -= before[i] * share;
                picked[i].to += after[i] * share;
            }
        }
    }
    if (o.order == Order::Time)
        std::stable_sort(picked.begin(), picked.end(), [](const Moment& a, const Moment& b) {
            return a.file != b.file ? a.file < b.file : a.from < b.from;
        });
    // Cuts on the beat: each moment's end moves so the cut after it lands on the nearest beat (within snapS), never
    // past the recording's end nor into a later moment of the same recording (grown moments meet: the footage would
    // be in the video twice)
    if (o.beats && o.beats->found && o.beats->period > 0) {
        double at = 0;
        for (int i = 0; i < picked.size(); ++i) {
            Moment& m = picked[i];
            double hi = duration(m.file);
            for (int j = 0; j < picked.size(); ++j)
                if (j != i && picked[j].file == m.file && picked[j].from >= m.from) hi = std::min(hi, picked[j].from);
            const double end = at + (m.to - m.from);
            const double snapped = Beats::snapToGrid({end}, *o.beats, o.snapS).front();
            const double to = std::clamp(m.to + snapped - end, std::min(m.from + 0.5, hi), hi);
            m.to = to; // no room in the recording: as close as it gets
            at += m.to - m.from;
        }
    }
    return picked;
}

} // namespace ReelPick
