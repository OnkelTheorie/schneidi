// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SfxPlace.h"

#include <algorithm>
#include <cmath>

namespace SfxPlace {

Result place(QVector<Moment> moments, double anchor, double end, const QVector<Existing>& existing, Rules rules)
{
    Result r;
    rules.minGap = std::max(rules.minGap, rules.minGapLimit);
    std::sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.at < b.at; });
    // Higher weight first; equal weight: earlier first
    std::stable_sort(moments.begin(), moments.end(), [](const Moment& a, const Moment& b) { return a.weight > b.weight; });
    QVector<double> taken;
    bool impactUsed = false;
    for (const Existing& e : existing) {
        taken << e.at;
        impactUsed = impactUsed || e.impact;
    }
    for (const Moment& m : moments) {
        const double start = m.at - anchor;
        QString why;
        if (start < -1e-9) why = "before_start";
        else if (m.at >= end || start >= end) why = "after_end";
        else if (rules.impact && impactUsed) why = "impact_once";
        else if (std::any_of(taken.begin(), taken.end(), [&](double t) { return std::abs(t - m.at) < rules.minGap - 1e-9; }))
            why = "too_close";
        else if (rules.maxPerMinute > 0) {
            // Every 60 s window holding this moment keeps at most maxPerMinute sounds
            QVector<double> near;
            for (double t : taken)
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
        impactUsed = impactUsed || rules.impact;
        r.placed << Placed{m.at, start};
    }
    std::sort(r.placed.begin(), r.placed.end(), [](const Placed& a, const Placed& b) { return a.moment < b.moment; });
    std::sort(r.skipped.begin(), r.skipped.end(), [](const Skipped& a, const Skipped& b) { return a.moment < b.moment; });
    return r;
}

double levelDb(const QString& level) { return level == "normal" ? -4.0 : -10.0; }
int perMinute(const QString& level) { return level == "normal" ? 6 : 3; }

} // namespace SfxPlace
