// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: edit ops on music - `fit_music` (a song cut to a length on its phrases, core/MusicFit) and
// `snap_to_beat` (cuts, hits or markers onto the beat grid of the music on the timeline). Both read the beats of the
// song files (core/Beats), analysed once per file and kept in the cache (CacheLocation/beats); live, schneidi-cli
// analyses before it sends the edit to the app (prepareMusic), so the app does not decode.
#include "cli/CommandsDetail.h"

#include "core/Beats.h"
#include "core/I18n.h"
#include "core/MusicFit.h"
#include "core/Speech.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Cli::detail {

namespace {

constexpr int kCacheVersion = 1; // bump when core/Beats changes its results

double round3(double v) { return std::round(v * 1000) / 1000; }
double round4(double v) { return std::round(v * 10000) / 10000; } // song times: 0.1 ms (cuts on the grid)

// ---------- Beats of a song file (cached) ----------

QJsonObject beatsJson(const Beats::Result& r)
{
    QJsonArray energy, bass, breaks, drops;
    for (double v : r.barEnergyDb) energy << v;
    for (double v : r.barBassDb) bass << v;
    for (const Beats::Break& b : r.breaks)
        breaks << QJsonArray{b.from, b.to, b.bassDb, b.energyDb, b.atStart, b.atEnd};
    for (const Beats::Drop& d : r.drops)
        drops << QJsonArray{d.beat, d.score, d.bassJumpDb, d.energyJumpDb, d.breakS, d.breakIndex};
    return {{"found", r.found}, {"bpm", r.bpm}, {"confidence", r.confidence}, {"period", r.period},
            {"phase", r.phase}, {"beats", r.beats}, {"downbeat", r.downbeat},
            {"downbeat_confidence", r.downbeatConfidence}, {"phrase_bars", r.phraseBars},
            {"phrase_offset", r.phraseOffset}, {"phrase_confidence", r.phraseConfidence}, {"energy", energy},
            {"bass", bass}, {"breaks", breaks}, {"drops", drops}};
}

Beats::Result beatsFrom(const QJsonObject& o)
{
    Beats::Result r;
    r.found = o["found"].toBool();
    r.bpm = o["bpm"].toDouble();
    r.confidence = o["confidence"].toDouble();
    r.period = o["period"].toDouble();
    r.phase = o["phase"].toDouble();
    r.beats = o["beats"].toInt();
    r.downbeat = o["downbeat"].toInt();
    r.downbeatConfidence = o["downbeat_confidence"].toDouble();
    r.phraseBars = std::max(1, o["phrase_bars"].toInt(4));
    r.phraseOffset = o["phrase_offset"].toInt();
    r.phraseConfidence = o["phrase_confidence"].toDouble();
    for (const QJsonValue& v : o["energy"].toArray()) r.barEnergyDb.push_back(v.toDouble());
    for (const QJsonValue& v : o["bass"].toArray()) r.barBassDb.push_back(v.toDouble());
    r.barBassDb.resize(r.barEnergyDb.size(), -120);
    for (const QJsonValue& v : o["breaks"].toArray()) {
        const QJsonArray a = v.toArray();
        r.breaks.push_back({a[0].toInt(), a[1].toInt(), a[2].toDouble(), a[3].toDouble(), a[4].toBool(), a[5].toBool()});
    }
    for (const QJsonValue& v : o["drops"].toArray()) {
        const QJsonArray a = v.toArray();
        r.drops.push_back({a[0].toInt(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble(), a[5].toInt()});
    }
    return r;
}

// Key of a song in the cache and in `_beats`: file, size, time, stream
QString beatsKey(const QString& path, int stream)
{
    const QFileInfo fi(path);
    const QString id = QString("%1|%2|%3|%4|%5")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(stream)
                           .arg(kCacheVersion);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString cacheFile(const QString& key)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/beats/" + key + ".json";
}

// The whole file's beats (times from the file start); throws NO_AUDIO / NO_BEAT. `given` = beats schneidi-cli
// measured before (live: `_beats` of the op)
Beats::Result songBeats(const ProjectFormat& f, const MediaInfo& m, int stream, const QJsonObject& given,
                        Context* ctx = nullptr)
{
    if (!m.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.path);
    const QString key = beatsKey(m.path, stream);
    std::optional<Beats::Result> r;
    if (given.contains(key)) r = beatsFrom(given[key].toObject());
    if (!r) {
        QFile file(cacheFile(key));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
            if (o["version"].toInt() == kCacheVersion) r = beatsFrom(o["beats"].toObject());
        }
    }
    if (!r) {
        std::optional<Beats::Features> features;
        std::vector<float> mono;
        const double fps = f.rate.fps();
        const auto sink = [&](const float* pcm, int samples, int channels, int rate) {
            if (!features) features.emplace(rate);
            if (!pcm) { // a frame that could not be decoded: silence of its length
                mono.assign(size_t(std::lround(rate / fps)), 0.f);
            } else {
                mono.assign(size_t(samples), 0.f);
                for (int c = 0; c < channels; ++c)
                    for (int i = 0; i < samples; ++i) mono[size_t(i)] += pcm[size_t(c) * size_t(samples) + size_t(i)];
                for (float& s : mono) s /= float(std::max(1, channels));
            }
            features->add(mono.data(), int(mono.size()));
        };
        const auto progress = [ctx](double p) {
            if (ctx && ctx->progress) ctx->progress(p);
            return true;
        };
        Clip c;
        c.mediaPath = m.path;
        c.audioStream = stream;
        c.in = 0;
        c.out = m.length - 1;
        if (!AudioAnalysis::clipSound(f, c, sink, progress) || !features)
            fail("NO_AUDIO", "could not decode the audio of " + m.path);
        r = Beats::analyze(features->take());
        QDir().mkpath(QFileInfo(cacheFile(key)).absolutePath());
        QFile file(cacheFile(key));
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(QJsonObject{{"version", kCacheVersion}, {"file", m.path}, {"beats", beatsJson(*r)}})
                           .toJson(QJsonDocument::Compact));
    }
    if (!r->found || r->period <= 0)
        fail("NO_BEAT", QString("no beat found in %1 (silence, or shorter than four beats)").arg(QFileInfo(m.path).fileName()));
    return *r;
}

