// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli edit operations for sound: clip volume/pan/stream, the mixer (track and master faders, mute, solo,
// limiter) and Normalize Audio Levels (measured like the app's dialog)
#include "cli/CommandsDetail.h"

#include "core/Automation.h"
#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/GameHud.h"
#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/Speech.h"
#include "core/StreamPresets.h"
#include "core/TaskProgress.h"
#include "core/TimelineOps.h"
#include "core/Transcript.h"
#include "core/Voiceover.h"
#include "engine/AudioAnalysis.h"
#include "engine/Extensions.h"
#include "engine/Tts.h"
#include "engine/VoiceClean.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace Cli::detail {

namespace {

void opVolume(Session& s, const QJsonObject& op)
{
    if (!op.contains("db") && !op.contains("pan")) fail("BAD_ARGUMENT", "missing 'db' (and/or 'pan')");
    const QVector<int> ids = clipsOn(s, op, TrackKind::Audio, "volume and pan");
    if (op.contains("db")) {
        const double db = numberOf(op, "db", -1000, kMaxVolumeDb);
        for (int id : ids) {
            const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
            if (!Keys::animated(c, AnimParam::Volume)) {
                s.editor.setClipVolume(id, db);
                continue;
            }
            // Volume keyframes: the static volume does nothing, move the whole curve (its level becomes db), like
            // dragging the volume line in the app
            Clip moved = c;
            const double delta = Keys::setVolumeLevel(moved, db);
            s.editor.modifyClips({id}, "Volume", [db](Clip& x) { Keys::setVolumeLevel(x, db); });
            s.notes << QString("clip %1 has volume keyframes: the whole curve moved %2 by %3 dB")
                           .arg(id)
                           .arg(delta > 0 ? "up" : "down")
                           .arg(std::abs(delta), 0, 'f', 1);
        }
    }
    if (op.contains("pan")) {
        const double pan = numberOf(op, "pan", -100, 100);
        s.editor.modifyClips(ids, "Pan", [pan](Clip& c) { c.pan = pan; });
    }
}

void opAudioStream(Session& s, const QJsonObject& op)
{
    const int stream = int(numberOf(op, "stream", 0, 63));
    const QVector<int> ids = clipsOn(s, op, TrackKind::Audio, "audio streams");
    for (int id : ids) {
        const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
        const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
        if (!m || stream >= m->audioStreamCount())
            fail("BAD_ARGUMENT", QString("clip %1 has %2 audio stream(s); stream counts from 0").arg(id).arg(m ? m->audioStreamCount() : 0));
        requirePlayableStream(*m, stream);
    }
    s.editor.modifyClips(ids, "Audio stream", [&s, stream](Clip& c) {
        c.audioStream = stream;
        if (const MediaInfo* m = s.project.mediaInfo(c.mediaPath)) fitSourceChannels(c, *m);
    });
}

// Channel routing: which channels of their stream the clips play (Clip::sourceChannels), e.g. a wireless microphone on
// channels 3/4 of a camera file. With track: a copy of each clip on that audio track plays those channels (the
// microphone as its own track under the camera sound), the clips themselves stay.
void opRoute(Session& s, const QJsonObject& op)
{
    if (!op.contains("channels")) fail("BAD_ARGUMENT", "missing 'channels': [3, 4], \"3-4\", 3 or \"all\" (counted from 1)");
    QVector<int> pick; // 0-based
    const QJsonValue v = op.value("channels");
    const auto add = [&pick](double n) {
        if (n < 1 || n > 64 || n != std::floor(n)) fail("BAD_ARGUMENT", "channels count from 1 (to 64)");
        pick << int(n) - 1;
    };
    if (v.isArray()) {
        for (const QJsonValue& x : v.toArray()) add(x.toDouble(0));
    } else if (v.isDouble()) {
        add(v.toDouble());
    } else {
        const QString t = v.toString().trimmed().toLower();
        if (t != "all") {
            const QStringList parts = t.split(QRegularExpression("[-,/ ]+"), Qt::SkipEmptyParts);
            for (const QString& part : parts) {
                bool ok = false;
                const double n = part.toDouble(&ok);
                if (!ok) fail("BAD_ARGUMENT", "channels: [3, 4], \"3-4\", 3 or \"all\" (counted from 1)");
                add(n);
            }
            if (pick.isEmpty()) fail("BAD_ARGUMENT", "channels: [3, 4], \"3-4\", 3 or \"all\" (counted from 1)");
        }
    }
    if (pick.size() > 2)
        fail("BAD_ARGUMENT", "at most two channels per clip (one = mono in the middle, two = left and right); route "
                             "more channels as further clips with track");
    const QVector<int> ids = clipsOn(s, op, TrackKind::Audio, "channel routing");
    for (int id : ids) {
        const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
        if (c.isNested() || c.mediaPath.isEmpty()) fail("BAD_ARGUMENT", QString("clip %1 is not from a file").arg(id));
        const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
        const int channels = m && c.audioStream < m->audioStreams.size() ? m->audioStreams[c.audioStream].channels : 2;
        for (int ch : pick)
            if (ch >= channels)
                fail("BAD_ARGUMENT", QString("clip %1 plays a stream with %2 channel(s) (`probe` lists them); channel %3 "
                                             "is not there").arg(id).arg(channels).arg(ch + 1));
    }
    const auto label = [&pick] {
        if (pick.isEmpty()) return QStringLiteral("all channels");
        QStringList n;
        for (int ch : pick) n << QString::number(ch + 1);
        return (pick.size() == 1 ? QStringLiteral("channel ") : QStringLiteral("channels ")) + n.join('+');
    };
    if (!op.contains("track")) {
        s.editor.modifyClips(ids, T("Kanäle"), [pick](Clip& c) { c.sourceChannels = pick; });
        s.notes << QString("%1 clip(s) play %2").arg(ids.size()).arg(label());
        return;
    }
    // Copies on another audio track
    const Timeline& tl = s.project.timeline();
    int track = -1;
    const bool fresh = op.value("track").toString().trimmed().toLower() == "new";
    if (fresh) {
        track = int(tl.audio.size());
    } else {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio || track >= tl.audio.size())
            fail("BAD_ARGUMENT", "track must be an audio track (A1 …) or 'new'");
        if (tl.audio[track].locked) fail("FAILED", trackName({TrackKind::Audio, track}) + " is locked");
        for (int id : ids) {
            const Clip& c = *TimelineOps::findClip(tl, id);
            for (const Clip& o : tl.audio[track].clips)
                if (o.start < c.end() && c.start < o.end())
                    fail("FAILED", QString("%1 is not free where clip %2 lies (%3); give another track or 'new'")
                                       .arg(trackName({TrackKind::Audio, track})).arg(id).arg(tc(c.start, s.format())));
        }
    }
    // The copies land on one track: they must not overlap each other either (clips of A1 and A2 at the same time)
    for (int i = 0; i < ids.size(); ++i)
        for (int j = i + 1; j < ids.size(); ++j) {
            const Clip& a = *TimelineOps::findClip(tl, ids[i]);
            const Clip& b = *TimelineOps::findClip(tl, ids[j]);
            if (a.start < b.end() && b.start < a.end())
                fail("FAILED", QString("clips %1 and %2 overlap in time (%3): their copies cannot share one track; route "
                                       "them one at a time onto different tracks")
                                   .arg(ids[i]).arg(ids[j]).arg(tc(std::max(a.start, b.start), s.format())));
        }
    const QString name = op.value("name").toString().trimmed();
    QVector<int> made;
    s.project.edit(T("Kanäle auf eigene Spur"), [&](Timeline& t) {
        if (fresh) {
            Track nt;
            nt.kind = TrackKind::Audio;
            nt.name = name;
            t.audio << nt;
        } else if (!name.isEmpty()) {
            t.audio[track].name = name;
        }
        for (int id : ids) {
            Clip c = *TimelineOps::findClip(t, id);
            c.id = s.project.newClipId();
            c.sourceChannels = pick;
            QVector<Clip>& clips = t.audio[track].clips;
            const auto at = std::lower_bound(clips.begin(), clips.end(), c.start,
                                             [](const Clip& x, int start) { return x.start < start; });
            clips.insert(at, c);
            made << c.id;
        }
    });
    s.notes << QString("%1 clip(s) with %2 on %3%4").arg(made.size()).arg(label(), trackName({TrackKind::Audio, track}),
                                                                         fresh ? QStringLiteral(" (a new track)") : QString());
}

using Range = std::pair<int, int>; // timeline frames [first, second)

// The 400 ms loudness blocks of the mix in each of `ranges` (sorted, not overlapping), loudness only (the steps
// below need no true peak, which costs as much again). One pass over [first start, last end); what lies between the
// ranges is mixed too, so callers give ranges close together (clips one after the other) or call it per range.
std::vector<std::vector<double>> rangeBlocks(const ProjectFormat& f, const Timeline& tl, const std::vector<Range>& ranges,
                                             const std::function<bool(double)>& progress = {})
{
    std::vector<LoudnessMeter> meters(ranges.size());
    if (ranges.empty()) return {};
    int frame = ranges.front().first;
    size_t r = 0;
    AudioAnalysis::timelineSound(f, tl, ranges.front().first, ranges.back().second,
                                 [&](const float* pcm, int samples, int channels, int rate) {
                                     while (r < ranges.size() && frame >= ranges[r].second) ++r;
                                     if (r < ranges.size() && frame >= ranges[r].first && pcm)
                                         meters[r].addPlanar(pcm, samples, channels, rate);
                                     ++frame;
                                 },
                                 progress, 0); // loudness over every channel of a 5.1/7.1 mix
    std::vector<std::vector<double>> out;
    out.reserve(meters.size());
    for (const LoudnessMeter& m : meters) out.push_back(m.blocks());
    return out;
}

