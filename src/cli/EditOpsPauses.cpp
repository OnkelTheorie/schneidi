// SPDX-License-Identifier: AGPL-3.0-only
// Edit op cut_pauses (0.25, the recipe "talk"): the pauses where every chosen track is quiet, and optionally filler
// words, cut out with ripple over all tracks in one undo step. Measuring per track (core/PauseCut decides), cached per
// track content in engine/AnalysisCache; live the measuring runs here in schneidi-cli (preparePauses), the app cuts.
#include "cli/AnalysisDetail.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/I18n.h"
#include "core/PauseCut.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "core/Transcript.h"
#include "core/TranscriptCut.h"
#include "engine/AnalysisCache.h"
#include "engine/AudioAnalysis.h"
#include "engine/FileStamp.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

constexpr int kPeaksVersion = 1; // raise when the per-track peaks are measured differently

double r2(double v) { return std::round(v * 100) / 100; }
double r1(double v) { return std::round(v * 10) / 10; }

// The audio tracks to listen to: `tracks`, else the voice tracks (when there is more to hear), else every audible
// track with clips
QVector<int> chosenTracks(Session& s, const Timeline& tl, const QJsonObject& op)
{
    if (op.contains("tracks")) {
        const QJsonValue v = op.value("tracks");
        QStringList names;
        if (v.isArray())
            for (const QJsonValue& x : v.toArray()) names << x.toVariant().toString();
        else names << v.toVariant().toString();
        const QVector<int> out = audioTracksOf(s.project, tl, names.join(','));
        if (out.isEmpty()) fail("BAD_ARGUMENT", "tracks: the audio tracks to listen to (\"A1,A2\" or names)");
        return out;
    }
    QVector<int> out = Transcript::voiceTracks(tl, [&s](const Clip& c) {
        const MediaInfo* m = c.mediaPath.isEmpty() ? nullptr : s.project.mediaInfo(c.mediaPath);
        return m && m->audioStreamCount() > 1 ? m->audioStreamName(c.audioStream) : QString();
    });
    if (out.isEmpty())
        for (int i = 0; i < tl.audio.size(); ++i)
            if (!tl.audio[i].muted && !tl.audio[i].clips.isEmpty()) out << i;
    if (out.isEmpty()) fail("NO_AUDIO", "the timeline has no audible sound clips to find pauses in");
    return out;
}

// The cache entry of one track's peaks: the track alone (everything else of the timeline that changes its sound -
// master, format - included, other tracks' clips left out) and every file's size and time
AnalysisCache::Key peaksKey(const ProjectFormat& f, const Timeline& alone, int track)
{
    const Track& t = alone.audio[track];
    if (t.clips.isEmpty()) return {};
    QJsonArray stamps;
    for (const Clip& c : t.clips) {
        if (c.kind == ClipKind::Compound || c.kind == ClipKind::Multicam) return {}; // nested content is not in the key
        if (c.mediaPath.isEmpty()) continue;
        const FileStamp::Stamp st = FileStamp::now(c.mediaPath);
        stamps << QJsonArray{c.mediaPath, double(st.size), double(st.modified)};
    }
    ProjectData d;
    d.format = f;
    d.timeline = alone;
    const QByteArray hash = QCryptographicHash::hash(ProjectFile::toJson(d, {}), QCryptographicHash::Sha1).toHex();
    const QString file = t.clips.first().mediaPath;
    if (file.isEmpty()) return {};
    return AnalysisCache::key("pause-peaks", file, QJsonObject{{"track", QString::fromLatin1(hash)}, {"files", stamps}},
                              kPeaksVersion);
}

struct TrackPeaks {
    int track = 0;
    std::vector<float> peaks; // timeline frames from 0
    double thresholdDb = 0;
    bool cached = false;
};