int streamOf(const QJsonObject& op, const MediaInfo& m, int fallback)
{
    const int stream = op.contains("stream") ? op.value("stream").toInt(-1) : fallback;
    if (stream < 0 || stream >= std::max(1, m.audioStreamCount()))
        fail("BAD_ARGUMENT", QString("stream: the file has %1 audio stream(s)").arg(m.audioStreamCount()));
    return stream;
}

// ---------- fit_music ----------

// The song of the op: an audio clip on the timeline (replaced) or a file (placed)
struct Song {
    const Clip* clip = nullptr; // `clip`
    int track = -1;             // audio track of the clip
    MediaInfo media;
    int stream = 0;
};

Song songOf(Session& s, const QJsonObject& op)
{
    Song g;
    if (op.contains("clip") == op.contains("media"))
        fail("BAD_ARGUMENT", "give the song as 'clip' (an audio clip id on the timeline) or 'media' (a file)");
    if (op.contains("clip")) {
        const Clip& c = clipOf(s, op);
        TrackRef r;
        TimelineOps::findClip(s.project.timeline(), c.id, &r);
        if (r.kind != TrackKind::Audio || c.kind != ClipKind::Media)
            fail("BAD_ARGUMENT", QString("clip %1 is no audio clip of a file (the music's clip on an A track)").arg(c.id));
        if (c.isRetimed()) fail("BAD_ARGUMENT", QString("clip %1 has another speed: set it back to 100 % first").arg(c.id));
        if (s.editor.hasPartner(c.id))
            fail("BAD_ARGUMENT", QString("clip %1 is linked to a picture: unlink it first ({op:'unlink', clips:[%1]})").arg(c.id));
        g.clip = &c;
        g.track = r.index;
        g.media = s.media(c.mediaPath);
        g.stream = streamOf(op, g.media, c.audioStream);
    } else {
        g.media = s.media(op.value("media").toString());
        g.stream = streamOf(op, g.media, 0);
    }
    if (!g.media.hasAudio || g.media.length <= 0) fail("NO_AUDIO", "file has no audio: " + g.media.path);
    return g;
}

