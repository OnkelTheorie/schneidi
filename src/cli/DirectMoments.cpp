// SPDX-License-Identifier: AGPL-3.0-only
// The moment collector of `direct` (cli/CommandsDirect.cpp): what stands out in one recording, from three sources
// that each have their cache - highlights (kill feed of a game, else loud moments; detail::highlightsFile), sound
// events with times (laughter, applause, cheering, screams; engine/AudioEvents) and picture changes (lots of movement,
// a change of scene; pictureScores as `scenes` reads them). The whole answer per file is kept in AnalysisCache too, so
// a second recipe on the same material reads one small file per recording. The material index only adds to the score
// when it is cached already (a face, sharp or blurry, dark); it is never built here (too slow cold).
#include "cli/AnalysisDetail.h"
#include "cli/DirectDetail.h"

#include "core/GameHud.h"
#include "core/StreamPresets.h"
#include "core/Transcript.h"
#include "engine/AnalysisCache.h"
#include "engine/AudioEvents.h"
#include "engine/ClipFormat.h"
#include "engine/KillFeedLines.h"
#include "engine/MediaIndex.h"
#include "engine/Snapshot.h"

#include <QFileInfo>
#include <QJsonArray>

#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

namespace Cli::detail {

namespace {

constexpr int kMomentsVersion = 12; // raise when the moments of a file change for the same sources

constexpr double kHighlightsPreS = 2.0;  // highlights' default lead-in before the first kill or loud moment
constexpr double kHighlightsPostS = 1.0; // ... and lead-out after the last one

// How far a short's moment may grow (lo_s/hi_s of a moment; core/ShortPick keeps inside them): from at most
// kLeadInS before its first hit, up to kAfterS after its last one - and never past the player's death (kAfterDeathS
// after it: the fall is seen, the death screen and the spectating after it are not)
constexpr double kLeadInS = 1.5;
constexpr double kAfterS = 6.0;
constexpr double kAfterDeathS = 0.7;
// A recording this short is a clip tool's replay buffer (saved right after something happened): without a moment of
// its own its end is one, looked for in its last kClipEndLookS
constexpr double kClipEndMaxS = 60;
constexpr double kClipEndLookS = 12;
constexpr double kClipEndScore = 0.5; // below every found moment: those win when the length is short
// The death screen is looked for in recordings up to this long (it reads the whole picture once more)
constexpr double kDeathScanMaxS = 20 * 60;

double round2(double v) { return std::round(v * 100) / 100; }

QJsonObject momentJson(double from, double to, double score, const QString& source, const Direction::Reason& r,
                       const QStringList& labels)
{
    return {{"from_s", std::round(from * 1000) / 1000},
            {"to_s", std::round(to * 1000) / 1000},
            {"score", std::round(score * 10) / 10},
            {"source", source},
            {"reason", QJsonObject{{"key", r.key}, {"values", r.values}}},
            {"labels", QJsonArray::fromStringList(labels)}};
}

// The highlights' candidates as moments: kills (with a kill feed) or loud moments
// *status: "done", "none" (nothing to look at: no sound and no kill feed) or "failed"; *feed: a game's kill feed read
QJsonArray highlightMoments(const QJsonObject& a, Context& ctx, QString* status, bool* feed)
{
    QJsonArray out;
    *status = "done";
    QJsonObject h{{"file", a["file"]}, {"game", a["game"].toString().isEmpty() ? QString("none") : a["game"].toString()}};
    if (a.contains("project")) h["project"] = a["project"];
    QJsonObject r;
    try {
        r = highlightsFile(h, ctx);
    } catch (const Error& e) {
        if (e.code == QLatin1String("CANCELED")) throw;
        *status = e.code == QLatin1String("NO_AUDIO") ? "none" : "failed"; // never the whole suggest
        return out;
    }
    *feed = r.contains("game");
    for (const QJsonValue& c : r["candidates"].toArray()) {
        const int kills = c["kills"].toInt(), loud = c["loud_moments"].toInt();
        Direction::Reason reason;
        QStringList labels;
        if (kills > 0) {
            reason = {"kills", {{"n", kills}}};
            if (kills > 1) reason.values["extra"] = "multi";
            labels << "kill";
        } else {
            reason = {"loud", {{"n", std::max(1, loud)}}};
            if (c["loudest_db"].isDouble()) reason.values["db"] = c["loudest_db"];
        }
        if (loud > 0) labels << "loud";
        QJsonObject moment = momentJson(c["from_s"].toDouble(), c["to_s"].toDouble(), c["score"].toDouble(), "highlights",
                                        reason, labels);
        // Where the action begins (the first hit; a loud moment's first peak: highlights' default lead-in of 2 s after
        // the start), before it only lead-in - a hook may start later (suggestShort). Unknown at the recording's start
        if (const QJsonArray hits = c["hit_s"].toArray(); !hits.isEmpty()) {
            moment["first_s"] = hits.first().toDouble();
            moment["last_s"] = hits.last().toDouble();
            moment["hit_s"] = hits;
        } else {
            if (c["from_s"].toDouble() > 0) moment["first_s"] = c["from_s"].toDouble() + kHighlightsPreS;
            // The last loud moment: highlights' lead-out before the end
            moment["last_s"] = std::max(c["from_s"].toDouble(), c["to_s"].toDouble() - kHighlightsPostS);
        }
        out << moment;
    }
    return out;
}

// Laughter, applause, cheering and screams as moments: a little before (what made people laugh) and after
QJsonArray eventMoments(const MediaSource& m, double length, QString* status)
{
    QJsonArray out;
    *status = "none";
    if (!m.info.hasAudio) return out;
    AudioEvents::Options o;
    o.only = {"laughter", "applause", "cheering", "scream"};
    o.minConf = 0.3;
    o.progress = goOn();
    const AudioEvents::Result r = AudioEvents::analyzeFile(m.info.path, 0, o);
    failIfCanceled();
    // No sound to read (or it broke off): what was read counts, the answer is not cached
    *status = r.incomplete || (!r.error.isEmpty() && r.events.isEmpty() && r.duration <= 0) ? "failed" : "done";
    // A score below a kill (10) and about a loud burst: the kind, how sure, how long
    static const QHash<QString, double> base{{"cheering", 6}, {"laughter", 5.5}, {"applause", 5}, {"scream", 4.5}};
    for (const AudioEvents::Event& e : r.events) {
        const double len = std::max(0.0, e.to - e.from);
        const double score = base.value(e.kind, 4) * std::clamp(e.conf, 0.3, 1.0) + std::min(len, 4.0) * 0.5;
        out << momentJson(std::max(0.0, e.from - 1.5), std::min(length, e.to + 1.5), score, "events",
                          {e.kind, {{"s", round2(len)}}}, {e.kind});
    }
    return out;
}

// Picture: the stretches with the most movement (mean scene score over a second well above the recording's usual)
// and clear changes of scene; a few per recording, apart from each other
QJsonArray pictureMoments(const MediaSource& m, double length, QString* status)
{
    QJsonArray out;
    *status = "none";
    if (!m.info.hasVideo || m.info.isImage) return out;
    std::vector<PictureScore> scores;
    try {
        scores = pictureScores(m, 320, [](double) {});
    } catch (const Error& e) {
        if (e.code == QLatin1String("CANCELED")) throw;
        *status = "failed"; // a broken file: the other sources still count
        return out;
    }
    failIfCanceled();
    *status = "done";
    if (scores.size() < 10) return out;
    // Mean score per second, as far as the decoded picture goes (a broken header's length of millions of seconds
    // sized these arrays at several GB)
    double lastSecond = 0;
    for (const PictureScore& p : scores) lastSecond = std::max(lastSecond, p.seconds);
    std::vector<double> sum(size_t(std::ceil(std::max(0.0, std::min(length, lastSecond)))) + 1, 0.0);
    std::vector<int> count(sum.size(), 0);
    for (const PictureScore& p : scores) {
        const size_t s = std::min(sum.size() - 1, size_t(std::max(0.0, p.seconds)));
        sum[s] += p.score;
        ++count[s];
    }
    std::vector<double> mean(sum.size(), 0.0);
    for (size_t i = 0; i < sum.size(); ++i) mean[i] = count[i] ? sum[i] / count[i] : 0;
    std::vector<double> sorted = mean;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    const double threshold = std::max(0.02, median * 2.0);
    struct Peak {
        double at, value;
    };
    std::vector<Peak> motion;
    for (size_t i = 0; i < mean.size(); ++i)
        if (mean[i] >= threshold) motion.push_back({double(i), mean[i]});
    std::sort(motion.begin(), motion.end(), [](const Peak& a, const Peak& b) { return a.value > b.value; });
    std::vector<double> taken;
    const auto free = [&taken](double at) {
        return std::none_of(taken.begin(), taken.end(), [at](double t) { return std::abs(t - at) < 6; });
    };
    for (const Peak& p : motion) {
        if (taken.size() >= 3) break;
        if (!free(p.at)) continue;
        taken.push_back(p.at);
        const double score = std::min(6.0, 2.5 + 20 * p.value);
        out << momentJson(std::max(0.0, p.at - 2), std::min(length, p.at + 3), score, "picture", {"motion", {}}, {"motion"});
    }
    // Clear scene changes (a cut in the recording, the camera turned away): the new picture
    std::vector<Peak> cuts;
    for (const PictureScore& p : scores)
        if (p.score >= 0.4 && p.seconds > 0.5) cuts.push_back({p.seconds, p.score});
    std::sort(cuts.begin(), cuts.end(), [](const Peak& a, const Peak& b) { return a.value > b.value; });
    int changes = 0;
    for (const Peak& c : cuts) {
        if (changes >= 2) break;
        if (!free(c.at)) continue;
        taken.push_back(c.at);
        ++changes;
        out << momentJson(std::max(0.0, c.at - 0.5), std::min(length, c.at + 3.5), 2 + 2 * c.value, "picture",
                          {"scene_change", {}}, {"scene_change"});
    }
    return out;
}

// ---------- The player's death (CS2) ----------

// CS2 shows the player's death as a red tint over the whole picture (about 1-2 s) that fades into a black screen with
// the killer's card (about 1-3 s); then the round goes on with the player spectating a team mate. The middle of the
// picture (no HUD) is read 10 times a second: red = red well above green and blue, black = nearly no light. A death
// is a black screen of at least 1 s with the red tint right before it (a loading screen or a fade has no tint), or a
// tint of at least 0.8 s up to the recording's end (the clip saved while it showed). Measured on CS2 replay clips:
// tint R ~120 G ~60 B ~60 (fading to R ~40 G ~15), black luma ~2, the game picture's red/green at most ~1.4.
bool showsDeath(const QString& game) { return game == QLatin1String("cs2"); }

}

QJsonArray deathScreensOf(const std::vector<std::array<double, 3>>& rgb, int rate)
{
    const int n = int(rgb.size());
    const auto luma = [&](int i) { return 0.299 * rgb[size_t(i)][0] + 0.587 * rgb[size_t(i)][1] + 0.114 * rgb[size_t(i)][2]; };
    const auto red = [&](int i) {
        const auto& c = rgb[size_t(i)];
        return c[0] >= 30 && c[0] >= 1.5 * c[1] && c[0] >= 1.5 * c[2];
    };
    const auto black = [&](int i) { return luma(i) < 12; };
    QJsonArray out;
    const auto add = [&](int tint, double end) {
        const double at = double(tint) / rate;
        out << QJsonObject{{"s", std::round(at * 100) / 100}, {"end_s", std::round(end * 100) / 100}};
    };
    for (int i = 0; i < n; ++i) {
        if (!black(i) || (i > 0 && black(i - 1))) continue;
        int j = i;
        while (j < n && black(j)) ++j;
        if (j - i < rate) continue; // under 1 s: a cut, a flicker
        // The tint right before: red samples up to 3 s back, at most 3 others between them (the fade into black)
        int first = -1;
        for (int k = i - 1, gap = 0; k >= 0 && i - k <= 3 * rate; --k) {
            if (red(k)) {
                first = k;
                gap = 0;
            } else if (++gap > 3) {
                break;
            }
        }
        if (first >= 0) add(first, std::min(double(first) / rate + kAfterDeathS, double(i) / rate + 0.2));
        i = j;
    }
    // A tint up to the end
    int k = n - 1;
    while (k >= 0 && !red(k)) --k;
    if (k >= 0 && n - 1 - k <= rate) {
        int first = k;
        while (first > 0 && red(first - 1)) --first;
        const bool counted = !out.isEmpty() && out.last()["s"].toDouble() >= double(first) / rate - 0.01;
        if (k - first + 1 >= rate * 8 / 10 && !counted) add(first, double(first) / rate + kAfterDeathS);
    }
    return out;
}

namespace {

QJsonArray deathScreens(const MediaSource& m, QString* status)
{
    *status = "done";
    constexpr int kRateDeath = 10, w = 48, h = 20;
    std::mutex mutex;
    std::vector<std::array<double, 3>> rgb;
    std::vector<char> have;
    try {
        pictureSamples(
            m, QRectF(0.2, 0.25, 0.6, 0.5), QSize(w, h), kRateDeath,
            QString("crop=w=iw*0.6:h=ih*0.5:x=iw*0.2:y=ih*0.25,scale=%1:%2").arg(w).arg(h),
            [&](int sample, const uchar* p) {
                std::array<double, 3> sum{0, 0, 0};
                for (int i = 0; i < w * h; ++i)
                    for (int c = 0; c < 3; ++c) sum[size_t(c)] += p[3 * i + c];
                for (double& v : sum) v /= w * h;
                std::lock_guard lock(mutex);
                if (size_t(sample) >= rgb.size()) {
                    rgb.resize(size_t(sample) + 1);
                    have.resize(size_t(sample) + 1, 0);
                }
                rgb[size_t(sample)] = sum;
                have[size_t(sample)] = 1;
            },
            {});
    } catch (const Error& e) {
        if (e.code == QLatin1String("CANCELED")) throw;
        *status = "failed";
        return {};
    }
    size_t n = 0;
    while (n < have.size() && have[n]) ++n;
    rgb.resize(n);
    return deathScreensOf(rgb, kRateDeath);
}

// ---------- Voices ----------

// The voice streams of a recording with several streams: where something is said or laughed (Material::voice) and
// their laughter and shouts as moments - from the sentence that leads into them (a joke, a call) to a little after
QJsonArray voiceMoments(const MediaSource& m, double length, QJsonArray* voice, QString* status)
{
    QJsonArray out;
    *status = "none";
    if (!m.info.hasAudio || m.info.audioStreamCount() < 2) return out;
    for (int k = 0; k < m.info.audioStreamCount(); ++k) {
        const QString name = m.info.audioStreamName(k);
        if (!voiceStream(name) || !m.info.audioStreamPlayable(k)) continue;
        AudioEvents::Options o;
        o.only = {"speech", "laughter", "scream", "cheering"};
        o.minConf = 0.3;
        o.progress = goOn();
        const AudioEvents::Result r = AudioEvents::analyzeFile(m.info.path, k, o);
        failIfCanceled();
        if (r.incomplete) *status = "failed";
        else if (*status != QLatin1String("failed")) *status = "done";
        // Where the stream is heard (close stretches joined)
        QVector<QPair<double, double>> heard;
        for (const AudioEvents::Event& e : r.events) {
            if (!heard.isEmpty() && e.from - heard.last().second < 0.5) heard.last().second = std::max(heard.last().second, e.to);
            else heard.append({e.from, e.to});
        }
        if (heard.isEmpty()) continue;
        QJsonArray spans;
        for (const auto& [a, b] : heard) spans << QJsonArray{round2(a), round2(b)};
        voice->append(QJsonObject{{"stream", k}, {"name", name}, {"s", spans}});
        // Laughter and shouts, close ones of a kind as one
        struct Burst {
            QString kind;
            double from, to, conf;
        };
        QVector<Burst> bursts;
        for (const AudioEvents::Event& e : r.events) {
            if (e.kind == QLatin1String("speech")) continue;
            const QString kind = e.kind == QLatin1String("laughter") ? "laughter" : "shout";
            if (!bursts.isEmpty() && bursts.last().kind == kind && e.from - bursts.last().to < 2) {
                bursts.last().to = std::max(bursts.last().to, e.to);
                bursts.last().conf = std::max(bursts.last().conf, e.conf);
            } else {
                bursts << Burst{kind, e.from, e.to, e.conf};
            }
        }
        for (const Burst& b : bursts) {
            // The sentence before it: a stretch of speech that runs into it, starting up to 8 s before; else 3.5 s
            double lead = -1;
            for (const AudioEvents::Event& e : r.events)
                if (e.kind == QLatin1String("speech") && e.to >= b.from - 1.0 && e.from < b.from && e.from >= b.from - 8)
                    lead = lead < 0 ? e.from : std::min(lead, e.from);
            const double from = std::max(0.0, lead >= 0 ? std::min(lead, b.from - 1.0) : b.from - 3.5);
            const double to = std::min(length, b.to + 1.5);
            const bool laugh = b.kind == QLatin1String("laughter");
            const double score = (laugh ? 6.0 : 5.0) + 2 * std::clamp(b.conf, 0.0, 1.0); // under a kill (10)
            QJsonObject moment = momentJson(from, to, score, "voice",
                                            {laugh ? "voice_laughter" : "voice_shout",
                                             {{"stream", name.isEmpty() ? QString("stream %1").arg(k) : name},
                                              {"s", round2(b.to - b.from)}}},
                                            {laugh ? "laughter" : "loud", "voice"});
            moment["first_s"] = std::round(b.from * 1000) / 1000;
            moment["last_s"] = std::round(b.to * 1000) / 1000;
            moment["lo_s"] = moment["from_s"];
            moment["hi_s"] = std::round(std::min(length, to + 3) * 1000) / 1000;
            out << moment;
        }
    }
    return out;
}

// ---------- The end of a replay clip ----------

// A short recording without a moment of its own: its end. Around the player's death in its last seconds (the fight
// before it), else its loudest moment there (the game sound), else its last 8 s
QJsonObject clipEndMoment(const MediaSource& m, const QJsonObject& h, double length, const QJsonArray& deaths)
{
    const double fps = m.format.rate.fps();
    std::vector<double> db; // the game sound's loudest sample per frame
    if (m.info.hasAudio) {
        QJsonObject a = h;
        QStringList names;
        for (int i = 0; i < m.info.audioStreamCount(); ++i) names << m.info.audioStreamName(i);
        if (const QVector<int> game = StreamPresets::gameStreams(names); !game.isEmpty()) a["audio_stream"] = game.first();
        try {
            db = loudPeaks(m, a, {}).db;
        } catch (const Error& e) {
            if (e.code == QLatin1String("CANCELED")) throw;
        }
    }
    const auto loudest = [&](double from, double to) {
        double best = -200, at = -1;
        for (int i = std::max(0, int(from * fps)); i < int(db.size()) && i / fps < to; ++i)
            if (db[size_t(i)] > best) best = db[size_t(i)], at = i / fps;
        return at;
    };
    const double window = std::max(0.0, length - kClipEndLookS);
    double death = -1, deathEnd = -1;
    for (const QJsonValue& d : deaths)
        if (d["s"].toDouble() >= window) death = d["s"].toDouble(), deathEnd = d["end_s"].toDouble();
    double from, to, first;
    Direction::Reason reason;
    if (death >= 0) {
        const double loud = loudest(death - 6, death);
        from = loud >= 0 ? loud - kLeadInS : death - 5;
        to = deathEnd;
        first = loud >= 0 ? loud : death;
        reason = {"clip_end_death", {}};
        from = std::min(from, death - 5); // the fight that ended in it
    } else if (const double loud = loudest(window, length); loud >= 0) {
        from = loud - kLeadInS;
        to = std::min(length, loud + 4.5);
        first = loud;
        reason = {"clip_end_loud", {}};
    } else {
        from = length - 8;
        to = length;
        first = -1;
        reason = {"clip_end", {}};
    }
    from = std::clamp(from, 0.0, length);
    to = std::clamp(to, from, length);
    QJsonObject moment = momentJson(from, to, kClipEndScore, "end", reason, {"end"});
    if (first >= 0) moment["first_s"] = std::round(first * 1000) / 1000;
    moment["lo_s"] = moment["from_s"];
    moment["hi_s"] = death >= 0 ? moment["to_s"] : QJsonValue(std::round(length * 1000) / 1000);
    if (death >= 0) moment["labels"] = QJsonArray{"end", "death"};
    return moment;
}

// How far a moment may grow in a short (lo_s, hi_s; a voice or end moment has them already): from kLeadInS before its
// first hit to kAfterS after its last, and never into the player's death screen
void bound(QJsonObject& m, const QJsonArray& deaths, double length)
{
    // Nothing reaches past `length` (the end of the game picture when the recording's last seconds show the desktop)
    if (m["to_s"].toDouble() > length) m["to_s"] = std::round(length * 1000) / 1000;
    if (m["from_s"].toDouble() > m["to_s"].toDouble()) m["from_s"] = m["to_s"];
    if (m.contains("hi_s") && m["hi_s"].toDouble() > length) m["hi_s"] = std::round(length * 1000) / 1000;
    if (m.contains("lo_s") && m["lo_s"].toDouble() > length) m["lo_s"] = std::round(length * 1000) / 1000;
    const double from = m["from_s"].toDouble(), to = m["to_s"].toDouble();
    const double first = m["first_s"].toDouble(-1), last = m["last_s"].toDouble(first >= 0 ? first : to);
    double lo = m.contains("lo_s") ? m["lo_s"].toDouble() : first >= 0 ? std::max(0.0, first - kLeadInS) : from;
    double hi = m.contains("hi_s") ? m["hi_s"].toDouble() : std::min(length, std::max(to, last + kAfterS));
    const double start = first >= 0 ? first : from;
    bool died = false;
    for (const QJsonValue& d : deaths) {
        const double at = d["s"].toDouble(), end = d["end_s"].toDouble();
        if (at >= start - 0.3) { // the first death after the action begins ends it
            if (end < hi) hi = end, died = true;
            break;
        }
        if (end > lo) lo = std::min(end, start); // a death before it (the round before): not shown
    }
    m["lo_s"] = std::round(lo * 1000) / 1000;
    m["hi_s"] = std::round(std::max(hi, lo) * 1000) / 1000;
    if (died) {
        QJsonArray labels = m["labels"].toArray();
        if (!labels.contains("death")) labels << "death";
        m["labels"] = labels;
    }
}

// The index (only when cached): a face +2, sharp +1, blurry -2, dark -2 for the shot at a moment's middle
void indexBonus(const QString& file, QJsonArray& moments)
{
    const std::optional<MediaIndex::Entry> e = MediaIndex::cached(file);
    if (!e) return;
    for (int i = 0; i < moments.size(); ++i) {
        QJsonObject m = moments[i].toObject();
        const double mid = (m["from_s"].toDouble() + m["to_s"].toDouble()) / 2;
        const auto shot = std::find_if(e->shots.begin(), e->shots.end(),
                                       [mid](const MediaIndex::Shot& s) { return mid >= s.from && mid < s.to; });
        if (shot == e->shots.end()) continue;
        const MediaIndex::Quality& q = shot->quality;
        double bonus = 0;
        QJsonArray labels = m["labels"].toArray();
        if (q.faces > 0 || q.face == QLatin1String("likely")) {
            bonus += 2;
            labels << "face";
        }
        if (q.blurry) bonus -= 2;
        else if (q.sharp >= 0.5) bonus += 1;
        if (q.dark) bonus -= 2;
        if (bonus == 0) continue;
        m["score"] = std::round((m["score"].toDouble() + bonus) * 10) / 10;
        m["labels"] = labels;
        m["index"] = true;
        moments[i] = m;
    }
}

} // namespace

// ---------- The end of the game picture ----------

namespace {
constexpr double kTailLookS = 5.0; // the last seconds looked at
constexpr int kTailRate = 10;
constexpr double kTailHard = 18;   // mean luma difference (0..255) of a hard change of the whole picture
constexpr double kTailStill = 5;   // ... and of a picture that stays still (a mouse, a blinking cursor)
constexpr double kTailBar = 11;    // ... of the bottom row, far more than the rest: a task bar coming up
constexpr int kTailW = 32, kTailH = 18;

std::vector<double> lumaOf(const QImage& img)
{
    std::vector<double> out(size_t(kTailW * kTailH), 0.0);
    if (img.isNull()) return {};
    const QImage s = img.scaled(kTailW, kTailH, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < kTailH; ++y)
        for (int x = 0; x < kTailW; ++x) {
            const QRgb p = s.pixel(x, y);
            out[size_t(y * kTailW + x)] = 0.299 * qRed(p) + 0.587 * qGreen(p) + 0.114 * qBlue(p);
        }
    return out;
}

double lumaDistance(const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.empty() || b.empty() || a.size() != b.size()) return 255;
    double sum = 0;
    for (size_t i = 0; i < a.size(); ++i) sum += std::abs(a[i] - b[i]);
    return sum / double(a.size());
}
} // namespace

