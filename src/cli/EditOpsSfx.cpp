// SPDX-License-Identifier: AGPL-3.0-only
// Edit op 'sfx' (0.17 package "sfx"): effect sounds on moments. The sounds are schneidi's own (core/SfxSynth,
// made once into the cache) or the user's files (effects folder "Sounds"); core/SfxPlace decides where they go:
// the anchor of a sound on its moment, sparing (spacing, cap per minute, one impact per video). Nothing places
// sounds by itself: only this op, asked for, does.

#include "cli/AnalysisDetail.h"
#include "cli/CommandsDetail.h"

#include "core/EffectFolders.h"
#include "core/GameHud.h"
#include "core/SfxPlace.h"
#include "core/SfxSynth.h"
#include "core/TimelineOps.h"
#include "engine/Extensions.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace Cli::detail {

namespace {

struct Moment {
    double frame = 0; // timeline frame (exact)
    double weight = 1;
};

// Hard cuts on the visible video tracks: a media clip right after another one, with no transition between them
QVector<Moment> cutMoments(const Timeline& tl)
{
    QVector<Moment> out;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (qsizetype i = 1; i < t.clips.size(); ++i) {
            const Clip& a = t.clips[i - 1];
            const Clip& b = t.clips[i];
            if (a.kind != ClipKind::Media || b.kind != ClipKind::Media || !a.enabled || !b.enabled) continue;
            if (a.end() != b.start || b.start <= 0 || a.transOut > 0 || b.transIn > 0) continue;
            if (std::none_of(out.begin(), out.end(), [&](const Moment& m) { return m.frame == b.start; }))
                out << Moment{double(b.start)};
        }
    }
    return out;
}

// The hits of the recordings on the timeline (cached `highlights` per file with a known kill feed, or `game`):
// [[frame, weight]]
QJsonArray killMoments(Session& s, const QJsonObject& op, Context& ctx)
{
    const Timeline& tl = s.project.timeline();
    QStringList files;
    for (const Track& t : tl.video) {
        if (t.hidden) continue;
        for (const Clip& c : t.clips)
            if (c.kind == ClipKind::Media && !files.contains(c.mediaPath))
                if (const MediaInfo* m = s.project.mediaInfo(c.mediaPath); m && m->hasVideo && !m->isImage)
                    files << c.mediaPath;
    }
    QJsonArray out;
    for (const QString& file : files) {
        QJsonObject args{{"file", file}};
        if (op.contains("game")) args["game"] = op.value("game");
        else if (!GameHud::guess(file)) continue;
        const QJsonObject r = highlightsFile(args, ctx);
        for (const QJsonValue& cv : r["candidates"].toArray())
            for (const QJsonValue& hv : cv["hit_s"].toArray()) {
                try {
                    const QJsonObject ref{{"media", file}, {"source", QString("%1s").arg(hv.toDouble())}};
                    out << QJsonArray{timeOf(s, QJsonObject{{"at", ref}}, "at"), std::max(1.0, cv["score"].toDouble())};
                } catch (const Error&) {
                    // the timeline does not play that hit
                }
            }
    }
    return out;
}

QVector<Moment> momentsOf(Session& s, const QJsonObject& op, const QString& on)
{
    const Timeline& tl = s.project.timeline();
    QVector<Moment> out;
    const QJsonValue at = op.value("at");
    const QJsonArray times = at.isArray() ? at.toArray() : at.isUndefined() ? QJsonArray{} : QJsonArray{at};
    for (const QJsonValue& v : times) out << Moment{double(timeOf(s, QJsonObject{{"at", v}}, "at"))};
    if (on == "cuts") {
        out += cutMoments(tl);
    } else if (on == "markers") {
        for (int m : tl.markers) out << Moment{double(m)};
    } else if (on == "drops") {
        for (const auto& [frame, score] : musicDrops(s, op)) out << Moment{frame, std::max(0.01, score)};
    } else if (on == "kills") {
        QJsonArray kills = op.value("_kills").toArray();
        if (!op.contains("_kills")) {
            Context quiet;
            kills = killMoments(s, op, quiet);
        }
        for (const QJsonValue& v : kills) out << Moment{v[0].toDouble(), v[1].toDouble(1)};
    } else if (!on.isEmpty()) {
        fail("BAD_ARGUMENT", "on: 'cuts', 'drops', 'kills' or 'markers'");
    }
    return out;
}

// A sound: a built-in kind or the user's file
struct Source {
    std::optional<SfxSynth::Kind> kind;
    QString file; // the user's file (built-in: made per seed)
};