// End of the timeline without the song's clip (length:'timeline')
int timelineEnd(const Session& s, int without)
{
    int end = 0;
    const Timeline& tl = s.project.timeline();
    for (const QVector<Track>* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips)
                if (c.id != without) end = std::max(end, c.end());
    return end;
}

void opFitMusic(Session& s, const QJsonObject& op)
{
    const ProjectFormat& f = s.format();
    const double fps = f.rate.fps();
    const Song g = songOf(s, op);
    if (!op.contains("length")) fail("BAD_ARGUMENT", "missing 'length': the length of the result (\"30s\", or \"timeline\")");
    int frames = 0;
    if (op.value("length").toString() == "timeline") {
        // From where the song will start (at, else the clip's own start) to the end of the rest of the timeline
        frames = timelineEnd(s, g.clip ? g.clip->id : 0) - (op.contains("at") ? timeOf(s, op, "at") : g.clip ? g.clip->start : 0);
        if (frames <= 0) fail("EMPTY", "length:'timeline': the timeline has nothing besides the song to take the length from");
    } else {
        frames = timeOf(s, op, "length");
    }
    if (frames <= 0) fail("BAD_ARGUMENT", "'length' must be more than 0");

    MusicFit::Options o;
    o.length = frames / fps;
    const QString end = op.value("end").toString("natural");
    if (end != "natural" && end != "fade") fail("BAD_ARGUMENT", "end: 'natural' (the song's own end, default) or 'fade'");
    o.end = end == "fade" ? MusicFit::End::Fade : MusicFit::End::Natural;
    const QString keep = op.value("keep").toString();
    if (op.contains("keep") && keep != "drop") fail("BAD_ARGUMENT", "keep: only 'drop' (the strongest drop must stay)");
    o.keepDrop = keep == "drop";
    if (op.contains("climax_at")) o.climaxAt = timeOf(s, op, "climax_at") / fps;
    if (op.contains("crossfade")) {
        o.crossfade = timeOf(s, op, "crossfade") / fps;
        if (o.crossfade <= 0) fail("BAD_ARGUMENT", "crossfade must be at least one frame");
    }
    const Beats::Result beats = songBeats(f, g.media, g.stream, op.value("_beats").toObject());
    const MusicFit::Plan plan = MusicFit::plan(beats, double(g.media.length) / fps, o);
    if (!plan.ok) fail(plan.errorCode, plan.error);

    const int at = op.contains("at") ? timeOf(s, op, "at") : g.clip ? g.clip->start : 0;
    int track = g.track;
    if (op.contains("track")) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
    }
    Clip like;
    if (g.clip) like = *g.clip;
    like.mediaPath = g.media.path;
    like.audioStream = g.stream;
    const QVector<Clip> parts = MusicFit::clips(plan, like, fps, at, g.media.length);
    if (parts.isEmpty()) fail("FAILED", "could not cut the song");
    if (track < 0) track = Speech::freeAudioTrack(s.project.timeline(), at, parts.last().end(), -1);
    // The cut song must not cover other clips of its track (placing overwrites: the next song was cut away unasked)
    if (track < s.project.timeline().audio.size())
        for (const Clip& c : s.project.timeline().audio[track].clips)
            if ((!g.clip || c.id != g.clip->id) && c.start < parts.last().end() && c.end() > at)
                fail("FAILED", QString("the fitted song (%1 - %2) would cover clip %3 on A%4: move that clip, or give "
                                       "another track")
                                   .arg(tc(at, f), tc(parts.last().end(), f))
                                   .arg(c.id)
                                   .arg(track + 1));
    if (g.clip && !g.clip->keys.isEmpty())
        s.notes << QString("fit_music: the keyframes of clip %1 (volume …) do not fit the cut song and were left out")
                       .arg(g.clip->id);
    const QVector<int> ids = s.editor.placeAudioClips(parts, track, g.clip ? g.clip->id : 0, T("Musik auf Länge"));
    if (ids.isEmpty()) fail("FAILED", QString("could not place the song on A%1 (locked?)").arg(track + 1));

    // The answer: what plays, where it was cut, how close the length is
    QJsonArray segments, cutsAt, cuts, clipIds;
    for (const MusicFit::Segment& seg : plan.segments)
        segments << QJsonObject{{"from_s", round4(seg.from)}, {"to_s", round4(seg.to)}};
    for (size_t i = 0; i < plan.cuts.size(); ++i) {
        const MusicFit::Cut& c = plan.cuts[i];
        const double atS = (parts[int(i) + 1].start - at) / fps;
        cutsAt << round3(atS);
        cuts << QJsonObject{{"at_s", round3(atS)}, {"from_s", round3(c.from)}, {"to_s", round3(c.to)},
                            {"bars", QString("%1 -> %2").arg(c.fromBar + 1).arg(c.toBar + 1)},
                            {"phrase", c.phrase}, {"level_db", std::round(c.levelDb * 10) / 10},
                            {"crossfade_ms", int(std::lround(parts[int(i)].transOut / fps * 1000))}};
    }
    for (int id : ids) clipIds << id;
    const int length = parts.last().end() - at;
    QJsonObject r{{"segments", segments}, {"cuts_at", cutsAt}, {"cuts", cuts},
                  {"length_s", round3(length / fps)}, {"residual_ms", int(std::lround((length / fps - o.length) * 1000))},
                  {"start_trim_s", round3(plan.startTrim)}, {"bpm", std::round(beats.bpm * 100) / 100},
                  {"track", trackName({TrackKind::Audio, track})}, {"clips", clipIds}};
    r["drop_at"] = plan.dropAt >= 0 ? QJsonValue(round3(plan.dropAt)) : QJsonValue();
    if (plan.startInBar)
        r["note"] = "no room before a bar one for the rest below a bar: the song starts inside its first bar (short fade-in)";
    if (beats.confidence < 0.3)
        r["warning"] = QString("weak beat (confidence %1): the cuts may not sit on the song's bars").arg(beats.confidence, 0, 'f', 2);
    s.notes << QJsonObject{{"fit_music", r}};
}

