// SPDX-License-Identifier: AGPL-3.0-only
// Multicam (docs/archiv/plan-0.8.md), model and Editor without rendering: a multicam clip is a sequence with one angle
// per recording (video track + one audio track per stream, placed by the sync offsets). In the timeline its segments
// carry the active angle; cutting/switching writes normal edits that can be changed later (other angle, roll, trim,
// ripple, transitions, copy). Flatten turns segments into the angle's own clips. Resync inside the multicam
// sequence, renaming/adding angles, protections, project file (also broken files) and a long undo chain.
#include "check.h"

#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QUndoStack>

namespace {

using Mode = Multicam::Mode;

MediaInfo media(const QString& path, int length, bool video = true, bool audio = true)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

// OBS recording with several titled audio streams
MediaInfo obs(const QString& path, int length, const QStringList& titles)
{
    MediaInfo m = media(path, length);
    for (const QString& t : titles) m.audioStreams << AudioStreamInfo{2, t};
    return m;
}

// Timeline as text like Check::dump, multicam segments as M<angle 1-based>
QString mdump(const Timeline& tl)
{
    QString s;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const auto& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            s += QString("%1%2:").arg(k == TrackKind::Video ? "V" : "A").arg(i + 1);
            for (const Clip& c : tracks[i].clips)
                s += QString(" %1[%2-%3|%4-%5]")
                         .arg(c.isMulticam() ? QString("M%1").arg(c.angle + 1)
                                             : c.isTitle() ? QString("T") : QFileInfo(c.mediaPath).baseName())
                         .arg(c.start).arg(c.end()).arg(c.in).arg(c.out);
            s += "  ";
        }
    }
    return s.trimmed();
}

const Clip* clipOn(const Timeline& tl, TrackKind kind, int track, int index)
{
    const auto& tracks = tl.tracks(kind);
    if (track >= tracks.size() || index >= tracks[track].clips.size()) return nullptr;
    return &tracks[track].clips[index];
}

int idOn(const Timeline& tl, TrackKind kind, int track, int index)
{
    const Clip* c = clipOn(tl, kind, track, index);
    return c ? c->id : 0;
}

// Project file as text; the counters for new ids keep going on undo (ids are never reused, like test_undo)
// Selection as the timeline makes it: a click on a linked clip selects its partners too
QSet<int> linked(const Editor& ed, int id)
{
    const QVector<int> ids = ed.withLinked({id});
    return QSet<int>(ids.begin(), ids.end());
}

QByteArray json(const Project& p)
{
    ProjectData d = p.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/p.schneidi");
}

// Three player POVs: pov2 started 20 frames before pov1, pov3 100 frames after (and is shorter)
QVector<Multicam::Angle> threeAngles()
{
    return {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/pov2.mp4", -20}}}, {"P3", {{"/x/pov3.mp4", 100}}}};
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    QUndoStack* undo = p.undoStack();
    Fixture()
    {
        p.addMedia(media("/x/pov1.mp4", 1000));
        p.addMedia(media("/x/pov2.mp4", 1000));
        p.addMedia(media("/x/pov3.mp4", 800));
        p.addMedia(media("/x/pov4.mp4", 1000));
        p.addMedia(media("/x/part1.mp4", 300));
        p.addMedia(media("/x/part2.mp4", 400));
        p.addMedia(media("/x/caster.wav", 1200, false, true));
        p.addMedia(obs("/x/obs1.mkv", 1000, {"Mix", "Discord", "Mikro"}));
        p.addMedia(media("/x/music.mp3", 3000, false, true));
    }
    // Multicam clip of threeAngles() (sequence 0..1020) placed at `at` on V1/A1; returns the sequence id
    int placed(int at = 0)
    {
        const int seq = ed.createMulticamClip("Match", threeAngles());
        ed.addSequenceAt(seq, at, 0);
        return seq;
    }
};

// ---- Creating ----

void testCreateMulticamSequence()
{
    Fixture f;
    const int timeline = f.p.currentSequence();
    const int steps = f.undo->count();
    const int seq = f.ed.createMulticamClip({}, threeAngles());
    if (!CHECK(seq > 0)) return;
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(f.p.currentSequence(), timeline); // stays in the timeline, nothing placed yet
    CHECK_EQ(TimelineOps::endFrame(f.p.timeline()), 0);
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s && s->multicam)) return;
    CHECK_EQ(f.p.sequenceName(seq), T("Multicam-Clip %1").arg(1));
    // Offsets normalized: the earliest recording (pov2, -20) starts at frame 0
    CHECK_EQ(Check::dump(s->timeline), QString("V1: pov1[20-1020|0-999]  V2: pov2[0-1000|0-999]  V3: pov3[120-920|0-799]  "
                                               "A1: pov1[20-1020|0-999]  A2: pov2[0-1000|0-999]  A3: pov3[120-920|0-799]"));
    CHECK_EQ(f.p.sequenceLength(seq), 1020);
    if (!CHECK_EQ(s->angles.size(), 3)) return;
    CHECK(s->angles[0] == (MulticamAngle{"P1", 0, {0}}));
    CHECK(s->angles[1] == (MulticamAngle{"P2", 1, {1}}));
    CHECK(s->angles[2] == (MulticamAngle{"P3", 2, {2}}));
    // Picture and sound of an angle are linked, angles not with each other
    QSet<int> links;
    for (int i = 0; i < 3; ++i) {
        const Clip* v = clipOn(s->timeline, TrackKind::Video, i, 0);
        const Clip* a = clipOn(s->timeline, TrackKind::Audio, i, 0);
        if (!CHECK(v && a)) continue;
        CHECK(v->linkId != 0 && v->linkId == a->linkId);
        links << v->linkId;
    }
    CHECK_EQ(links.size(), 3);

    f.undo->undo();
    CHECK(!f.p.sequence(seq));
    f.undo->redo();
    CHECK(f.p.sequence(seq) && f.p.sequence(seq)->multicam);

    // Named, in a bin; numbering of unnamed ones goes on
    const int bin = f.p.addBin(0, "Turnier");
    const int named = f.ed.createMulticamClip(" Finale ", threeAngles(), bin);
    CHECK_EQ(f.p.sequenceName(named), QString("Finale"));
    CHECK_EQ(f.p.sequence(named)->bin, bin);
    const int third = f.ed.createMulticamClip({}, threeAngles());
    CHECK_EQ(f.p.sequenceName(third), T("Multicam-Clip %1").arg(2));
}