Source sourceOf(const QString& name)
{
    Source src;
    if ((src.kind = SfxSynth::kindFrom(name))) return src;
    for (const QJsonValue& v : sfxSounds()) {
        const QString file = v["file"].toString();
        if (!file.isEmpty()
            && (v["name"].toString().compare(name, Qt::CaseInsensitive) == 0
                || QFileInfo(file).fileName().compare(name, Qt::CaseInsensitive) == 0)) {
            src.file = file;
            return src;
        }
    }
    const QString path = absolute(name);
    if (QFileInfo(path).isFile()) {
        src.file = path;
        return src;
    }
    fail("NOT_FOUND", QString("no sound '%1': %2, a sound of `effects` kind:'sfx' or a file")
                          .arg(name, SfxSynth::kindNames().join(", ")));
}

// The SFX track (by name); -1 = none yet
int sfxTrack(const Timeline& tl)
{
    for (int i = 0; i < tl.audio.size(); ++i)
        if (tl.audio[i].name == kSfxTrackName) return i;
    return -1;
}

// A sound on the track already: its moment (a built-in sound from its cache name, else its start) and whether it is
// of the kind being placed (its own cap per minute)
SfxPlace::Existing existingOf(const Clip& c, double fps, const Source& src)
{
    SfxPlace::Existing e{c.start / fps, false, !src.kind && c.mediaPath == src.file};
    if (const auto n = SfxSynth::parseCachedFile(c.mediaPath)) {
        e.at = (c.start - c.in) / fps + SfxSynth::anchorOf(n->kind, n->seed, n->asked);
        e.impact = n->kind == SfxSynth::Kind::Impact;
        e.sameKind = src.kind == n->kind;
    } else if (const auto r = SfxSynth::recordedOfFile(c.mediaPath)) {
        e.at = (c.start - c.in) / fps + r->anchor;
        e.impact = r->kind == SfxSynth::Kind::Impact;
        e.sameKind = src.kind == r->kind;
    }
    return e;
}

double rounded(double seconds) { return std::round(seconds * 1000) / 1000; }

