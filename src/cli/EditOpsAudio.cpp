// schneidi-cli edit operations for sound: clip volume/pan/stream, the mixer (track and master faders, mute, solo,
// limiter) and Normalize Audio Levels (measured like the app's dialog)
#include "cli/CommandsDetail.h"

#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

#include <cmath>

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
            // Volume keyframes: the static volume does nothing, move the whole curve (its loudest key becomes db),
            // like dragging the volume line in the app
            double top = kMinVolumeDb;
            for (const Keyframe& k : c.keys.value(AnimParam::Volume)) top = std::max(top, k.value);
            const double delta = db - top;
            s.editor.modifyClips({id}, "Volume", [delta](Clip& x) {
                for (Keyframe& k : x.keys[AnimParam::Volume]) k.value = std::clamp(k.value + delta, kMinVolumeDb, kMaxVolumeDb);
            });
            s.notes << QString("clip %1 has volume keyframes: the whole curve moved by %2 dB").arg(id).arg(delta, 0, 'f', 1);
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
    }
    s.editor.modifyClips(ids, "Audio stream", [stream](Clip& c) { c.audioStream = stream; });
}

// Fader so that the mix (master) or one track alone reaches target LUFS; the limiter and the +12 dB fader end
// can keep it from getting there, so measure again after setting it (twice at most)
void mixToLoudness(Session& s, int track, double target)
{
    const ProjectFormat& f = s.format();
    AudioAnalysis::MixMeter meter(f);
    for (int round = 0; round < 3; ++round) {
        Timeline tl = s.project.renderTimeline();
        const int end = TimelineOps::endFrame(tl);
        if (end <= 0) fail("EMPTY", "the timeline is empty");
        if (track >= 0)
            for (int i = 0; i < tl.audio.size(); ++i) {
                tl.audio[i].muted = i != track;
                tl.audio[i].solo = false;
            }
        const auto l = meter.measure(tl, 0, end);
        if (!l || l->integrated <= LoudnessMeter::kSilence) fail("FAILED", "no sound to measure");
        const double now = track >= 0 ? s.project.timeline().audio[track].volumeDb : s.project.timeline().masterVolumeDb;
        const double want = std::clamp(now + target - l->integrated, kMinVolumeDb, kMaxVolumeDb);
        if (std::abs(l->integrated - target) < 0.3 || std::abs(want - now) < 0.05 || round == 2) {
            QString note = QString("%1 measured %2 LUFS (target %3), fader %4 dB")
                               .arg(track >= 0 ? QString("A%1").arg(track + 1) : QString("mix"))
                               .arg(l->integrated, 0, 'f', 1)
                               .arg(target, 0, 'f', 1)
                               .arg(now, 0, 'f', 1);
            if (std::abs(l->integrated - target) >= 0.3)
                note += now >= kMaxVolumeDb - 0.05 ? " - the fader is at its +12 dB end"
                                                   : " - the master limiter holds it back (lower the target or raise limiter_db)";
            s.notes << note + QString(", true peak %1 dBTP").arg(l->truePeakDb, 0, 'f', 1);
            return;
        }
        s.project.edit(track >= 0 ? "Mixer" : "Master", [&](Timeline& t) {
            (track >= 0 ? t.audio[track].volumeDb : t.masterVolumeDb) = want;
        });
        // Without the limiter a fader moves the loudness by exactly its change: no need to measure again
        // (measuring a long edit takes a while)
        if (!s.project.timeline().masterLimiter) {
            const double reached = l->integrated + (want - now);
            QString note = QString("%1 was %2 LUFS, fader now %3 dB -> %4 LUFS (target %5)")
                               .arg(track >= 0 ? QString("A%1").arg(track + 1) : QString("mix"))
                               .arg(l->integrated, 0, 'f', 1)
                               .arg(want, 0, 'f', 1)
                               .arg(reached, 0, 'f', 1)
                               .arg(target, 0, 'f', 1);
            const double peak = l->truePeakDb + (want - now);
            if (peak > -1.0)
                note += QString(", true peak now about %1 dBTP: turn on the master limiter "
                                "({op:'mixer', track:'master', limiter:true})").arg(peak, 0, 'f', 1);
            else
                note += QString(", true peak about %1 dBTP").arg(peak, 0, 'f', 1);
            if (want >= kMaxVolumeDb - 0.05 && reached < target - 0.3) note += " - the fader is at its +12 dB end";
            s.notes << note;
            return;
        }
    }
}