double gameEndOf(const QVector<QImage>& pictures, double firstS, int rate)
{
    const int n = int(pictures.size());
    if (n < 3 || rate <= 0) return -1;
    std::vector<std::vector<double>> l;
    for (const QImage& p : pictures) l.push_back(lumaOf(p));
    // Still from c on: every later picture close to the one at c
    const auto stillFrom = [&](int c, int upTo) {
        for (int j = c + 1; j < upTo; ++j)
            if (lumaDistance(l[size_t(j)], l[size_t(c)]) > kTailStill) return false;
        return true;
    };
    const auto hardAt = [&](int c) { return c > 0 && lumaDistance(l[size_t(c)], l[size_t(c - 1)]) >= kTailHard; };
    const bool debug = qEnvironmentVariableIsSet("SCHNEIDI_TAIL_DEBUG");
    if (debug)
        for (int i = 1; i < n; ++i)
            std::fprintf(stderr, "tail %.2f s: change %.1f\n", firstS + double(i) / rate, lumaDistance(l[size_t(i)], l[size_t(i - 1)]));
    // A hard change followed by at least 1 s of still pictures with at most 1 s left after them (the recorder's own
    // stop can move the picture once more): the earliest such one
    int end = -1;
    for (int c = 1; c < n && end < 0; ++c) {
        if (!hardAt(c)) continue;
        int j = c + 1;
        while (j < n && lumaDistance(l[size_t(j)], l[size_t(c)]) <= kTailStill) ++j;
        if (j - c >= rate && n - j <= rate) end = c;
    }
    // Else the last hard change whose pictures after it stay still to the end (at least 0.2 s)
    for (int c = n - 2; c >= 1 && end < 0; --c)
        if (hardAt(c) && stillFrom(c, n)) end = c;
    if (end < 0) return -1;
    // Still pictures before it, one after the other (a window, then another): the first of them
    for (int c = end - 1; c >= 1; --c)
        if (hardAt(c) && stillFrom(c, end)) end = c;
    // The task bar coming up over the game just before (the mouse went down to stop the recorder): a hard change of
    // the bottom row of the picture alone, up to 1 s before
    const auto rowsChange = [&](int c, int y0, int y1) {
        if (l[size_t(c)].empty() || l[size_t(c - 1)].empty()) return 255.0;
        double sum = 0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < kTailW; ++x)
                sum += std::abs(l[size_t(c)][size_t(y * kTailW + x)] - l[size_t(c - 1)][size_t(y * kTailW + x)]);
        return sum / ((y1 - y0) * kTailW);
    };
    const auto barAt = [&](int c) {
        const double bottom = rowsChange(c, kTailH - 1, kTailH), rest = rowsChange(c, 0, kTailH - 1);
        return bottom >= kTailBar && bottom >= 2.5 * rest;
    };
    if (debug)
        for (int c = std::max(1, end - rate); c < end; ++c)
            std::fprintf(stderr, "tail bottom %.2f s: %.1f rest %.1f\n", firstS + double(c) / rate,
                         rowsChange(c, kTailH - 1, kTailH), rowsChange(c, 0, kTailH - 1));
    for (int c = end - 1; c >= std::max(1, end - rate); --c)
        if (barAt(c)) {
            end = c;
            break;
        }
    return firstS + double(end) / rate;
}