void testCreateWithMultiStreamAngles()
{
    // OBS file with three streams: one audio track per stream, all linked to the angle's picture
    Fixture f;
    const int seq = f.ed.createMulticamClip("OBS", {{"Theo", {{"/x/obs1.mkv", 0}}}, {"P2", {{"/x/pov2.mp4", 10}}}});
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s)) return;
    CHECK_EQ(Check::dump(s->timeline), QString("V1: obs1[0-1000|0-999]  V2: pov2[10-1010|0-999]  "
                                               "A1: obs1[0-1000|0-999]  A2: obs1[0-1000|0-999]  A3: obs1[0-1000|0-999]  "
                                               "A4: pov2[10-1010|0-999]"));
    CHECK(s->angles.value(0) == (MulticamAngle{"Theo", 0, {0, 1, 2}}));
    CHECK(s->angles.value(1) == (MulticamAngle{"P2", 1, {3}}));
    for (int k = 0; k < 3; ++k) {
        const Clip* a = clipOn(s->timeline, TrackKind::Audio, k, 0);
        CHECK(a && a->audioStream == k && a->linkId == clipOn(s->timeline, TrackKind::Video, 0, 0)->linkId);
    }
}

void testCreateWithSplitRecording()
{
    // One POV in two files (OBS restarted after a crash): gap between them; an overlap cuts the later file
    Fixture f;
    const int seq = f.ed.createMulticamClip(
        "Split", {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/part1.mp4", 0}, {"/x/part2.mp4", 500}}}});
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s)) return;
    CHECK_EQ(Check::dump(s->timeline), QString("V1: pov1[0-1000|0-999]  V2: part1[0-300|0-299] part2[500-900|0-399]  "
                                               "A1: pov1[0-1000|0-999]  A2: part1[0-300|0-299] part2[500-900|0-399]"));
    CHECK(s->angles.value(1) == (MulticamAngle{"P2", 1, {1}}));

    const int over = f.ed.createMulticamClip(
        "Overlap", {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/part1.mp4", 0}, {"/x/part2.mp4", 250}}}});
    if (!CHECK(f.p.sequence(over))) return;
    CHECK_EQ(Check::dump(f.p.sequence(over)->timeline),
             QString("V1: pov1[0-1000|0-999]  V2: part1[0-300|0-299] part2[300-650|50-399]  "
                     "A1: pov1[0-1000|0-999]  A2: part1[0-300|0-299] part2[300-650|50-399]"));
}

void testCreateAudioOnlyAngle()
{
    // The caster's sound as an angle: no video track, only its audio track
    Fixture f;
    const int seq = f.ed.createMulticamClip(
        "Cast", {{"P1", {{"/x/pov1.mp4", 0}}}, {"Caster", {{"/x/caster.wav", -100}}}, {"P2", {{"/x/pov2.mp4", 0}}}});
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s)) return;
    CHECK_EQ(Check::dump(s->timeline), QString("V1: pov1[100-1100|0-999]  V2: pov2[100-1100|0-999]  "
                                               "A1: pov1[100-1100|0-999]  A2: caster[0-1200|0-1199]  A3: pov2[100-1100|0-999]"));
    CHECK(s->angles.value(1) == (MulticamAngle{"Caster", -1, {1}}));
    CHECK(s->angles.value(2) == (MulticamAngle{"P2", 1, {2}}));
}

void testCreateRejectsBadInput()
{
    Fixture f;
    const int steps = f.undo->count();
    const int sequences = f.p.sequences().size();
    CHECK_EQ(f.ed.createMulticamClip("x", {}), 0);
    CHECK_EQ(f.ed.createMulticamClip("x", {{"A", {}}, {"B", {{"/x/missing.mp4", 0}}}}), 0);
    CHECK_EQ(f.undo->count(), steps);
    CHECK_EQ(f.p.sequences().size(), sequences);
    // Unusable files/angles are left out, one usable angle is enough (more can be added later)
    const int one = f.ed.createMulticamClip("One", {{"A", {{"/x/missing.mp4", 0}}}, {"B", {{"/x/pov1.mp4", 0}}}});
    if (!CHECK(one > 0)) return;
    CHECK_EQ(f.p.sequence(one)->angles.size(), 1);
    CHECK_EQ(f.p.sequence(one)->angles.value(0).name, QString("B"));
    // Empty angle name = file name without extension
    const int unnamed = f.ed.createMulticamClip("U", {{"", {{"/x/pov3.mp4", 0}}}});
    CHECK_EQ(f.p.sequence(unnamed)->angles.value(0).name, QString("pov3"));
}

// ---- Placing, cutting, switching ----