// One track's sample peak per frame, the others muted
TrackPeaks measureTrack(const ProjectFormat& f, const Timeline& render, int track,
                        const std::function<void(double)>& report)
{
    Timeline alone = render;
    for (Track& v : alone.video) v.clips.clear();
    alone.subtitles.clear();
    alone.markers.clear();
    alone.notes.clear();
    alone.sections.clear();
    alone.markIn = alone.markOut = -1;
    for (int i = 0; i < alone.audio.size(); ++i) {
        if (i != track) alone.audio[i].clips.clear();
        alone.audio[i].muted = i != track;
        alone.audio[i].solo = false;
    }
    TrackPeaks out;
    out.track = track;
    const int end = TimelineOps::endFrame(alone);
    if (end <= 0) return out;
    const AnalysisCache::Key key = peaksKey(f, alone, track);
    if (key.valid())
        if (const auto hit = AnalysisCache::get(key)) {
            out.peaks = AnalysisCache::unpackFloats(hit->value("peaks"));
            if (int(out.peaks.size()) == end) {
                out.cached = true;
                return out;
            }
        }
    const auto peaks = AudioAnalysis::timelineFramePeaks(f, alone, 0, end, goOn(report));
    failIfCanceled();
    if (!peaks) fail("DECODE_FAILED", QString("could not mix the sound of A%1").arg(track + 1));
    out.peaks = *peaks;
    if (key.valid()) AnalysisCache::put(key, QJsonObject{{"peaks", AnalysisCache::pack(out.peaks)}});
    return out;
}

struct Analysis {
    QVector<PauseCut::Range> ranges;
    QVector<int> tracks;
    QVector<double> thresholds;
    int cached = 0;
    bool fillers = false;
    int fillerWords = -1; // words looked at for fillers; 0 = no transcript
};

QJsonObject analysisJson(const Analysis& a)
{
    QJsonArray ranges, tracks;
    for (const PauseCut::Range& r : a.ranges) {
        QJsonArray tr;
        for (int t : r.tracks) tr << t;
        QJsonObject o{{"from", r.from}, {"to", r.to}, {"kind", PauseCut::kindId(r.kind)}, {"tracks", tr}};
        if (!r.text.isEmpty()) o["text"] = r.text;
        ranges << o;
    }
    for (int i = 0; i < a.tracks.size(); ++i) tracks << QJsonObject{{"track", a.tracks[i]}, {"threshold_db", a.thresholds.value(i)}};
    return {{"ranges", ranges}, {"tracks", tracks}, {"cached", a.cached}, {"fillers", a.fillers},
            {"filler_words", a.fillerWords}};
}

Analysis analysisFromJson(const QJsonObject& o)
{
    Analysis a;
    for (const QJsonValue& v : o.value("ranges").toArray()) {
        const QJsonObject r = v.toObject();
        PauseCut::Range x;
        x.from = r.value("from").toInt();
        x.to = r.value("to").toInt();
        x.kind = r.value("kind").toString() == "filler" ? PauseCut::Kind::Filler : PauseCut::Kind::Pause;
        for (const QJsonValue& t : r.value("tracks").toArray()) x.tracks << t.toInt();
        x.text = r.value("text").toString();
        a.ranges << x;
    }
    for (const QJsonValue& v : o.value("tracks").toArray()) {
        a.tracks << v.toObject().value("track").toInt();
        a.thresholds << v.toObject().value("threshold_db").toDouble();
    }
    a.cached = o.value("cached").toInt();
    a.fillers = o.value("fillers").toBool();
    a.fillerWords = o.value("filler_words").toInt(-1);
    return a;
}

int framesOf(Session& s, const QJsonObject& op, const char* key, double fallbackS)
{
    if (!op.contains(key)) return int(std::lround(fallbackS * s.format().rate.fps()));
    const int v = parseTime(op.value(key), s.format(), key);
    if (v < 0) fail("BAD_ARGUMENT", QString("'%1' must not be negative").arg(key));
    return v;
}