// ---------- snap_to_beat ----------

// The music the grid comes from: audio clips of files on one audio track (`music`: a clip id or a track; default the
// track whose unlinked media clips play the longest)
QVector<const Clip*> musicClips(Session& s, const QJsonObject& op)
{
    const Timeline& tl = s.project.timeline();
    int track = -1;
    const QJsonValue v = op.value("music");
    if (v.isDouble()) {
        TrackRef r;
        const Clip* c = TimelineOps::findClip(tl, v.toInt(), &r);
        if (!c || r.kind != TrackKind::Audio) fail("NOT_FOUND", QString("music: no audio clip with id %1").arg(v.toInt()));
        track = r.index;
    } else if (v.isString()) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(v, &k);
        if (k != TrackKind::Audio || track >= tl.audio.size()) fail("BAD_ARGUMENT", "music: an audio track (A2 …) or clip id");
    } else {
        int longest = 0;
        for (int i = 0; i < tl.audio.size(); ++i) {
            int len = 0;
            for (const Clip& c : tl.audio[i].clips)
                if (c.kind == ClipKind::Media && c.enabled && !s.editor.hasPartner(c.id)) len += c.length();
            if (len > longest) longest = len, track = i;
        }
        if (track < 0) fail("NOT_FOUND", "no music on the timeline (an audio clip without picture); give 'music'");
    }
    QVector<const Clip*> out;
    for (const Clip& c : tl.audio[track].clips)
        if (c.kind == ClipKind::Media && c.enabled) out << &c;
    if (out.isEmpty()) fail("NOT_FOUND", QString("no music clip on A%1").arg(track + 1));
    return out;
}

// Grid times on the timeline (frames, exact) from the music clips' beats: every beat, bar one or phrase start
QVector<double> gridOf(Session& s, const QJsonObject& op, const QString& grid)
{
    const ProjectFormat& f = s.format();
    const double fps = f.rate.fps();
    const QJsonObject given = op.value("_beats").toObject();
    QVector<double> out;
    for (const Clip* c : musicClips(s, op)) {
        if (c->isRetimed()) {
            s.notes << QString("snap_to_beat: music clip %1 has another speed: its beats are left out").arg(c->id);
            continue;
        }
        const MediaInfo& m = s.media(c->mediaPath);
        const Beats::Result r = songBeats(f, m, c->audioStream, given);
        const int bars = int(r.barEnergyDb.size());
        for (int k = 0; k <= r.beats; ++k) {
            const int fromDown = k - r.downbeat;
            if (grid != "beat") {
                if (fromDown < 0 || fromDown % 4 != 0) continue;
                const int bar = fromDown / 4;
                if (grid == "phrase" && !(bar == 0 || (bar >= r.phraseOffset && (bar - r.phraseOffset) % r.phraseBars == 0)))
                    continue;
                if (bar > bars) continue;
            }
            const double t = c->start + c->subframe / 100.0 + Beats::beatTime(r, k) * fps - c->in;
            if (t >= c->start && t < c->end()) out << t;
        }
    }
    std::sort(out.begin(), out.end());
    if (out.isEmpty()) fail("NO_BEAT", "the music clips play no " + grid + " of their song");
    return out;
}

