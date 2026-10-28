// schneidi-cli: looking at the sound of the material (silence, peaks, loudness, transcribe)
#include "cli/AnalysisDetail.h"

#include "core/Loudness.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "core/Transcript.h"
#include "engine/AudioAnalysis.h"
#include "engine/Extensions.h"
#include "engine/Transcriber.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

namespace Cli::detail {

LoudPeaks loudPeaks(const MediaSource& m, const QJsonObject& a, const std::function<void(double)>& progress)
{
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    c.audioStream = a["audio_stream"].toInt();
    if (c.audioStream >= m.info.audioStreamCount())
        fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, [&progress](double p) {
        if (progress) progress(p);
        return true;
    });
    if (!peaks || peaks->empty()) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    std::vector<double> db(peaks->size());
    for (size_t i = 0; i < db.size(); ++i) db[i] = 20 * std::log10(std::max(1e-6f, (*peaks)[i]));
    LoudPeaks out;
    out.db = db;
    // Default threshold: well above the file's typical level (median), so it adapts to quiet recordings
    std::vector<double> sorted = db;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    out.medianDb = sorted[sorted.size() / 2];
    out.thresholdDb = a.contains("threshold_db") ? a["threshold_db"].toDouble() : std::min(-1.0, out.medianDb + 12);
    const int minGap = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
    QVector<Peak>& hits = out.peaks;
    for (int i = 0; i < int(db.size()); ++i) {
        if (db[i] < out.thresholdDb) continue;
        if (!hits.isEmpty() && i - hits.last().frame < minGap) {
            if (db[i] > hits.last().db) hits.last() = {i, db[i]}; // the loudest frame of a burst counts
            continue;
        }
        hits << Peak{i, db[i]};
    }
    return out;
}

QVector<PeakGroup> groupPeaks(const QVector<Peak>& peaks, int gap)
{
    QVector<PeakGroup> out;
    for (int i = 0; i < peaks.size();) {
        int j = i;
        double loudest = peaks[i].db;
        while (j + 1 < peaks.size() && peaks[j + 1].frame - peaks[j].frame < gap) loudest = std::max(loudest, peaks[++j].db);
        out << PeakGroup{peaks[i].frame, peaks[j].frame + 1, j - i + 1, loudest};
        i = j + 1;
    }
    return out;
}

namespace {
// Pauses in per-frame peaks: quiet runs of at least minLen frames; the pad of sound stays on both sides (not at
// the start/end). Ranges are shifted by `offset`.
QJsonObject silenceJson(const std::vector<float>& peaks, int offset, double thresholdDb, int minLen, int pad,
                        const ProjectFormat& f)
{
    const float limit = float(std::pow(10.0, thresholdDb / 20.0));
    const int n = int(peaks.size());
    QJsonArray silence, sound;
    int keepFrom = 0, silent = 0;
    for (int i = 0; i <= n; ++i) {
        if (i < n && peaks[i] < limit) {
            ++silent;
            continue;
        }
        if (silent >= minLen) {
            const int from = i - silent, to = i;
            const int cutFrom = from == 0 ? 0 : std::min(to, from + pad);
            const int cutTo = to == n ? n : std::max(cutFrom, to - pad);
            if (cutTo > cutFrom) {
                silence << range(offset + cutFrom, offset + cutTo, f);
                if (cutFrom > keepFrom) sound << range(offset + keepFrom, offset + cutFrom, f);
                keepFrom = cutTo;
            }
        }
        silent = 0;
    }
    if (keepFrom < n) sound << range(offset + keepFrom, offset + n, f);
    int kept = 0;
    for (const QJsonValue& r : sound) kept += r["to"].toInt() - r["from"].toInt();
    return {{"frames", n}, {"fps", formatJson(f)["fps"]}, {"threshold_db", thresholdDb}, {"silence", silence},
            {"sound", sound}, {"kept_frames", kept}, {"kept_s", seconds(kept, f)}, {"removed_s", seconds(n - kept, f)}};
}

// Loud moments of one file: local maxima of the per-frame peak above the threshold, at least `min` apart
QJsonObject peaksOne(const QJsonObject& a, Context& ctx)
{
    const MediaSource m = openMedia(a);
    const ProjectFormat& f = m.format;
    LoudPeaks lp = loudPeaks(m, a, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
    });
    QVector<Peak>& hits = lp.peaks;
    if (a.contains("top")) {
        const int top = std::max(1, a["top"].toInt());
        std::sort(hits.begin(), hits.end(), [](const Peak& x, const Peak& y) { return x.db > y.db; });
        if (hits.size() > top) hits.resize(top);
        std::sort(hits.begin(), hits.end(), [](const Peak& x, const Peak& y) { return x.frame < y.frame; });
    }
    QJsonArray list;
    for (const Peak& h : hits)
        list << QJsonObject{{"frame", h.frame}, {"s", seconds(h.frame, f)}, {"tc", tc(h.frame, f)},
                            {"peak_db", std::round(h.db * 10) / 10}};
    QJsonObject out{{"file", m.info.path}, {"frames", m.info.length}, {"fps", formatJson(f)["fps"]},
                    {"median_db", std::round(lp.medianDb * 10) / 10}, {"threshold_db", std::round(lp.thresholdDb * 10) / 10},
                    {"peaks", list}};
    if (a.contains("group")) {
        const int gap = parseTime(a["group"], f, "group");
        if (gap <= 0) fail("BAD_ARGUMENT", "'group' must be > 0");
        QJsonArray groups;
        for (const PeakGroup& pg : groupPeaks(hits, gap)) {
            QJsonObject g = range(pg.from, pg.to, f);
            g["hits"] = pg.hits;
            g["peak_db"] = std::round(pg.peakDb * 10) / 10;
            g["per_s"] = std::round(pg.hits / std::max(1.0, seconds(pg.to - pg.from, f)) * 10) / 10;
            groups << g;
        }
        out["groups"] = groups;
    }
    return out;
}
} // namespace