// Measuring: the quiet of every chosen track, the pauses where all are quiet, the fillers
Analysis analyse(Session& s, const QJsonObject& op, const std::function<void(double)>& report)
{
    const Timeline& tl = s.project.timeline();
    const ProjectFormat f = s.format();
    Analysis a;
    a.tracks = chosenTracks(s, tl, op);
    const int minPause = std::max(1, framesOf(s, op, "min_pause", 0.6));
    const int pad = framesOf(s, op, "pad", 0.15);
    std::optional<double> fixedDb;
    if (op.contains("threshold")) {
        const QJsonValue t = op.value("threshold");
        if (t.isDouble()) fixedDb = t.toDouble();
        else if (t.toString().trimmed().toLower() != "auto") {
            bool ok = false;
            const double db = t.toString().trimmed().remove("db", Qt::CaseInsensitive).toDouble(&ok);
            if (!ok) fail("BAD_ARGUMENT", "threshold: 'auto' or a level in dBFS like -40");
            fixedDb = db;
        }
        if (fixedDb && (*fixedDb > 0 || *fixedDb < -120)) fail("BAD_ARGUMENT", "threshold: a level in dBFS from -120 to 0");
    }
    // Where the conversation is: from the first to the last sound clip of the chosen tracks (a picture before or after
    // it is no pause)
    int lo = INT_MAX, hi = 0;
    for (int t : a.tracks)
        for (const Clip& c : tl.audio[t].clips) {
            lo = std::min(lo, c.start);
            hi = std::max(hi, c.end());
        }
    if (hi <= lo) fail("NO_AUDIO", "the chosen tracks have no sound clips");
    const Timeline render = s.project.renderTimeline();
    QVector<std::vector<bool>> quiet;
    QVector<std::vector<float>> peaks;
    for (int i = 0; i < a.tracks.size(); ++i) {
        const int n = int(a.tracks.size());
        TrackPeaks p = measureTrack(f, render, a.tracks[i], [&report, i, n](double x) {
            if (report) report((i + x) / n);
        });
        if (p.cached) ++a.cached;
        p.thresholdDb = fixedDb ? *fixedDb : PauseCut::autoThresholdDb(p.peaks);
        a.thresholds << r1(p.thresholdDb);
        // The conversation's range only, frame `lo` = index 0
        std::vector<float> part(size_t(hi - lo), 0.f);
        for (int k = lo; k < hi && size_t(k) < p.peaks.size(); ++k) part[size_t(k - lo)] = p.peaks[size_t(k)];
        quiet << PauseCut::quietFrames(part, p.thresholdDb, hi - lo);
        peaks << std::move(part);
    }
    QVector<PauseCut::Range> pauses = PauseCut::pauses(quiet, a.tracks, minPause, pad);
    QVector<PauseCut::Range> fillers;
    a.fillers = op.value("fillers").toBool();
    if (a.fillers) {
        QVector<TranscriptCut::Word> words = TranscriptCut::timelineWords(tl, s.project.media(), f.rate.fps());
        words.erase(std::remove_if(words.begin(), words.end(), [&](const TranscriptCut::Word& w) {
                        return !a.tracks.contains(w.track) || w.from < lo || w.from >= hi;
                    }),
                    words.end());
        a.fillerWords = int(words.size());
        for (TranscriptCut::Word& w : words) { // into the measured range's frames
            w.from -= lo;
            w.to -= lo;
            w.spokenTo -= lo;
        }
        fillers = PauseCut::fillers(words, peaks, a.thresholds, a.tracks);
    }
    a.ranges = PauseCut::combined(pauses, fillers);
    for (PauseCut::Range& r : a.ranges) {
        r.from += lo;
        r.to += lo;
    }
    return a;
}

QJsonArray trackNames(const QVector<int>& tracks)
{
    QJsonArray out;
    for (int t : tracks) out << QString("A%1").arg(t + 1);
    return out;
}