// Fader so that the mix (master) or one track alone reaches target LUFS. Without the limiter a fader moves the
// loudness by exactly its change (one measurement). With it the limiter takes part of every raise (peaky game sound:
// 1 dB more fader gives maybe 0.5 LU), so step with the measured slope after the limiter. On a long edit the steps
// measure 8 spread 10 s windows (only the change counts), then the whole once more (and one last correction if it
// is still off). The master fader ends at +12 dB: with the limiter on, the rest goes onto the audible tracks'
// faders, all by the same amount (the limiter holds the peaks, so that is safe; the balance stays).
void mixToLoudness(Session& s, int track, double target)
{
    constexpr double kClose = 0.2, kTolerance = 0.5, kCollapse = 0.1;
    constexpr int kSteps = 6;
    const ProjectFormat& f = s.format();
    AudioAnalysis::MixMeter meter(f);
    const auto timeline = [&] {
        Timeline tl = s.project.renderTimeline();
        if (track >= 0)
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = i != track;
                tl.audio[i].solo = false;
            }
        return tl;
    };
    const int end = TimelineOps::endFrame(s.project.renderTimeline());
    if (end <= 0) s.failEmpty();
    // Where the steps measure: the whole, or on a long edit 8 windows of 10 s spread over it
    std::vector<Range> windows;
    const int window = int(std::lround(10 * f.rate.fps()));
    if (end > 12 * window)
        for (int i = 0; i < 8; ++i) {
            const int mid = int((i + 0.5) * end / 8);
            windows.push_back({mid - window / 2, mid + window / 2});
        }
    // Progress for a caller that follows it (TaskProgress: the Studio page's dialog, ~50 s on a 30 min edit) and its
    // cancel. In units of one measurement of the whole; how many there are is known only at the end: about the first,
    // the steps (on the windows a fraction of one each) and the check of the whole
    const double windowUnit = windows.empty() ? 1.0 : double(windows.size()) * window / end;
    const double expected = windows.empty() ? 3.0 : 2.0 + 4 * windowUnit;
    double done = 0, shown = 0;
    const auto progressOf = [&](double unit) -> std::function<bool(double)> {
        if (!TaskProgress::active()) return {};
        return [&, unit](double fraction) {
            shown = std::clamp((done + fraction * unit) / std::max(expected, done + unit), shown, 0.99);
            return TaskProgress::report(shown);
        };
    };
    const auto stopIfCanceled = [](bool canceled) {
        if (canceled) fail("CANCELED", "canceled");
    };
    const auto measure = [&] { // the whole, with true peak (what the answer shows)
        stopIfCanceled(!TaskProgress::report(shown));
        const auto l = meter.measure(timeline(), 0, end, progressOf(1.0));
        stopIfCanceled(!TaskProgress::report(shown));
        done += 1.0;
        if (!l || l->integrated <= LoudnessMeter::kSilence) fail("FAILED", "no sound to measure");
        return *l;
    };
    const auto sampled = [&] {
        const Timeline tl = timeline();
        std::vector<double> blocks;
        const double each = windowUnit / std::max<size_t>(1, windows.size());
        for (const Range& w : windows) {
            for (const std::vector<double>& b : rangeBlocks(f, tl, {w}, progressOf(each)))
                blocks.insert(blocks.end(), b.begin(), b.end());
            stopIfCanceled(!TaskProgress::report(shown));
            done += each;
        }
        return LoudnessMeter::integratedOf(blocks);
    };
    const QString what = track >= 0 ? QString("A%1").arg(track + 1) : QString("mix");
    const bool limiter = s.project.timeline().masterLimiter;
    // The master's helpers beyond +12 dB: audible tracks with clips (solo wins), and how far they can all go up
    QVector<int> helpers;
    double headroom = 0;
    if (track < 0 && limiter) {
        const Timeline& tl = s.project.timeline();
        const bool solo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
        headroom = kMaxVolumeDb;
        for (int i = 0; i < tl.audio.size(); ++i) {
            const Track& t = tl.audio[i];
            if (t.clips.isEmpty() || (solo ? !t.solo : t.muted)) continue;
            helpers << i;
            headroom = std::min(headroom, kMaxVolumeDb - t.volumeDb);
        }
        if (helpers.isEmpty()) headroom = 0;
        headroom = std::max(0.0, headroom);
    }
    double extra = 0; // dB added to the helpers' faders
    const auto gainNow = [&] {
        return (track >= 0 ? s.project.timeline().audio[track].volumeDb : s.project.timeline().masterVolumeDb) + extra;
    };
    const double maxGain = kMaxVolumeDb + headroom;
    const auto setGain = [&](double g) {
        g = std::clamp(g, kMinVolumeDb, maxGain);
        if (track >= 0) return s.editor.setTrackVolume(track, g);
        const double fader = std::min(g, kMaxVolumeDb), more = g - fader;
        s.editor.setTrackVolume(-1, fader);
        if (std::abs(more - extra) < 1e-9) return;
        for (int i : helpers) s.editor.setTrackVolume(i, s.project.timeline().audio[i].volumeDb + more - extra);
        extra = more;
    };
    const auto faders = [&] {
        QString text = QString("fader %1 dB").arg(gainNow() - extra, 0, 'f', 1);
        if (extra > 0.05) {
            QStringList names;
            for (int i : helpers) names << QString("A%1").arg(i + 1);
            text += QString(", %1 +%2 dB as well (the master fader ends at +12 dB)").arg(names.join(", ")).arg(extra, 0, 'f', 1);
        }
        return text;
    };

    AudioAnalysis::Loudness l = measure();
    const double before = l.integrated;
    if (!limiter) {
        const double now = gainNow(), want = std::clamp(now + target - l.integrated, kMinVolumeDb, kMaxVolumeDb);
        if (std::abs(want - now) >= 0.05) setGain(want);
        const double reached = l.integrated + (want - now), peak = l.truePeakDb + (want - now);
        QString note = QString("%1 was %2 LUFS, %3 -> %4 LUFS (target %5)")
                           .arg(what)
                           .arg(l.integrated, 0, 'f', 1)
                           .arg(faders())
                           .arg(reached, 0, 'f', 1)
                           .arg(target, 0, 'f', 1);
        if (peak > -1.0)
            note += QString(", true peak now about %1 dBTP: turn on the master limiter "
                            "({op:'mixer', track:'master', limiter:true})").arg(peak, 0, 'f', 1);
        else
            note += QString(", true peak about %1 dBTP").arg(peak, 0, 'f', 1);
        if (want >= kMaxVolumeDb - 0.05 && reached < target - kTolerance)
            note += QString(" - not reached: the fader is at its +12 dB end, %1 LU short").arg(target - reached, 0, 'f', 1);
        s.notes << note;
        s.notes << QJsonObject{{"loudness", QJsonObject{{"of", what}, {"target_lufs", target}, {"before_lufs", l.integrated},
                                                        {"after_lufs", reached}, {"true_peak_db", peak}, {"limiter", false}}}};
        return;
    }
    // Steps on what `probe` measures toward `goal`; false = at an end or the limiter takes (almost) everything
    double lastGain = 0, lastLufs = 0, slope = 1.0;
    bool havePrev = false;
    const auto steps = [&](const std::function<double()>& probe, double now, double goal, int count) {
        for (int i = 0; i < count && std::abs(now - goal) >= kClose; ++i) {
            const double g = gainNow();
            if (havePrev && std::abs(g - lastGain) > 0.05) {
                const double measured = (now - lastLufs) / (g - lastGain);
                if (measured < kCollapse) return false;
                slope = std::min(measured, 1.0);
            }
            const double want = std::clamp(g + (goal - now) / slope, kMinVolumeDb, maxGain);
            if (std::abs(want - g) < 0.05) return false;
            lastGain = g, lastLufs = now, havePrev = true;
            setGain(want);
            now = probe();
        }
        return true;
    };
    const auto whole = [&] {
        l = measure();
        return l.integrated;
    };
    if (std::abs(l.integrated - target) >= kClose) {
        if (windows.empty()) {
            steps(whole, l.integrated, target, kSteps);
        } else {
            // The windows move like the whole: step them by the change the whole needs, then check the whole and
            // correct once more with the windows' slope (the whole is another curve: no secant across the two)
            const double start = sampled();
            steps(sampled, start, start + target - l.integrated, kSteps);
            havePrev = false;
            steps(whole, whole(), target, 1);
        }
    }
    QString note = QString("%1 was %2 LUFS, measured after the master limiter now %3 LUFS (target %4), %5, true peak %6 dBTP")
                       .arg(what)
                       .arg(before, 0, 'f', 1)
                       .arg(l.integrated, 0, 'f', 1)
                       .arg(target, 0, 'f', 1)
                       .arg(faders())
                       .arg(l.truePeakDb, 0, 'f', 1);
    if (l.integrated < target - kTolerance) {
        note += QString(" - not reachable, %1 LU short: ").arg(target - l.integrated, 0, 'f', 1);
        note += gainNow() >= maxGain - 0.05
                    ? QString(track >= 0 ? "the track fader is at its +12 dB end"
                                         : "the faders are at their +12 dB end")
                          + " and the limiter takes the peaks (a compressor on the loudest track evens them out, "
                            "or lower the target)"
                    : QString("the limiter takes (almost) every further dB (lower the target or raise limiter_db)");
    } else if (l.integrated > target + kTolerance) {
        note += QString(" - %1 LU over the target").arg(l.integrated - target, 0, 'f', 1);
    }
    s.notes << note;
    // The same in numbers (the Studio page shows before -> after)
    s.notes << QJsonObject{{"loudness", QJsonObject{{"of", what}, {"target_lufs", target}, {"before_lufs", before},
                                                    {"after_lufs", l.integrated}, {"true_peak_db", l.truePeakDb},
                                                    {"limiter", true}}}};
}