QJsonObject cmdSilence(const QJsonObject& a, Context& ctx)
{
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    };
    const auto settings = [&a](const ProjectFormat& f) {
        const double thresholdDb = a.contains("threshold_db") ? a["threshold_db"].toDouble() : -35.0;
        const int minLen = a.contains("min") ? parseTime(a["min"], f, "min") : int(std::lround(0.5 * f.rate.fps()));
        const int pad = a.contains("pad") ? parseTime(a["pad"], f, "pad") : int(std::lround(0.1 * f.rate.fps()));
        return std::tuple{thresholdDb, minLen, pad};
    };
    const QString file = a["file"].toString();
    if (isProjectFile(file)) {
        // The timeline's sound as exported (optionally some tracks); ranges in timeline frames, ready for keep
        Session s(file);
        s.selectTimeline(a["timeline"]);
        const ProjectFormat& f = s.format();
        Timeline tl = s.project.renderTimeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) s.failEmpty();
        const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        const QVector<int> only = audioTracksOf(s.project, tl, a["tracks"]);
        if (!only.isEmpty())
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = !only.contains(i);
                tl.audio[i].solo = false;
            }
        const auto peaks = AudioAnalysis::timelineFramePeaks(f, tl, from, to, progress);
        if (!peaks) fail("DECODE_FAILED", "could not mix the sound");
        const auto [thresholdDb, minLen, pad] = settings(f);
        QJsonObject o{{"project", s.path.isEmpty() ? absolute(file) : s.path}, {"range", range(from, to, f)}};
        const QJsonObject r = silenceJson(*peaks, from, thresholdDb, minLen, pad, f);
        for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = it.value();
        return o;
    }
    const MediaSource m = openMedia(a);
    if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
    const ProjectFormat& f = m.format;
    Clip c;
    c.mediaPath = m.info.path;
    c.out = m.info.length - 1;
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c, progress);
    if (!peaks) fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
    const auto [thresholdDb, minLen, pad] = settings(f);
    QJsonObject o{{"file", m.info.path}};
    const QJsonObject r = silenceJson(*peaks, 0, thresholdDb, minLen, pad, f);
    for (auto it = r.begin(); it != r.end(); ++it) o[it.key()] = it.value();
    return o;
}

QJsonObject cmdPeaks(const QJsonObject& a, Context& ctx) { return perFile(a, ctx, peaksOne); }

namespace {

double db1(double v) { return v <= LoudnessMeter::kSilence ? -200.0 : std::round(v * 10) / 10; }

QJsonObject loudnessJson(const AudioAnalysis::Loudness& l)
{
    return {{"integrated_lufs", db1(l.integrated)}, {"range_lu", db1(l.range)},
            {"true_peak_db", db1(l.truePeakDb)}, {"sample_peak_db", db1(l.peakDb)},
            {"max_momentary_lufs", db1(l.maxMomentary)}, {"max_short_term_lufs", db1(l.maxShortTerm)}};
}
} // namespace