namespace {
// The end of the game picture in a recording's last seconds (-1: none)
double gameEnd(const MediaSource& m, double length)
{
    if (!m.info.hasVideo || m.info.isImage || length < 2 * kTailLookS) return -1;
    const auto snap = Snapshot::Source::media(m.format, m.info.path);
    const double fps = m.format.rate.fps(), first = length - kTailLookS;
    QVector<QImage> pictures;
    for (int i = 0; i < int(kTailLookS * kTailRate); ++i) {
        const int frame = std::min(m.info.length - 1, int(std::lround((first + double(i) / kTailRate) * fps)));
        pictures << snap->frame(frame, 160);
    }
    return gameEndOf(pictures, first, kTailRate);
}
} // namespace

bool voiceStream(const QString& title)
{
    static const QRegularExpression chat("discord|teamspeak|\\bts3?\\b|mumble|chat", QRegularExpression::CaseInsensitiveOption);
    return !StreamPresets::isMixTitle(title) && (Transcript::isVoiceName(title) || chat.match(title).hasMatch());
}

QString gameInPicture(const QStringList& files, Context& ctx, int upTo)
{
    for (int i = 0; i < std::min(upTo, int(files.size())); ++i) {
        failIfCanceled();
        QString id;
        MediaSource m;
        try {
            m = openMedia({{"file", files[i]}});
        } catch (const Error& e) {
            if (e.code == QLatin1String("CANCELED")) throw;
            continue;
        }
        if (!m.info.hasVideo || m.info.isImage) continue;
        // The detector's version in the key: a better one looks again (the pictures are read at 1080 lines as the
        // kill feed is measured; smaller ones lose its thin lines)
        const AnalysisCache::Key key = AnalysisCache::key("game-in-picture", m.info.path,
                                                          {{"detector", KillFeedLines::version()}}, 1);
        if (const auto hit = AnalysisCache::get(key)) {
            id = (*hit)["game"].toString();
        } else {
            const auto snap = Snapshot::Source::media(m.format, m.info.path);
            const double fps = m.format.rate.fps();
            const GameHud::Game* g = KillFeedLines::recognise(
                [&](double s) { return snap->frame(int(s * fps), 1920); }, seconds(m.info.length, m.format));
            if (g) {
                // Confirmed by the player's own kills (a racing game's fixed boxes at the top right are no kill feed)
                Context sub;
                sub.cancel = ctx.cancel;
                try {
                    const QJsonObject h = highlightsFile({{"file", m.info.path}, {"game", g->id}}, sub);
                    for (const QJsonValue& c : h["candidates"].toArray())
                        if (c["kills"].toInt() > 0) id = g->id;
                } catch (const Error& e) {
                    if (e.code == QLatin1String("CANCELED")) throw;
                }
            }
            AnalysisCache::put(key, {{"game", id}});
        }
        if (!id.isEmpty()) return id;
    }
    return {};
}