void testPlaceMulticamClip()
{
    Fixture f;
    const int seq = f.ed.createMulticamClip("Match", threeAngles());
    CHECK(f.ed.addSequenceAt(seq, 50, 0));
    // One video and one audio part (the audio part plays all streams of the active angle), linked
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[50-1070|0-1019]  V2:  A1: M1[50-1070|0-1019]  A2:"));
    const Clip* v = clipOn(f.p.timeline(), TrackKind::Video, 0, 0);
    const Clip* a = clipOn(f.p.timeline(), TrackKind::Audio, 0, 0);
    if (!CHECK(v && a)) return;
    CHECK(v->isMulticam() && a->isMulticam() && !v->isCompound());
    CHECK(v->sequenceId == seq && a->sequenceId == seq);
    CHECK(v->angle == 0 && a->angle == 0);
    CHECK(v->linkId != 0 && v->linkId == a->linkId);
    CHECK_EQ(f.p.clipName(*v), QString("P1"));
    CHECK(f.p.sequenceUsed(seq) && !f.p.canRemoveSequence(seq));
}

void testCutToAngleSplitsAndSwitches()
{
    Fixture f;
    f.placed();
    const int steps = f.undo->count();
    CHECK(f.ed.multicamCut(300, 1, Mode::Both));
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-300|0-299] M2[300-1020|300-1019]  V2:  A1: M1[0-300|0-299] M2[300-1020|300-1019]  A2:"));
    const Timeline& tl = f.p.timeline();
    const Clip* v0 = clipOn(tl, TrackKind::Video, 0, 0);
    const Clip* v1 = clipOn(tl, TrackKind::Video, 0, 1);
    const Clip* a0 = clipOn(tl, TrackKind::Audio, 0, 0);
    const Clip* a1 = clipOn(tl, TrackKind::Audio, 0, 1);
    if (!CHECK(v0 && v1 && a0 && a1)) return;
    CHECK(v0->linkId == a0->linkId && v1->linkId == a1->linkId && v0->linkId != v1->linkId); // pairs stay 1:1
    CHECK_EQ(f.p.clipName(*v1), QString("P2"));

    CHECK(f.ed.multicamCut(600, 2, Mode::Both));
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[0-300|0-299] M2[300-600|300-599] M3[600-1020|600-1019]  V2:  "
                                            "A1: M1[0-300|0-299] M2[300-600|300-599] M3[600-1020|600-1019]  A2:"));
    f.undo->undo();
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-300|0-299] M2[300-1020|300-1019]  V2:  A1: M1[0-300|0-299] M2[300-1020|300-1019]  A2:"));
    f.undo->undo();
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[0-1020|0-1019]  V2:  A1: M1[0-1020|0-1019]  A2:"));
}

void testCutToSameAngleDoesNothing()
{
    Fixture f;
    f.placed();
    const int steps = f.undo->count();
    CHECK(!f.ed.multicamCut(300, 0, Mode::Both));
    CHECK_EQ(f.undo->count(), steps);
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[0-1020|0-1019]  V2:  A1: M1[0-1020|0-1019]  A2:"));
}

void testCutAtSegmentStartOnlySwitches()
{
    // A cut exactly on an existing edit (or the clip start) switches that segment instead of making a 0-frame one
    Fixture f;
    f.placed(100);
    CHECK(f.ed.multicamCut(400, 1, Mode::Both));
    CHECK(f.ed.multicamCut(400, 2, Mode::Both));
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[100-400|0-299] M3[400-1120|300-1019]  V2:  A1: M1[100-400|0-299] M3[400-1120|300-1019]  A2:"));
    CHECK(f.ed.multicamCut(100, 1, Mode::Both));
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M2[100-400|0-299] M3[400-1120|300-1019]  V2:  A1: M2[100-400|0-299] M3[400-1120|300-1019]  A2:"));
    // Outside the multicam clip: nothing to cut
    CHECK(!f.ed.multicamCut(1120, 0, Mode::Both));
    CHECK(!f.ed.multicamCut(50, 0, Mode::Both));
}

void testCutVideoOnlyAndAudioOnly()
{
    // Video only: the sound keeps its angle (a through edit, so the pairs stay linked 1:1); audio only the other way
    Fixture f;
    f.placed();
    CHECK(f.ed.multicamCut(300, 1, Mode::Video));
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-300|0-299] M2[300-1020|300-1019]  V2:  A1: M1[0-300|0-299] M1[300-1020|300-1019]  A2:"));
    CHECK(f.ed.multicamCut(600, 2, Mode::Audio));
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[0-300|0-299] M2[300-600|300-599] M2[600-1020|600-1019]  V2:  "
                                            "A1: M1[0-300|0-299] M1[300-600|300-599] M3[600-1020|600-1019]  A2:"));
    const Timeline& tl = f.p.timeline();
    for (int i = 0; i < 3; ++i) CHECK(clipOn(tl, TrackKind::Video, 0, i)->linkId == clipOn(tl, TrackKind::Audio, 0, i)->linkId);
    // Video already shows angle 2 there: a video-only cut to it changes nothing
    const int steps = f.undo->count();
    CHECK(!f.ed.multicamCut(700, 1, Mode::Video));
    CHECK_EQ(f.undo->count(), steps);
    // Both: picture and sound go to angle 1 together
    CHECK(f.ed.multicamCut(800, 0, Mode::Both));
    const Clip* v = clipOn(f.p.timeline(), TrackKind::Video, 0, 3);
    const Clip* a = clipOn(f.p.timeline(), TrackKind::Audio, 0, 3);
    CHECK(v && a && v->start == 800 && v->angle == 0 && a->angle == 0);
}

void testSwitchChangesSegmentWithoutCut()
{
    Fixture f;
    f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    const int steps = f.undo->count();
    CHECK(f.ed.multicamSwitch(450, 2, Mode::Both));
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-300|0-299] M3[300-1020|300-1019]  V2:  A1: M1[0-300|0-299] M3[300-1020|300-1019]  A2:"));
    CHECK(f.ed.multicamSwitch(100, 1, Mode::Video));
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M2[0-300|0-299] M3[300-1020|300-1019]  V2:  A1: M1[0-300|0-299] M3[300-1020|300-1019]  A2:"));
    CHECK(!f.ed.multicamSwitch(100, 1, Mode::Video)); // already
    CHECK(!f.ed.multicamSwitch(2000, 1, Mode::Both)); // no multicam clip there
    CHECK_EQ(f.undo->count(), steps + 2);
}