// compressor:{on?, threshold_db?, ratio?, attack_ms?, release_ms?, knee_db?, makeup_db?} onto d; giving values
// without `on` switches it on (like the app's dialog, where changing a value is meant to be heard)
TrackDynamics compressorFrom(const QJsonValue& v, TrackDynamics d)
{
    if (!v.isObject()) fail("BAD_ARGUMENT", "'compressor' must be an object like {on:true, threshold_db:-20, ratio:4}");
    const QJsonObject o = v.toObject();
    using C = Compressor;
    const std::tuple<const char*, double*, double, double> fields[] = {
        {"threshold_db", &d.thresholdDb, C::kMinThreshold, C::kMaxThreshold},
        {"ratio", &d.ratio, C::kMinRatio, C::kMaxRatio},
        {"attack_ms", &d.attackMs, C::kMinAttack, C::kMaxAttack},
        {"release_ms", &d.releaseMs, C::kMinRelease, C::kMaxRelease},
        {"knee_db", &d.kneeDb, 0, C::kMaxKnee},
        {"makeup_db", &d.makeupDb, C::kMinMakeup, C::kMaxMakeup}};
    bool values = false;
    for (const auto& [key, field, lo, hi] : fields)
        if (o.contains(key)) {
            *field = numberOf(o, key, lo, hi);
            values = true;
        }
    for (const QString& key : o.keys())
        if (key != "on" && std::none_of(std::begin(fields), std::end(fields), [&](const auto& f) { return key == std::get<0>(f); }))
            fail("BAD_ARGUMENT", QString("unknown compressor field '%1'").arg(key));
    d.on = o.contains("on") ? o.value("on").toBool() : (values || d.on);
    return d;
}

// eq:{on?, bands?:[{band (1-6), on?, freq?, gain_db?, q?}]} onto eq; changing a band without `on` switches it and
// the EQ on (like dragging a point in the app)
TrackEq eqFrom(const QJsonValue& v, TrackEq eq)
{
    if (v.isString()) { // a preset by name (the Studio page's "Stimme")
        if (v.toString().trimmed().toLower() == "voice") return Equalizer::voicePreset();
        fail("BAD_ARGUMENT", "unknown eq preset '" + v.toString() + "' (there is 'voice')");
    }
    if (!v.isObject()) fail("BAD_ARGUMENT", "'eq' must be an object like {on:true, bands:[{band:3, freq:2500, gain_db:3}]} or 'voice'");
    const QJsonObject o = v.toObject();
    bool changed = false;
    for (const QJsonValue& bv : o.value("bands").toArray()) {
        const QJsonObject b = bv.toObject();
        const int index = int(numberOf(b, "band", 1, 6)) - 1;
        if (index + 1 != b.value("band").toDouble()) fail("BAD_ARGUMENT", "'band' must be a whole number 1-6");
        EqBand& band = eq.bands[size_t(index)];
        const bool filter = band.kind == EqBand::Kind::HighPass || band.kind == EqBand::Kind::LowPass;
        if (b.contains("gain_db") && filter)
            fail("BAD_ARGUMENT", QString("band %1 is a %2 pass filter: it has freq and q, no gain_db")
                                     .arg(index + 1).arg(band.kind == EqBand::Kind::HighPass ? "high" : "low"));
        bool values = false;
        if (b.contains("freq")) band.freq = numberOf(b, "freq", Equalizer::kMinFreq, Equalizer::kMaxFreq), values = true;
        if (b.contains("gain_db")) band.gainDb = numberOf(b, "gain_db", Equalizer::kMinGain, Equalizer::kMaxGain), values = true;
        if (b.contains("q")) band.q = numberOf(b, "q", Equalizer::kMinQ, Equalizer::kMaxQ), values = true;
        band.on = b.contains("on") ? b.value("on").toBool() : (values || band.on);
        changed = true;
    }
    if (o.contains("bands") && !o.value("bands").isArray()) fail("BAD_ARGUMENT", "'bands' must be an array");
    eq.on = o.contains("on") ? o.value("on").toBool() : (changed || eq.on);
    return eq;
}

void opMixer(Session& s, const QJsonObject& op)
{
    const QString track = op.value("track").toString().trimmed().toLower();
    const bool toLoudness = op.contains("target_lufs");
    const double targetLufs = toLoudness ? numberOf(op, "target_lufs", -70, 0) : 0;
    if (toLoudness && op.contains("volume_db")) fail("BAD_ARGUMENT", "give volume_db or target_lufs, not both");
    if (track == "master") {
        if (op.contains("pan") || op.contains("mute") || op.contains("solo") || op.contains("compressor") || op.contains("eq")
            || op.contains("depth") || op.contains("center") || op.contains("lfe_db"))
            fail("BAD_ARGUMENT", "the master has volume_db, limiter and limiter_db (eq and compressor are per track)");
        double db = 0, ceiling = 0;
        if (op.contains("volume_db")) db = numberOf(op, "volume_db", kMinVolumeDb, kMaxVolumeDb);
        if (op.contains("limiter_db")) ceiling = numberOf(op, "limiter_db", -20, 0);
        // Same path as the app's Mixer (Editor); a ceiling switches the limiter on unless limiter:false says otherwise
        if (op.contains("volume_db")) s.editor.setTrackVolume(-1, db);
        if (op.contains("limiter_db")) s.editor.setMasterLimiterCeiling(ceiling);
        if (op.contains("limiter")) s.editor.setMasterLimiter(op.value("limiter").toBool());
        if (toLoudness) mixToLoudness(s, -1, targetLufs);
        return;
    }
    TrackKind k = TrackKind::Video;
    const int index = parseTrackIn(s.project.timeline(), op.value("track"), &k, TrackKind::Audio);
    if (k != TrackKind::Audio || index >= s.project.timeline().audio.size())
        fail("BAD_ARGUMENT", "track must be an audio track like A1 or its name (or 'master')");
    if (op.contains("limiter") || op.contains("limiter_db")) fail("BAD_ARGUMENT", "the limiter is on the master only");
    double db = 0, pan = 0;
    const Track& before = s.project.timeline().audio[index];
    const double depth = op.contains("depth") ? numberOf(op, "depth", 0, 100) : before.panDepth;
    const double center = op.contains("center") ? numberOf(op, "center", 0, 100) : before.panCenter;
    const double lfe = op.contains("lfe_db") ? numberOf(op, "lfe_db", kMinVolumeDb, kMaxVolumeDb) : before.lfeDb;
    if (op.contains("volume_db")) db = numberOf(op, "volume_db", kMinVolumeDb, kMaxVolumeDb);
    if (op.contains("pan")) pan = numberOf(op, "pan", -100, 100);
    const Track& now = s.project.timeline().audio[index];
    const TrackDynamics dynamics = op.contains("compressor") ? compressorFrom(op.value("compressor"), now.dynamics) : now.dynamics;
    const TrackEq eq = op.contains("eq") ? eqFrom(op.value("eq"), now.eq) : now.eq;
    // Same path as the app's Mixer faders, buttons and dialogs (Editor)
    if (op.contains("volume_db")) s.editor.setTrackVolume(index, db);
    if (op.contains("pan")) s.editor.setTrackPan(index, pan);
    if (op.contains("mute")) s.editor.setTrackMuted(index, op.value("mute").toBool());
    if (op.contains("solo")) s.editor.setTrackSolo(index, op.value("solo").toBool());
    s.editor.setTrackDynamics(index, dynamics);
    s.editor.setTrackEq(index, eq);
    if (op.contains("depth") || op.contains("center") || op.contains("lfe_db")) {
        s.editor.setTrackSurround(index, depth, center, lfe);
        if (s.project.format().audioChannels == 2)
            s.notes << "depth/center/lfe_db act in a 5.1/7.1 mix only (the project is stereo: {op:'format', channels:'5.1'})";
    }
    if (toLoudness) mixToLoudness(s, index, targetLufs);
}

// A clip's volume level: its static volume, or with volume keyframes the level Keys::setVolumeLevel moves
double volumeLevel(const Clip& c)
{
    Clip probe = c;
    return -Keys::setVolumeLevel(probe, 0.0);
}