QJsonObject directMomentsFile(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    const QFileInfo fi(m.info.path);
    const double length = seconds(m.info.length, m.format);
    // Key taken before measuring: the frame rate frames count in (highlights' ranges), the game, the sources' versions
    const QJsonObject options{{"rate", QString("%1/%2").arg(m.format.rate.num).arg(m.format.rate.den)},
                              {"game", a["game"].toString()},
                              {"events", AudioEvents::way(AudioEvents::Engine::Auto)},
                              {"way", ffmpegAnalysis() ? "ffmpeg" : "own"}};
    const AnalysisCache::Key key = AnalysisCache::key("direct-moments", m.info.path, options, kMomentsVersion);
    QJsonObject out;
    if (const auto hit = AnalysisCache::get(key)) {
        out = *hit;
        out["cached"] = true;
    } else {
        const ClipFormat cf = a.contains("project") ? detectClipFormat(m.info.path) : m.clip;
        Direction::Material mat;
        mat.file = m.info.path;
        mat.length = length;
        mat.hasVideo = m.info.hasVideo && !m.info.isImage;
        mat.hasAudio = m.info.hasAudio;
        mat.size = fi.size();
        mat.modified = fi.lastModified().toMSecsSinceEpoch();
        if (cf.ok) {
            mat.fps = std::round(cf.rate.fps() * 1000) / 1000;
            mat.width = cf.width;
            mat.height = cf.height;
        }
        if (m.info.hasAudio)
            for (int i = 0; i < m.info.audioStreamCount(); ++i) mat.audioStreams << m.info.audioStreamName(i);
        const auto part = [&ctx](double from, double to) {
            Context sub;
            sub.cancel = ctx.cancel;
            if (ctx.progress) sub.progress = [&ctx, from, to](double p) { ctx.progress(from + (to - from) * p); };
            return sub;
        };
        // Game footage (a kill feed read, or the game given): the highlights alone, as `short` always took them.
        // Other material: the loud moments and the sound events; the picture's movement only when neither found one
        Context h = part(0, 0.6);
        QString sound, events = "none", picture = "none";
        bool feed = false;
        QJsonArray highlights = highlightMoments(a, h, &sound, &feed);
        const bool game = feed || (!a["game"].toString().isEmpty() && a["game"].toString() != QLatin1String("none"));
        if (ctx.progress) ctx.progress(0.6);
        QJsonArray extra, deaths;
        QString died = "none", voices = "none";
        // Game footage: where the game picture ends (the desktop in the last seconds): no moment reaches past it
        double usable = length;
        if (game) {
            const double end = gameEnd(m, length);
            if (end > 0) usable = end;
        }
        if (!game) {
            extra = eventMoments(m, length, &events);
            if (ctx.progress) ctx.progress(0.75);
            if (highlights.isEmpty() && extra.isEmpty()) extra = pictureMoments(m, length, &picture);
        } else {
            // Game footage: where the player died (the moments end there), what the voices say and laugh (moments of
            // their own, below a kill), and a replay clip without any moment: its end
            if (showsDeath(a["game"].toString()) && mat.hasVideo && length <= kDeathScanMaxS) deaths = deathScreens(m, &died);
            if (ctx.progress) ctx.progress(0.75);
            const bool found = !highlights.isEmpty();
            // A laugh or shout inside a kill's moment is part of it (its label), not a moment of its own
            for (const QJsonValue& v : voiceMoments(m, length, &mat.voice, &voices)) {
                const double vf = v["first_s"].toDouble(), vl = v["last_s"].toDouble();
                bool inside = false;
                for (int i = 0; i < highlights.size() && !inside; ++i) {
                    QJsonObject x = highlights[i].toObject();
                    if (x["source"].toString() != QLatin1String("highlights") || vl < x["from_s"].toDouble()
                        || vf > x["to_s"].toDouble())
                        continue;
                    QJsonArray labels = x["labels"].toArray();
                    for (const QJsonValue& l : v["labels"].toArray())
                        if (!labels.contains(l)) labels << l;
                    x["labels"] = labels;
                    highlights[i] = x;
                    inside = true;
                }
                if (!inside) highlights << v;
            }
            if (!found && length <= kClipEndMaxS) highlights << clipEndMoment(m, {}, usable, deaths);
        }
        // How far each may grow in a short: around its hits (or loud peaks), not into a death; none past the game
        boundMoments(highlights, deaths, usable);
        if (ctx.progress) ctx.progress(1);
        mat.moments = int(highlights.size() + extra.size());
        mat.analysis = {{"sound", sound}, {"picture", picture}, {"events", events}, {"speech", "none"}};
        if (game) {
            mat.analysis["death"] = died;
            mat.analysis["voice"] = voices;
            if (usable < length) mat.analysis["game_end_s"] = std::round(usable * 100) / 100;
        }
        out = {{"file", m.info.path}, {"material", Direction::toJson(mat)}, {"highlights", highlights}, {"extra", extra}};
        if (!deaths.isEmpty()) out["deaths"] = deaths;
        // A source that failed (a broken tail, a decoder missing) is asked again next time: not cached
        if (sound != QLatin1String("failed") && events != QLatin1String("failed") && picture != QLatin1String("failed")
            && died != QLatin1String("failed") && voices != QLatin1String("failed"))
            AnalysisCache::put(key, out);
    }
    // The file as it is spelled now (the cache keys on the path's key: another case on Windows)
    QJsonObject mat = out["material"].toObject();
    mat["file"] = m.info.path;
    out["material"] = mat;
    out["file"] = m.info.path;
    // The index's bonus on top (it may have been built since); not for recipe short (the highlights as they are)
    if (!a["no_index"].toBool())
        for (const char* k : {"highlights", "extra"}) {
            QJsonArray list = out[k].toArray();
            indexBonus(m.info.path, list);
            out[k] = list;
        }
    return out;
}

