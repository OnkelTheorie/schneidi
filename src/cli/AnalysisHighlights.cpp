// schneidi-cli: highlights – candidates for a highlight video from the kill feed (one colour in a region) and loud
// moments, ready for the 'keep' edit operation
#include "cli/AnalysisDetail.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

struct Candidate {
    int from, to;          // source frames, with lead-in and lead-out
    QVector<int> kills;    // frames of the kill feed events inside
    int peaks = 0;         // loud moments inside
    double loudest = -200; // dBFS of the loudest of them
    double score = 0;
};

QJsonObject highlightsOne(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    const ProjectFormat& f = m.format;
    const auto timeArg = [&](const char* key, double defaultSeconds) {
        const int v = a.contains(key) ? parseTime(a[key], f, key) : int(std::lround(defaultSeconds * f.rate.fps()));
        if (v < 0) fail("BAD_ARGUMENT", QString("'%1' must not be negative").arg(key));
        return v;
    };
    const bool feed = a.contains("color");
    const int pre = timeArg("pre", 2.0), post = timeArg("post", 1.0), join = timeArg("join", feed ? 4.0 : 2.0);
    if (feed && !a.contains("region")) fail("BAD_ARGUMENT", "color needs a region (where the kill feed is)");
    if (!feed && !m.info.hasAudio) fail("NO_AUDIO", "no sound and no kill feed (region + color) to look at: " + m.info.path);
    const auto progress = [&ctx](double from, double to) {
        return [&ctx, from, to](double p) {
            if (ctx.progress) ctx.progress(from + p * (to - from));
        };
    };

    // Kill feed: a new line of the player's colour; sound: loud moments (shots, explosions, shouts)
    QVector<int> kills;
    double most = 0;
    if (feed) {
        if (!m.info.hasVideo || m.info.isImage) fail("NO_VIDEO", "file has no moving picture: " + m.info.path);
        QJsonObject ca = a;
        ca["only"] = "appear";
        const ColorEvents ce = colorEvents(m, ca, progress(0.0, m.info.hasAudio ? 0.85 : 1.0));
        for (const ColorEvent& e : ce.events) kills << e.frame;
        most = ce.most;
    }
    LoudPeaks sound;
    if (m.info.hasAudio) sound = loudPeaks(m, a, progress(feed ? 0.85 : 0.0, 1.0));

    // Candidates: kills (or with no kill feed bursts of sound) closer than `join` are one moment (a multi kill)
    QVector<Candidate> list;
    const auto add = [&](int first, int last) {
        Candidate c{std::max(0, first - pre), std::min(m.info.length, last + 1 + post), {}};
        if (!list.isEmpty() && c.from <= list.last().to) list.last().to = std::max(list.last().to, c.to);
        else list << c;
    };
    if (feed) {
        for (int i = 0; i < kills.size();) {
            int j = i;
            while (j + 1 < kills.size() && kills[j + 1] - kills[j] <= join) ++j;
            add(kills[i], kills[j]);
            i = j + 1;
        }
    } else {
        for (const PeakGroup& g : groupPeaks(sound.peaks, join)) add(g.from, g.to - 1);
    }
    for (Candidate& c : list) {
        for (int k : kills)
            if (k >= c.from && k < c.to) c.kills << k;
        for (const Peak& p : sound.peaks)
            if (p.frame >= c.from && p.frame < c.to) {
                ++c.peaks;
                c.loudest = std::max(c.loudest, p.db);
            }
        // A kill counts most, a multi kill more than its kills; sound breaks ties (a fight, a loud reaction)
        const int n = int(c.kills.size());
        c.score = feed ? n * 10.0 + std::max(0, n - 1) * 5.0 + std::min(c.peaks, 10) * 0.5
                       : c.peaks + std::max(0.0, c.loudest - sound.thresholdDb) / 3;
    }

    QJsonArray keep;
    for (const Candidate& c : list) keep << QJsonArray{c.from, c.to};
    std::stable_sort(list.begin(), list.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    QJsonArray candidates;
    for (const Candidate& c : list) {
        QJsonObject o = range(c.from, c.to, f);
        o["score"] = std::round(c.score * 10) / 10;
        QStringList reasons;
        if (feed) {
            QJsonArray at, s;
            for (int k : c.kills) {
                at << k;
                s << seconds(k, f);
            }
            o["kills"] = int(c.kills.size());
            o["kill_frames"] = at;
            o["kill_s"] = s;
            reasons << (c.kills.size() == 1 ? QString("1 kill") : QString("%1 kills (multi kill)").arg(c.kills.size()));
        }
        o["loud_moments"] = c.peaks;
        if (c.peaks > 0) {
            o["loudest_db"] = std::round(c.loudest * 10) / 10;
            reasons << QString("%1 loud moment%2").arg(c.peaks).arg(c.peaks == 1 ? "" : "s");
        }
        o["reasons"] = QJsonArray::fromStringList(reasons);
        candidates << o;
    }
    QJsonObject out{{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]},
                    {"candidates", candidates}, {"keep", keep}};
    if (feed) out["kill_feed_most_pct"] = std::round(most * 1e5) / 1e3;
    return out;
}

} // namespace

QJsonObject cmdHighlights(const QJsonObject& a, Context& ctx)
{
    QJsonObject out = perFile(a, ctx, highlightsOne, 2);
    if (!out.contains("files")) {
        if (a.contains("top")) {
            QJsonArray c = out["candidates"].toArray();
            while (c.size() > std::max(1, a["top"].toInt())) c.removeLast();
            out["candidates"] = c;
        }
        return out;
    }
    // Several files: the best moments of all of them
    QJsonArray best;
    for (const QJsonValue& r : out["files"].toArray())
        for (const QJsonValue& v : r["candidates"].toArray()) {
            QJsonObject c = v.toObject();
            c["file"] = r["file"];
            best << c;
        }
    QList<QJsonValue> sorted(best.begin(), best.end());
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const QJsonValue& x, const QJsonValue& y) { return x["score"].toDouble() > y["score"].toDouble(); });
    if (a.contains("top")) sorted = sorted.mid(0, std::max(1, a["top"].toInt()));
    QJsonArray list;
    for (const QJsonValue& v : sorted) list << v;
    out["best"] = list;
    return out;
}

} // namespace Cli::detail