// A clip whose content is shown at timeline frame t: on top of the video tracks
const Clip* videoClipAt(const Timeline& tl, int t)
{
    for (int i = tl.video.size() - 1; i >= 0; --i) {
        if (tl.video[i].hidden) continue;
        for (const Clip& c : tl.video[i].clips)
            if (c.kind == ClipKind::Media && c.enabled && t >= c.start && t < c.end()) return &c;
    }
    return nullptr;
}

void opSnapToBeat(Session& s, const QJsonObject& op)
{
    const ProjectFormat& f = s.format();
    const QString event = op.value("event").toString("start"), grid = op.value("grid").toString("beat");
    if (event != "start" && event != "hit" && event != "marker")
        fail("BAD_ARGUMENT", "event: 'start' (the clips' cuts, default), 'hit' (with events: the hit_s of `highlights`) or 'marker'");
    if (grid != "beat" && grid != "bar" && grid != "phrase") fail("BAD_ARGUMENT", "grid: 'beat' (default), 'bar' or 'phrase'");
    const QString how = op.value("how").toString(event == "start" ? "roll" : "slip");
    if (how != "slip" && how != "move" && how != "roll") fail("BAD_ARGUMENT", "how: 'slip', 'move' or 'roll'");
    if (event == "start" && how == "slip") fail("BAD_ARGUMENT", "event 'start' moves cuts: how 'roll' (default) or 'move'");
    if (event != "start" && how == "roll") fail("BAD_ARGUMENT", "how 'roll' is for event 'start' (the cuts)");
    const int maxShift = op.contains("max_shift") ? timeOf(s, op, "max_shift") : int(std::lround(0.25 * f.rate.fps()));
    const QVector<double> targets = gridOf(s, op, grid);
    const auto nearest = [&](int t) {
        const auto it = std::lower_bound(targets.begin(), targets.end(), double(t));
        double best = it == targets.end() ? targets.last() : *it;
        if (it != targets.begin() && std::abs(*(it - 1) - t) < std::abs(best - t)) best = *(it - 1);
        return int(std::lround(best));
    };
    const Timeline& tl = s.project.timeline();

    // The moments: (timeline frame, clip id)
    QVector<QPair<int, int>> moments;
    if (event == "start") {
        QVector<int> ids;
        if (op.contains("clips") || op.contains("clip")) {
            QJsonObject alone = op;
            alone["linked"] = false;
            ids = clipIds(s, alone);
        } else {
            for (const Track& t : tl.video)
                for (const Clip& c : t.clips)
                    if (c.start > 0) ids << c.id;
        }
        for (int id : ids)
            if (const Clip* c = TimelineOps::findClip(tl, id)) moments << qMakePair(c->start, id);
    } else if (event == "marker") {
        for (int m : tl.markers)
            if (const Clip* c = videoClipAt(tl, m)) moments << qMakePair(m, c->id);
        if (moments.isEmpty()) fail("NOT_FOUND", "no timeline marker lies on a video clip");
    } else {
        const QJsonArray events = op.value("events").isArray() ? op.value("events").toArray() : QJsonArray{op.value("events")};
        if (events.isEmpty() || events.first().isUndefined())
            fail("BAD_ARGUMENT", "event 'hit' needs 'events': the hit_s of `highlights` as [{media:'/path/clip.mkv', source:'7.67s'}]");
        for (const QJsonValue& v : events) {
            const int t = timeOf(s, QJsonObject{{"events", v}}, "events");
            const Clip* c = v.toObject().contains("clip") ? &clipOf(s, v.toObject()) : videoClipAt(tl, t);
            if (!c) fail("NOT_FOUND", QString("no video clip plays the event at %1").arg(tc(t, f)));
            moments << qMakePair(t, c->id);
        }
    }
    std::sort(moments.begin(), moments.end());

    QVector<int> done;
    QJsonArray report;
    for (const auto& [t, id] : moments) {
        QJsonObject r{{"clip", id}, {"event", tc(t, f)}};
        if (done.contains(id)) {
            r["note"] = "same clip as an earlier event: moved with it";
            report << r;
            continue;
        }
        done << id;
        const int target = nearest(t);
        r["beat"] = tc(target, f);
        if (target == t) {
            r["shift_frames"] = 0;
            report << r;
            continue;
        }
        if (std::abs(target - t) > maxShift) {
            r["note"] = "nearest beat is more than max_shift away: left alone";
            report << r;
            continue;
        }
        int got = 0;
        const int want = target - t;
        // How far the clip really moved (a locked track does not move: moveClips leaves it as it is)
        const auto moved = [&](int start) {
            const Clip* c = TimelineOps::findClip(s.project.timeline(), id);
            return c ? c->start - start : 0;
        };
        const int startBefore = TimelineOps::findClip(s.project.timeline(), id)->start;
        if (how == "move") {
            const QVector<int> ids = s.editor.withFollowers({id});
            s.editor.moveClips(ids, want, TrackKind::Video, 0);
            got = moved(startBefore);
        } else {
            const bool roll = how == "roll";
            const TimelineOps::TrimEdit e = s.editor.trimEdit(roll ? TimelineOps::TrimKind::Roll : TimelineOps::TrimKind::Slip,
                                                               id, TimelineOps::Edge::Start);
            if (roll && (e.isNull() || e.rightIds.isEmpty() || e.ids.isEmpty())) {
                // No clip right before it: the clip moves
                s.editor.moveClips(s.editor.withFollowers({id}), want, TrackKind::Video, 0);
                got = moved(startBefore);
                r["how"] = "move";
            } else if (!e.isNull()) {
                // Slip: the content moves the other way (by > 0 = later material shows here)
                const int delta = roll ? want : -want;
                const Clip before = *TimelineOps::findClip(s.project.timeline(), id);
                const int d = s.editor.clampTrimEdit(e, delta);
                if (d != 0) s.editor.applyTrimEdit(e, d);
                got = roll ? d : -d;
                // Animations (accents on the hit) belong to the content: they go with it
                if (!roll && d != 0 && !before.keys.isEmpty())
                    s.editor.modifyClips({id}, "Snap to beat", [&](Clip& c) { c.keys = before.keys; });
            }
        }
        if (event == "marker" && got != 0) {
            QVector<int> markers = s.project.timeline().markers;
            if (markers.contains(t)) s.editor.toggleMarker(t);
            if (!s.project.timeline().markers.contains(t + got)) s.editor.toggleMarker(t + got);
        }
        r["to"] = tc(t + got, f);
        r["shift_frames"] = got;
        if (got == 0 && TimelineOps::isLocked(s.project.timeline(), id))
            r["note"] = "on a locked track: left as it is";
        else if (got != want)
            r["note"] = QString("not enough material or room to move it all the way (%1 frames off)").arg(want - got);
        report << r;
    }
    s.notes << QJsonObject{{"snap_to_beat", report}};
}