void boundMoments(QJsonArray& moments, const QJsonArray& deaths, double usable)
{
    QJsonArray kept;
    for (const QJsonValue& v : std::as_const(moments)) {
        // One that begins where the game picture has ended (a kill read off the desktop in the recording's tail):
        // nothing of it can be shown
        if (v["from_s"].toDouble() >= usable - 0.05) continue;
        QJsonObject x = v.toObject();
        bound(x, deaths, usable);
        kept << x;
    }
    moments = kept;
}

CollectedMoments collectMoments(const QStringList& files, const QString& project, const QString& game, bool index,
                                Context& ctx)
{
    QJsonObject a{{"files", QJsonArray::fromStringList(files)}, {"game", game}};
    if (!index) a["no_index"] = true;
    if (!project.isEmpty()) a["project"] = project;
    const QJsonObject r = perFile(a, ctx, directMomentsFile, 3);
    CollectedMoments out;
    const QJsonArray list = r["files"].toArray();
    for (int i = 0; i < list.size(); ++i) {
        const QJsonObject f = list[i].toObject();
        if (f.contains("error")) {
            const QJsonObject e = f["error"].toObject();
            fail(e["code"].toString(), e["message"].toString());
        }
        const std::optional<Direction::Session> one = Direction::fromJson(
            QJsonObject{{"recipe", "x"}, {"material", QJsonArray{f["material"]}}});
        out.material << (one && !one->material.isEmpty() ? one->material.first() : Direction::Material{});
        const auto moments = [i](const QJsonArray& l) {
            QVector<DirectMoment> v;
            for (const QJsonValue& x : l)
                v << DirectMoment{i,
                                  x["from_s"].toDouble(),
                                  x["to_s"].toDouble(),
                                  x["score"].toDouble(),
                                  {x["reason"]["key"].toString(), x["reason"]["values"].toObject()},
                                  [&] {
                                      QStringList s;
                                      for (const QJsonValue& y : x["labels"].toArray()) s << y.toString();
                                      return s;
                                  }(),
                                  x["source"].toString(),
                                  x["first_s"].toDouble(-1),
                                  x["lo_s"].toDouble(-1),
                                  x["hi_s"].toDouble(-1),
                                  [&] {
                                      QVector<double> hits;
                                      for (const QJsonValue& y : x["hit_s"].toArray()) hits << y.toDouble();
                                      return hits;
                                  }()};
            return v;
        };
        out.highlights += moments(f["highlights"].toArray());
        out.extra += moments(f["extra"].toArray());
    }
    std::stable_sort(out.extra.begin(), out.extra.end(),
                     [](const DirectMoment& x, const DirectMoment& y) { return x.score > y.score; });
    return out;
}

} // namespace Cli::detail