// A locked track keeps its content where it is while the others move up: the sound would slip
void failLocked(const Timeline& tl, const QVector<PauseCut::Range>& ranges, const ProjectFormat& f)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i) {
            const Track& t = tl.tracks(k)[i];
            if (!t.locked) continue;
            for (const Clip& c : t.clips)
                for (const PauseCut::Range& r : ranges)
                    if (c.start < r.to && r.from < c.end())
                        fail("TRACK_LOCKED", QString("%1%2 is locked and has a clip in the pause at %3 - unlock it (or "
                                                     "leave that range out), else its content would slip out of sync")
                                                 .arg(k == TrackKind::Video ? "V" : "A")
                                                 .arg(i + 1)
                                                 .arg(tc(r.from, f)));
        }
}

void opCutPauses(Session& s, const QJsonObject& op)
{
    const ProjectFormat f = s.format();
    const Timeline& tl = s.project.timeline();
    if (TimelineOps::endFrame(tl) <= 0) s.failEmpty();
    Analysis a;
    bool analysed = false;
    if (op.contains("ranges")) {
        // Only these (a choice of the ranges an earlier run listed): nothing is measured
        const QJsonArray list = op.value("ranges").toArray();
        if (list.isEmpty() && !op.value("ranges").isArray())
            fail("BAD_ARGUMENT", "ranges: [[from, to], ...] (timeline times)");
        for (const QJsonValue& v : list) {
            const QJsonArray pair = v.toArray();
            if (pair.size() != 2) fail("BAD_ARGUMENT", "ranges: each one is [from, to]");
            PauseCut::Range r;
            r.from = parseTime(pair[0], f, "ranges");
            r.to = parseTime(pair[1], f, "ranges");
            if (r.to <= r.from) fail("BAD_ARGUMENT", QString("ranges: %1 must end after it starts").arg(tc(r.from, f)));
            a.ranges << r;
        }
        std::sort(a.ranges.begin(), a.ranges.end(), [](const PauseCut::Range& x, const PauseCut::Range& y) { return x.from < y.from; });
    } else if (op.contains("_pauses")) {
        a = analysisFromJson(op.value("_pauses").toObject()); // measured by schneidi-cli (preparePauses)
        analysed = true;
    } else {
        a = analyse(s, op, {});
        analysed = true;
    }
    QJsonObject note;
    if (analysed) {
        QJsonArray tracks;
        for (int i = 0; i < a.tracks.size(); ++i)
            tracks << QJsonObject{{"track", QString("A%1").arg(a.tracks[i] + 1)}, {"threshold_db", a.thresholds.value(i)}};
        note["tracks"] = tracks;
        if (a.cached > 0) note["cached"] = a.cached;
    }
    QJsonArray list;
    int removed = 0;
    for (int i = 0; i < a.ranges.size(); ++i) {
        const PauseCut::Range& r = a.ranges[i];
        QJsonObject o{{"n", i + 1}, {"from", r.from}, {"to", r.to}, {"from_s", r2(seconds(r.from, f))},
                      {"to_s", r2(seconds(r.to, f))}, {"length_s", r2(seconds(r.to - r.from, f))}};
        if (analysed) {
            o["kind"] = PauseCut::kindId(r.kind);
            o["tracks"] = trackNames(r.tracks);
        }
        if (!r.text.isEmpty()) o["text"] = r.text;
        list << o;
        removed += r.to - r.from;
    }
    note["ranges"] = list;
    note["cut"] = int(a.ranges.size());
    note["removed_s"] = r2(seconds(removed, f));
    if (a.fillers && a.fillerWords == 0)
        note["fillers"] = "no transcript on these tracks: transcribe them first (transcribe verbatim:true writes the "
                          "fillers whisper otherwise leaves out); only pauses were cut";
    else if (a.fillers)
        note["fillers"] = "only fillers the transcript contains (transcribe verbatim:true writes more of them)";
    if (a.ranges.isEmpty()) {
        note["note"] = analysed ? "no pause long enough where every chosen track is quiet (try a shorter min_pause)"
                                : "no ranges given";
        s.notes << QJsonObject{{"cut_pauses", note}};
        return;
    }
    failLocked(tl, a.ranges, f);
    QVector<QPair<int, int>> pairs;
    for (const PauseCut::Range& r : a.ranges) pairs << qMakePair(r.from, r.to);
    const int before = TimelineOps::endFrame(tl);
    s.editor.deleteRanges(pairs, true, T("Pausen schneiden"), TimelineOps::MarkerCut::MoveToCut);
    note["length_before_s"] = r2(seconds(before, f));
    note["length_after_s"] = r2(seconds(TimelineOps::endFrame(s.project.timeline()), f));
    s.notes << QJsonObject{{"cut_pauses", note}};
}

} // namespace

