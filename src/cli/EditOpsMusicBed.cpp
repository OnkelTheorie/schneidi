// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli: edit op `music_bed` - background music of schneidi's own (core/MusicBed: moods hype, lofi, epic, funk)
// made in the exact length, written as a WAV next to the project ("<name>-music/", like the speech of op 'speak'),
// placed on a music track (the earlier bed's, an empty "Musik" or a new one) and ducked under voice and recorded
// sound (op 'duck'). Its beat grid goes
// into the beats cache of the file (songBeats), so snap_to_beat, fit_music and `beats` take it without analysing.
// The same options give the same file name and samples: a bed made before is reused. Live, schneidi-cli makes the
// file before the edit goes to the app (prepareMusicBed), so the app does not compute.
#include "cli/CommandsDetail.h"

#include "core/AppPaths.h"
#include "core/I18n.h"
#include "core/MusicBed.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <new>
#include <system_error>

namespace Cli::detail {

namespace {

double round3(double v) { return std::round(v * 1000) / 1000; }

// Folder for the beds of a project: "<project folder>/<project name>-music" next to a saved project, else "music" in
// the app's data folder (an unsaved project has no folder; the files must outlive any cache)
QString bedFolder(const QString& projectPath)
{
    if (projectPath.isEmpty() || projectPath.startsWith('@'))
        return QDir(AppPaths::writable(QStandardPaths::AppDataLocation)).filePath("music");
    const QFileInfo fi(projectPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + "-music");
}

QString moodList()
{
    return MusicBed::moodIds().join(", ");
}

// The options of the op (length in timeline frames from `at`: *at, *frames)
MusicBed::Options bedOptions(Session& s, const QJsonObject& op, int* at, int* frames)
{
    MusicBed::Options o;
    if (!op.contains("mood")) fail("BAD_ARGUMENT", "missing 'mood': " + moodList() + " (`effects` kind:'music' lists them)");
    const std::optional<MusicBed::Mood> mood = MusicBed::moodFrom(op.value("mood").toString());
    if (!mood) fail("BAD_ARGUMENT", "mood: " + moodList() + ", got '" + op.value("mood").toVariant().toString() + "'");
    o.mood = *mood;
    if (op.contains("bpm")) o.bpm = numberOf(op, "bpm", MusicBed::kMinBpm, MusicBed::kMaxBpm);
    if (op.contains("key")) {
        o.key = MusicBed::keyFrom(op.value("key").toString());
        if (!o.key) fail("BAD_ARGUMENT", "key: a note and a mode like \"A minor\", \"F#m\", \"Eb major\" or \"C\"");
    }
    if (op.contains("seed")) o.seed = unsigned(numberOf(op, "seed", 1, 1000000));
    const double fps = s.format().rate.fps();
    *at = op.contains("at") ? timeOf(s, op, "at") : 0;
    if (*at < 0) fail("BAD_ARGUMENT", "'at' must not be before 0");
    const QJsonValue length = op.value("length");
    if (length.isUndefined() || length.toString() == "timeline") {
        *frames = TimelineOps::endFrame(s.project.timeline()) - *at;
        if (*frames <= 0)
            fail("EMPTY", "length:'timeline' (the default): the timeline has nothing after 'at' to take the length "
                          "from - give a length (\"30s\")");
    } else {
        *frames = spanOf(s, op, "length");
    }
    checkFrameLimit(qint64(*at) + std::max(0, *frames), "music_bed");
    const double seconds = *frames / fps;
    const double halfFrame = 0.5 / fps; // the limits in whole frames (600 s at 23.976 fps: 14386 frames)
    if (seconds < MusicBed::kMinLength - halfFrame || seconds > MusicBed::kMaxLength + halfFrame)
        fail("BAD_ARGUMENT", QString("length: %1 to %2 s, got %3 s")
                                 .arg(MusicBed::kMinLength)
                                 .arg(MusicBed::kMaxLength)
                                 .arg(seconds, 0, 'f', 2));
    o.length = seconds;
    return o;
}

bool isMusicName(const QString& name)
{
    const QString t = name.trimmed();
    return !t.isEmpty()
           && (t.compare(T("Musik"), Qt::CaseInsensitive) == 0 || t.compare("Musik", Qt::CaseInsensitive) == 0
               || t.compare("Music", Qt::CaseInsensitive) == 0);
}

bool isBedFile(const QString& path) { return QFileInfo(path).fileName().startsWith("music-bed-"); }

// How far a new bed sits under the program sound (LU): background, not a second foreground
constexpr double kLevelBelowLu = 7;

// The audio tracks whose sound lowers the bed: voice and recorded sound. Not the bed's own track, muted ones (solo
// wins), the effect sounds (op 'sfx'), tracks named "Musik"/"Music", nor tracks of only audio-only songs (other beds,
// files whose beats were analysed: fit_music, snap_to_beat)
QStringList duckUnder(Session& s, int bedTrack)
{
    const Timeline& tl = s.project.timeline();
    const bool solo = std::any_of(tl.audio.begin(), tl.audio.end(), [](const Track& t) { return t.solo; });
    QStringList names;
    for (int i = 0; i < tl.audio.size(); ++i) {
        const Track& t = tl.audio[i];
        if (i == bedTrack || t.clips.isEmpty() || (solo ? !t.solo : t.muted)) continue;
        if (t.name == kSfxTrackName || isMusicName(t.name)) continue;
        bool any = false, songs = true;
        for (const Clip& c : t.clips) {
            if (c.kind != ClipKind::Media || !c.enabled) continue;
            any = true;
            if (s.editor.hasPartner(c.id) || !(isBedFile(c.mediaPath) || cachedSongBeats(c.mediaPath, c.audioStream))) {
                songs = false;
                break;
            }
        }
        if (any && songs) continue;
        names << QString("A%1").arg(i + 1);
    }
    return names;
}

struct Bed {
    QString path;
    MusicBed::Result info; // without samples when it was made before
    bool made = false;
    double ms = 0; // time to make it
};

// The bed's file, made unless it is there with its grid in the cache
Bed ensureBed(const QString& projectPath, const MusicBed::Options& o)
{
    Bed b;
    const QString dir = bedFolder(projectPath);
    if (!QDir().mkpath(dir)) fail("FAILED", "cannot create the folder " + QDir::toNativeSeparators(dir));
    b.path = QDir(dir).absoluteFilePath(MusicBed::fileName(o));
    if (QFileInfo::exists(b.path)) {
        if (const std::optional<Beats::Result> beats = cachedSongBeats(b.path, 0)) {
            b.info = MusicBed::describe(o);
            b.info.beats = *beats;
            return b;
        }
    }
    QElapsedTimer t;
    t.start();
    try {
        b.info = MusicBed::make(o);
    } catch (const std::bad_alloc&) {
        fail("FAILED", QString("not enough memory for a music bed of %1 s").arg(o.length, 0, 'f', 1));
    } catch (const std::system_error& e) {
        fail("FAILED", QString("cannot make the music bed: %1").arg(QString::fromLocal8Bit(e.what())));
    }
    // The file is there, only its grid left the cache: made again for the grid only (the same options make the same
    // sound). Written again, Windows refused to replace it while the app plays it (FAILED "cannot write")
    if (!QFileInfo::exists(b.path) && !MusicBed::writeWav(b.path, b.info))
        fail("FAILED", "cannot write " + QDir::toNativeSeparators(b.path));
    b.info.samples = {};
    storeSongBeats(b.path, 0, b.info.beats);
    b.made = true;
    b.ms = t.nsecsElapsed() / 1e6;
    return b;
}

void opMusicBed(Session& s, const QJsonObject& op)
{
    const ProjectFormat& f = s.format();
    const double fps = f.rate.fps();
    int at = 0, frames = 0;
    const MusicBed::Options o = bedOptions(s, op, &at, &frames);
    const Timeline& tl = s.project.timeline();
    int track = -1;
    if (op.contains("track")) {
        TrackKind k = TrackKind::Audio;
        track = parseTrack(op.value("track"), &k);
        if (k != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
        if (track < tl.audio.size() && tl.audio[track].locked) fail("FAILED", trackName({TrackKind::Audio, track}) + " is locked");
    } else {
        // The track of an earlier bed (the new one replaces it), else an empty "Musik" track (a start template's),
        // else a new one
        for (int i = 0; i < tl.audio.size() && track < 0; ++i)
            if (!tl.audio[i].locked)
                for (const Clip& c : tl.audio[i].clips)
                    if (isBedFile(c.mediaPath)) track = i;
        for (int i = 0; i < tl.audio.size() && track < 0; ++i)
            if (!tl.audio[i].locked && tl.audio[i].clips.isEmpty() && isMusicName(tl.audio[i].name)) track = i;
        if (track < 0) track = int(tl.audio.size());
    }
    const bool newTrack = track >= tl.audio.size();
    // Beds on the track within the new one's range go (replaced whole, not cut into pieces)
    if (!newTrack) {
        QJsonArray old;
        for (const Clip& c : tl.audio[track].clips)
            if (isBedFile(c.mediaPath) && c.start < at + frames && c.end() > at) old << c.id;
        if (!old.isEmpty()) {
            applyOp(s, QJsonObject{{"op", "delete"}, {"clips", old}});
            s.notes << QString("music_bed: replaced the bed(s) %1 on %2")
                           .arg(QString::fromUtf8(QJsonDocument(old).toJson(QJsonDocument::Compact)),
                                trackName({TrackKind::Audio, track}));
        }
    }
    const Bed bed = ensureBed(s.path, o);
    const MediaInfo& m = s.media(bed.path);
    if (m.length <= 0 || !m.hasAudio) fail("FAILED", "cannot read the bed " + QDir::toNativeSeparators(bed.path));
    // The file's length in frames is the asked one (within the rounding of its samples to frames)
    const int len = s.editor.placeSourceRange(m.path, 0, std::min(frames, m.length) - 1, at, track);
    if (len <= 0) fail("FAILED", QString("could not place the music bed on A%1 (locked?)").arg(track + 1));
    if (newTrack) s.editor.renameTrack(TrackRef{TrackKind::Audio, track}, T("Musik"));
    int id = 0;
    for (const Clip& c : s.project.timeline().audio.value(track).clips)
        if (c.start == at && c.mediaPath == m.path) id = c.id;
    if (!id) fail("FAILED", "the placed music bed is not on the timeline");
    // The level: gain as given, else under the program sound - kLevelBelowLu below the loudness of the voice and
    // recorded sound in the bed's range (both measured in the mix as exported: track faders, master), so the
    // music never sits as loud as the game or the voice (it did: a -16 LUFS bed on -15.5 LUFS game sound). Alone
    // (nothing else to hear) it stays at its own level, about -16 LUFS.
    const QStringList under = duckUnder(s, track);
    QJsonObject level;
    if (op.contains("gain")) {
        const double gain = numberOf(op, "gain", -60, 12);
        s.editor.modifyClips({id}, T("Musikbett"), [gain](Clip& c) { c.volumeDb = gain; });
        level = QJsonObject{{"gain_db", gain}, {"by", "gain"}};
    } else if (!under.isEmpty()) {
        QVector<int> tracks;
        for (const QString& n : under) tracks << n.mid(1).toInt() - 1;
        const Timeline rt = s.project.renderTimeline();
        const auto program = AudioAnalysis::tracksLoudness(f, rt, tracks, at, at + len);
        const auto music = AudioAnalysis::tracksLoudness(f, rt, {track}, at, at + len);
        if (program && music && program->integrated > -70 && music->integrated > -70) {
            const double gain = std::clamp(std::round((program->integrated - kLevelBelowLu - music->integrated) * 10) / 10,
                                           -30.0, 6.0);
            s.editor.modifyClips({id}, T("Musikbett"), [gain](Clip& c) { c.volumeDb = gain; });
            level = QJsonObject{{"gain_db", gain},
                                {"by", "program"},
                                {"program_lufs", std::round(program->integrated * 10) / 10},
                                {"music_lufs", std::round((music->integrated + gain) * 10) / 10},
                                {"text", QString("%1 LU under the sound of %2 (%3 LUFS): gain %4 dB")
                                             .arg(kLevelBelowLu, 0, 'f', 0)
                                             .arg(under.join(','))
                                             .arg(program->integrated, 0, 'f', 1)
                                             .arg(gain, 0, 'f', 1)}};
        }
    }

    // Ducking under the voice and recorded sound (op 'duck' with those tracks); without any: nothing to do
    QString ducked;
    if (op.value("duck").toBool(true)) {
        if (under.isEmpty()) {
            ducked = "not ducked: no voice or recorded sound on the other tracks (effect sounds and music do not count)";
        } else {
            try {
                const int before = int(s.notes.size());
                applyOp(s, QJsonObject{{"op", "duck"}, {"clips", QJsonArray{id}}, {"by", under.join(',')}});
                if (s.notes.size() > before) ducked = s.notes.takeAt(s.notes.size() - 1).toString();
            } catch (const Error& e) {
                ducked = QString("not ducked: %1").arg(e.message);
            }
        }
    }

    const MusicBed::Result& r = bed.info;
    QJsonArray sections;
    for (const MusicBed::Section& sec : r.sections) {
        const double from = sec.fromBar * 4 * r.beats.period, to = sec.kind == MusicBed::Section::Ending
                                                                       ? r.length
                                                                       : std::min(r.length, sec.toBar * 4 * r.beats.period);
        sections << QJsonObject{{"kind", MusicBed::sectionName(sec.kind)}, {"from_s", round3(from)}, {"to_s", round3(to)}};
    }
    QJsonObject n{{"file", QDir::toNativeSeparators(bed.path)},
                  {"mood", MusicBed::moodId(r.mood)},
                  {"bpm", r.bpm},
                  {"key", MusicBed::keyName(r.key)},
                  {"chords", QJsonArray::fromStringList(r.chords)},
                  {"seed", int(r.seed)},
                  {"length_s", round3(len / fps)},
                  {"sections", sections},
                  {"beat_s", round3(r.beats.period)},
                  {"bar_s", round3(4 * r.beats.period)},
                  {"track", trackName({TrackKind::Audio, track})},
                  {"clip", id},
                  {"made", bed.made}};
    n["final_hit_s"] = r.endBar >= 0 ? QJsonValue(round3(r.endBar * 4 * r.beats.period)) : QJsonValue();
    if (bed.made) {
        n["lufs"] = std::round(r.lufs * 10) / 10;
        n["true_peak_db"] = std::round(r.truePeakDb * 10) / 10;
        n["made_ms"] = int(std::lround(bed.ms));
    }
    if (!level.isEmpty()) n["level"] = level;
    if (!ducked.isEmpty()) n["ducking"] = ducked;
    s.notes << QJsonObject{{"music_bed", n}};
}

} // namespace

QJsonArray musicBedMoods()
{
    QJsonArray out;
    for (const MusicBed::Mood m : MusicBed::kMoods) {
        const MusicBed::Result d = MusicBed::describe({m, 30});
        out << QJsonObject{{"mood", MusicBed::moodId(m)},
                           {"name", T(MusicBed::moodLabel(m))},
                           {"bpm", MusicBed::defaultBpm(m)},
                           {"key", MusicBed::keyName(MusicBed::defaultKey(m))},
                           {"chords", QJsonArray::fromStringList(d.chords)},
                           {"chord_beats", d.chordBeats},
                           {"description", MusicBed::description(m)}};
    }
    return out;
}

QJsonObject prepareMusicBed(const QJsonObject& editArgs, Context&)
{
    const QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    int last = -1; // the last music_bed: nothing after it matters here
    for (int i = 0; i < ops.size(); ++i)
        if (ops[i].toObject().value("op").toString() == "music_bed") last = i;
    if (last < 0) return editArgs;
    // The ops before a bed change what it is made of (length:'timeline', the frame rate): they run first on a copy
    // of what the app shows. When one of them fails here, the state after it is unknown and the app makes the rest
    // itself (a bed of a wrong length must never be made)
    try {
        Session s(editArgs.value("project").toString());
        s.selectTimeline(editArgs.value("timeline"));
        for (int i = 0; i <= last; ++i) {
            const QJsonObject op = ops[i].toObject();
            if (op.value("op").toString() == "music_bed") {
                int at = 0, frames = 0;
                ensureBed(s.path, bedOptions(s, op, &at, &frames));
            }
            if (i < last) applyOp(s, op); // made already: the op only places it
        }
    } catch (const Error&) {
        // the op reports it where it runs
    } catch (const std::exception&) {
        // (no memory, no thread): the app tries itself
    }
    return editArgs;
}

void addMusicBedOps(QVector<OpDef>& ops)
{
    ops << OpDef{"music_bed",
                 "{op:'music_bed', mood:'hype'|'lofi'|'epic'|'funk', "
                 "length?:'timeline'|time, at?, bpm?, key?, seed?, track?, gain?, duck?}",
                 "background music of schneidi's own in the exact length (no file needed, free to use, its beats known)",
                 "Computed from synthesized instruments (no samples); each mood has its own tempo, key, chords and "
                 "how often they change, instruments, drum groove, bass line, space and arrangement: intro, main "
                 "parts with a lift, breaks in long beds and a final hit on a bar one that rings out exactly to the "
                 "end. mood: hype (128 BPM, E minor: supersaw, four on the floor, drops - gaming highlights, shorts, "
                 "sport, action), lofi (84 BPM, Eb major: electric piano, swung boom-bap, vinyl - vlog, tutorial, "
                 "podcast, under speech, study, chill, cooking), epic "
                 "(90 BPM, D minor: strings, taiko, horns - trailer, intro, reveal) or funk (104 BPM, E minor: slap "
                 "bass, clavinet, horn stabs - fashion, food, dance, playful product); 'drive' (older projects) is "
                 "hype, 'calm' (older projects) is lofi, also 'chill', 'cinematic', 'dark', 'groove', 'upbeat' … - `effects` kind:'music' lists "
                 "them. length: a time "
                 "(\"30s\") or 'timeline' "
                 "(default: from `at` to the end of the timeline); at (default 0); bpm 60-180 and key (\"A minor\", "
                 "\"F#m\", \"Eb major\") replace the mood's; seed (1 = default) picks another progression, "
                 "patterns and tune (2, 3 …: the same mood, another piece). The bed file is about -16 LUFS, true peak under -1 dBTP; "
                 "gain = clip volume in dB; without gain it sits 7 LU under the voice and recorded sound of the "
                 "other tracks in its range (measured in the mix; alone: gain 0). "
                 "It goes onto `track`, else the track of an earlier bed (which it replaces in its range), else an "
                 "empty track named \"Musik\" (start template podcast), else a new track \"Musik\". It is ducked "
                 "like op 'duck' under the voice and recorded sound - not under effect sounds (track SFX), other "
                 "beds or music tracks (duck:false: not at all). The WAV lies next to the project (<name>-music/); the same "
                 "options reuse it. Its beat grid is stored with the file: snap_to_beat, fit_music and `beats` take "
                 "it without analysing. The answer's notes give music_bed {file, bpm, key, chords, sections "
                 "[{kind, from_s, to_s}], final_hit_s, beat_s, bar_s, track, clip, lufs, level {gain_db, by, program_lufs, "
                 "music_lufs}, ducking}.",
                 opMusicBed};
}

} // namespace Cli::detail