void opMixer(Session& s, const QJsonObject& op)
{
    const QString track = op.value("track").toString().trimmed().toLower();
    const bool toLoudness = op.contains("target_lufs");
    const double targetLufs = toLoudness ? numberOf(op, "target_lufs", -70, 0) : 0;
    if (toLoudness && op.contains("volume_db")) fail("BAD_ARGUMENT", "give volume_db or target_lufs, not both");
    if (track == "master") {
        if (op.contains("pan") || op.contains("mute") || op.contains("solo"))
            fail("BAD_ARGUMENT", "the master has volume_db, limiter and limiter_db");
        double db = 0, ceiling = 0;
        if (op.contains("volume_db")) db = numberOf(op, "volume_db", kMinVolumeDb, kMaxVolumeDb);
        if (op.contains("limiter_db")) ceiling = numberOf(op, "limiter_db", -20, 0);
        s.project.edit("Master", [&](Timeline& tl) {
            if (op.contains("volume_db")) tl.masterVolumeDb = db;
            if (op.contains("limiter")) tl.masterLimiter = op.value("limiter").toBool();
            if (op.contains("limiter_db")) tl.masterLimiterDb = ceiling;
        });
        if (toLoudness) mixToLoudness(s, -1, targetLufs);
        return;
    }
    TrackKind k = TrackKind::Video;
    const int index = parseTrack(op.value("track"), &k);
    if (k != TrackKind::Audio || index >= s.project.timeline().audio.size())
        fail("BAD_ARGUMENT", "track must be an audio track like A1 (or 'master')");
    if (op.contains("limiter") || op.contains("limiter_db")) fail("BAD_ARGUMENT", "the limiter is on the master only");
    double db = 0, pan = 0;
    if (op.contains("volume_db")) db = numberOf(op, "volume_db", kMinVolumeDb, kMaxVolumeDb);
    if (op.contains("pan")) pan = numberOf(op, "pan", -100, 100);
    s.project.edit("Mixer", [&](Timeline& tl) {
        Track& t = tl.audio[index];
        if (op.contains("volume_db")) t.volumeDb = db;
        if (op.contains("pan")) t.pan = pan;
        if (op.contains("mute")) t.muted = op.value("mute").toBool();
        if (op.contains("solo")) t.solo = op.value("solo").toBool();
    });
    if (toLoudness) mixToLoudness(s, index, targetLufs);
}

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
    QHash<int, double> levels;
    QHash<int, double> truePeaks;
    std::vector<double> blocks;
    const Timeline tl = s.project.timeline();
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (lufs) {
            if (const auto l = AudioAnalysis::clipLoudness(s.format(), *c)) {
                levels[id] = l->integrated;
                truePeaks[id] = l->truePeakDb;
                blocks.insert(blocks.end(), l->blocks.begin(), l->blocks.end());
            }
        } else if (const auto peak = AudioAnalysis::clipPeakDb(s.format(), *c)) {
            levels[id] = *peak;
        }
    }
    if (levels.isEmpty()) fail("FAILED", "no sound to measure (media missing?)");
    std::optional<double> ref;
    if (lufs) ref = LoudnessMeter::integratedOf(blocks);
    s.editor.normalizeAudio(levels, target, op.value("relative").toBool(), ref);
    // The clip fader ends at +12 dB: quiet clips may not reach the target
    if (!op.value("relative").toBool())
        for (auto it = levels.begin(); it != levels.end(); ++it)
            if (it.value() > -100.0 && target - it.value() > kMaxVolumeDb + 0.05)
                s.notes << QString("clip %1 needs %2 dB but clip volume ends at +%3 dB: it stays %4 dB under the target "
                                   "(raise its track: {op:'mixer', track, target_lufs} or the master)")
                               .arg(it.key())
                               .arg(target - it.value(), 0, 'f', 1)
                               .arg(kMaxVolumeDb, 0, 'f', 0)
                               .arg(target - it.value() - kMaxVolumeDb, 0, 'f', 1);
    // Loudness up can push the peaks over 0 dB: say so (the mix of several clips may peak higher still)
    const Timeline& after = s.project.timeline();
    for (auto it = truePeaks.begin(); it != truePeaks.end(); ++it) {
        const Clip* c = TimelineOps::findClip(after, it.key());
        if (!c || Keys::animated(*c, AnimParam::Volume)) continue;
        const double peak = it.value() + c->volumeDb;
        if (peak > -1.0)
            s.notes << QString("clip %1 now peaks at %2 dBTP (over -1): lower the target or turn on the master "
                               "limiter ({op:'mixer', track:'master', limiter:true}); `loudness` measures the mix")
                           .arg(it.key())
                           .arg(peak, 0, 'f', 1);
    }
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
    if (!op.contains("under")) fail("BAD_ARGUMENT", "missing 'under' (the tracks whose sound lowers the music, e.g. \"A1\")");
    Timeline tl = s.project.renderTimeline();
    const QVector<int> voice = audioTracksOf(s.project, tl, op.value("under"));
    for (int i = 0; i < tl.audio.size(); ++i) {
        tl.audio[i].muted = !voice.contains(i);
        tl.audio[i].solo = false;
    }
    for (int id : ids)
        for (const int v : voice)
            for (const Clip& c : cur.audio.value(v).clips)
                if (c.id == id) fail("BAD_ARGUMENT", QString("clip %1 is on a track in 'under' itself").arg(id));
    const double amount = op.contains("amount_db") ? numberOf(op, "amount_db", -60, 0) : -12.0;
    const double threshold = op.contains("threshold_db") ? numberOf(op, "threshold_db", -80, 0) : -35.0;
    const int attack = op.contains("attack") ? timeOf(s, op, "attack") : int(std::lround(0.15 * f.rate.fps()));
    const int release = op.contains("release") ? timeOf(s, op, "release") : int(std::lround(0.4 * f.rate.fps()));
    const int hold = op.contains("hold") ? timeOf(s, op, "hold") : int(std::lround(0.5 * f.rate.fps()));
    const int end = TimelineOps::endFrame(tl);
    if (end <= 0) fail("EMPTY", "the timeline is empty");
    const auto peaks = AudioAnalysis::timelineFramePeaks(f, tl, 0, end);
    if (!peaks) fail("DECODE_FAILED", "could not mix the sound");
    const float limit = float(std::pow(10.0, threshold / 20.0));
    // Sound ranges [from, to) of the voice tracks; short pauses are bridged
    QVector<QPair<int, int>> ranges;
    for (int i = 0; i < int(peaks->size()); ++i) {
        if ((*peaks)[size_t(i)] < limit) continue;
        if (!ranges.isEmpty() && i - ranges.last().second <= hold + attack + release) ranges.last().second = i + 1;
        else ranges.append({i, i + 1});
    }
    int ducked = 0;
    s.editor.modifyClips(ids, "Ducking", [&](Clip& c) {
        if (Keys::animated(c, AnimParam::Volume) && !op.value("replace").toBool(true)) return;
        Keys::clear(c, AnimParam::Volume);
        const double base = c.volumeDb, low = std::max(kMinVolumeDb, base + amount);
        const int len = c.length();
        bool any = false;
        for (const auto& [from, to] : ranges) {
            const int a = from - c.start, b = to - c.start; // clip frames
            if (b + release <= 0 || a - attack >= len) continue;
            any = true;
            const auto key = [&](int t, double v) {
                if (t >= 0 && t < len) Keys::setKey(c, AnimParam::Volume, t, v);
            };
            key(a - attack, base);
            key(std::max(0, a), low);
            key(std::min(len - 1, b), low);
            key(b + release, base);
            if (a <= 0) Keys::setKey(c, AnimParam::Volume, 0, low);       // starts inside a sound
            if (b >= len - 1) Keys::setKey(c, AnimParam::Volume, len - 1, low); // ends inside one
        }
        if (any) ++ducked;
    });
    s.notes << QString("ducked %1 of %2 clip(s) by %3 dB during %4 sound range(s) of %5")
                   .arg(ducked)
                   .arg(ids.size())
                   .arg(amount, 0, 'f', 1)
                   .arg(ranges.size())
                   .arg(op.value("under").toVariant().toString());
}

} // namespace

