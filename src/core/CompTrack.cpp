// SPDX-License-Identifier: AGPL-3.0-only
#include "core/CompTrack.h"

#include "core/CompGraph.h"
#include "core/Keyframes.h"
#include "core/Types.h"

#include <algorithm>

namespace Comp {

bool lostModeFromId(const QString& id, LostMode* mode)
{
    LostMode m;
    if (id == QLatin1String("bridge")) m = LostMode::Bridge;
    else if (id == QLatin1String("hold")) m = LostMode::Hold;
    else if (id == QLatin1String("fade")) m = LostMode::Fade;
    else return false;
    if (mode) *mode = m;
    return true;
}

const char* lostModeId(LostMode mode)
{
    switch (mode) {
    case LostMode::Bridge: return "bridge";
    case LostMode::Hold: return "hold";
    case LostMode::Fade: break;
    }
    return "fade";
}

QVector<TrackGap> trackGaps(const QVector<std::pair<int, double>>& seen, int last)
{
    QVector<TrackGap> out;
    bool in = false;
    TrackGap g;
    for (int i = 0; i < seen.size(); ++i) {
        const bool lost = !(seen[i].second >= 0.5); // NaN counts as lost
        if (lost && !in) {
            in = true;
            g = TrackGap{};
            g.openStart = i == 0;
            g.from = g.openStart ? 0 : seen[i].first;
        } else if (!lost && in) {
            in = false;
            g.to = seen[i].first - 1;
            if (g.to >= g.from) out << g;
        }
    }
    if (in) {
        g.openEnd = true;
        g.to = std::max(g.from, last);
        out << g;
    }
    return out;
}

QVector<std::pair<int, double>> seenKeys(const Clip& c, int node)
{
    QVector<std::pair<int, double>> keys;
    const AnimParam p = Keys::effectParam(QString::fromLatin1(kEffectId), CompGraph::paramKey(node, QString::fromLatin1(kSeenKey)));
    if (!Keys::animated(c, p)) return keys;
    for (int t : Keys::keyTimes(c, {p}))
        if (const Keyframe* k = Keys::keyAt(c, p, t)) keys.append({t, k->value});
    return keys;
}

QVector<TrackGap> trackGaps(const Clip& c, int node)
{
    return trackGaps(seenKeys(c, node), std::max(0, c.length() - 1));
}

const TrackGap* gapAt(const QVector<TrackGap>& gaps, double t)
{
    for (const TrackGap& g : gaps)
        if (!g.bridged() && (g.openStart || t >= g.from) && (g.openEnd || t < g.to + 1)) return &g;
    return nullptr;
}

double followVisibility(const FollowTime& time)
{
    if (time.lost != LostMode::Fade || time.gaps.isEmpty()) return 1.0;
    // Box filter: the share of the last d frames [t - d + 1, t + 1) that lie in a gap not bridged
    const double d = std::max(1.0, std::round(kFadeSeconds * (time.fps > 0 ? time.fps : 25)));
    const double w0 = time.t - d + 1, w1 = time.t + 1;
    double hidden = 0;
    for (const TrackGap& g : time.gaps) {
        if (g.bridged()) continue;
        const double a = g.openStart ? -1e18 : g.from, b = g.openEnd ? 1e18 : g.to + 1.0;
        hidden += std::max(0.0, std::min(b, w1) - std::max(a, w0));
    }
    return std::clamp(1.0 - hidden / d, 0.0, 1.0);
}

} // namespace Comp
