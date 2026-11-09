// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: highlights – candidates for a highlight video from the kill feed (one colour in a region) and loud
// moments, ready for the 'keep' edit operation. Results are cached per file and settings (hit_s for the review
// render, snap to beat, sound effects and the material index read the same cache)
#include "cli/AnalysisDetail.h"

#include "core/GameHud.h"
#include "engine/FrameGaps.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

// A kill feed line's mark covers well under 0.3 % of the picture (Valorant at 1080p: 0.03-0.08 %); a patch of the
// colour bigger than 1 % that appears at once is a banner or menu in that colour (buy phase, round won), not a kill
constexpr double kBannerArea = 0.01;

// Picture height from which framed kill feed lines are found by their shape (1080p, a little less for odd sizes)
constexpr int kFramedFrom = 1000;

// The hit behind a kill feed line: the line shows up a few frames after the shot (the server confirms the kill, the
// line slides in and is only counted once half of it is there). The last sharp rise of the sound (a shot) in the
// 0.6 s before the line began; without one the frame where the line began.
struct Hit {
    int frame;
    bool bySound;
};
Hit hitBefore(const ColorEvent& e, const LoudPeaks& sound, double fps)
{
    const std::vector<double>& db = sound.db;
    const int end = std::min(e.onset, int(db.size()) - 1), from = std::max(3, e.onset - int(std::lround(0.6 * fps)));
    // A shot: well above the file's usual level and at least 6 dB above the frames just before it
    const double loud = std::max(sound.medianDb + 10, sound.thresholdDb - 6);
    for (int i = end; i >= from; --i) {
        const double before = std::min({db[size_t(i - 1)], db[size_t(i - 2)], db[size_t(i - 3)]});
        if (db[size_t(i)] >= loud && db[size_t(i)] - before >= 6) return {i, true};
    }
    return {e.onset, false};
}

struct Candidate {
    int from, to;          // source frames, with lead-in and lead-out
    QVector<int> kills;    // frames of the kill feed events inside
    QVector<int> teamkills; // frames of teamkill lines inside (not kills: they lower the score)
    QVector<Hit> hits;     // the hit behind each of them
    int peaks = 0;         // loud moments inside
    double loudest = -200; // dBFS of the loudest of them
    double score = 0;
    QVector<Stutter::Stretch> stutter; // jumps in the recording's time stamps inside (the game lagged)
    QVector<QPair<int, int>> clean;    // with stutter: the parts without it, source frames
};

// The game whose kill feed to read (core/GameHud.h): `game`, else from the folder or file name; none when a region or
// colour is given or game is "none"
const GameHud::Game* gameOf(const QJsonObject& a, const QString& file)
{
    if (a.contains("color") || a.contains("region")) return nullptr;
    const QString g = a["game"].toString().trimmed();
    if (g.compare("none", Qt::CaseInsensitive) == 0) return nullptr;
    if (!g.isEmpty()) {
        if (const GameHud::Game* game = GameHud::find(g)) return game;
        QStringList ids{"none"};
        for (const GameHud::Game& x : GameHud::games()) ids << x.id;
        fail("BAD_ARGUMENT", "game must be " + ids.join(", ") + " (or leave it out: from the folder or file name)");
    }
    return GameHud::guess(file);
}

