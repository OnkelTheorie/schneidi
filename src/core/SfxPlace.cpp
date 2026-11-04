// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SfxPlace.h"

#include <algorithm>
#include <cmath>

namespace SfxPlace {

namespace {

// Equal weights: the earliest first, then always the one farthest from those already in the order (a cap keeps a
// spread instead of the first moments in a row)
QVector<Moment> spread(QVector<Moment> group)
{
    QVector<Moment> out;
    if (group.isEmpty()) return out;
    std::sort(group.begin(), group.end(), [](const Moment& a, const Moment& b) { return a.at < b.at; });
    out << group.takeFirst();
    while (!group.isEmpty()) {
        qsizetype best = 0;
        double far = -1;
        for (qsizetype i = 0; i < group.size(); ++i) {
            double d = 1e300;
            for (const Moment& m : out) d = std::min(d, std::abs(m.at - group[i].at));
            if (d > far + 1e-9) far = d, best = i;
        }
        out << group.takeAt(best);
    }
    return out;
}

} // namespace

Result place(QVector<Moment> moments, double anchor, double length, double end, const QVector<Existing>& existing,
             Rules rules)
{
    Result r;
    rules.minGap = std::max(rules.minGap, rules.minGapLimit);
    // Higher weight first; within one weight spread over the time
    std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.weight > b.weight; });
    QVector<Moment> order;
    for (qsizetype i = 0; i < moments.size();) {
        qsizetype j = i;
        while (j < moments.size() && moments[j].weight == moments[i].weight) ++j;
        order += spread(moments.mid(i, j - i));
        i = j;
    }
    QVector<double> taken, sameKind; // every sound (spacing), the ones of this kind (cap)
    QVector<std::pair<double, double>> spans; // the time the sounds take on the target track
    bool impactUsed = false;
    for (const Existing& e : existing) {
        taken << e.at;
        if (e.sameKind) sameKind << e.at;
        if (e.sameTrack) spans << std::pair{e.start, e.end};
        impactUsed = impactUsed || e.impact;
    }
    for (const Moment& m : order) {
        const double start = m.at - anchor;
        QString why;
        if (start < -1e-9) why = "before_start";
        else if (m.at >= end || start + length > end + 1e-9) why = "after_end";
        else if (rules.impact && impactUsed) why = "impact_once";
        else if (std::any_of(spans.begin(), spans.end(), [&](const std::pair<double, double>& sp) {
                     return sp.first < start + length - 1e-9 && start < sp.second - 1e-9;
                 }))
            why = "overlaps";
        else if (std::any_of(taken.begin(), taken.end(), [&](double t) { return std::abs(t - m.at) < rules.minGap - 1e-9; }))
            why = "too_close";
        else if (rules.maxPerMinute > 0) {
            // Every 60 s window holding this moment keeps at most maxPerMinute sounds of this kind
            QVector<double> near;
            for (double t : sameKind)
                if (std::abs(t - m.at) < 60.0) near << t;
            near << m.at;
            std::sort(near.begin(), near.end());
            for (qsizetype i = 0, j = 0; i < near.size() && why.isEmpty(); ++i) {
                while (near[i] - near[j] >= 60.0) ++j;
                if (i - j + 1 > rules.maxPerMinute) why = "per_minute";
            }
        }
        if (!why.isEmpty()) {
            r.skipped << Skipped{m.at, why};
            continue;
        }
        taken << m.at;
        sameKind << m.at;
        spans << std::pair{start, start + length};
        impactUsed = impactUsed || rules.impact;
        r.placed << Placed{m.at, start};
    }
    std::sort(r.placed.begin(), r.placed.end(), [](const Placed& a, const Placed& b) { return a.moment < b.moment; });
    std::sort(r.skipped.begin(), r.skipped.end(), [](const Skipped& a, const Skipped& b) { return a.moment < b.moment; });
    return r;
}

double levelDb(const QString& level) { return level == "normal" ? -4.0 : -10.0; }
int perMinute(const QString& level) { return level == "normal" ? 6 : 3; }

double underPeakDb(std::optional<double> gamePeakDb, double soundPeakDb, double under)
{
    if (!gamePeakDb) return levelDb("mild");
    return std::clamp(*gamePeakDb - under - soundPeakDb, -40.0, 0.0);
}

} // namespace SfxPlace