// normalize to LUFS with the master limiter on: the limiter takes part of the raise on peaky sound (game clips:
// shots at 0 dBFS over quiet ambience), so a clip lands under the target. Each clip whose peaks reach the limiter is
// measured alone after it, its track unmuted and the faders as the limiter-off path counts them (master and track at
// 0 dB, only the gain allow_track_gain added stays), and its volume corrected with the measured slope; relative: all
// clips by the same amount until their blocks together (the basis of the shared measurement) reach it. A clip already
// within 0.5 LU stays; a correction aims at 0.2 LU, two at most. All clips are measured in one pass per round (clips
// at the same time in one pass each). What goes beyond the clip volume's +12 dB goes onto its track with
// allow_track_gain (the track's other clips lowered by as much); the master never moves. Stops when the limiter takes
// almost every further dB (< 0.2 dB per dB). `before` = the timeline before normalize, `peaks` = the files' true peaks.
struct AfterLimiter {
    QStringList notes;
    QSet<int> handled; // clips the notes speak of (the plain +12 dB notes are not for them)
};
AfterLimiter normalizeAfterLimiter(Session& s, const Timeline& before, const QVector<int>& clips,
                                   const QHash<int, double>& peaks, double target, bool relative, bool trackGain)
{
    constexpr double kClose = 0.2, kTolerance = 0.5, kCollapse = 0.2;
    constexpr int kCorrections = 2;
    AfterLimiter out;
    const auto added = [&](int track) {
        return track < before.audio.size() ? s.project.timeline().audio[track].volumeDb - before.audio[track].volumeDb : 0.0;
    };
    // Where the limiter has nothing to do the limiter-off result is exact: no measuring
    const double ceiling = s.project.timeline().masterLimiterDb;
    const auto reaches = [&](int id) {
        TrackRef where;
        const Clip* c = TimelineOps::findClip(s.project.timeline(), id, &where);
        return c && peaks.contains(id) && peaks[id] + volumeLevel(*c) + added(where.index) > ceiling - 0.5;
    };
    struct Unit {
        QVector<int> ids;
        double lufs = LoudnessMeter::kSilence, lastGain = 0, lastLufs = 0;
        bool havePrev = false, done = false, pinned = false, collapsed = false;
    };
    std::vector<Unit> units;
    if (relative) {
        if (std::any_of(clips.begin(), clips.end(), reaches)) units.push_back({clips});
    } else {
        for (int id : clips)
            if (reaches(id)) units.push_back({{id}});
    }
    // The clips' loudness after the limiter (faders as above), clips at the same time in separate passes
    const auto measure = [&](const QVector<int>& ids) {
        const Timeline base = s.project.renderTimeline();
        QVector<const Clip*> todo;
        for (int id : ids)
            if (const Clip* c = TimelineOps::findClip(base, id)) todo << c;
        std::sort(todo.begin(), todo.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
        QHash<int, std::vector<double>> blocks;
        while (!todo.isEmpty()) {
            QVector<const Clip*> pass, later;
            int free = std::numeric_limits<int>::min();
            for (const Clip* c : todo) {
                if (c->start < free) {
                    later << c;
                    continue;
                }
                pass << c;
                free = c->end();
            }
            QSet<int> in;
            std::vector<Range> ranges;
            for (const Clip* c : pass) in.insert(c->id), ranges.push_back({c->start, c->end()});
            Timeline tl = base;
            tl.masterVolumeDb = 0;
            for (int i = 0; i < tl.audio.size(); ++i) {
                Track& t = tl.audio[i];
                t.solo = false;
                t.muted = false;
                t.volumeDb = added(i);
                QVector<Clip> kept;
                for (const Clip& c : t.clips)
                    if (in.contains(c.id)) kept << c;
                t.clips = kept;
            }
            for (Track& t : tl.video) t.clips.clear();
            const std::vector<std::vector<double>> b = rangeBlocks(s.format(), tl, ranges);
            for (int i = 0; i < pass.size() && i < int(b.size()); ++i) blocks[pass[i]->id] = b[size_t(i)];
            todo = later;
        }
        return blocks;
    };
    const auto measureUnits = [&] {
        QVector<int> ids;
        for (const Unit& u : units)
            if (!u.done) ids += u.ids;
        const QHash<int, std::vector<double>> blocks = measure(ids);
        for (Unit& u : units) {
            if (u.done) continue;
            std::vector<double> all;
            for (int id : u.ids) {
                const std::vector<double> b = blocks.value(id);
                all.insert(all.end(), b.begin(), b.end());
            }
            u.lufs = LoudnessMeter::integratedOf(all);
        }
    };
    // What the steps move: the unit's mean of clip volume + added track gain
    const auto gainOf = [&](const Unit& u) {
        double sum = 0;
        for (int id : u.ids) {
            TrackRef where;
            if (const Clip* c = TimelineOps::findClip(s.project.timeline(), id, &where)) sum += volumeLevel(*c) + added(where.index);
        }
        return sum / u.ids.size();
    };
    measureUnits();
    for (Unit& u : units) {
        if (u.lufs <= LoudnessMeter::kSilence) u.done = true; // nothing measured: the limiter-off result stays
        else for (int id : u.ids) out.handled.insert(id);
        if (std::abs(u.lufs - target) < kTolerance) u.done = true; // close enough already: one pass, no step
    }
    for (int round = 0; round < kCorrections; ++round) {
        QHash<int, double> step; // clip -> dB
        QHash<size_t, double> gains;
        for (size_t i = 0; i < units.size(); ++i) {
            Unit& u = units[i];
            if (u.done) continue;
            const double g = gainOf(u);
            double slope = 1.0;
            if (u.havePrev && std::abs(g - u.lastGain) > 0.05) {
                slope = (u.lufs - u.lastLufs) / (g - u.lastGain);
                if (slope < kCollapse) {
                    u.collapsed = u.done = true;
                    continue;
                }
                slope = std::min(slope, 1.0);
            }
            for (int id : u.ids) step[id] = (target - u.lufs) / slope;
            gains[i] = g;
        }
        if (step.isEmpty()) break;
        s.project.edit("Normalize: after the limiter", [&](Timeline& t) {
            QHash<int, double> rest; // track -> dB its clips need beyond +12
            QHash<int, double> left; // clip -> its own part of that
            for (int index = 0; index < t.audio.size(); ++index)
                for (Clip& c : t.audio[index].clips) {
                    if (!step.contains(c.id) || TimelineOps::isLocked(t, c.id)) continue;
                    const double want = std::max(kMinVolumeDb, volumeLevel(c) + step[c.id]), now = std::min(want, kMaxVolumeDb);
                    Keys::setVolumeLevel(c, now);
                    if (trackGain && want - now > 0.05) {
                        rest[index] = std::max(rest.value(index), want - now);
                        left[c.id] = want - now;
                    }
                }
            for (auto it = rest.begin(); it != rest.end(); ++it) {
                Track& track = t.audio[it.key()];
                const double add = std::min(it.value(), kMaxVolumeDb - track.volumeDb);
                if (add <= 0) continue;
                track.volumeDb += add;
                for (Clip& c : track.clips) {
                    const double down = add - (step.contains(c.id) ? std::min(add, left.value(c.id)) : 0.0);
                    if (down > 0) Keys::setVolumeLevel(c, volumeLevel(c) - down);
                }
            }
        });
        for (auto it = gains.begin(); it != gains.end(); ++it) {
            Unit& u = units[it.key()];
            if (std::abs(gainOf(u) - it.value()) < 0.05) { // every clip of it at the end of its faders
                u.pinned = u.done = true;
                continue;
            }
            u.lastGain = it.value(), u.lastLufs = u.lufs, u.havePrev = true;
        }
        measureUnits();
        for (Unit& u : units)
            if (!u.done && std::abs(u.lufs - target) < kClose) u.done = true;
    }
    for (const Unit& u : units) {
        if (u.lufs <= LoudnessMeter::kSilence) continue;
        const QString who = relative ? QString("the clips together") : QString("clip %1").arg(u.ids.first());
        QString note = QString("%1: %2 LUFS measured after the master limiter (target %3; master and track faders "
                               "counted as 0 dB like without a limiter)")
                           .arg(who)
                           .arg(u.lufs, 0, 'f', 1)
                           .arg(target, 0, 'f', 1);
        if (u.lufs < target - kTolerance) {
            note += QString(" - not reached, %1 LU short: ").arg(target - u.lufs, 0, 'f', 1);
            if (u.pinned && !trackGain)
                note += "the clip volume ends at +12 dB (allow_track_gain:true puts the rest on its track, or "
                        "{op:'mixer', track:'master', target_lufs:...} raises the whole mix)";
            else if (u.pinned)
                note += "clip and track volume are at their +12 dB end and the limiter takes the peaks (a compressor "
                        "on the track evens them out, or lower the target)";
            else if (u.collapsed)
                note += "the limiter takes almost every further dB (under 0.2 dB per dB; lower the target, raise "
                        "limiter_db or put a compressor on the track)";
            else
                note += "the limiter takes most of every further dB (lower the target or raise limiter_db)";
        }
        out.notes << note;
    }
    for (int i = 0; i < before.audio.size() && i < s.project.timeline().audio.size(); ++i)
        if (added(i) > 0.05)
            out.notes << QString("A%1: track fader +%2 dB (allow_track_gain), its other clips lowered by as much")
                             .arg(i + 1)
                             .arg(added(i), 0, 'f', 1);
    return out;
}

// Track volume / pan automation (Track::volumeAuto / panAuto): points replace the curve (or the part [from, to]),
// clear removes it. Volume points carry db, pan points pan (both on top of the fader / knob).
void opTrackAutomation(Session& s, const QJsonObject& op, AutoTarget target)
{
    const bool pan = target == AutoTarget::Pan;
    const char* key = pan ? "pan" : "db";
    const QString what = pan ? QStringLiteral("pan automation") : QStringLiteral("volume automation");
    TrackKind k = TrackKind::Video;
    const int index = parseTrack(op.value("track"), &k);
    if (k != TrackKind::Audio || index < 0 || index >= s.project.timeline().audio.size())
        fail("BAD_ARGUMENT", "track must be an audio track like A1");
    const Track& t = s.project.timeline().audio[index];
    if (t.locked) fail("LOCKED", QString("track %1 is locked").arg(trackName({k, index})));
    const bool clear = op.value("clear").toBool();
    if (clear == op.contains("points")) fail("BAD_ARGUMENT", "give points (a curve) or clear:true");
    const ProjectFormat& fmt = s.project.format();
    const bool part = op.contains("from") || op.contains("to");
    if (clear) {
        if (part) fail("BAD_ARGUMENT", "clear removes the whole curve (for a part give points with from/to)");
        if (!s.editor.setTrackAutomation(index, {}, T("Spur-Automation entfernen"), {}, target))
            s.notes << QString("%1 had no %2").arg(trackName({k, index}), what);
        return;
    }
    if (!op.value("points").isArray())
        fail("BAD_ARGUMENT", pan ? "points must be a list like [{at:0, pan:0}, {at:'2s', pan:-50}]"
                                 : "points must be a list like [{at:0, db:0}, {at:'2s', db:-12}]");
    AutoCurve pts;
    const double lo = Automation::minValue(target), hi = Automation::maxValue(target);
    for (const QJsonValue& v : op.value("points").toArray()) {
        QJsonValue at, val;
        if (v.isArray() && v.toArray().size() == 2) {
            at = v.toArray()[0];
            val = v.toArray()[1];
        } else if (v.isObject()) {
            at = v.toObject().value("at");
            val = v.toObject().value(key);
        }
        if (at.isUndefined() || !val.isDouble())
            fail("BAD_ARGUMENT", QString("every point needs at (a time) and %1 (a number)").arg(key));
        const double d = val.toDouble();
        if (d < lo || d > hi)
            fail("BAD_ARGUMENT", QString("%1 %2 out of range %3..%4 (on top of the %5)")
                                     .arg(key).arg(d).arg(lo).arg(hi).arg(pan ? "pan knob" : "fader"));
        pts << AutoPoint{parseTime(at, fmt, "points.at"), d};
    }
    if (pts.isEmpty() && !part) fail("BAD_ARGUMENT", "points is empty (to remove the curve give clear:true)");
    AutoCurve next = Automation::normalized(pts, target);
    if (part) {
        const int from = op.contains("from") ? parseTime(op.value("from"), fmt, "from") : (next.isEmpty() ? 0 : next.first().frame);
        const int to = op.contains("to") ? parseTime(op.value("to"), fmt, "to") : (next.isEmpty() ? from : next.last().frame);
        if (to < from) fail("BAD_ARGUMENT", "to lies before from");
        AutoCurve whole = t.automation(target);
        Automation::replaceRange(whole, from, to, next, target);
        next = whole;
    }
    if (!s.editor.setTrackAutomation(index, next, {}, {}, target))
        s.notes << QString("%1: the %2 is already like that").arg(trackName({k, index}), what);
}

void opTrackVolume(Session& s, const QJsonObject& op) { opTrackAutomation(s, op, AutoTarget::Volume); }
void opTrackPan(Session& s, const QJsonObject& op) { opTrackAutomation(s, op, AutoTarget::Pan); }

void opNormalize(Session& s, const QJsonObject& op)
{
    const QString mode = op.value("mode").toString("peak");
    if (mode != "peak" && mode != "lufs") fail("BAD_ARGUMENT", "mode must be 'peak' (sample peak, dBFS) or 'lufs' (loudness)");
    const bool lufs = mode == "lufs";
    const double target = op.contains("target") ? numberOf(op, "target", lufs ? -70 : -60, 0) : (lufs ? -14.0 : -1.0);
    const QVector<int> ids = clipsOn(s, op, TrackKind::Audio, "normalizing");
    if (op.value("linked").toBool(true)) {
        QJsonObject given = op;
        given["linked"] = false;
        const QVector<int> asked = clipIds(s, given);
        QJsonArray partners;
        for (int id : ids)
            if (!asked.contains(id)) partners << id;
        if (!partners.isEmpty())
            s.notes << QString("normalize also changed the linked clips %1 (linked:false = only the given ones)")
                           .arg(QJsonDocument(partners).toJson(QJsonDocument::Compact));
    }
    // Measure every clip's sound (without its volume), like the app; relative: one shared reference
    const Timeline tl = s.project.timeline();
    AudioAnalysis::ClipLevels measured = *AudioAnalysis::measureClips(s.format(), tl, ids, lufs); // same as the app
    QHash<int, double>& levels = measured.levels;
    const QHash<int, double>& truePeaks = measured.truePeaks;
    if (levels.isEmpty()) fail("FAILED", "no sound to measure (media missing?)");
    std::optional<double> ref = measured.shared;
    const bool relative = op.value("relative").toBool();
    // allow_track_gain: what a clip needs beyond the clip fader's +12 dB goes onto its track's fader; the track's
    // other clips are lowered by as much, so they stay as loud as they were
    QHash<int, double> trackGain; // audio track index -> dB added to its fader
    QSet<int> adjusted;           // clips normalizeAudio sets
    if (op.value("allow_track_gain").toBool()) {
        const double shared = relative ? target - (ref ? *ref : *std::max_element(levels.begin(), levels.end())) : 0;
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            TrackRef where;
            const Clip* c = TimelineOps::findClip(tl, it.key(), &where);
            if (!c || where.kind != TrackKind::Audio || it.value() <= -100.0 || TimelineOps::isLocked(tl, c->id)) continue;
            adjusted.insert(it.key());
            const double over = (relative ? shared : target - it.value()) - kMaxVolumeDb;
            if (over > 0.05) trackGain[where.index] = std::max(trackGain.value(where.index), over);
        }
        for (auto it = trackGain.begin(); it != trackGain.end(); ++it)
            it.value() = std::min(it.value(), kMaxVolumeDb - tl.audio[it.key()].volumeDb);
        if (relative && !trackGain.isEmpty()) { // one shared change: the same amount on every track
            const double t = std::max(0.0, *std::min_element(trackGain.begin(), trackGain.end()));
            for (auto it = trackGain.begin(); it != trackGain.end(); ++it) it.value() = t;
            if (ref) *ref += t;
        }
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            TrackRef where;
            if (adjusted.contains(it.key()) && TimelineOps::findClip(tl, it.key(), &where))
                it.value() += std::max(0.0, trackGain.value(where.index));
        }
    }
    s.editor.normalizeAudio(levels, target, relative, ref);
    if (std::any_of(trackGain.begin(), trackGain.end(), [](double g) { return g > 0.05; })) {
        s.project.edit("Normalize: track gain", [&](Timeline& t) {
            for (auto it = trackGain.begin(); it != trackGain.end(); ++it) {
                if (it.value() <= 0.05) continue;
                Track& track = t.audio[it.key()];
                track.volumeDb += it.value();
                for (Clip& c : track.clips) {
                    if (adjusted.contains(c.id)) continue;
                    if (Keys::animated(c, AnimParam::Volume))
                        for (Keyframe& k : c.keys[AnimParam::Volume])
                            k.value = std::clamp(k.value - it.value(), kMinVolumeDb, kMaxVolumeDb);
                    else
                        c.volumeDb = std::clamp(c.volumeDb - it.value(), kMinVolumeDb, kMaxVolumeDb);
                }
            }
        });
        for (auto it = trackGain.begin(); it != trackGain.end(); ++it)
            if (it.value() > 0.05 && !(lufs && s.project.timeline().masterLimiter)) // else said after the limiter
                s.notes << QString("A%1: track fader +%2 dB (allow_track_gain), its other clips lowered by as much")
                               .arg(it.key() + 1)
                               .arg(it.value(), 0, 'f', 1);
    }
    // With the master limiter on, measure after it and correct (it says itself what was reached)
    const bool limited = lufs && s.project.timeline().masterLimiter;
    AfterLimiter afterLimiter;
    if (limited) {
        QVector<int> clips;
        for (auto it = levels.begin(); it != levels.end(); ++it) {
            TrackRef where;
            const Clip* c = TimelineOps::findClip(tl, it.key(), &where);
            if (c && where.kind == TrackKind::Audio && it.value() > -100.0 && !TimelineOps::isLocked(tl, c->id)) clips << c->id;
        }
        std::sort(clips.begin(), clips.end());
        afterLimiter = normalizeAfterLimiter(s, tl, clips, truePeaks, target, relative, op.value("allow_track_gain").toBool());
        for (const QString& note : afterLimiter.notes) s.notes << note;
    }
    // The clip fader ends at +12 dB: quiet clips may not reach the target (relative: all of them together)
    if (relative && afterLimiter.handled.isEmpty()) {
        double loudest = -1000;
        for (const double l : levels) loudest = std::max(loudest, l);
        const double reference = ref ? *ref : loudest, need = target - reference;
        if (reference > -100.0 && need > kMaxVolumeDb + 0.05)
            s.notes << QString("the clips need %1 dB together but clip volume ends at +%2 dB: they stay %3 dB under "
                               "the target (allow_track_gain:true puts the rest on their tracks, or raise the master)")
                           .arg(need, 0, 'f', 1)
                           .arg(kMaxVolumeDb, 0, 'f', 0)
                           .arg(need - kMaxVolumeDb, 0, 'f', 1);
    } else if (!relative) {
        for (auto it = levels.begin(); it != levels.end(); ++it)
            if (it.value() > -100.0 && target - it.value() > kMaxVolumeDb + 0.05 && !afterLimiter.handled.contains(it.key()))
                s.notes << QString("clip %1 needs %2 dB but clip volume ends at +%3 dB: it stays %4 dB under the target "
                                   "(allow_track_gain:true puts the rest on its track, or raise the master)")
                               .arg(it.key())
                               .arg(target - it.value(), 0, 'f', 1)
                               .arg(kMaxVolumeDb, 0, 'f', 0)
                               .arg(target - it.value() - kMaxVolumeDb, 0, 'f', 1);
    }
    // Loudness up can push the peaks over 0 dB: say so (the mix of several clips may peak higher still); the
    // limiter, when on, holds them
    if (s.project.timeline().masterLimiter) return;
    const Timeline& after = s.project.timeline();
    for (auto it = truePeaks.begin(); it != truePeaks.end(); ++it) {
        TrackRef where;
        const Clip* c = TimelineOps::findClip(after, it.key(), &where);
        if (!c || Keys::animated(*c, AnimParam::Volume)) continue;
        const double peak = it.value() + c->volumeDb + std::max(0.0, trackGain.value(where.index));
        if (peak > -1.0)
            s.notes << QString("clip %1 now peaks at %2 dBTP (over -1): lower the target or turn on the master "
                               "limiter ({op:'mixer', track:'master', limiter:true}); `loudness` measures the mix")
                           .arg(it.key())
                           .arg(peak, 0, 'f', 1);
    }
}

