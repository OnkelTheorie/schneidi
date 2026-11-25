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
#include "engine/AnalysisCache.h"
#include "engine/AudioEvents.h"
#include "engine/ClipFormat.h"
#include "engine/MediaIndex.h"

#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

constexpr int kMomentsVersion = 3; // raise when the moments of a file change for the same sources

constexpr double kHighlightsPreS = 2.0; // highlights' default lead-in before the first kill or loud moment

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
        if (const QJsonArray hits = c["hit_s"].toArray(); !hits.isEmpty()) moment["first_s"] = hits.first().toDouble();
        else if (c["from_s"].toDouble() > 0) moment["first_s"] = c["from_s"].toDouble() + kHighlightsPreS;
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
        const QJsonArray highlights = highlightMoments(a, h, &sound, &feed);
        const bool game = feed || (!a["game"].toString().isEmpty() && a["game"].toString() != QLatin1String("none"));
        if (ctx.progress) ctx.progress(0.6);
        QJsonArray extra;
        if (!game) {
            extra = eventMoments(m, length, &events);
            if (ctx.progress) ctx.progress(0.75);
            if (highlights.isEmpty() && extra.isEmpty()) extra = pictureMoments(m, length, &picture);
        }
        if (ctx.progress) ctx.progress(1);
        mat.moments = int(highlights.size() + extra.size());
        mat.analysis = {{"sound", sound}, {"picture", picture}, {"events", events}, {"speech", "none"}};
        out = {{"file", m.info.path}, {"material", Direction::toJson(mat)}, {"highlights", highlights}, {"extra", extra}};
        // A source that failed (a broken tail, a decoder missing) is asked again next time: not cached
        if (sound != QLatin1String("failed") && events != QLatin1String("failed") && picture != QLatin1String("failed"))
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
                                  x["first_s"].toDouble(-1)};
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