QJsonObject highlightsOne(const QJsonObject& given, Context& ctx)
{
    const MediaSource m = openMedia(given);
    const ProjectFormat& f = m.format;
    // A known game: its kill feed, as if region and colour were given
    QJsonObject a = given;
    const GameHud::Game* game = m.info.hasVideo && !m.info.isImage ? gameOf(given, m.info.path) : nullptr;
    // Framed lines (CS2) by their shape from 1080p on. In a smaller picture (mostly a 1080p game scaled down) the
    // 1 px outline smears over several rows and the edges of lines under each other run together: the colour is
    // counted there
    const bool framed = game && game->frameHeight > 0 && pictureFormat(m).height >= kFramedFrom;
    if (game) {
        a["region"] = GameHud::regionText(framed ? game->frameArea : game->mark);
        a["color"] = game->color;
        if (game->tolerance > 0 && !a.contains("tolerance")) a["tolerance"] = game->tolerance;
    }
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
    QVector<int> kills, banners, teamkills;
    QVector<ColorEvent> killEvents;
    double most = 0;
    if (feed) {
        if (!m.info.hasVideo || m.info.isImage) fail("NO_VIDEO", "file has no moving picture: " + m.info.path);
        QJsonObject ca = a;
        ca["only"] = "appear";
        // A game whose lines are framed (CS2): by that shape, the game picture behind does not count
        const auto pictureProgress = progress(0.0, m.info.hasAudio ? 0.85 : 1.0);
        const ColorEvents ce = framed ? framedLineEvents(m, game->frameArea, QColor(game->color), game->frameHeight,
                                                         game->frameRight, game->frameLeft, pictureProgress)
                                      : colorEvents(m, ca, pictureProgress);
        // A teamkill is not a highlight: listed, not counted as a kill
        for (const ColorEvent& e : ce.events) {
            if (e.area > kBannerArea) {
                banners << e.frame;
            } else if (e.teamkill) {
                teamkills << e.frame;
            } else {
                kills << e.frame;
                killEvents << e;
            }
        }
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
        for (const ColorEvent& e : killEvents)
            if (e.frame >= c.from && e.frame < c.to) {
                c.kills << e.frame;
                c.hits << (sound.db.empty() ? Hit{e.onset, false} : hitBefore(e, sound, f.rate.fps()));
            }
        for (const int t : teamkills)
            if (t >= c.from && t < c.to) c.teamkills << t;
        for (const Peak& p : sound.peaks)
            if (p.frame >= c.from && p.frame < c.to) {
                ++c.peaks;
                c.loudest = std::max(c.loudest, p.db);
            }
        // A kill counts most, a multi kill more than its kills; sound breaks ties (a fight, a loud reaction)
        const int n = int(c.kills.size());
        // A teamkill in the moment takes a little away (the kill it was not)
        c.score = feed ? n * 10.0 + std::max(0, n - 1) * 5.0 + std::min(c.peaks, 10) * 0.5 - c.teamkills.size() * 5.0
                       : c.peaks + std::max(0.0, c.loudest - sound.thresholdDb) / 3;
    }

    // Stutter (variable frame rate capture that dropped frames while the game lagged): the stretches inside each
    // candidate and its parts without them, so only the lagging part is cut instead of the whole moment
    const std::optional<Stutter::Scan> gaps =
        m.info.hasVideo && !m.info.isImage ? FrameGaps::scan(m.info.path) : std::nullopt;
    const double fps = f.rate.fps();
    bool anyStutter = false;
    if (gaps && !gaps->jumps.isEmpty())
        for (Candidate& c : list) {
            c.stutter = Stutter::stretches(Stutter::inRange(*gaps, c.from / fps, c.to / fps));
            if (c.stutter.isEmpty()) continue;
            anyStutter = true;
            for (const auto& [s0, s1] : Stutter::cleanPieces(*gaps, c.from / fps, c.to / fps))
                if (const int x = int(std::ceil(s0 * fps - 1e-6)), y = int(std::ceil(s1 * fps - 1e-6)); y > x)
                    c.clean.append({x, y});
        }

    QJsonArray keep, keepClean;
    for (const Candidate& c : list) {
        keep << QJsonArray{c.from, c.to};
        if (c.stutter.isEmpty()) keepClean << QJsonArray{c.from, c.to};
        for (const auto& [x, y] : c.clean) keepClean << QJsonArray{x, y};
    }
    std::stable_sort(list.begin(), list.end(), [](const Candidate& x, const Candidate& y) { return x.score > y.score; });
    QJsonArray candidates;
    for (const Candidate& c : list) {
        QJsonObject o = range(c.from, c.to, f);
        o["score"] = std::round(c.score * 10) / 10;
        QStringList reasons;
        if (feed) {
            QJsonArray at, s, hitAt, hitS, hitBy;
            for (int k : c.kills) {
                at << k;
                s << seconds(k, f);
            }
            for (const Hit& h : c.hits) {
                hitAt << h.frame;
                hitS << seconds(h.frame, f);
                hitBy << (h.bySound ? "sound" : "feed");
            }
            o["kills"] = int(c.kills.size());
            o["kill_frames"] = at;
            o["kill_s"] = s;
            o["hit_frames"] = hitAt;
            o["hit_s"] = hitS;
            o["hit_by"] = hitBy;
            reasons << (c.kills.size() == 1 ? QString("1 kill") : QString("%1 kills (multi kill)").arg(c.kills.size()));
            if (!c.teamkills.isEmpty()) {
                QJsonArray ts;
                for (int k : c.teamkills) ts << seconds(k, f);
                o["teamkills"] = int(c.teamkills.size());
                o["teamkill_s"] = ts;
                reasons << (c.teamkills.size() == 1 ? QString("Teamkill (not counted)")
                                                    : QString("%1 teamkills (not counted)").arg(c.teamkills.size()));
            }
        }
        o["loud_moments"] = c.peaks;
        if (c.peaks > 0) {
            o["loudest_db"] = std::round(c.loudest * 10) / 10;
            reasons << QString("%1 loud moment%2").arg(c.peaks).arg(c.peaks == 1 ? "" : "s");
        }
        if (!c.stutter.isEmpty()) {
            QJsonArray st, clean;
            int missing = 0;
            for (const Stutter::Stretch& x : c.stutter) {
                st << stretchJson(x, f);
                missing += x.missing;
            }
            for (const auto& [x, y] : c.clean) {
                QJsonObject piece = range(x, y, f);
                piece["kills"] = int(std::count_if(c.kills.begin(), c.kills.end(), [&](int k) { return k >= x && k < y; }));
                clean << piece;
            }
            o["stutter"] = st;
            o["clean"] = clean;
            reasons << QString("stutter: %1 frame%2 missing").arg(missing).arg(missing == 1 ? "" : "s");
        }
        o["reasons"] = QJsonArray::fromStringList(reasons);
        candidates << o;
    }
    QJsonObject out{{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]},
                    {"candidates", candidates}, {"keep", keep}};
    if (anyStutter) out["keep_clean"] = keepClean;
    if (game) {
        out["game"] = game->id;
        out["region"] = a["region"];
    }
    if (feed) {
        out["kill_feed_most_pct"] = std::round(most * 1e5) / 1e3;
        out["kills_note"] = "kill_s = when the kill feed line is there (a few frames after the hit); hit_s = the hit "
                            "itself: the last shot in the sound before the line began (hit_by 'sound'), or the frame "
                            "the line began to show (hit_by 'feed', no shot heard). Put accents and beats on hit_s";
    }
    if (!teamkills.isEmpty()) {
        QJsonArray times;
        for (int t : teamkills) times << seconds(t, f);
        out["teamkills"] = times;
        out["teamkills_note"] = "the player's own kill feed lines whose victim has the player's team colour (a "
                                "teamkill): not counted in 'kills', not a moment of their own; inside a moment they "
                                "lower its score";
    }
    if (!banners.isEmpty()) {
        QJsonArray times;
        for (int b : banners) times << seconds(b, f);
        out["banners"] = times;
        out["banners_note"] = "patches of the colour bigger than 1 % of the picture at once (a banner or menu in that "
                              "colour, e.g. the buy phase), not counted as kills";
    }
    return out;
}