// Beat analyses an op needs, measured here (`_beats`, key per song): the song of fit_music, the music of snap_to_beat
void addBeats(Session* s, const QJsonObject& op, QJsonObject* beats, Context& ctx)
{
    const QString name = op.value("op").toString();
    if (name == "fit_music") {
        if (op.contains("media")) {
            const MediaInfo m = s ? s->media(op.value("media").toString()) : MediaInfo{};
            if (!m.path.isEmpty()) {
                const int stream = op.value("stream").toInt(0);
                (*beats)[beatsKey(m.path, stream)] = beatsJson(songBeats(s->format(), m, stream, {}, &ctx));
            }
        } else if (s) {
            const Clip& c = clipOf(*s, op);
            const MediaInfo& m = s->media(c.mediaPath);
            const int stream = op.contains("stream") ? op.value("stream").toInt() : c.audioStream;
            (*beats)[beatsKey(m.path, stream)] = beatsJson(songBeats(s->format(), m, stream, {}, &ctx));
        }
    } else if (name == "snap_to_beat" && s) {
        for (const Clip* c : musicClips(*s, op)) {
            const MediaInfo& m = s->media(c->mediaPath);
            (*beats)[beatsKey(m.path, c->audioStream)] = beatsJson(songBeats(s->format(), m, c->audioStream, {}, &ctx));
        }
    }
}

} // namespace