QJsonObject cmdLoudness(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
        return true;
    };
    QJsonObject result{{"source", absolute(src)}};
    if (!isProjectFile(src)) {
        QJsonObject ma{{"file", src}};
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "the file has no sound");
        Clip c;
        c.mediaPath = m.info.path;
        c.out = m.info.length - 1;
        c.audioStream = a["audio_stream"].toInt();
        if (c.audioStream >= m.info.audioStreamCount()) fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
        const auto l = AudioAnalysis::clipLoudness(m.format, c, progress);
        if (!l) fail("DECODE_FAILED", "could not read the sound");
        result["loudness"] = loudnessJson(*l);
        return result;
    }
    Session s(src);
    s.selectTimeline(a["timeline"]);
    const ProjectFormat& f = s.format();
    const QJsonArray clips = a["clips"].toArray();
    if (!clips.isEmpty()) {
        // Every clip on its own (its sound without volume/fades, what 'normalize' measures)
        QJsonArray list;
        for (int i = 0; i < clips.size(); ++i) {
            const Clip* c = TimelineOps::findClip(s.project.timeline(), clips[i].toVariant().toInt());
            if (!c) fail("NOT_FOUND", "no clip with id " + clips[i].toVariant().toString());
            QJsonObject o{{"clip", c->id}};
            if (const auto l = AudioAnalysis::clipLoudness(f, *c)) o["loudness"] = loudnessJson(*l);
            else o["no_sound"] = true;
            list << o;
            progress(double(i + 1) / clips.size());
        }
        result["clips"] = list;
        return result;
    }
    // The mix as exported, optionally only some audio tracks
    Timeline tl = s.project.renderTimeline();
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) s.failEmpty();
    const int from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
    const int to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
    const QVector<int> only = audioTracksOf(s.project, tl, a["tracks"]);
    if (!only.isEmpty())
        for (int i = 0; i < tl.audio.size(); ++i) {
            tl.audio[i].muted = !only.contains(i);
            tl.audio[i].solo = false;
        }
    const auto l = AudioAnalysis::timelineLoudness(f, tl, from, to, progress);
    if (!l) fail("DECODE_FAILED", "could not mix the sound");
    result["range"] = range(from, to, f);
    result["loudness"] = loudnessJson(*l);
    return result;
}