void opSfx(Session& s, const QJsonObject& op)
{
    const QString on = op.value("on").toString();
    if (!op.contains("at") && on.isEmpty()) fail("BAD_ARGUMENT", "missing 'at' (times) or 'on' (cuts, drops, kills, markers)");
    static const QHash<QString, QString> kindFor{{"cuts", "whoosh"}, {"drops", "riser"}, {"kills", "hit"}, {"markers", "hit"}};
    const QString kindName = op.contains("kind") ? op.value("kind").toString() : kindFor.value(on);
    if (kindName.isEmpty()) fail("BAD_ARGUMENT", "missing 'kind' (" + SfxSynth::kindNames().join(", ") + " or a file)");
    const Source src = sourceOf(kindName);
    if (!src.kind && !s.media(src.file).hasAudio) fail("NO_AUDIO", "the sound file has no audio: " + src.file);
    const QString level = op.value("level").toString("mild");
    if (level != "mild" && level != "normal") fail("BAD_ARGUMENT", "level: 'mild' or 'normal'");
    const double volume = op.contains("volume_db") ? numberOf(op, "volume_db", -60, 6) : SfxPlace::levelDb(level);
    const bool seedGiven = op.contains("seed");
    const unsigned seed = seedGiven ? unsigned(numberOf(op, "seed", 1, 1000000)) : 1;
    const ProjectFormat& f = s.format();
    const double fps = f.rate.fps();
    const double length = op.contains("length") ? seconds(timeOf(s, op, "length"), f) : 0;

    const QVector<Moment> moments = momentsOf(s, op, on);
    if (moments.isEmpty()) {
        s.notes << QJsonObject{{"sfx", QJsonObject{{"kind", kindName}, {"placed", QJsonArray{}},
                                                   {"note", "no " + (on.isEmpty() ? QString("times") : on) + " found"}}}};
        return;
    }

    // Track: given, else the one named SFX (made at the bottom if there is none)
    int track = -1;
    {
        const Timeline& tl = s.project.timeline();
        if (op.contains("track")) {
            TrackKind k = TrackKind::Audio;
            track = parseTrack(op.value("track"), &k);
            if (k != TrackKind::Audio || track >= tl.audio.size()) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
        } else {
            track = sfxTrack(tl);
        }
    }
    // The sounds already there: every clip of the target track, and the built-in sounds on the other tracks (the
    // spacing and the one impact per video hold for the whole timeline)
    QVector<SfxPlace::Existing> existing;
    for (int k = 0; k < s.project.timeline().audio.size(); ++k)
        for (const Clip& c : s.project.timeline().audio[k].clips) {
            if (k != track && !SfxSynth::parseCachedFile(c.mediaPath) && !SfxSynth::recordedOfFile(c.mediaPath)) continue;
            SfxPlace::Existing e = existingOf(c, fps, src);
            e.sameTrack = k == track;
            e.start = c.start / fps;
            e.end = c.end() / fps;
            existing << e;
        }

    // The variants: one seed if given, else 1, 2, 3 in turn (the same sound three times in a row sounds made). The
    // anchors differ a little per variant: the rules see the first one, each sound lands with its own
    const auto seedOf = [&](int i) { return seedGiven ? seed : unsigned(1 + i % 3); };
    const auto anchorOf = [&](unsigned sd) { return src.kind ? SfxSynth::anchorOf(*src.kind, sd, length) : 0.0; };
    SfxPlace::Rules rules;
    rules.minGap = op.contains("min_gap") ? seconds(timeOf(s, op, "min_gap"), f) : 1.5;
    rules.maxPerMinute = op.contains("max_per_min") ? int(numberOf(op, "max_per_min", 0, 60)) : SfxPlace::perMinute(level);
    rules.impact = src.kind == SfxSynth::Kind::Impact;
    QVector<SfxPlace::Moment> pm;
    for (const Moment& m : moments) pm << SfxPlace::Moment{m.frame / fps, m.weight};
    // The timeline's end without the sounds of this track: a sound must end inside the picture (no black frames)
    int endFrame = 0;
    {
        const Timeline& tl = s.project.timeline();
        for (int k = 0; k < tl.video.size(); ++k)
            for (const Clip& c : tl.video[k].clips) endFrame = std::max(endFrame, c.end());
        for (int k = 0; k < tl.audio.size(); ++k)
            if (k != track)
                for (const Clip& c : tl.audio[k].clips) endFrame = std::max(endFrame, c.end());
    }
    const double end = endFrame / fps;
    const double soundLength = src.kind ? SfxSynth::soundLength(*src.kind, seedOf(0), length) : s.media(src.file).length / fps;
    const SfxPlace::Result r = SfxPlace::place(pm, anchorOf(seedOf(0)), soundLength, end, existing, rules);

    QJsonArray placed, skipped;
    for (const SfxPlace::Skipped& k : r.skipped) skipped << QJsonObject{{"at_s", rounded(k.moment)}, {"why", k.why}};
    if (track < 0 && !r.placed.isEmpty()) {
        track = int(s.project.timeline().audio.size());
        s.editor.addTrack(TrackKind::Audio, track);
        s.editor.renameTrack(TrackRef{TrackKind::Audio, track}, kSfxTrackName);
    }
    int i = 0;
    for (const SfxPlace::Placed& p : r.placed) {
        const unsigned sd = seedOf(i);
        const QString file = src.kind ? SfxSynth::cachedFile(*src.kind, sd, length) : src.file;
        if (file.isEmpty()) fail("FAILED", "could not write the sound into the cache folder");
        const MediaInfo& m = s.media(file);
        const int start = int(std::lround((p.moment - anchorOf(sd)) * fps));
        const Track& t = s.project.timeline().audio[track];
        const bool busy = std::any_of(t.clips.begin(), t.clips.end(),
                                      [&](const Clip& c) { return c.start < start + m.length && start < c.end(); });
        if (start < 0 || busy || start + m.length > endFrame) {
            skipped << QJsonObject{{"at_s", rounded(p.moment)},
                                   {"why", busy ? "overlaps" : start < 0 ? "before_start" : "after_end"}};
            continue;
        }
        // Only its sound (a video file as the sound must not overwrite the picture)
        if (s.editor.placeSourceRange(m.path, 0, m.length - 1, start, track, 1.0, true) <= 0)
            fail("FAILED", QString("could not place the sound on A%1 (locked?)").arg(track + 1));
        // The clip's own volume: the level on the sound's peak plus the evening-out of this sound
        const double clipDb = volume + (src.kind ? SfxSynth::levelGainDb(*src.kind, sd, length) : 0.0);
        for (const Clip& c : s.project.timeline().audio[track].clips)
            if (c.start == start && c.mediaPath == m.path) s.editor.setClipVolume(c.id, clipDb);
        placed << QJsonObject{{"at_s", rounded(p.moment)}, {"start", tc(start, f)}, {"clip_db", rounded(clipDb)}};
        ++i;
    }
    QJsonObject note{{"kind", src.kind ? SfxSynth::kindName(*src.kind) : QFileInfo(src.file).fileName()},
                     {"volume_db", volume}, {"placed", placed}};
    if (track >= 0) note["track"] = QString("A%1").arg(track + 1);
    if (!skipped.isEmpty()) note["skipped"] = skipped;
    s.notes << QJsonObject{{"sfx", note}};
}

} // namespace