QJsonObject preparePauses(const QJsonObject& editArgs, Context& ctx)
{
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    // Only a first op measures what the app shows now (a later one would see the edits before it: the app measures it)
    if (ops.isEmpty()) return editArgs;
    QJsonObject op = ops[0].toObject();
    if (op.value("op").toString() != "cut_pauses" || op.contains("ranges") || op.contains("_pauses")) return editArgs;
    try {
        Session s(editArgs.value("project").toString()); // a copy of what the app shows
        s.selectTimeline(editArgs.value("timeline"));
        if (ctx.stage) ctx.stage("cut_pauses");
        const Analysis a = analyse(s, op, ctx.progress);
        op["_pauses"] = analysisJson(a);
    } catch (const Error& e) {
        if (e.code == "CANCELED") throw;
        return editArgs; // the op reports it where it runs
    }
    ops[0] = op;
    QJsonObject out = editArgs;
    out["ops"] = ops;
    return out;
}

void addPauseOps(QVector<OpDef>& ops)
{
    ops << OpDef{"cut_pauses",
                 "{op:'cut_pauses', tracks?, min_pause?:'0.6s', pad?:'0.15s', fillers?:false, threshold?:'auto', "
                 "ranges?:[[from, to], ...]}",
                 "a conversation without pauses: cut out where every chosen track is quiet (and filler words), with "
                 "ripple over all tracks so they stay in sync (one undo step)",
                 "tracks = the audio tracks to listen to (\"A1,A2\" or names; default: the voice tracks when there is "
                 "music or game sound too, else every audible track with clips). A pause counts only where EVERY one "
                 "of them is quiet at least min_pause long; pad of quiet stays at both ends of it. Each track is "
                 "measured alone against its own threshold (threshold 'auto': its noise floor - 10th percentile of "
                 "its frames' peaks - plus 8 dB, between -60 and -35 dBFS, so a noisy microphone does not hold up "
                 "every cut; a number = that level in dBFS for all). Only between the first and the last sound clip of "
                 "those tracks. fillers:true also cuts hesitation sounds (äh, ähm, uh, um, euh, eee …) of the "
                 "transcripts while the other tracks are quiet - only as good as the transcript: whisper leaves most "
                 "of them out, transcribe with verbatim:true first. Without ranges the op measures and cuts everything "
                 "it finds; ranges = cut exactly these (timeline times, as an earlier run listed them - leave out "
                 "the ones to keep). Markers in a pause move to the cut, subtitles follow; a locked track with a clip "
                 "in a range = error TRACK_LOCKED. The answer's notes give cut_pauses {ranges:[{n, kind:'pause'|"
                 "'filler', from, to, from_s, to_s, length_s, tracks, text?}] (timeline frames before the cut), cut, "
                 "removed_s, tracks:[{track, threshold_db}], length_before_s, length_after_s}. To look first: "
                 "dry_run:true or plan:true.",
                 opCutPauses, {"_pauses"}};
}

} // namespace Cli::detail