// Game sound: a track whose clips all play the game of a recording - a game stream of a recording with several
// (OBS, Clipi: "System Output", not "Mikrofon", "Discord", "Spotify"), or any stream but voice, chat and music of a
// file in a game's folder or named after it (cs2_clip_…). Voice, talk and everything else are not.
bool gameSoundTrack(Session& s, const Track& t)
{
    bool any = false;
    for (const Clip& c : t.clips) {
        if (c.kind != ClipKind::Media || !c.enabled || c.mediaPath.isEmpty()) continue;
        const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
        QStringList titles;
        if (m)
            for (const AudioStreamInfo& a : m->audioStreams) titles << a.title;
        const QString title = titles.value(c.audioStream);
        const bool game = StreamPresets::gameStreams(titles).contains(c.audioStream)
                          || (GameHud::guess(c.mediaPath) && !Transcript::isVoiceName(title)
                              && !StreamPresets::isChatOrMusicTitle(title));
        if (!game) return false;
        any = true;
    }
    return any;
}

// Ducking: lower the music (target clips) while the voice/game tracks (`under`) have sound, with volume keyframes
// (ramps of `attack` before and `release` after each sound; pauses shorter than `hold` stay ducked)
void opDuck(Session& s, const QJsonObject& op)
{
    const ProjectFormat& f = s.format();
    const Timeline& cur = s.project.timeline();
    QVector<int> ids;
    if (op.contains("track")) {
        TrackKind k = TrackKind::Video;
        const int index = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio || index >= cur.audio.size()) fail("BAD_ARGUMENT", "track must be an audio track like A2");
        for (const Clip& c : cur.audio[index].clips) ids << c.id;
        if (ids.isEmpty()) fail("BAD_ARGUMENT", "the track has no clips");
    } else {
        ids = clipsOn(s, op, TrackKind::Audio, "ducking");
    }
    if (op.value("remove").toBool()) { // ducking off: the volume keyframes go, the clip volume stays
        const int n = s.editor.unduck(ids);
        s.notes << QString("removed the volume keyframes (ducking) of %1 of %2 clip(s)").arg(n).arg(ids.size());
        return;
    }
    // The tracks whose sound lowers it: by (a list ["A1","A3"] or "A1,A3"; 'under' is the same), default all
    // other audible tracks with clips (solo wins), as the Studio page does
    if (op.contains("by") && op.contains("under")) fail("BAD_ARGUMENT", "give 'by' or 'under', not both");
    QJsonValue by = op.contains("by") ? op.value("by") : op.value("under");
    if (by.isArray()) {
        QStringList names;
        for (const QJsonValue& v : by.toArray()) names << (v.isDouble() ? QString("A%1").arg(v.toInt()) : v.toString());
        by = names.join(',');
    }
    const Timeline tl = s.project.renderTimeline();
    QVector<int> voice;
    if (!by.isUndefined() && !by.isNull()) {
        voice = audioTracksOf(s.project, tl, by);
        if (voice.isEmpty()) fail("BAD_ARGUMENT", "'by' names no track");
    } else {
        const bool solo = std::any_of(cur.audio.begin(), cur.audio.end(), [](const Track& t) { return t.solo; });
        QStringList names;
        for (int i = 0; i < cur.audio.size(); ++i) {
            const Track& t = cur.audio[i];
            const bool music = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return ids.contains(c.id); });
            if (music || t.clips.isEmpty() || (solo ? !t.solo : t.muted)) continue;
            voice << i;
            names << QString("A%1").arg(i + 1);
        }
        if (voice.isEmpty()) fail("BAD_ARGUMENT", "no other audible track with clips to duck under; give 'by'");
        by = names.join(',');
    }
    for (int id : ids)
        for (const int v : voice)
            for (const Clip& c : cur.audio.value(v).clips)
                if (c.id == id) fail("BAD_ARGUMENT", QString("clip %1 is on a track in 'under' itself").arg(id));
    Ducking::Params params = Ducking::defaults(f.rate.fps());
    if (op.contains("amount_db")) params.amountDb = numberOf(op, "amount_db", -60, 0);
    // threshold_db: a number for all of 'under', or 'auto' (default): -35 dBFS for voice and recorded sound, for game
    // sound its own loud moments (Ducking::gameThreshold)
    const QJsonValue given = op.value("threshold_db");
    const bool autoThreshold = given.isUndefined() || given.isNull()
                               || (given.isString() && given.toString().trimmed().compare("auto", Qt::CaseInsensitive) == 0);
    if (!autoThreshold) params.thresholdDb = numberOf(op, "threshold_db", -80, 0);
    if (op.contains("attack")) params.attack = timeOf(s, op, "attack");
    if (op.contains("release")) params.release = timeOf(s, op, "release");
    if (op.contains("hold")) params.hold = timeOf(s, op, "hold");
    if (TimelineOps::endFrame(tl) <= 0) s.failEmpty();
    QVector<int> game, rest = voice;
    if (autoThreshold)
        for (const int v : voice)
            if (gameSoundTrack(s, cur.audio.value(v))) game << v;
    for (const int g : game) rest.removeAll(g);
    const auto names = [](const QVector<int>& l) {
        QStringList n;
        for (const int i : l) n << QString("A%1").arg(i + 1);
        return n.join(',');
    };
    std::optional<Ducking::Ranges> ranges = Ducking::Ranges{};
    if (!rest.isEmpty()) ranges = AudioAnalysis::soundRanges(f, tl, rest, params);
    double gameDb = params.thresholdDb;
    if (ranges && !game.isEmpty()) {
        const auto peaks = AudioAnalysis::tracksFramePeaks(f, tl, game);
        if (!peaks) ranges.reset();
        else {
            Ducking::Params gp = params;
            gp.thresholdDb = gameDb = Ducking::gameThreshold(*peaks);
            ranges = Ducking::unite(*ranges, Ducking::soundRanges(*peaks, 0, gp), params.hold + params.attack + params.release);
        }
    }
    if (!ranges) fail("DECODE_FAILED", "could not mix the sound");
    if (!op.value("replace").toBool(true)) // keep clips that already have volume keyframes
        ids.removeIf([&](int id) {
            const Clip* c = TimelineOps::findClip(cur, id);
            return c && Keys::animated(*c, AnimParam::Volume);
        });
    const int ducked = s.editor.duck(ids, *ranges, params);
    // How long the sound counts: one range over everything means the music is simply lower all the time
    int covered = 0;
    for (const auto& [from, to] : *ranges) covered += to - from;
    const int length = TimelineOps::endFrame(tl);
    const double fps = f.rate.fps();
    QString threshold = QString("threshold_db %1").arg(params.thresholdDb, 0, 'f', 0);
    if (!game.isEmpty())
        threshold = rest.isEmpty() ? QString("threshold_db %1 (auto: the loud moments of the game sound %2)")
                                         .arg(gameDb, 0, 'f', 1)
                                         .arg(names(game))
                                   : QString("threshold_db %1 for %2, %3 (auto: the loud moments) for the game sound %4")
                                         .arg(params.thresholdDb, 0, 'f', 0)
                                         .arg(names(rest))
                                         .arg(gameDb, 0, 'f', 1)
                                         .arg(names(game));
    QString note = QString("ducked %1 of %2 clip(s) by %3 dB during %4 sound range(s) of %5: %6 s of %7 s above %8")
                       .arg(ducked)
                       .arg(ids.size())
                       .arg(params.amountDb, 0, 'f', 1)
                       .arg(ranges->size())
                       .arg(by.toString())
                       .arg(covered / fps, 0, 'f', 1)
                       .arg(length / fps, 0, 'f', 1)
                       .arg(threshold);
    if (length > 0 && covered > 0.6 * length && game.isEmpty())
        note += " - the sound is above it most of the time; a higher threshold_db (e.g. -20) ducks only the loud "
                "moments (shots, words)";
    s.notes << note;
}