void addAudioOps(QVector<OpDef>& ops)
{
    ops << OpDef{"volume", "{op:'volume', clips, db?, pan?}",
                 "clip volume in dB (-60 = silent, max +12) and pan (-100 left … 100 right)", {}, opVolume}
        << OpDef{"audio_stream", "{op:'audio_stream', clips, stream}",
                 "which audio stream of the file the clips play (0 = first; `probe` lists them)", {}, opAudioStream}
        << OpDef{"mixer", "{op:'mixer', track:'A1'|'master', volume_db?|target_lufs?, pan?, mute?, solo?, limiter?, limiter_db?}",
                 "track faders like the Mixer; the master has volume_db, limiter (on/off) and limiter_db (ceiling, "
                 "default -1). target_lufs measures the mix (master) or the track alone and sets the fader so it "
                 "reaches that loudness (e.g. -14 for streaming; turn the limiter on first)",
                 {}, opMixer}
        << OpDef{"normalize", "{op:'normalize', clips, mode?:'peak'|'lufs', target?, relative?}",
                 "Normalize Audio Levels: measure the clips and set their volume",
                 "mode peak: sample peak to target dBFS (default -1); lufs: integrated loudness to target LUFS (default "
                 "-14, streaming; -23 = EBU R128 broadcast). relative:true moves all clips by the same amount (the "
                 "loudest/whole hits the target), otherwise each clip on its own. Measures the files (takes a while).",
                 opNormalize}
        << OpDef{"duck", "{op:'duck', clips|track, under, amount_db?, threshold_db?, attack?, release?, hold?}",
                 "ducking: lower the music while other tracks have sound (volume keyframes)",
                 "clips or track = the music; under = the tracks that duck it (\"A1\", \"A1,A3\" or stream/track "
                 "names like \"Mikrofon\"). amount_db: how far down (default -12), threshold_db: what counts as "
                 "sound on 'under' (default -35 dBFS), attack/release: ramp before/after a sound (default 0.15s / "
                 "0.4s), hold: pauses shorter than this stay ducked (default 0.5s). Replaces the clips' volume "
                 "keyframes; run it again after editing.",
                 opDuck};
}

} // namespace Cli::detail
