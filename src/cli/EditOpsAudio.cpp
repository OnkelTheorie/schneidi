// schneidi-cli edit operations for sound: clip volume/pan/stream, the mixer (track and master faders, mute, solo,
// limiter) and Normalize Audio Levels (measured like the app's dialog)
#include "cli/CommandsDetail.h"

#include "core/Loudness.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <cmath>

namespace Cli::detail {

namespace {

void opVolume(Session& s, const QJsonObject& op)
{
    if (!op.contains("db") && !op.contains("pan")) fail("BAD_ARGUMENT", "missing 'db' (and/or 'pan')");
    const QVector<int> ids = onTracks(s, clipIds(s, op), TrackKind::Audio, "volume and pan");
    if (op.contains("db")) {
        const double db = numberOf(op, "db", -1000, kMaxVolumeDb);
        for (int id : ids) s.editor.setClipVolume(id, db);
    }
    if (op.contains("pan")) {
        const double pan = numberOf(op, "pan", -100, 100);
        s.editor.modifyClips(ids, "Pan", [pan](Clip& c) { c.pan = pan; });
    }
}

void opAudioStream(Session& s, const QJsonObject& op)
{
    const int stream = int(numberOf(op, "stream", 0, 63));
    const QVector<int> ids = onTracks(s, clipIds(s, op), TrackKind::Audio, "audio streams");
    for (int id : ids) {
        const Clip& c = *TimelineOps::findClip(s.project.timeline(), id);
        const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
        if (!m || stream >= m->audioStreamCount())
            fail("BAD_ARGUMENT", QString("clip %1 has %2 audio stream(s); stream counts from 0").arg(id).arg(m ? m->audioStreamCount() : 0));
    }
    s.editor.modifyClips(ids, "Audio stream", [stream](Clip& c) { c.audioStream = stream; });
}

void opMixer(Session& s, const QJsonObject& op)
{
    const QString track = op.value("track").toString().trimmed().toLower();
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
}

void opNormalize(Session& s, const QJsonObject& op)
{
    const QString mode = op.value("mode").toString("peak");
    if (mode != "peak" && mode != "lufs") fail("BAD_ARGUMENT", "mode must be 'peak' (sample peak, dBFS) or 'lufs' (loudness)");
    const bool lufs = mode == "lufs";
    const double target = op.contains("target") ? numberOf(op, "target", lufs ? -70 : -60, 0) : (lufs ? -14.0 : -1.0);
    const QVector<int> ids = onTracks(s, clipIds(s, op), TrackKind::Audio, "normalizing");
    // Measure every clip's sound (without its volume), like the app; relative: one shared reference
    QHash<int, double> levels;
    std::vector<double> blocks;
    const Timeline tl = s.project.timeline();
    for (int id : ids) {
        const Clip* c = TimelineOps::findClip(tl, id);
        if (lufs) {
            if (const auto l = AudioAnalysis::clipLoudness(s.format(), *c)) {
                levels[id] = l->integrated;
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
}

} // namespace

void addAudioOps(QVector<OpDef>& ops)
{
    ops << OpDef{"volume", "{op:'volume', clips, db?, pan?}",
                 "clip volume in dB (-60 = silent, max +12) and pan (-100 left … 100 right)", {}, opVolume}
        << OpDef{"audio_stream", "{op:'audio_stream', clips, stream}",
                 "which audio stream of the file the clips play (0 = first; `probe` lists them)", {}, opAudioStream}
        << OpDef{"mixer", "{op:'mixer', track:'A1'|'master', volume_db?, pan?, mute?, solo?, limiter?, limiter_db?}",
                 "track faders like the Mixer; the master has volume_db, limiter (on/off) and limiter_db (ceiling, "
                 "default -1)",
                 {}, opMixer}
        << OpDef{"normalize", "{op:'normalize', clips, mode?:'peak'|'lufs', target?, relative?}",
                 "Normalize Audio Levels: measure the clips and set their volume",
                 "mode peak: sample peak to target dBFS (default -1); lufs: integrated loudness to target LUFS (default "
                 "-14, streaming; -23 = EBU R128 broadcast). relative:true moves all clips by the same amount (the "
                 "loudest/whole hits the target), otherwise each clip on its own. Measures the files (takes a while).",
                 opNormalize};
}

} // namespace Cli::detail
