// schneidi-cli: `beats` - tempo and structure of a song in one call (core/Beats): BPM, beat grid, bar ones, phrases,
// energy per bar, breaks and drops, in seconds and frames; optionally timeline markers on the phrase starts or bars
// (through `edit`, one undo step in the app). The sound is decoded like `peaks`/`silence` (engine/AudioAnalysis).
#include "cli/AnalysisDetail.h"

#include "core/Beats.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace Cli::detail {

namespace {

constexpr int kDrops = 5; // best drop candidates in the answer

double round(double v, int decimals)
{
    const double f = std::pow(10.0, decimals);
    return std::round(v * f) / f;
}

// The sound of the range as beat features: channels averaged to mono, block by block
struct Sound {
    std::optional<Beats::Features> features;
    std::vector<float> mono;
    double fps = 25;
    void add(const float* pcm, int samples, int channels, int rate)
    {
        if (!features) features.emplace(rate);
        if (!pcm) { // a frame that could not be decoded (MLT path): silence of its length
            mono.assign(size_t(std::lround(rate / fps)), 0.f);
            features->add(mono.data(), int(mono.size()));
            return;
        }
        mono.assign(size_t(samples), 0.f);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < samples; ++i) mono[size_t(i)] += pcm[size_t(c) * size_t(samples) + size_t(i)];
        for (float& s : mono) s /= float(std::max(1, channels));
        features->add(mono.data(), samples);
    }
};

// 0..9 on a 2 dB scale, 9 = within a step of the loudest bar
int level(double db, double top) { return std::clamp(int(std::lround(9 + (db - top) / 2)), 0, 9); }

double meanDb(const std::vector<double>& db, int from, int to)
{
    double sum = 0;
    for (int i = from; i < to; ++i) sum += std::pow(10.0, db[size_t(i)] / 10);
    return to > from ? 10 * std::log10(sum / (to - from) + 1e-12) : -120;
}

} // namespace