// "Stimme säubern" in one call: voice cleaning on clips or whole audio tracks, or off again
void opCleanVoice(Session& s, const QJsonObject& op)
{
    const Timeline& tl = s.project.timeline();
    QVector<int> ids;
    if (op.contains("track") || op.contains("tracks")) {
        QJsonArray given = op.value("tracks").toArray();
        if (given.isEmpty()) given = op.value("track").isArray() ? op.value("track").toArray() : QJsonArray{op.value("track")};
        for (const QJsonValue& v : given) {
            TrackKind k = TrackKind::Video;
            const int index = parseTrack(v, &k);
            if (k != TrackKind::Audio || index >= tl.audio.size())
                fail("BAD_ARGUMENT", "track must be an audio track like A2, got " + v.toVariant().toString());
            if (tl.audio[index].locked) fail("FAILED", trackName({TrackKind::Audio, index}) + " is locked");
            ids += s.editor.audioTrackClips(index);
        }
        if (ids.isEmpty()) fail("BAD_ARGUMENT", "the track has no clips");
    } else {
        ids = clipsOn(s, op, TrackKind::Audio, "voice cleaning");
    }
    if (op.value("remove").toBool()) {
        s.editor.removeEffect(ids, "voiceclean");
        return;
    }
    QVariantMap values;
    if (op.contains("amount")) values["amount"] = numberOf(op, "amount", 0, 100);
    if (op.contains("room")) values["room"] = numberOf(op, "room", VoiceClean::kRoomMinDb, VoiceClean::kRoomMaxDb);
    for (const char* key : {"gate", "clicks", "shape"})
        if (op.contains(key)) {
            if (!op.value(key).isBool()) fail("BAD_ARGUMENT", QString("'%1' must be true or false").arg(key));
            values[key] = op.value(key).toBool();
        }
    if (op.contains("eq") && !op.value("eq").isBool()) fail("BAD_ARGUMENT", "'eq' must be true or false");
    const bool voiceEq = op.value("eq").toBool();
    const QVector<int> done = s.editor.cleanVoice(ids, values, voiceEq);
    s.notes << QString("voice cleaning on %1 audio clip(s); the sound is %2 ms late inside the effect, compensated "
                       "(stays in sync)")
                   .arg(done.size())
                   .arg(std::lround(VoiceClean::latencyFor(48000) * 1000.0 / 48000));
    if (voiceEq && !done.isEmpty()) s.notes << QString("EQ preset 'voice' on the clips' audio track(s)");
}

// ---------- speak ----------

// The speech request of a 'speak' op (voice resolved: an installed id or a file; empty voice = default)
Tts::Request speechRequest(const QString& projectPath, const QJsonObject& op)
{
    Tts::Request r;
    r.text = op.value("text").toString();
    if (Speech::normalizedText(r.text).isEmpty()) fail("BAD_ARGUMENT", "missing 'text': what the voice says");
    if (op.contains("speed")) r.options.speed = numberOf(op, "speed", Speech::kMinSpeed, Speech::kMaxSpeed);
    if (op.contains("pause")) r.options.pause = numberOf(op, "pause", 0, Speech::kMaxPause);
    QString voice = op.value("voice").toString().trimmed();
    if (voice.contains('/') || voice.contains('\\') || voice.endsWith(".onnx", Qt::CaseInsensitive)) voice = absolute(voice);
    else if (!voice.isEmpty()) { // short names ("ramona", "de_DE-ramona") -> the id
        const QVector<const Extensions::Item*> items = Extensions::parseList(voice);
        if (items.size() == 1 && items.first()->kind == Extensions::Kind::Voice) voice = items.first()->id;
    }
    r.options.voice = voice.isEmpty() ? Tts::defaultVoice() : voice;
    if (const QString why = Tts::notReady(r.options.voice); !why.isEmpty()) {
        QStringList voices;
        for (const Extensions::Item& i : Extensions::catalog())
            if (i.kind == Extensions::Kind::Voice) voices << Extensions::voiceName(i.id);
        fail("NOT_INSTALLED", why + " - `extensions` with install:'piper," + voices.value(0, "de_DE-ramona")
                                  + "' (voices: " + voices.join(", ") + ")");
    }
    r.folder = Speech::folder(projectPath);
    r.words = op.contains("captions") && op.value("captions") != QJsonValue(false);
    return r;
}

Tts::Result makeSpeech(const Tts::Request& r)
{
    Tts::Result result;
    QString error;
    if (!Tts::speak(r, &result, &error)) fail("TTS_FAILED", error);
    return result;
}

