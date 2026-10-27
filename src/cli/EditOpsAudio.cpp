// schneidi-cli edit operations for sound: clip volume/pan/stream, the mixer (track and master faders, mute, solo,
// limiter) and Normalize Audio Levels (measured like the app's dialog)
#include "cli/CommandsDetail.h"

#include "core/Dynamics.h"
#include "core/Equalizer.h"
#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <tuple>

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
        s.editor.setTrackVolume(track, want);
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
    if (!v.isObject()) fail("BAD_ARGUMENT", "'eq' must be an object like {on:true, bands:[{band:3, freq:2500, gain_db:3}]}");
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
        if (op.contains("pan") || op.contains("mute") || op.contains("solo") || op.contains("compressor") || op.contains("eq"))
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
    const int index = parseTrack(op.value("track"), &k);
    if (k != TrackKind::Audio || index >= s.project.timeline().audio.size())
        fail("BAD_ARGUMENT", "track must be an audio track like A1 (or 'master')");
    if (op.contains("limiter") || op.contains("limiter_db")) fail("BAD_ARGUMENT", "the limiter is on the master only");
    double db = 0, pan = 0;
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
            if (it.value() > 0.05)
                s.notes << QString("A%1: track fader +%2 dB (allow_track_gain), its other clips lowered by as much")
                               .arg(it.key() + 1)
                               .arg(it.value(), 0, 'f', 1);
    }
    // The clip fader ends at +12 dB: quiet clips may not reach the target
    if (!relative)
        for (auto it = levels.begin(); it != levels.end(); ++it)
            if (it.value() > -100.0 && target - it.value() > kMaxVolumeDb + 0.05)
                s.notes << QString("clip %1 needs %2 dB but clip volume ends at +%3 dB: it stays %4 dB under the target "
                                   "(allow_track_gain:true puts the rest on its track, or raise the master)")
                               .arg(it.key())
                               .arg(target - it.value(), 0, 'f', 1)
                               .arg(kMaxVolumeDb, 0, 'f', 0)
                               .arg(target - it.value() - kMaxVolumeDb, 0, 'f', 1);
    // Loudness up can push the peaks over 0 dB: say so (the mix of several clips may peak higher still)
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
    const Timeline tl = s.project.renderTimeline();
    const QVector<int> voice = audioTracksOf(s.project, tl, op.value("under"));
    for (int id : ids)
        for (const int v : voice)
            for (const Clip& c : cur.audio.value(v).clips)
                if (c.id == id) fail("BAD_ARGUMENT", QString("clip %1 is on a track in 'under' itself").arg(id));
    Ducking::Params params = Ducking::defaults(f.rate.fps());
    if (op.contains("amount_db")) params.amountDb = numberOf(op, "amount_db", -60, 0);
    if (op.contains("threshold_db")) params.thresholdDb = numberOf(op, "threshold_db", -80, 0);
    if (op.contains("attack")) params.attack = timeOf(s, op, "attack");
    if (op.contains("release")) params.release = timeOf(s, op, "release");
    if (op.contains("hold")) params.hold = timeOf(s, op, "hold");
    if (TimelineOps::endFrame(tl) <= 0) fail("EMPTY", "the timeline is empty");
    const auto ranges = AudioAnalysis::soundRanges(f, tl, voice, params);
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
    QString note = QString("ducked %1 of %2 clip(s) by %3 dB during %4 sound range(s) of %5: %6 s of %7 s above "
                           "threshold_db %8")
                       .arg(ducked)
                       .arg(ids.size())
                       .arg(params.amountDb, 0, 'f', 1)
                       .arg(ranges->size())
                       .arg(op.value("under").toVariant().toString())
                       .arg(covered / fps, 0, 'f', 1)
                       .arg(length / fps, 0, 'f', 1)
                       .arg(params.thresholdDb, 0, 'f', 0);
    if (length > 0 && covered > 0.6 * length)
        note += " - the sound is above it most of the time; a higher threshold_db (e.g. -20) ducks only the loud "
                "moments (shots, words)";
    s.notes << note;
}

} // namespace

void addAudioOps(QVector<OpDef>& ops)
{
    ops << OpDef{"volume", "{op:'volume', clips, db?, pan?}",
                 "clip volume in dB (-60 = silent, max +12) and pan (-100 left … 100 right)", {}, opVolume}
        << OpDef{"audio_stream", "{op:'audio_stream', clips, stream}",
                 "which audio stream of the file the clips play (0 = first; `probe` lists them)", {}, opAudioStream}
        << OpDef{"mixer", "{op:'mixer', track:'A1'|'master', volume_db?|target_lufs?, pan?, mute?, solo?, limiter?, "
                          "limiter_db?, compressor?, eq?}",
                 "track faders like the Mixer; the master has volume_db, limiter (on/off) and limiter_db (ceiling, "
                 "default -1). target_lufs measures the mix (master) or the track alone and sets the fader so it "
                 "reaches that loudness (e.g. -14 for streaming; turn the limiter on first). Tracks also have a "
                 "compressor and an EQ (before the fader)",
                 "compressor:{on?, threshold_db? (-60..0, default -20), ratio? (1..100, default 4), attack_ms? "
                 "(default 10), release_ms? (default 150), knee_db? (default 6), makeup_db? (default 0)} - tames peaks "
                 "(game sound, shouting); stereo-linked. eq:{on?, bands?:[{band, on?, freq?, gain_db?, q?}]} - 6 bands "
                 "like Fairlight: 1 high pass (off by default), 2 low shelf, 3 and 4 bell, 5 high shelf, 6 low pass "
                 "(off by default); freq 20..20000 Hz, gain_db -24..24 (not for 1 and 6), q 0.1..10. Giving values "
                 "switches the compressor / the band and the EQ on unless 'on' is given. `info` shows both on the track.",
                 opMixer}
        << OpDef{"normalize", "{op:'normalize', clips, mode?:'peak'|'lufs', target?, relative?, allow_track_gain?}",
                 "Normalize Audio Levels: measure the clips and set their volume",
                 "mode peak: sample peak to target dBFS (default -1); lufs: integrated loudness to target LUFS (default "
                 "-14, streaming; -23 = EBU R128 broadcast). relative:true moves all clips by the same amount (the "
                 "loudest/whole hits the target), otherwise each clip on its own. allow_track_gain:true: what a clip needs "
                 "beyond the clip volume's +12 dB goes onto its track fader (the track's other clips are lowered by as "
                 "much, so they stay as they were). Measures the files (takes a while).",
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