QJsonObject cmdBeats(const QJsonObject& a, Context& ctx)
{
    const QString file = a["file"].toString();
    const bool isProject = isProjectFile(file);
    // Checked before decoding (that takes a while)
    const QString apply = a["apply"].toString(), marks = a.contains("markers") ? a["markers"].toString() : "phrases";
    if (a.contains("apply") && apply != "markers")
        fail("BAD_ARGUMENT", "apply: only \"markers\" (on the phrase starts or bars)");
    if (!apply.isEmpty() && !isProject)
        fail("BAD_ARGUMENT", "apply needs a project as file (\"@app\" or a .schneidi file)");
    if (marks != "phrases" && marks != "bars") fail("BAD_ARGUMENT", "markers: phrases or bars");
    Beats::Options options;
    if (a.contains("min_bpm")) options.minBpm = a["min_bpm"].toDouble();
    if (a.contains("max_bpm")) options.maxBpm = a["max_bpm"].toDouble();
    if (options.minBpm < 30 || options.maxBpm > 400 || options.maxBpm < options.minBpm * 1.2)
        fail("BAD_ARGUMENT", "min_bpm/max_bpm: 30..400, max_bpm at least 1.2 x min_bpm");

    Sound sound;
    const auto progress = [&ctx](double p) {
        if (ctx.progress) ctx.progress(0.95 * p);
        return true;
    };
    const auto sink = [&sound](const float* pcm, int samples, int channels, int rate) {
        sound.add(pcm, samples, channels, rate);
    };
    ProjectFormat f;
    int from = 0, to = 0;
    QJsonObject out;
    if (isProject) {
        // The timeline's sound as exported (optionally some tracks); times in timeline frames
        Session s(file);
        s.selectTimeline(a["timeline"]);
        f = s.format();
        Timeline tl = s.project.renderTimeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        to = a.contains("to") ? std::min(end, parseTime(a["to"], f, "to")) : end;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from' (and 'from' before the end of the timeline)");
        const QVector<int> only = audioTracksOf(s.project, tl, a["tracks"]);
        if (!only.isEmpty())
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = !only.contains(i);
                tl.audio[i].solo = false;
            }
        sound.fps = f.rate.fps();
        if (!AudioAnalysis::timelineSound(f, tl, from, to, sink, progress))
            fail("DECODE_FAILED", "could not mix the sound");
        out["project"] = s.path.isEmpty() ? absolute(file) : s.path;
    } else {
        const MediaSource m = openMedia(a);
        if (!m.info.hasAudio) fail("NO_AUDIO", "file has no audio: " + m.info.path);
        f = m.format;
        Clip c;
        c.mediaPath = m.info.path;
        c.audioStream = a["audio_stream"].toInt();
        if (c.audioStream < 0 || c.audioStream >= m.info.audioStreamCount())
            fail("BAD_ARGUMENT", QString("the file has %1 audio stream(s)").arg(m.info.audioStreamCount()));
        from = a.contains("from") ? parseTime(a["from"], f, "from") : 0;
        to = a.contains("to") ? std::min(m.info.length, parseTime(a["to"], f, "to")) : m.info.length;
        if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from' (and 'from' before the end of the file)");
        c.in = from;
        c.out = to - 1;
        sound.fps = f.rate.fps();
        if (!AudioAnalysis::clipSound(f, c, sink, progress))
            fail("NO_AUDIO", "could not decode the audio of " + m.info.path);
        out["file"] = m.info.path;
    }
    if (!sound.features) fail("NO_AUDIO", "no sound in the range");
    const Beats::Result r = Beats::analyze(sound.features->take(), options);
    if (!r.found) fail("NO_BEAT", "no beat found (silence, or shorter than four beats)");

    // Times of the range start on: seconds and frames in the frame rate of the answer
    const double fps = f.rate.fps(), start = from / fps;
    const auto frame = [fps, start](double rel) { return int(std::lround((start + rel) * fps)); };
    const auto at = [&](double rel) { return QJsonObject{{"s", round(start + rel, 3)}, {"frame", frame(rel)}}; };
    out["range"] = range(from, to, f);
    out["fps"] = formatJson(f)["fps"];
    out["bpm"] = round(r.bpm, 2);
    out["confidence"] = round(r.confidence, 2);
    out["beat_phase"] = at(r.phase);
    out["beat_period"] = QJsonObject{{"s", round(r.period, 5)}, {"frames", round(r.period * fps, 3)}};

    const int bars = int(r.barEnergyDb.size());
    const double firstBar = Beats::beatTime(r, r.downbeat), bar = 4 * r.period;
    QJsonObject down = at(firstBar);
    down["every_s"] = round(bar, 5);
    down["every_frames"] = round(bar * fps, 3);
    down["confidence"] = round(r.downbeatConfidence, 2);
    down["bars"] = bars;
    out["downbeats"] = down;

    // Phrases (a part before the first phrase start and a short last one say how many bars they have), the energy
    // per bar as digits with a space before every phrase
    QJsonArray phrases;
    QString overall, bassLine;
    QVector<int> phraseStarts;
    if (bars > 0) {
        const double topE = *std::max_element(r.barEnergyDb.begin(), r.barEnergyDb.end());
        const double topB = *std::max_element(r.barBassDb.begin(), r.barBassDb.end());
        const int n = r.phraseBars;
        for (int b = 0; b < bars;) {
            const int len = std::min(b < r.phraseOffset ? r.phraseOffset : n, bars - b);
            QJsonObject p = at(firstBar + b * bar);
            p["bar"] = b + 1;
            p["energy"] = level(meanDb(r.barEnergyDb, b, b + len), topE);
            p["bass"] = level(meanDb(r.barBassDb, b, b + len), topB);
            if (len != n) p["bars"] = len;
            phrases << p;
            if (b >= r.phraseOffset) phraseStarts << b;
            if (b > 0) overall += ' ', bassLine += ' ';
            for (int i = b; i < b + len; ++i) {
                overall += QChar('0' + level(r.barEnergyDb[size_t(i)], topE));
                bassLine += QChar('0' + level(r.barBassDb[size_t(i)], topB));
            }
            b += len;
        }
    }
    out["phrase_bars"] = r.phraseBars;
    out["phrase_confidence"] = round(r.phraseConfidence, 2);
    out["phrases"] = phrases;
    out["energy"] = QJsonObject{{"overall", overall},
                                {"bass", bassLine},
                                {"note", "one digit per bar from the first bar one (downbeats), 9 = the loudest bar, "
                                         "one step = 2 dB; a space before every phrase"}};

    // Breaks and drops
    const auto barOf = [&r](int beat) { return int(std::floor((beat - r.downbeat) / 4.0)) + 1; };
    QJsonArray breaks;
    for (const Beats::Break& b : r.breaks) {
        const double t0 = Beats::beatTime(r, b.from), t1 = Beats::beatTime(r, b.to);
        QJsonObject o{{"from", frame(t0)}, {"to", frame(t1)}, {"from_s", round(start + t0, 3)},
                      {"to_s", round(start + t1, 3)}, {"bars", round((b.to - b.from) / 4.0, 2)},
                      {"bass_db", round(b.bassDb, 1)}, {"energy_db", round(b.energyDb, 1)}};
        if (b.atStart) o["intro"] = true;
        if (b.atEnd) o["outro"] = true;
        breaks << o;
    }
    out["breaks"] = breaks;
    QJsonArray drops;
    const double best = r.drops.empty() ? 1 : std::max(1e-9, r.drops.front().score);
    for (int i = 0; i < int(r.drops.size()) && i < kDrops; ++i) {
        const Beats::Drop& d = r.drops[size_t(i)];
        QJsonObject o = at(Beats::beatTime(r, d.beat));
        o["bar"] = barOf(d.beat);
        o["beat"] = ((d.beat - r.downbeat) % 4 + 4) % 4 + 1;
        o["score"] = round(d.score / best, 2);
        o["bass_jump_db"] = round(d.bassJumpDb, 1);
        o["energy_jump_db"] = round(d.energyJumpDb, 1);
        o["break_s"] = round(d.breakS, 2);
        drops << o;
    }
    out["drops"] = drops;
    if (a["beats"].toBool()) {
        QJsonArray list;
        for (int k = 0; k < r.beats; ++k) {
            QJsonObject o = at(Beats::beatTime(r, k));
            if (k % 4 == r.downbeat) o["bar_one"] = true;
            list << o;
        }
        out["beats"] = list;
    }

    if (!apply.isEmpty()) {
        // Markers on the phrase starts or bars in the range (one edit: one undo step, live in the app too)
        QVector<int> starts = phraseStarts;
        if (marks == "bars") {
            starts.clear();
            for (int b = 0; b < bars; ++b) starts << b;
        }
        QJsonArray ops;
        QVector<int> frames;
        for (int b : starts)
            if (const int pos = frame(firstBar + b * bar); pos >= from && pos < to && !frames.contains(pos)) {
                frames << pos;
                ops << QJsonObject{{"op", "marker"}, {"at", pos}};
            }
        if (ops.isEmpty()) fail("NO_BEAT", "no " + marks + " to mark in the range");
        QJsonObject edit{{"project", file}, {"ops", ops}};
        if (a.contains("timeline")) edit["timeline"] = a["timeline"];
        const QJsonObject done = execute(*find("edit"), edit, ctx);
        QJsonObject applied{{"markers", ops.size()}, {"saved", done["saved"].toBool()}};
        if (done["live"].toBool()) applied["live"] = true;
        out["applied"] = applied;
    }
    if (ctx.progress) ctx.progress(1.0);
    return out;
}

} // namespace Cli::detail