// Text -> spoken voice clip (piper) on an audio track, optionally word captions of it
void opSpeak(Session& s, const QJsonObject& op)
{
    const Tts::Request req = speechRequest(s.path, op);
    const Tts::Result speech = makeSpeech(req); // made by schneidi-cli already when live (prepareSpeech): cached
    const MediaInfo& m = s.media(speech.path);
    const int at = op.contains("at") ? timeOf(s, op, "at") : s.app.value("playhead").toInt(0);
    int track = -1;
    if (op.contains("track")) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
    }
    track = Speech::freeAudioTrack(s.project.timeline(), at, at + m.length, track);
    const int len = s.editor.placeSourceRange(m.path, 0, m.length - 1, at, track);
    if (len <= 0) fail("FAILED", QString("could not place the speech on A%1 (locked?)").arg(track + 1));
    s.notes << QString("speech %1 (%2 s, voice %3%4) on A%5 at %6")
                   .arg(QFileInfo(speech.path).fileName())
                   .arg(speech.durationMs / 1000.0, 0, 'f', 2)
                   .arg(Extensions::voiceName(speech.voice), speech.cached ? ", made before" : "")
                   .arg(track + 1)
                   .arg(tc(at, s.format()));
    if (!req.words) return;
    // The words as the media transcript (Transcript panel, op 'captions' later) and as captions now
    const QString lang = Speech::voiceLanguage(Extensions::voiceName(speech.voice));
    s.project.setTranscript(m.path, MediaTranscript{0, lang, speech.words});
    const double fps = s.project.frameRate();
    QJsonArray words;
    for (const TranscriptWord& w : speech.words) {
        const int from = at + int(std::lround(w.from * fps / 1000.0));
        words << QJsonObject{{"from", from}, {"to", std::max(from + 1, at + int(std::lround(w.to * fps / 1000.0)))},
                             {"text", w.text}};
    }
    const QJsonValue style = op.value("captions");
    QJsonObject c{{"op", "captions"}, {"words", words}, {"style", style.isString() ? style.toString() : QString("karaoke")}};
    for (const char* key : {"max_words", "name"})
        if (op.contains(key)) c[key] = op.value(key);
    captionsOp(s, c);
    s.notes << (speech.wordSource == "whisper" ? QString("caption word times measured by whisper")
                : speech.wordSource == "whisper-span"
                    ? QString("caption word times estimated between the start and end of speech whisper measured (it "
                              "heard other words than the text: numbers, names)")
                    : QString("caption word times estimated from the text (install whisper and a model for measured "
                              "times: `extensions` install:'whisper,small')"));
}

// ---------- voiceover ----------

// A recorded voice (the app records it; here any audio file) at `at` onto an audio track: the given one (like an armed
// track: replaces what lies there), else the app's rule (Voiceover::recordTrack)
// A take that runs past the picture's end (duel 17.11.: 0.2 s of black at the end): like op 'sfx' it must not make
// black frames. Silence there (the pause a take ends with) is cut off at the picture's end; spoken words are never
// cut - then the notes say how much runs past and what to do (the `check` reports the black frames too)
void voiceoverEnd(Session& s, int clipId, int pictureEnd)
{
    const Clip* found = TimelineOps::findClip(s.project.timeline(), clipId);
    if (!found || pictureEnd <= 0 || found->end() <= pictureEnd) return;
    const Clip c = *found;
    const ProjectFormat& f = s.format();
    if (c.start >= pictureEnd) { // all of it after the picture: only black under it
        s.notes << QString("voiceover: the take lies entirely after the picture's end (%1) - black frames under all of "
                           "it: place it earlier or put a picture under it")
                       .arg(tc(pictureEnd, f));
        return;
    }
    const int over = c.end() - pictureEnd;
    const auto peaks = AudioAnalysis::clipFramePeaks(f, c);
    // Silence relative to the take (a quiet take's words are still words): 40 dB under its peak, at most -45 dBFS;
    // from 2 frames before the picture's end on (a word ending right at the cut keeps its tail)
    constexpr int kMargin = 2;
    float peak = 0;
    if (peaks)
        for (float v : *peaks) peak = std::max(peak, v);
    const float quiet = std::min(0.0056f, peak * 0.01f);
    const size_t first = size_t(std::max(0, pictureEnd - kMargin - c.start));
    bool silent = peaks.has_value() && peak > 0 && first < peaks->size();
    if (silent)
        for (size_t i = first; i < peaks->size(); ++i) silent = silent && (*peaks)[i] < quiet;
    const QString seconds = QString::number(over / f.rate.fps(), 'f', 2);
    if (silent) {
        s.editor.trimClip(clipId, TimelineOps::Edge::End, -over);
        s.notes << QString("voiceover: its last %1 s (silence) ran past the picture's end - cut off there (no black frames)")
                       .arg(seconds);
    } else {
        s.notes << QString("voiceover: the take runs %1 s past the picture's end (%2) - black frames there: lengthen the "
                           "last picture (trim its end, a still or a title) or place the take earlier")
                       .arg(seconds, tc(pictureEnd, f));
    }
}

void opVoiceover(Session& s, const QJsonObject& op)
{
    const QString file = op.value("file").toString();
    if (file.isEmpty()) fail("BAD_ARGUMENT", "missing 'file': the recorded audio file");
    const MediaInfo& m = s.media(absolute(file));
    if (!m.hasAudio || m.length <= 0) fail("BAD_ARGUMENT", "the file has no sound: " + file);
    const int at = op.contains("at") ? timeOf(s, op, "at") : s.app.value("playhead").toInt(0);
    const Timeline& tl = s.project.timeline();
    int track = -1;
    if (op.value("track").toString() == "new") {
        track = int(tl.audio.size());
    } else if (op.contains("track")) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …) or 'new'");
        if (track < tl.audio.size() && tl.audio[track].locked) fail("FAILED", trackName({TrackKind::Audio, track}) + " is locked");
    } else {
        track = Voiceover::recordTrack(tl, -1, -1, at, at + m.length, Voiceover::folder(s.path));
    }
    const int before = int(tl.audio.size());
    // The picture's end before the take (a take past it would leave black frames there)
    int pictureEnd = 0;
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips) pictureEnd = std::max(pictureEnd, c.end());
    int placed = -1;
    const int id = s.editor.placeRecording(m.path, track, at, at, &placed);
    if (!id) fail("FAILED", "could not place the recording");
    s.notes << QString("voiceover %1 (%2 s) on %3%4 at %5")
                   .arg(QFileInfo(m.path).fileName())
                   .arg(m.length / s.project.frameRate(), 0, 'f', 2)
                   .arg(trackName({TrackKind::Audio, placed}), placed >= before ? QString(" (a new track)") : QString())
                   .arg(tc(at, s.format()));
    voiceoverEnd(s, id, pictureEnd);
}

} // namespace

QJsonObject prepareSpeech(const QJsonObject& editArgs, Context& ctx)
{
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    bool any = false;
    for (const QJsonValue& v : ops) any = any || v.toObject().value("op").toString() == "speak";
    if (!any) return editArgs;
    try {
        Session s(editArgs.value("project").toString()); // a copy of what the app shows: its path
        for (int i = 0; i < ops.size(); ++i) {
            QJsonObject op = ops[i].toObject();
            if (op.value("op").toString() != "speak") continue;
            const Tts::Request r = speechRequest(s.path, op);
            Tts::Result result;
            QString error;
            Tts::speak(r, &result, &error, ctx.progress);
            op["voice"] = r.options.voice; // the app takes the same voice (its default could differ)
            ops[i] = op;
        }
    } catch (const Error&) {
        return editArgs; // the op reports it where it runs
    }
    QJsonObject out = editArgs;
    out["ops"] = ops;
    return out;
}