// Raise when the analysis gives other results for the same file and settings (old cache entries are then ignored)
constexpr int kCacheVersion = 6; // 4, 5: CS2 kill feed lines by their frame (2 and 3 were experiments); 6: teamkills

QString cacheFile(const QString& key)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/highlights/" + key + ".json";
}

} // namespace

QJsonObject highlightsFile(const QJsonObject& a, Context& ctx)
{
    // Key: the file (path, size, modification time), the frame rate frames count in and every setting that changes
    // the result
    const MediaSource m = openMedia(a);
    const QFileInfo fi(m.info.path);
    QJsonObject settings;
    for (const char* k : {"game", "region", "color", "tolerance", "min_area", "pre", "post", "join", "threshold_db",
                          "min", "audio_stream"})
        if (a.contains(k)) settings[k] = a[k];
    const QString id = QString("%1|%2|%3|%4/%5|%6|%7")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(m.format.rate.num)
                           .arg(m.format.rate.den)
                           .arg(QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact)))
                           .arg(kCacheVersion);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
    {
        QFile f(cacheFile(key));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            if (o["version"].toInt() == kCacheVersion && o["result"].isObject()) {
                QJsonObject r = o["result"].toObject();
                r["cached"] = true;
                return r;
            }
        }
    }
    QJsonObject r = highlightsOne(a, ctx);
    const QString file = cacheFile(key);
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly)
        && out.write(QJsonDocument(QJsonObject{{"version", kCacheVersion}, {"result", r}}).toJson(QJsonDocument::Compact)) >= 0)
        out.commit();
    return r;
}

QJsonObject cmdHighlights(const QJsonObject& a, Context& ctx)
{
    QJsonObject out = perFile(a, ctx, highlightsFile, 2);
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