QJsonObject cmdTranscribe(const QJsonObject& a, Context& ctx)
{
    const QString src = a["source"].toString();
    TranscribeRequest req;
    QString model = a["model"].toString();
    // A model file (not the name of an installed model like "small") is a path like any other (a server's work folder:
    // checked before looking whether it exists, so an answer never tells about files outside it)
    if (model.contains('/') || model.contains('\\') || QFileInfo(model).suffix().compare("bin", Qt::CaseInsensitive) == 0)
        model = absolute(model);
    req.model = Extensions::whisperModelPath(model);
    if (req.model.isEmpty())
        fail("NOT_INSTALLED", a.contains("model") ? "whisper model not installed: " + a["model"].toString()
                                                  : "no whisper model installed (see `extensions`, e.g. --install whisper,small)");
    if (Extensions::whisperProgram().isEmpty())
        fail("NOT_INSTALLED", "whisper is not installed (see `extensions`, e.g. --install whisper,small)");
    req.language = a.contains("language") ? a["language"].toString() : "auto";
    req.threads = a["threads"].toInt();
    req.music = a["music"].toBool();
    ProjectFormat f;
    int offset = 0, length = 0;
    if (isProjectFile(src)) {
        // The timeline's sound as it is mixed for the export; times are timeline frames
        Session s(src);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        req.timeline = s.project.renderTimeline();
        req.format = f;
        const int end = TimelineOps::endFrame(req.timeline);
        if (end <= 0) s.failEmpty();
        req.from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        req.to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (req.to <= req.from) fail("BAD_ARGUMENT", "'to' must be after 'from'");
        req.audioTracks = audioTracksOf(s.project, req.timeline, a["tracks"]);
        offset = req.from;
        length = req.to - req.from;
    } else {
        QJsonObject ma{{"file", src}};
        if (a.contains("project")) ma["project"] = a["project"];
        const MediaSource m = openMedia(ma);
        if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
        f = m.format;
        req.media = m.info.path;
        req.audioStream = std::clamp(a["audio_stream"].toInt(), 0, std::max(0, m.info.audioStreamCount() - 1));
        length = m.info.length;
    }

    Transcriber t;
    QEventLoop loop;
    bool ok = false;
    QString message;
    QObject::connect(&t, &Transcriber::progress, &loop, [&ctx](double p) {
        if (ctx.progress) ctx.progress(p);
    });
    QObject::connect(&t, &Transcriber::finished, &loop, [&](bool success, const QString& msg) {
        ok = success;
        message = msg;
        loop.quit();
    });
    QString error;
    if (!t.start(req, &error)) fail("TRANSCRIBE_FAILED", error);
    loop.exec();
    if (!ok) fail("TRANSCRIBE_FAILED", message);

    const Transcript::Result& r = t.result();
    const int maxChars = a.contains("max_chars") ? std::clamp(a["max_chars"].toInt(), 10, 200) : 42;
    QVector<SubtitleCue> cues = Transcript::toCues(r.words, f.rate.fps(), maxChars, 700, offset);
    // Not for music: its "loud" is measured against the file, a quieter intro of a song counts as silence (the first
    // shouted line was dropped while its words were there)
    if (!req.music) Transcript::startAtSound(cues, r.levels, f.rate.fps(), offset);
    QJsonArray segments;
    for (const SubtitleCue& c : cues) {
        QJsonObject o = range(c.start, std::min(c.end, offset + length), f);
        o["text"] = c.text;
        segments << o;
    }
    QJsonObject out{{"source", absolute(src)}, {"language", r.language}, {"fps", formatJson(f)["fps"]},
                    {"model", QFileInfo(req.model).fileName()}, {"segments", segments}};
    if (segments.isEmpty() && !req.music)
        out["notes"] = QJsonArray{"no speech found; singing or shouts in music: try again with music:true"};
    if (a["words"].toBool()) {
        // Word times for cutting: `to` is where the word is spoken to (pauses after it are left out)
        QJsonArray words;
        const auto fr = [&](qint64 ms) { return offset + int(std::lround(ms / 1000.0 * f.rate.fps())); };
        for (const Transcript::Word& w : r.words)
            words << QJsonObject{{"from", fr(w.from)}, {"to", std::max(fr(w.from) + 1, fr(Transcript::spokenEnd(w)))},
                                 {"text", w.text}};
        out["words"] = words;
    }
    if (a["apply"].toBool()) {
        // Straight onto the timeline as a new subtitle track (live in the app: one undo step there)
        if (!isProjectFile(src)) fail("BAD_ARGUMENT", "apply needs a project as source (\"@app\" or a .schneidi file)");
        QJsonArray list;
        for (const SubtitleCue& c : cues)
            list << QJsonObject{{"from", c.start}, {"to", std::min(c.end, offset + length)}, {"text", c.text}};
        QJsonObject op{{"op", "subtitles"}, {"cues", list}};
        if (a.contains("name")) op["name"] = a["name"];
        QJsonObject edit{{"project", src}, {"ops", QJsonArray{op}}};
        if (a.contains("timeline")) edit["timeline"] = a["timeline"];
        if (list.isEmpty()) fail("NO_SPEECH", "no speech found, nothing to apply");
        const QJsonObject done = execute(*find("edit"), edit, ctx);
        const QJsonArray tracks = done["timeline"]["subtitles"].toArray();
        QJsonObject applied{{"cues", list.size()}, {"saved", done["saved"].toBool()}};
        if (!tracks.isEmpty()) applied["track"] = tracks.last()["track"]; // the new one comes last
        if (done["live"].toBool()) applied["live"] = true;
        out["applied"] = applied;
    }
    if (a.contains("srt")) {
        const QString path = absolute(a["srt"].toString());
        QFile file(path);
        QVector<SubtitleCue> shifted = cues;
        if (!file.open(QIODevice::WriteOnly) || file.write(Subtitles::toSrt(shifted, f.rate.fps(), offset)) < 0)
            fail("WRITE_FAILED", "cannot write " + path);
        out["srt"] = path;
    }
    return out;
}

} // namespace Cli::detail