void addAudioOps(QVector<OpDef>& ops)
{
    ops << OpDef{"volume", "{op:'volume', clips, db?, pan?}",
                 "clip volume in dB (-60 = silent, max +12) and pan (-100 left … 100 right)",
                 "A clip with volume keyframes (ducked music, a drawn curve) keeps its shape: the whole curve moves so "
                 "that the clip's level becomes db - its loudest key, or its volume when `duck` lowered all of it.",
                 opVolume, kClipKeys}
        << OpDef{"audio_stream", "{op:'audio_stream', clips, stream}",
                 "which audio stream of the file the clips play (0 = first; `probe` lists them)", {}, opAudioStream, kClipKeys}
        << OpDef{"route", "{op:'route', clips, channels, track?, name?}",
                 "channel routing: which channels of their audio stream the clips play (a wireless microphone on "
                 "channels 3/4 of a camera file); with track a copy on that audio track plays them",
                 "channels counted from 1 as `probe` lists them (audio_streams[].channels/layout): [3, 4] or \"3-4\" "
                 "= left and right, 3 = one channel in the middle (mono), \"all\" = every channel again (the "
                 "default). Without track the clips themselves change. track:'A3' or 'new' = a copy of each clip (same "
                 "place, linked with its video) on that audio track playing those channels, the clips stay as they "
                 "are - e.g. {op:'route', clips:[5], channels:[1,2]} then {op:'route', clips:[5], channels:[3,4], "
                 "track:'new', name:'Funk'}. The target track must be free there. name = label of the track. Older "
                 "schneidi versions cannot open a project with routed clips (format 8).",
                 opRoute, kClipKeys}
        << OpDef{"mixer", "{op:'mixer', track:'A1'|'master', volume_db?|target_lufs?, pan?, depth?, center?, lfe_db?, "
                          "mute?, solo?, limiter?, limiter_db?, compressor?, eq?}",
                 "track faders like the Mixer; the master has volume_db, limiter (on/off) and limiter_db (ceiling, "
                 "default -1). target_lufs measures the mix (master) or the track alone and sets the fader so it "
                 "reaches that loudness (e.g. -14 for streaming; turn the limiter on first). Tracks also have a "
                 "compressor and an EQ (before the fader)",
                 "compressor:{on?, threshold_db? (-60..0, default -20), ratio? (1..100, default 4), attack_ms? "
                 "(default 10), release_ms? (default 150), knee_db? (default 6), makeup_db? (default 0)} - tames peaks "
                 "(game sound, shouting); stereo-linked. eq:{on?, bands?:[{band, on?, freq?, gain_db?, q?}]} - 6 bands "
                 "like Fairlight: 1 high pass (off by default), 2 low shelf, 3 and 4 bell, 5 high shelf, 6 low pass "
                 "(off by default); freq 20..20000 Hz, gain_db -24..24 (not for 1 and 6), q 0.1..10; eq:'voice' = the preset "
                 "for speech (as the Studio page's \"Stimme säubern\"). Giving values "
                 "switches the compressor / the band and the EQ on unless 'on' is given. `info` shows both on the track. "
                 "target_lufs with the limiter on: measured after the limiter and stepped until within 0.2 LU; the "
                 "master beyond its +12 dB raises the audible tracks' faders by the same amount (the limiter holds "
                 "the peaks); the notes say what was reached and, when it is not reachable, how far it got (and in numbers: "
                 "{loudness:{of, target_lufs, before_lufs, after_lufs, true_peak_db, limiter}}). "
                 "Surround panner (project in 5.1/7.1, {op:'format', channels}): pan = left/right, depth 0..100 = "
                 "front to surround (7.1: 50 = side, 100 = back), center 0..100 = share of the front pair in the "
                 "centre speaker, lfe_db = send to the LFE (-60 = off, low-passed at 120 Hz). A 5.1/7.1 file's centre, "
                 "LFE and surrounds pass through; the panner places its front pair and mono/stereo clips.",
                 opMixer}
        << OpDef{"track_volume", "{op:'track_volume', track:'A1', points?:[{at, db}], from?, to?, clear?}",
                 "volume automation of an audio track: a curve over the timeline's time on top of the track fader "
                 "(music down under a voice-over without touching the clips)",
                 "points = the curve: at = a time (frames, \"4.5s\", timecode), db = dB on top of the track fader "
                 "(0 = the fader's level, -12 = 12 dB quieter, -72 = silent; what plays is fader + curve, clamped "
                 "to -inf..+12). Linear between points; before the first and after the last their value holds. "
                 "Replaces the whole curve - with from/to only that part: the curve outside stays (a point with the "
                 "old value right before from and after to holds it), e.g. {op:'track_volume', track:'A2', from:'10s', "
                 "to:'20s', points:[{at:'10s', db:-12}, {at:'20s', db:-12}]} = A2 12 dB down from 10 to 20 s. "
                 "clear:true removes the curve (the fader alone again). The curve belongs to the timeline, not to "
                 "clips: moving a clip leaves it; ripple edits (ripple delete, insert, ripple trim) move it with the "
                 "time. The track fader ({op:'mixer', volume_db}) trims the whole curve. `info` lists it on the "
                 "track (volume_automation: [{at, db}]). Clip volume keyframes (op 'volume', 'duck') come on top. "
                 "Older schneidi versions cannot open a project with track automation (format 8). Pan: op 'track_pan'.",
                 opTrackVolume}
        << OpDef{"track_pan", "{op:'track_pan', track:'A1', points?:[{at, pan}], from?, to?, clear?}",
                 "pan automation of an audio track: a curve over the timeline's time on top of the track's pan knob "
                 "(a sound wandering from left to right)",
                 "Works like track_volume: points = the curve, at = a time, pan = on top of the pan knob ({op:'mixer', "
                 "pan}; -100 = left, 0 = the knob, +100 = right; what plays is knob + curve, clamped to -100..+100, "
                 "range -200..+200), [[at, pan]] works too. Linear between points, the first/last value holds; from/to "
                 "replace only that part, clear:true removes the curve (the knob alone again), e.g. {op:'track_pan', "
                 "track:'A1', points:[{at:0, pan:-100}, {at:'4s', pan:100}]} = A1 from left to right in 4 s. Ripple "
                 "edits move it like the volume curve; the pan knob trims the whole curve. In 5.1/7.1 it moves the "
                 "surround panner's left/right (depth, center, lfe stay). `info` lists it on the track "
                 "(pan_automation: [{at, pan}]). Format 8 like track_volume.",
                 opTrackPan}
        << OpDef{"normalize", "{op:'normalize', clips, mode?:'peak'|'lufs', target?, relative?, allow_track_gain?}",
                 "Normalize Audio Levels: sets only the given clips' volume",
                 "Changes only the clips given (with their linked partners unless linked:false): one clip id = that "
                 "clip alone, not its track or the mix. For a whole track give all its clips (`info` lists them); for "
                 "the mix or one track as a whole use {op:'mixer', track:'master'|'A1', target_lufs}. "
                 "mode peak: sample peak to target dBFS (default -1); lufs: integrated loudness to target LUFS (default "
                 "-14, streaming; -23 = EBU R128 broadcast). relative:true moves all clips by the same amount (the "
                 "loudest/whole hits the target), otherwise each clip on its own. allow_track_gain:true: what a clip needs "
                 "beyond the clip volume's +12 dB goes onto its track fader (the track's other clips are lowered by as "
                 "much, so they stay as they were). With the master limiter on, lufs clips whose peaks reach it are "
                 "measured after the limiter (each alone, track and master faders counted as 0 dB like without a "
                 "limiter; relative: all together) and corrected toward the target (never the master); the notes "
                 "say what was reached and why when not. Measures the files (takes a while).",
                 opNormalize, kClipKeys}
        << OpDef{"duck", "{op:'duck', clips|track, by?, under?, amount_db?, threshold_db?|'auto', attack?, release?, hold?, remove?}",
                 "ducking: lower the music while other tracks have sound (volume keyframes)",
                 "clips or track = the music; by = the tracks that duck it ([\"A1\",\"A3\"], \"A1,A3\" or stream/"
                 "track names like \"Mikrofon\"; 'under' is the same), default all other audible tracks with clips "
                 "(solo wins). amount_db: how far down (default -12), threshold_db: what counts as "
                 "sound on 'under' (sample peak in dBFS, or 'auto' = the default: -35 dBFS for voice and recorded "
                 "sound - every word ducks; for the game sound of game recordings (a game's folder or name, their "
                 "game stream) only its loud moments: 2 dB under the level its loudest tenth reaches, -35..-6 - "
                 "shots, explosions, shouts; a number applies to all of 'under'), attack/release: ramp before/after "
                 "a sound, hold: pauses shorter than this stay ducked - these three are times: a plain number is "
                 "frames (project rate), seconds are written \"0.3s\" (defaults 0.15s / 0.4s / 0.5s). "
                 "Replaces the clips' volume "
                 "keyframes; run it again after editing. remove:true takes the ducking off again (the clips' volume "
                 "keyframes go, their volume stays; no 'by' needed).",
                 opDuck, {"clip", "linked", "replace"}} // replace:false keeps clips that have volume keyframes
        << OpDef{"clean_voice", "{op:'clean_voice', clips|track, amount?, gate?, clicks?, shape?, room?, eq?, remove?}",
                 "\"Stimme säubern\": clean speech in one call (voice-overs, TTS, voice chat in game clips) - noise "
                 "and background sounds out (a soft room sound stays), pauses lowered, voice evened out",
                 "Adds the audio effect voiceclean to the clips (a video clip stands for its linked audio) or to every "
                 "clip of the audio tracks in track (\"A2\" or [\"A1\",\"A3\"]); runs again = sets the values. "
                 "amount 0..100 (default 100) = how much of the noise-suppressed sound (neural noise suppression, "
                 "RNNoise); gate (default true) = lower everything between words - turn it off where game sound or "
                 "music under the voice should stay; clicks (default true) = dampen bumps and key/mouse clicks away "
                 "from speech; shape (default true) = high pass, presence, compressor, de-esser and limiter (-1 dBFS), "
                 "the voice gets more even (quiet words up, loud ones down; not louder overall); room -80..-40 (default "
                 "-60) = dBFS of the room sound kept under the voice and in the pauses (the original's noise lowered "
                 "to this, but always by at least 12 dB), so pauses do not go dead silent. For speech only: "
                 "ambience, nature sound or music without a voice is removed as noise. eq:true also puts the EQ preset 'voice' (high "
                 "pass 80 Hz, -2.5 dB at 300 Hz, +1.5 dB presence and air) on the clips' audio tracks, in the same step "
                 "(the Studio page's \"Stimme säubern\"). remove:true takes it off. "
                 "Works at any sample rate, "
                 "offline and the same in preview and export; the effect's ~57 ms delay is compensated (lips stay in "
                 "sync). The same effect is in `effects` (id voiceclean) for the generic effect op.",
                 opCleanVoice, {"clip", "linked", "tracks"}}
        << OpDef{"speak", "{op:'speak', text, voice?, at?, track?, speed?, pause?, captions?, max_words?, name?}",
                 "text to speech: a spoken voice clip (offline, Piper) at `at` on a free audio track",
                 "voice: an installed voice ('de_DE-ramona', 'en_US-ljspeech', 'de_DE-thorsten'; `extensions` lists them, install:'piper,"
                 "de_DE-ramona' installs piper with a voice) or the path of a piper .onnx voice; default: an installed "
                 "voice of the app's language. at: default the app's playhead (0 on a file). track: that audio track "
                 "if free there, else the first free one, else a new one. speed 0.5..2 (default 1), pause = seconds "
                 "after each sentence (default 0.2). The WAV goes into '<project name>-tts/' beside the project; the "
                 "same text, voice and options again take that file (instant). captions:true or a style ('karaoke', "
                 "'word', 'lines') also makes word captions of it (as op 'captions'; max_words, name); the word times "
                 "are measured by whisper when it is installed, else estimated from the text (the notes say which).",
                 opSpeak}
        << OpDef{"voiceover", "{op:'voiceover', file, at?, track?}",
                 "a recorded voice (or any audio file) at `at` on an audio track",
                 "The step the app's voice-over recording ends with, for a take recorded elsewhere. at: default the "
                 "app's playhead (0 on a file). track: an audio track ('A2') = like an armed track in the app: the "
                 "take goes there and replaces what lies in its range (punch-in); a locked one fails. track 'new': a "
                 "new track. No track: the app's rule - the first track named Voiceover (earlier takes in the range "
                 "are replaced), else the first free track without a user-given name (not muted or locked), else one "
                 "new track named Voiceover. Tracks the user named (Discord, Mix …) only get it when given. One "
                 "undo step (track and clip together); the new clip is selected. The notes name the track. A take "
                 "running past the picture's end: its silent tail is cut off there (no black frames at the end); "
                 "spoken words past it are kept and the notes say so (lengthen the last picture or move the take).",
                 opVoiceover};
}

} // namespace Cli::detail