QJsonObject prepareMusic(const QJsonObject& editArgs, Context& ctx)
{
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    bool any = false;
    std::unique_ptr<Session> s;
    for (int i = 0; i < ops.size(); ++i) {
        QJsonObject op = ops[i].toObject();
        const QString name = op.value("op").toString();
        if ((name != "fit_music" && name != "snap_to_beat") || op.contains("_beats")) continue;
        try {
            if (!s) {
                s = std::make_unique<Session>(editArgs.value("project").toString()); // a copy of what the app shows
                s->selectTimeline(editArgs.value("timeline"));
            }
            QJsonObject beats;
            addBeats(s.get(), op, &beats, ctx);
            if (beats.isEmpty()) continue;
            op["_beats"] = beats;
        } catch (const Error&) {
            continue; // the op reports it where it runs
        }
        ops[i] = op;
        any = true;
    }
    if (!any) return editArgs;
    QJsonObject out = editArgs;
    out["ops"] = ops;
    return out;
}

void addMusicOps(QVector<OpDef>& ops)
{
    ops << OpDef{"fit_music",
                 "{op:'fit_music', clip|media, length, end?:'natural'|'fade', keep?:'drop', climax_at?, crossfade?, at?, "
                 "track?, stream?}",
                 "cut a song to a length on its phrases, with its real ending (beats analysed once per file)",
                 "Whole phrases (if need be bars) come out of the middle; the parts join on bar ones where the sound "
                 "around the cut is alike (level and bass of the bars, as `beats` measures them), with a short "
                 "equal-power crossfade (crossfade: per cut, default about 40 ms, two beats where the sound changes). "
                 "The start and the song's own end (its last phrase and what rings out after it) stay; what is left "
                 "below a bar is taken off before the first bar one (silence, pick-up). length: a time (\"30s\") or "
                 "'timeline' (the end of the other clips). clip = the song's audio clip on the timeline (replaced in "
                 "place by its parts), media = a file (placed at `at`, default 0, on `track` or a free audio track). "
                 "end:'fade' plays the song from the start and fades it out at the length instead. keep:'drop' makes "
                 "sure the strongest drop stays; climax_at = where in the result it should land (a second cut if "
                 "needed). The answer's notes give fit_music {segments:[{from_s, to_s}] (song times that play), "
                 "cuts_at (seconds into the result), cuts (bars, phrase:true = whole phrases removed, level_db, "
                 "crossfade_ms), residual_ms (length minus the target, below a frame), drop_at, start_trim_s, clips}. "
                 "Then snap_to_beat puts the cuts of the video on its beats.",
                 opFitMusic, {"_beats"}}
        << OpDef{"snap_to_beat",
                 "{op:'snap_to_beat', clips?, event?:'start'|'hit'|'marker', grid?:'beat'|'bar'|'phrase', max_shift?, "
                 "how?:'roll'|'slip'|'move', music?, events?}",
                 "put cuts (or hits, markers) on the nearest beat of the music on the timeline",
                 "The grid comes from the beats of the music clips (music: an audio clip id or track; default the "
                 "audio track whose clips without picture play longest; a song cut by fit_music keeps its grid). "
                 "event 'start' (default): the cuts at the starts of the clips (default every video clip after frame "
                 "0) move onto the grid, how 'roll' (default: the clip before gets longer or shorter, nothing else "
                 "moves) or 'move'. event 'hit': events = the hit_s of `highlights` ({media, source} or timeline "
                 "times), the clip's content slips until the hit lies on the beat (how 'slip', default; animations "
                 "such as accents go with it) or the clip moves (how 'move'). event 'marker': the same for the "
                 "timeline markers on video clips (the marker moves along). max_shift (default 0.25 s): farther "
                 "beats leave the moment alone. The answer's notes list every moment with its beat and shift.",
                 opSnapToBeat, {"_beats", "clip", "linked"}};
}

} // namespace Cli::detail
