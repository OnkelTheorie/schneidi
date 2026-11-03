// SPDX-License-Identifier: AGPL-3.0-only
// schneidi-cli edit operations for talk (interviews, vlogs, podcasts): J/L cuts (the sound edge of a cut alone)
// and room tone in the gaps of a dialogue track
#include "cli/CommandsDetail.h"

#include "core/RoomTone.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <climits>
#include <cmath>

namespace Cli::detail {

namespace {

// ---------- jl_cut ----------

// A signed time: frames, or "-0.5s" / "+12" / a timecode with a sign
int signedTime(Session& s, const QJsonValue& v, const QString& what)
{
    if (v.isDouble()) return int(std::lround(v.toDouble()));
    QString t = v.toString().trimmed();
    const bool minus = t.startsWith('-');
    if (minus || t.startsWith('+')) t = t.mid(1).trimmed();
    if (t.isEmpty()) fail("BAD_ARGUMENT", QString("'%1' is no time (frames, \"-0.5s\", \"+12\")").arg(what));
    const int f = parseTime(t, s.format(), what);
    return minus ? -f : f;
}

QString frameSpan(int frames, const ProjectFormat& f)
{
    return QString("%1 frames (%2 s)").arg(frames).arg(seconds(frames, f), 0, 'f', 2);
}

// The cut a jl_cut op means: (left clip, right clip) of a picture cut whose clips have linked sound
QPair<int, int> jlCutAt(Session& s, const QJsonObject& op)
{
    using namespace TimelineOps;
    const Timeline& tl = s.project.timeline();
    if (op.contains("clip")) {
        const Clip& given = clipOf(s, op);
        TrackRef ref;
        findClip(tl, given.id, &ref);
        const Clip* pic = &given;
        if (ref.kind == TrackKind::Audio) { // its picture stands for it
            pic = nullptr;
            for (int i = 0; i < tl.video.size() && !pic && given.linkId; ++i)
                for (const Clip& c : tl.video[i].clips)
                    if (c.linkId == given.linkId) {
                        pic = &c;
                        ref = {TrackKind::Video, i};
                        break;
                    }
            if (!pic)
                fail("BAD_ARGUMENT", QString("clip %1 is sound without a linked picture: a J/L cut moves the sound "
                                             "edge of a picture cut (a sound cut alone: trim mode:'roll')")
                                         .arg(given.id));
        }
        const QString edge = op.value("edge").toString("start");
        if (edge != "start" && edge != "end") fail("BAD_ARGUMENT", "edge must be 'start' or 'end'");
        const bool start = edge == "start";
        const int cut = start ? pic->start : pic->end();
        for (const Clip& o : tl.track(ref).clips)
            if (o.id != pic->id && (start ? o.end() == cut : o.start == cut))
                return start ? qMakePair(o.id, pic->id) : qMakePair(pic->id, o.id);
        fail("FAILED", QString("no clip meets clip %1 at its %2 (%3): a J/L cut needs two clips that touch")
                           .arg(pic->id)
                           .arg(edge, tc(cut, s.format())));
    }
    if (!op.contains("at")) fail("BAD_ARGUMENT", "missing 'at' (time of the cut) or 'clip' (the clip after the cut)");
    const int at = timeOf(s, op, "at");
    QPair<int, int> best{0, 0};
    int bestDist = INT_MAX;
    for (const Track& t : tl.video)
        for (int k = 0; k + 1 < t.clips.size(); ++k) {
            const Clip& a = t.clips[k];
            const Clip& b = t.clips[k + 1];
            if (a.end() != b.start || std::abs(b.start - at) >= bestDist || !s.editor.soundEdge(a.id, b.id)) continue;
            bestDist = std::abs(b.start - at);
            best = {a.id, b.id};
        }
    if (!best.first)
        fail("NOT_FOUND", "no picture cut whose clips both have linked sound that meets on an audio track (link "
                          "picture and sound with op 'link'; a sound cut alone: trim mode:'roll')");
    return best;
}

void opJlCut(Session& s, const QJsonObject& op)
{
    if (!op.contains("audio"))
        fail("BAD_ARGUMENT", "missing 'audio': where the sound cut lies from the picture cut, '-0.5s' = J (the next "
                             "clip's sound starts early), '+0.5s' = L (the previous sound runs on), 0 = back in sync");
    const int offset = signedTime(s, op.value("audio"), "audio");
    const auto [left, right] = jlCutAt(s, op);
    const std::optional<Editor::SoundEdge> e = s.editor.soundEdge(left, right);
    const ProjectFormat& f = s.format();
    if (!e)
        fail("FAILED", QString("the clips %1 and %2 at the cut have no linked sound clips that meet on an unlocked audio "
                               "track (link them with op 'link'; a sound cut alone: trim mode:'roll')")
                           .arg(left)
                           .arg(right));
    if (offset < e->lo || offset > e->hi)
        fail("BAD_ARGUMENT",
             QString("audio %1 needs more material than the clips have: at this cut the sound can start up to %2 early "
                     "(audio:'-%3s') and run on up to %4 (audio:'+%5s')")
                 .arg(offset)
                 .arg(frameSpan(std::max(0, -e->lo), f))
                 .arg(seconds(std::max(0, -e->lo), f), 0, 'f', 2)
                 .arg(frameSpan(std::max(0, e->hi), f))
                 .arg(seconds(std::max(0, e->hi), f), 0, 'f', 2));
    s.editor.setSoundEdge(left, right, offset);
    QStringList tracks;
    for (const auto& p : e->pairs) {
        TrackRef ref;
        if (TimelineOps::findClip(s.project.timeline(), p.first, &ref)) tracks << trackName(ref);
    }
    const QString what = offset < 0   ? QString("J cut: the sound of clip %1 starts %2 before").arg(right).arg(frameSpan(-offset, f))
                         : offset > 0 ? QString("L cut: the sound of clip %1 runs %2 past").arg(left).arg(frameSpan(offset, f))
                                      : QString("straight cut: the sound cuts with");
    s.notes << QString("jl_cut: %1 the picture cut at %2 (sound cut at %3 on %4; picture and links unchanged)")
                   .arg(what, tc(e->cut, f), tc(e->cut + offset, f), tracks.join(", "));
}

// ---------- room_tone ----------

constexpr double kQuietWant = 2.0;  // seconds of quiet the loop is made of
constexpr double kQuietLeast = 1.0; // at least
constexpr double kScanMargin = 30.0; // seconds of the file around the used parts searched for quiet
constexpr double kMaxGainDb = 12.0;

struct TakeQuiet {
    bool found = false;
    int in = 0, length = 0; // source frames
    double floorDb = -200, meanDb = -200;
};

TakeQuiet measureTake(Session& s, const Clip& take, int track)
{
    const MediaInfo* m = s.project.mediaInfo(take.mediaPath);
    const double fps = s.project.frameRate();
    TakeQuiet q;
    if (!m || m->length <= 0 || fps <= 0) return q;
    // The parts of the file this track plays, and some of it around them (the same recording)
    int lo = INT_MAX, hi = INT_MIN;
    for (const Clip& c : s.project.timeline().audio[track].clips)
        if (c.mediaPath == take.mediaPath && c.audioStream == take.audioStream && !c.isRetimed()) {
            lo = std::min(lo, c.in);
            hi = std::max(hi, c.out + 1);
        }
    const int margin = int(std::lround(kScanMargin * fps));
    lo = std::max(0, lo - margin);
    hi = std::min(m->length, hi + margin);
    if (hi <= lo) return q;
    const auto levels = AudioAnalysis::fileBlockLevels(take.mediaPath, take.audioStream, lo / fps, hi / fps,
                                                       RoomTone::kBlockSeconds);
    if (!levels) return q;
    const RoomTone::Quiet quiet = RoomTone::findQuiet(*levels, int(std::lround(kQuietWant / RoomTone::kBlockSeconds)),
                                                      int(std::lround(kQuietLeast / RoomTone::kBlockSeconds)));
    q.floorDb = quiet.floorDb;
    if (!quiet.found()) return q;
    // Whole frames inside the stretch
    q.in = lo + int(std::ceil(quiet.from * RoomTone::kBlockSeconds * fps));
    const int end = lo + int(std::floor(quiet.to * RoomTone::kBlockSeconds * fps));
    q.length = std::min(end, m->length) - q.in;
    q.meanDb = quiet.meanDb;
    q.found = q.length >= 2;
    return q;
}

bool roomToneSource(const Session& s, const Clip& c)
{
    const MediaInfo* m = s.project.mediaInfo(c.mediaPath);
    return c.kind == ClipKind::Media && !c.isRetimed() && m && m->hasAudio && !m->isImage;
}

void opRoomTone(Session& s, const QJsonObject& op)
{
    if (!op.contains("track")) fail("BAD_ARGUMENT", "missing 'track': the dialogue track (\"A1\")");
    TrackKind kind = TrackKind::Audio;
    const int track = parseTrack(op.value("track"), &kind);
    const Timeline& tl = s.project.timeline();
    if (kind != TrackKind::Audio) fail("BAD_ARGUMENT", "track must be an audio track (A1 …)");
    if (track < 0 || track >= tl.audio.size()) fail("NOT_FOUND", QString("there is no A%1").arg(track + 1));
    if (tl.audio[track].locked) fail("FAILED", QString("A%1 is locked").arg(track + 1));
    const ProjectFormat& f = s.format();
    const int from = op.contains("from") ? timeOf(s, op, "from") : 0;
    const int to = op.contains("to") ? timeOf(s, op, "to") : INT_MAX;
    if (to <= from) fail("BAD_ARGUMENT", "'to' must be after 'from'");

    // The gaps between clips of the track inside [from, to), each with the take it continues
    struct Gap {
        int from, to;
        Clip take;
    };
    QVector<Gap> gaps;
    int skipped = 0;
    const auto& clips = tl.audio[track].clips; // sorted by start
    for (int k = 0; k + 1 < clips.size(); ++k) {
        const int a = std::max(clips[k].end(), from), b = std::min(clips[k + 1].start, to);
        if (b <= a) continue;
        if (roomToneSource(s, clips[k])) gaps << Gap{a, b, clips[k]};
        else if (roomToneSource(s, clips[k + 1])) gaps << Gap{a, b, clips[k + 1]};
        else ++skipped;
    }
    if (gaps.isEmpty()) {
        s.notes << QString("room_tone: no gaps between recordings on A%1%2").arg(track + 1).arg(
                       op.contains("from") || op.contains("to") ? " in that range" : "");
        return;
    }
    // One measurement per recording
    QHash<QString, TakeQuiet> quiet;
    QVector<RoomTone::Fill> fills;
    QHash<QString, QPair<int, int>> perTake; // key -> (gaps, frames)
    QStringList keys;
    const int seam = RoomTone::seamFrames(s.project.frameRate());
    for (const Gap& g : gaps) {
        const QString key = g.take.mediaPath + '#' + QString::number(g.take.audioStream);
        if (!quiet.contains(key)) {
            quiet.insert(key, measureTake(s, g.take, track));
            keys << key;
        }
        const TakeQuiet& q = quiet[key];
        if (!q.found) continue;
        RoomTone::Fill fill;
        fill.take = g.take;
        fill.from = g.from;
        fill.to = g.to;
        fill.quietIn = q.in;
        fill.quietLength = q.length;
        fill.gainDb = std::clamp(q.floorDb - q.meanDb, -kMaxGainDb, kMaxGainDb);
        fill.seam = seam;
        fills << fill;
        perTake[key].first += 1;
        perTake[key].second += g.to - g.from;
    }
    const QVector<int> ids = s.editor.fillRoomTone(track, fills);
    if (!fills.isEmpty() && ids.isEmpty()) fail("FAILED", QString("could not fill A%1 (locked?)").arg(track + 1));
    for (const QString& key : keys) {
        const TakeQuiet& q = quiet[key];
        const QString file = QFileInfo(key.section('#', 0, -2)).fileName();
        if (!q.found) {
            s.notes << QString("room_tone: %1 has no steady quiet stretch of %2 s (all words, or digital silence): "
                               "its gaps stay empty")
                           .arg(file)
                           .arg(kQuietLeast, 0, 'f', 1);
            continue;
        }
        const double fps = s.project.frameRate();
        s.notes << QString("room_tone: A%1, %2 gap(s) (%3 s) filled with a loop of %4 %5-%6 s (the take's floor "
                           "%7 dBFS, loop %8 dBFS, %9 dB to match)")
                       .arg(track + 1)
                       .arg(perTake[key].first)
                       .arg(seconds(perTake[key].second, f), 0, 'f', 2)
                       .arg(file)
                       .arg(q.in / fps, 0, 'f', 2)
                       .arg((q.in + q.length) / fps, 0, 'f', 2)
                       .arg(q.floorDb, 0, 'f', 1)
                       .arg(q.meanDb, 0, 'f', 1)
                       .arg(std::clamp(q.floorDb - q.meanDb, -kMaxGainDb, kMaxGainDb), 0, 'f', 1);
    }
    if (skipped)
        s.notes << QString("room_tone: %1 gap(s) between clips without recorded sound (titles, music changed in "
                           "speed) stay empty")
                       .arg(skipped);
    if (!ids.isEmpty()) {
        QStringList list;
        for (int id : ids) list << QString::number(id);
        s.notes << QString("room_tone: new clips %1").arg(list.join(", "));
    }
}

} // namespace

void addTalkOps(QVector<OpDef>& ops)
{
    ops << OpDef{"jl_cut", "{op:'jl_cut', at|clip, audio, edge?}",
                 "J/L cut: only the sound edge of a picture cut moves (audio:'-0.5s' = J, the next clip's sound starts "
                 "early; '+0.5s' = L, the previous sound runs on; 0 = back in sync)",
                 "at = a time near the cut (the nearest picture cut whose clips both have linked sound), or clip = the "
                 "clip after the cut (edge:'end' = the clip before it; a sound clip stands for its linked picture). "
                 "audio is measured from the picture cut, not from where the sound cut is now: running it again with "
                 "another value replaces it. The picture, the links and the sync of everything else stay; the sound "
                 "clips on every audio track where the two linked sounds meet roll together (one undo step). Not "
                 "enough material in the files: an error with how far it can go. A sound cut alone (no picture): "
                 "trim mode:'roll' on the sound clip.",
                 opJlCut}
        << OpDef{"room_tone", "{op:'room_tone', track, from?, to?}",
                 "fill the gaps of a dialogue track with the quiet of the same recording (room tone), so pause cuts do "
                 "not drop into dead silence",
                 "track = the audio track (\"A1\"); from/to limit it to a range (default: every gap between two clips). "
                 "Each gap gets a loop of the quietest steady 1-2 s of the recording before it (the one after it when "
                 "the one before is a title or retimed), measured in the file around the parts the track plays; its "
                 "level is matched to the take's noise floor (10th percentile of its 50 ms levels) and it gets the "
                 "take's volume, pan and audio effects. Seams are crossfades that start on the cut (about 20 ms, at "
                 "least one frame); the takes fade into the room tone without touching their sound before the cut. "
                 "Only measured by level (no speech recognition): a recording without a quiet stretch of 1 s (all "
                 "words, digital silence) is left out with a note. Nothing happens on its own: only when asked.",
                 opRoomTone};
}

} // namespace Cli::detail