QJsonArray sfxSounds()
{
    QJsonArray out;
    for (SfxSynth::Kind k : SfxSynth::kKinds) {
        QJsonObject o{{"name", SfxSynth::kindName(k)}, {"from", "built-in"}, {"description", SfxSynth::description(k)},
                      {"length_s", SfxSynth::defaultLength(k)}};
        // Recorded (CC0) by default for this kind: the files seed 1, 2 … pick, with their source and licence
        if (const QVector<SfxSynth::Recorded> rec = SfxSynth::recorded(k); !rec.isEmpty()) {
            QJsonArray files;
            for (const SfxSynth::Recorded& r : rec)
                files << QJsonObject{{"file", QFileInfo(r.file).fileName()}, {"length_s", r.length}, {"author", r.author},
                                     {"licence", r.licence}};
            o["recorded"] = files;
            o["length_s"] = rec.first().length;
            o["note"] = "recorded (CC0); computed when a length is given";
        } else {
            o["computed"] = "earshot recipe port (MIT)";
        }
        out << o;
    }
    // Packs: "<pack>/<file name without extension>"
    for (const QString& pack : Extensions::installedSoundPacks()) {
        QDirIterator it(pack, QDir::Files, QDirIterator::Subdirectories);
        QStringList files;
        while (it.hasNext())
            if (const QString f = it.next(); EffectFolders::isSoundFile(f)) files << f;
        files.sort();
        for (const QString& f : files)
            out << QJsonObject{{"name", QFileInfo(pack).fileName() + "/" + QFileInfo(f).completeBaseName()},
                               {"from", "pack"}, {"file", f}};
    }
    for (const EffectFolders::LutEntry& e : EffectFolders::userSounds())
        out << QJsonObject{{"name", e.group.isEmpty() ? e.name : e.group + "/" + e.name}, {"from", "own"}, {"file", e.path}};
    return out;
}

QJsonObject prepareSfx(const QJsonObject& editArgs, Context& ctx)
{
    QJsonArray ops = editArgs.value("ops").isObject() ? QJsonArray{editArgs.value("ops")} : editArgs.value("ops").toArray();
    bool any = false;
    std::unique_ptr<Session> s;
    for (int i = 0; i < ops.size(); ++i) {
        QJsonObject op = ops[i].toObject();
        if (op.value("op").toString() != "sfx" || op.value("on").toString() != "kills" || op.contains("_kills")) continue;
        try {
            if (!s) {
                s = std::make_unique<Session>(editArgs.value("project").toString()); // a copy of what the app shows
                s->selectTimeline(editArgs.value("timeline"));
            }
            op["_kills"] = killMoments(*s, op, ctx);
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

void addSfxOps(QVector<OpDef>& ops)
{
    ops << OpDef{"sfx", "{op:'sfx', at?|on?:'cuts'|'drops'|'kills'|'markers', kind?, level?:'mild'|'normal', volume_db?}",
                 "effect sounds (whoosh, riser, hit, impact, click, swish or a file) on moments, sparingly",
                 "Puts a sound on each moment, on the audio track 'SFX' (made if missing; track: another one). "
                 "at: times; on: 'cuts' (hard cuts of the video), 'drops' (of the music as snap_to_beat finds it; "
                 "music), 'kills' (hit_s of the recordings' kill feed as `highlights` reads it; game) or 'markers'. "
                 "kind (default by on: cuts whoosh, drops riser, kills and markers hit): built-in (`effects` "
                 "kind:'sfx'), a sound of the effects folder 'Sounds' or a file. The whoosh's peak lands on the "
                 "moment, the riser ends on it, a hit starts on it. Sparing: never two sounds closer than min_gap "
                 "(1.5 s), at most max_per_min in any minute (mild 3, normal 6), one impact per video; the answer's "
                 "notes list what the rules left out (skipped, why). level 'mild' (default, -10 dB) or 'normal' "
                 "(-4 dB); volume_db sets it directly (on the sound at its kind's peak level). All but the riser are "
                 "CC0 recordings (computed ones when a length is given); the riser is computed. seed: one variant (default 1-3 in turn); length: "
                 "of the sound ('2s'; a riser 0.5-8 s). Only where asked: never on every cut by default.",
                 opSfx,
                 {"seed", "length", "track", "min_gap", "max_per_min", "music", "game", "_kills", "_beats"}};
}

} // namespace Cli::detail