void testSetAngleOnSelectedClips()
{
    // Right click → switch angle on several segments at once (ids may be video or audio parts)
    Fixture f;
    f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    f.ed.multicamCut(600, 2, Mode::Both);
    const Timeline& tl = f.p.timeline();
    const int first = idOn(tl, TrackKind::Video, 0, 0), third = idOn(tl, TrackKind::Audio, 0, 2);
    const int steps = f.undo->count();
    f.ed.setMulticamAngle({first, third}, 1, Mode::Both);
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M2[0-300|0-299] M2[300-600|300-599] M2[600-1020|600-1019]  V2:  "
                                            "A1: M2[0-300|0-299] M2[300-600|300-599] M2[600-1020|600-1019]  A2:"));
    f.ed.setMulticamAngle({first}, 0, Mode::Audio); // only the sound of that segment
    CHECK(clipOn(f.p.timeline(), TrackKind::Video, 0, 0)->angle == 1 && clipOn(f.p.timeline(), TrackKind::Audio, 0, 0)->angle == 0);
    // Nothing usable: no undo step
    const int now = f.undo->count();
    f.ed.setMulticamAngle({}, 0, Mode::Both);
    f.ed.setMulticamAngle({first}, 9, Mode::Both);
    CHECK_EQ(f.undo->count(), now);
}

void testCutTargetsSelectedOrTopMostMulticamClip()
{
    Fixture f;
    const int a = f.placed();                                                              // V1/A1, 3 angles
    const int b = f.ed.createMulticamClip("Two", {{"X", {{"/x/pov4.mp4", 0}}}, {"Y", {{"/x/pov1.mp4", 0}}}});
    CHECK(f.ed.addSequenceAt(b, 0, 1)); // V2/A2, 2 angles, 0..1000
    f.p.addMedia(media("/x/title.png", 50, true, false));
    f.ed.addMediaAt({"/x/title.png"}, 100, 2); // plain clip on V3 above both: not a multicam clip
    // Without selection: top-most multicam clip (V2)
    CHECK(f.ed.multicamCut(100, 1, Mode::Both));
    CHECK(clipOn(f.p.timeline(), TrackKind::Video, 1, 1) && clipOn(f.p.timeline(), TrackKind::Video, 1, 1)->sequenceId == b);
    CHECK_EQ(f.p.timeline().video[0].clips.size(), 1);
    // That clip has only 2 angles
    CHECK(!f.ed.multicamCut(200, 2, Mode::Both));
    // Selected multicam clip under the playhead wins
    f.sel.set({idOn(f.p.timeline(), TrackKind::Video, 0, 0)});
    CHECK(f.ed.multicamCut(200, 2, Mode::Both));
    CHECK_EQ(f.p.timeline().video[0].clips.size(), 2);
    CHECK_EQ(clipOn(f.p.timeline(), TrackKind::Video, 0, 1)->sequenceId, a);
    // No multicam clip at the frame
    f.sel.clear();
    CHECK(!f.ed.multicamCut(5000, 0, Mode::Both));
}

void testAngleOutOfRangeAndAudioOnlyAngle()
{
    Fixture f;
    const int seq = f.ed.createMulticamClip(
        "Cast", {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/pov2.mp4", 0}}}, {"Caster", {{"/x/caster.wav", 0}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    const int steps = f.undo->count();
    CHECK(!f.ed.multicamCut(100, 3, Mode::Both));
    CHECK(!f.ed.multicamCut(100, -1, Mode::Both));
    CHECK(!f.ed.multicamCut(100, 2, Mode::Video)); // the caster has no picture
    CHECK(!f.ed.multicamSwitch(100, 2, Mode::Video));
    CHECK_EQ(f.undo->count(), steps);
    // Both: only the sound goes to the caster, the picture stays
    CHECK(f.ed.multicamCut(100, 2, Mode::Both));
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-100|0-99] M1[100-1200|100-1199]  V2:  A1: M1[0-100|0-99] M3[100-1200|100-1199]  A2:"));
}

// ---- Segments are normal clips ----

void testSegmentsBehaveLikeNormalClips()
{
    Fixture f;
    f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    f.ed.multicamCut(600, 2, Mode::Both);
    const int left = idOn(f.p.timeline(), TrackKind::Video, 0, 0);
    const int middle = idOn(f.p.timeline(), TrackKind::Video, 0, 1);

    // Blade inside a segment: both pieces keep the angle
    f.ed.bladeAt(middle, 450);
    CHECK_EQ(mdump(f.p.timeline()).section("  ", 0, 0), QString("V1: M1[0-300|0-299] M2[300-450|300-449] M2[450-600|450-599] "
                                                                "M3[600-1020|600-1019]"));
    f.undo->undo();

    // Roll the cut between angle 1 and 2 by 20 frames: angles stay, the sequence frames follow
    const TimelineOps::TrimEdit roll = f.ed.trimEdit(TimelineOps::TrimKind::Roll, left, TimelineOps::Edge::End);
    f.ed.applyTrimEdit(roll, 20);
    CHECK_EQ(mdump(f.p.timeline()).section("  ", 0, 0),
             QString("V1: M1[0-320|0-319] M2[320-600|320-599] M3[600-1020|600-1019]"));
    f.undo->undo();

    // Ripple delete of the middle segment
    f.sel.set(linked(f.ed, middle));
    f.ed.rippleDeleteSelection();
    CHECK_EQ(mdump(f.p.timeline()),
             QString("V1: M1[0-300|0-299] M3[300-720|600-1019]  V2:  A1: M1[0-300|0-299] M3[300-720|600-1019]  A2:"));
    f.undo->undo();

    // Move: shows the same sequence frames elsewhere
    f.ed.moveClips({middle}, 2000, TrackKind::Video, 0);
    const Clip* moved = TimelineOps::findClip(f.p.timeline(), middle);
    CHECK(moved && moved->start == 2300 && moved->in == 300 && moved->angle == 1);
    f.undo->undo();

    // Cross dissolve between two angles (the sequence has material on both sides of the cut)
    TransitionStyle style;
    f.ed.addTransitionAt(left, middle, style);
    const Clip* l = TimelineOps::findClip(f.p.timeline(), left);
    const Clip* m = TimelineOps::findClip(f.p.timeline(), middle);
    CHECK(l && m && l->transOut > 0 && m->transIn > 0);
    CHECK(!f.ed.transitions({TrackKind::Video, 0}).isEmpty());
    f.undo->undo();

    // Copy/paste keeps the multicam clip and its angle
    f.sel.set(linked(f.ed, middle));
    f.ed.copySelection();
    f.ed.paste(3000);
    const Clip* pasted = clipOn(f.p.timeline(), TrackKind::Video, 0, 3);
    CHECK(pasted && pasted->isMulticam() && pasted->angle == 1 && pasted->start == 3000 && pasted->in == 300);
    const Clip* pastedAudio = clipOn(f.p.timeline(), TrackKind::Audio, 0, 3);
    CHECK(pastedAudio && pastedAudio->isMulticam() && pastedAudio->linkId == pasted->linkId);
}

void testTrimLimitedBySequenceLength()
{
    Fixture f;
    f.placed(100);
    f.ed.multicamCut(400, 1, Mode::Both);
    const int first = idOn(f.p.timeline(), TrackKind::Video, 0, 0);
    const int last = idOn(f.p.timeline(), TrackKind::Video, 0, 1);
    CHECK_EQ(f.ed.clampTrim(last, TimelineOps::Edge::End, 50), 0);     // sequence ends at frame 1019
    CHECK_EQ(f.ed.clampTrim(first, TimelineOps::Edge::Start, -50), 0); // starts at frame 0
    f.ed.trimClip(last, TimelineOps::Edge::End, -100);
    CHECK_EQ(TimelineOps::findClip(f.p.timeline(), last)->out, 919);
    CHECK_EQ(f.ed.clampTrim(last, TimelineOps::Edge::End, 500), 100);
}

// ---- Flatten ----

void testFlattenReplacesWithAngleClips()
{
    // Angle 1 = OBS file with three streams, angle 2 = pov2 (starts 20 frames later in the sequence)
    Fixture f;
    const int seq = f.ed.createMulticamClip("Flat", {{"Theo", {{"/x/obs1.mkv", 0}}}, {"P2", {{"/x/pov2.mp4", 20}}}});
    f.ed.addSequenceAt(seq, 100, 0); // 100..1120
    f.ed.multicamCut(400, 1, Mode::Both);
    const int steps = f.undo->count();
    CHECK(f.ed.flattenMulticamClips({idOn(f.p.timeline(), TrackKind::Video, 0, 0), idOn(f.p.timeline(), TrackKind::Video, 0, 1)}));
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(Check::dump(f.p.timeline()), QString("V1: obs1[100-400|0-299] pov2[400-1120|280-999]  V2:  "
                                                  "A1: obs1[100-400|0-299] pov2[400-1120|280-999]  A2: obs1[100-400|0-299]  "
                                                  "A3: obs1[100-400|0-299]"));
    const Timeline& tl = f.p.timeline();
    const Clip* v0 = clipOn(tl, TrackKind::Video, 0, 0);
    if (!CHECK(v0)) return;
    for (int k = 0; k < 3; ++k) {
        const Clip* a = clipOn(tl, TrackKind::Audio, k, 0);
        CHECK(a && a->audioStream == k && a->linkId == v0->linkId && !a->isMulticam());
    }
    CHECK(clipOn(tl, TrackKind::Video, 0, 1)->linkId == clipOn(tl, TrackKind::Audio, 0, 1)->linkId);
    CHECK(f.p.sequence(seq)); // stays in the Media Pool
    f.undo->undo();
    CHECK_EQ(mdump(f.p.timeline()), QString("V1: M1[100-400|0-299] M2[400-1120|300-1019]  V2:  "
                                            "A1: M1[100-400|0-299] M2[400-1120|300-1019]  A2:"));

    // Occupied tracks are not overwritten: the extra streams go above the music on A2
    f.ed.addMediaAt({"/x/music.mp3"}, 0, 1);
    CHECK(f.ed.flattenMulticamClips({idOn(f.p.timeline(), TrackKind::Video, 0, 0)}));
    const QString d = Check::dump(f.p.timeline());
    CHECK(d.contains("A2: music[0-3000|0-2999]"));
    CHECK(d.contains("A3: obs1[100-400|0-299]") && d.contains("A4: obs1[100-400|0-299]"));
    CHECK(mdump(f.p.timeline()).contains("M2[400-1120|300-1019]")); // only the selected segment
}

void testFlattenAcrossGapAndAudioOnlyAngle()
{
    Fixture f;
    const int seq = f.ed.createMulticamClip(
        "Gap", {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/part1.mp4", 0}, {"/x/part2.mp4", 500}}}, {"Caster", {{"/x/caster.wav", 0}}}});
    f.ed.addSequenceAt(seq, 0, 0); // 0..1200
    // Segment 200..700 on angle 2 (gap 300..500 in it), sound from the caster
    f.ed.multicamCut(200, 1, Mode::Video);
    f.ed.multicamCut(200, 2, Mode::Audio);
    f.ed.multicamCut(700, 0, Mode::Both);
    const int mid = idOn(f.p.timeline(), TrackKind::Video, 0, 1);
    CHECK(f.ed.flattenMulticamClips({mid}));
    const QString d = Check::dump(f.p.timeline());
    CHECK(d.contains("part1[200-300|200-299] part2[500-700|0-199]")); // picture: both files, gap stays empty
    CHECK(d.contains("caster[200-700|200-699]"));                     // sound: the caster
    CHECK_EQ(d.count("part1["), 1); // no sound of the parts (the segment's sound was the caster)
    CHECK_EQ(d.count("part2["), 1);

    // A segment entirely inside the gap leaves nothing (no empty clips)
    Fixture g;
    const int s2 = g.ed.createMulticamClip("Gap", {{"P1", {{"/x/pov1.mp4", 0}}}, {"P2", {{"/x/part1.mp4", 0}, {"/x/part2.mp4", 500}}}});
    g.ed.addSequenceAt(s2, 0, 0);
    g.ed.multicamCut(350, 1, Mode::Both);
    g.ed.multicamCut(450, 0, Mode::Both);
    CHECK(g.ed.flattenMulticamClips({idOn(g.p.timeline(), TrackKind::Video, 0, 1)}));
    CHECK_EQ(Check::dump(g.p.timeline()).count("part"), 0);
}

void testFlattenKeepsSegmentAttributes()
{
    // What was set on the segment (zoom, effect, volume, fade) stays on the clips that replace it
    Fixture f;
    f.placed();
    f.ed.multicamSwitch(0, 1, Mode::Both); // pov2 covers the sequence from frame 0
    const int v = idOn(f.p.timeline(), TrackKind::Video, 0, 0);
    const int a = idOn(f.p.timeline(), TrackKind::Audio, 0, 0);
    f.ed.modifyClips({v}, "zoom", [](Clip& c) { c.transform.zoomX = c.transform.zoomY = 1.25; });
    f.ed.addEffect({v}, "blur");
    f.ed.setClipVolume(a, 3.0);
    f.ed.setClipFade(a, TimelineOps::Edge::Start, 10);
    CHECK(f.ed.flattenMulticamClips({v}));
    const Clip* fv = clipOn(f.p.timeline(), TrackKind::Video, 0, 0);
    const Clip* fa = clipOn(f.p.timeline(), TrackKind::Audio, 0, 0);
    if (!CHECK(fv && fa && !fv->isMulticam())) return;
    CHECK(fv->mediaPath == "/x/pov2.mp4" && fv->start == 0 && fa->mediaPath == "/x/pov2.mp4");
    CHECK(fv->transform.zoomX == 1.25 && EffectRegistry::has(*fv, "blur"));
    CHECK(fa->volumeDb == 3.0 && fa->fadeIn == 10);
}

// ---- Resync and angles afterwards ----

void testSourceAtAndSequenceFrame()
{
    Fixture f;
    const int seq = f.placed(100);
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s)) return;
    // pov1 sits at 20, pov2 at 0, pov3 at 120..920
    auto at = [&](int angle, int frame, TrackKind kind = TrackKind::Video) { return Multicam::sourceAt(*s, angle, frame, kind); };
    CHECK(at(0, 500) && at(0, 500)->path == "/x/pov1.mp4" && at(0, 500)->frame == 480);
    CHECK(at(1, 500) && at(1, 500)->frame == 500);
    CHECK(at(2, 500) && at(2, 500)->frame == 380);
    CHECK(!at(2, 50));   // pov3 not started yet
    CHECK(!at(2, 950));  // already over
    CHECK(!at(3, 500));  // no such angle
    CHECK(!at(0, 10));   // pov1 starts at 20
    CHECK(at(1, 500, TrackKind::Audio) && at(1, 500, TrackKind::Audio)->path == "/x/pov2.mp4");
    const Clip* seg = clipOn(f.p.timeline(), TrackKind::Video, 0, 0);
    if (!CHECK(seg)) return;
    CHECK_EQ(Multicam::sequenceFrame(*seg, 100), 0);
    CHECK_EQ(Multicam::sequenceFrame(*seg, 600), 500);
}

void testResyncInMulticamSequence()
{
    Fixture f;
    const int seq = f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    const QString cut = mdump(f.p.timeline());
    auto pov2At500 = [&] { return Multicam::sourceAt(*f.p.sequence(seq), 1, 500)->frame; };
    CHECK_EQ(pov2At500(), 500);

    // Open the multicam clip like a timeline and slide pov2 (picture and sound) 5 frames later
    const int timeline = f.p.currentSequence();
    f.p.setCurrentSequence(seq);
    f.ed.moveClips(f.ed.withLinked({idOn(f.p.timeline(), TrackKind::Video, 1, 0)}), 5, TrackKind::Video, 0);
    CHECK_EQ(clipOn(f.p.timeline(), TrackKind::Audio, 1, 0)->start, 5);
    f.p.setCurrentSequence(timeline);
    CHECK_EQ(pov2At500(), 495);
    CHECK_EQ(mdump(f.p.timeline()), cut); // the edit itself stays, it just shows the corrected frames

    // Nudge from outside (grid/menu): moves all tracks of the angle, one undo step, others untouched
    const int steps = f.undo->count();
    f.ed.nudgeMulticamAngle(seq, 1, -5);
    CHECK_EQ(f.undo->count(), steps + 1);
    CHECK_EQ(pov2At500(), 500);
    CHECK_EQ(Multicam::sourceAt(*f.p.sequence(seq), 0, 500)->frame, 480);
    // Not before the start of the sequence
    f.ed.nudgeMulticamAngle(seq, 1, 10);
    f.ed.nudgeMulticamAngle(seq, 1, -1000);
    CHECK_EQ(clipOn(f.p.sequence(seq)->timeline, TrackKind::Video, 1, 0)->start, 0);
    CHECK_EQ(clipOn(f.p.sequence(seq)->timeline, TrackKind::Audio, 1, 0)->start, 0);
    CHECK_EQ(f.undo->count(), steps + 3);
    f.ed.nudgeMulticamAngle(seq, 1, -5);                  // already at the start: nothing
    f.ed.nudgeMulticamAngle(seq, 7, 5);                   // no such angle: nothing
    f.ed.nudgeMulticamAngle(f.p.currentSequence(), 0, 5); // not a multicam clip: nothing
    CHECK_EQ(f.undo->count(), steps + 3);
}

void testRenameAndAddAngle()
{
    Fixture f;
    const int seq = f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    f.ed.renameMulticamAngle(seq, 1, " Spieler 2 ");
    CHECK_EQ(f.p.sequence(seq)->angles.value(1).name, QString("Spieler 2"));
    CHECK_EQ(f.p.clipName(*clipOn(f.p.timeline(), TrackKind::Video, 0, 1)), QString("Spieler 2"));
    f.undo->undo();
    CHECK_EQ(f.p.sequence(seq)->angles.value(1).name, QString("P2"));
    f.ed.renameMulticamAngle(seq, 1, "  "); // empty = file name
    CHECK_EQ(f.p.sequence(seq)->angles.value(1).name, QString("pov2"));

    // A forgotten POV: offset relative to the reference (pov1, which sits at sequence frame 20)
    const QString before = mdump(f.p.timeline());
    const int steps = f.undo->count();
    const int added = f.ed.addMulticamAngle(seq, {"P4", {{"/x/pov4.mp4", 40}}});
    CHECK_EQ(added, 3);
    CHECK_EQ(f.undo->count(), steps + 1);
    const Sequence* s = f.p.sequence(seq);
    CHECK(s->angles.value(3) == (MulticamAngle{"P4", 3, {3}}));
    CHECK_EQ(clipOn(s->timeline, TrackKind::Video, 3, 0)->start, 60);
    CHECK_EQ(mdump(f.p.timeline()), before); // segments keep their angles
    // Starting before the sequence: the part before frame 0 is left out
    const int early = f.ed.addMulticamAngle(seq, {"P5", {{"/x/pov4.mp4", -50}}});
    const Clip* e = clipOn(f.p.sequence(seq)->timeline, TrackKind::Video, early, 0);
    CHECK(e && e->start == 0 && e->in == 30);
    CHECK_EQ(f.ed.addMulticamAngle(f.p.currentSequence(), {"X", {{"/x/pov4.mp4", 0}}}), -1);
    CHECK_EQ(f.ed.addMulticamAngle(seq, {"X", {{"/x/missing.mp4", 0}}}), -1);
}

void testMulticamSequenceProtections()
{
    Fixture f;
    const int seq = f.placed();
    const int timeline = f.p.currentSequence();
    CHECK(!f.p.canNest(seq, seq));
    CHECK(!f.p.canNest(timeline, seq)); // the timeline contains the multicam clip
    // A multicam clip is not taken apart like a compound clip
    const int v = idOn(f.p.timeline(), TrackKind::Video, 0, 0);
    CHECK(!f.ed.decomposeCompoundClips({v}));
    CHECK(clipOn(f.p.timeline(), TrackKind::Video, 0, 0)->isMulticam());
    // Inside: angle tracks can't be removed or reordered (the angle table points at them)
    f.p.setCurrentSequence(seq);
    CHECK(!f.ed.canRemoveTrack({TrackKind::Video, 0}));
    CHECK(!f.ed.canRemoveTrack({TrackKind::Audio, 2}));
    const QString inside = Check::dump(f.p.timeline());
    f.ed.moveTrack(TrackKind::Video, 0, 2);
    CHECK_EQ(Check::dump(f.p.timeline()), inside);
    CHECK(!f.ed.addSequenceAt(seq, 0, 0));
    f.p.setCurrentSequence(timeline);
    // Duplicated timelines point to the same multicam clip (not a copy of it)
    const int dup = f.p.duplicateSequence(timeline);
    CHECK_EQ(clipOn(f.p.sequence(dup)->timeline, TrackKind::Video, 0, 0)->sequenceId, seq);
}

void testClipAtForViewer()
{
    Fixture f;
    const int a = f.placed();
    const int b = f.ed.createMulticamClip("Two", {{"X", {{"/x/pov4.mp4", 0}}}, {"Y", {{"/x/pov1.mp4", 0}}}});
    f.ed.addSequenceAt(b, 500, 1); // V2/A2 500..1500
    const Timeline& tl = f.p.timeline();
    const Clip* c = Multicam::clipAt(tl, 100);
    CHECK(c && c->sequenceId == a && clipOn(tl, TrackKind::Video, 0, 0) == c);
    c = Multicam::clipAt(tl, 600); // top-most
    CHECK(c && c->sequenceId == b);
    c = Multicam::clipAt(tl, 600, {idOn(tl, TrackKind::Audio, 0, 0)}); // selected (audio part) wins
    CHECK(c && c->sequenceId == a);
    CHECK(!Multicam::clipAt(tl, 1600));
    // Disabled clips don't show: the next one down
    f.ed.modifyClips({idOn(tl, TrackKind::Video, 1, 0)}, "off", [](Clip& x) { x.enabled = false; });
    c = Multicam::clipAt(f.p.timeline(), 600);
    CHECK(c && c->sequenceId == a);
    // Only sound left (picture deleted): the audio part
    f.ed.setLinkedSelection(false);
    f.sel.set({idOn(f.p.timeline(), TrackKind::Video, 0, 0), idOn(f.p.timeline(), TrackKind::Video, 1, 0)});
    f.ed.deleteSelection();
    c = Multicam::clipAt(f.p.timeline(), 100);
    CHECK(c && c->sequenceId == a && clipOn(f.p.timeline(), TrackKind::Audio, 0, 0) == c);
}

// ---- Project file and undo ----

void testProjectFileRoundTrip()
{
    Fixture f;
    const int seq = f.ed.createMulticamClip(
        "Turnier", {{"Theo", {{"/x/obs1.mkv", 0}}}, {"P2", {{"/x/part1.mp4", 0}, {"/x/part2.mp4", 500}}}, {"Caster", {{"/x/caster.wav", -10}}}});
    f.ed.addSequenceAt(seq, 25, 0);
    f.ed.multicamCut(300, 1, Mode::Video);
    f.ed.multicamCut(400, 2, Mode::Audio);
    f.ed.renameMulticamAngle(seq, 0, "Theo POV");
    const QByteArray j = json(f.p);
    ProjectData back;
    QString err;
    if (!CHECK(ProjectFile::fromJson(j, "/x/p.schneidi", &back, &err))) return;
    Project q;
    q.load(back);
    const Sequence* s = q.sequence(seq);
    if (!CHECK(s && s->multicam)) return;
    CHECK(s->angles == f.p.sequence(seq)->angles);
    CHECK_EQ(Check::dump(s->timeline), Check::dump(f.p.sequence(seq)->timeline));
    CHECK_EQ(mdump(q.timeline()), mdump(f.p.timeline()));
    for (int i = 0; i < 3; ++i) {
        const Clip* x = clipOn(q.timeline(), TrackKind::Audio, 0, i);
        const Clip* y = clipOn(f.p.timeline(), TrackKind::Audio, 0, i);
        CHECK(x && y && x->isMulticam() && x->angle == y->angle && x->sequenceId == seq && x->linkId == y->linkId);
    }
    CHECK_EQ(json(q), j);
}

void testProjectFileRepairsBrokenMulticam()
{
    Fixture f;
    const int seq = f.placed();
    f.ed.multicamCut(300, 1, Mode::Both);
    ProjectData d = f.p.data();
    // Angle beyond the table -> first angle; angle track that does not exist -> left out of the angle
    // (the open timeline is read from ProjectData::timeline)
    d.timeline.video[0].clips[1].angle = 7;
    for (Sequence& s : d.sequences)
        if (s.id == seq) {
            s.angles[2].videoTrack = 9;
            s.angles[2].audioTracks = {2, 7};
        }
    Project q;
    q.load(d);
    CHECK(q.sequence(seq)->angles.value(2) == (MulticamAngle{"P3", -1, {2}}));
    CHECK_EQ(clipOn(q.timeline(), TrackKind::Video, 0, 1)->angle, 0);

    // Multicam clip pointing at a missing sequence or at a normal timeline: removed like a broken compound clip
    ProjectData missing = f.p.data();
    missing.sequences.removeIf([&](const Sequence& s) { return s.id == seq; });
    Project r;
    r.load(missing);
    CHECK_EQ(mdump(r.timeline()).count("M"), 0);
    ProjectData plain = f.p.data();
    for (Sequence& s : plain.sequences)
        if (s.id == seq) s.multicam = false;
    Project t;
    t.load(plain);
    CHECK_EQ(mdump(t.timeline()).count("M"), 0);
}

void testUndoRedoChain()
{
    // Every step one undo step; all the way back and forth again gives the same project file each time
    Fixture f;
    QVector<QByteArray> states{json(f.p)};
    const int seq = f.ed.createMulticamClip("Match", threeAngles());
    states << json(f.p);
    f.ed.addSequenceAt(seq, 10, 0);
    states << json(f.p);
    f.ed.multicamCut(200, 1, Mode::Both);
    states << json(f.p);
    f.ed.multicamCut(400, 2, Mode::Video);
    states << json(f.p);
    f.ed.multicamSwitch(250, 2, Mode::Audio);
    states << json(f.p);
    f.ed.renameMulticamAngle(seq, 2, "Observer");
    states << json(f.p);
    f.ed.nudgeMulticamAngle(seq, 2, 3);
    states << json(f.p);
    f.ed.addMulticamAngle(seq, {"P4", {{"/x/pov4.mp4", 0}}});
    states << json(f.p);
    f.ed.flattenMulticamClips({idOn(f.p.timeline(), TrackKind::Video, 0, 1)});
    states << json(f.p);
    for (int i = states.size() - 1; i > 0; --i) {
        CHECK(json(f.p) == states[i]);
        f.undo->undo();
    }
    CHECK(json(f.p) == states[0]);
    for (int i = 1; i < states.size(); ++i) {
        f.undo->redo();
        CHECK(json(f.p) == states[i]);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("multicam");

    testCreateMulticamSequence();
    testCreateWithMultiStreamAngles();
    testCreateWithSplitRecording();
    testCreateAudioOnlyAngle();
    testCreateRejectsBadInput();
    testPlaceMulticamClip();
    testCutToAngleSplitsAndSwitches();
    testCutToSameAngleDoesNothing();
    testCutAtSegmentStartOnlySwitches();
    testCutVideoOnlyAndAudioOnly();
    testSwitchChangesSegmentWithoutCut();
    testSetAngleOnSelectedClips();
    testCutTargetsSelectedOrTopMostMulticamClip();
    testAngleOutOfRangeAndAudioOnlyAngle();
    testSegmentsBehaveLikeNormalClips();
    testTrimLimitedBySequenceLength();
    testFlattenReplacesWithAngleClips();
    testFlattenAcrossGapAndAudioOnlyAngle();
    testFlattenKeepsSegmentAttributes();
    testSourceAtAndSequenceFrame();
    testResyncInMulticamSequence();
    testRenameAndAddAngle();
    testMulticamSequenceProtections();
    testClipAtForViewer();
    testProjectFileRoundTrip();
    testProjectFileRepairsBrokenMulticam();
    testUndoRedoChain();
    return Check::result();
}
