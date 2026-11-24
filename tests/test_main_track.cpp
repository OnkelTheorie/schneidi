// SPDX-License-Identifier: AGPL-3.0-only
// The magnetic main track (core/MainTrack.h, Editor::setMagneticMain; desktop "Lücken automatisch schließen (Spur 1)",
// phone "Hauptspur magnetisch"): with the switch on, deleting, trimming, moving and cutting out a range on V1 close the
// gap, placing onto V1 (drop, paste, a title, a clip moved there) pushes the later clips right at the nearest cut,
// moving along V1 reorders, a longer V1 clip pushes what follows; linked sound stays in sync, other tracks stay
// (except linked clips), locked tracks are respected, every action is one undo step that undo/redo restore exactly,
// subtitles and markers follow like a ripple. With the switch off everything stays as before (gaps, overwrites).
// TimelineGuard checks the invariants and the sync of linked clips after every step.
#include "check.h"
#include "timeline_guard.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/MainTrack.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QUndoStack>

using Check::dump;

namespace {

Clip mk(int id, const char* name, int start, int len, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = QString("/x/%1.mp4").arg(name);
    c.start = start;
    c.in = 0;
    c.out = len - 1;
    c.linkId = link;
    return c;
}

// V1: a[0-100] b[100-160] c[160-240], each with its sound on A1 (ids 11, 12, 13, linked 1, 2, 3). The files are long
// enough to make clips longer (200 frames: a and b start at file frame 0, so their heads cannot grow).
struct Fixture {
    Project project;
    TimelineGuard guard{project};
    Selection sel;
    Editor editor{&project, &sel};
    explicit Fixture(bool magnetic = true)
    {
        while (project.newClipId() < 100) {} // (the fixed ids below are taken)
        while (project.newLinkId() < 100) {}
        for (const char* n : {"a", "b", "c", "x"})
            project.addMedia(MediaInfo{QString("/x/%1.mp4").arg(n), QString("%1.mp4").arg(n), 200, true, true, false});
        project.edit("setup", [](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 2);
            tl.video[0].clips = {mk(1, "a", 0, 100, 1), mk(2, "b", 100, 60, 2), mk(3, "c", 160, 80, 3)};
            tl.audio[0].clips = {mk(11, "a", 0, 100, 1), mk(12, "b", 100, 60, 2), mk(13, "c", 160, 80, 3)};
        });
        project.undoStack()->clear();
        editor.setMagneticMain(magnetic);
    }
    const Timeline& tl() const { return project.timeline(); }
    QString d() const { return dump(project.timeline()); }
    int steps() { return project.undoStack()->index(); }
    // One undo step since `before`; undo gives `before` back exactly, redo `after`
    void checkUndo(const Timeline& before, int stepsBefore, const char* what, int line)
    {
        const Timeline after = tl();
        Check::report(steps() == stepsBefore + 1, "one undo step", __FILE__, line, what);
        project.undoStack()->undo();
        Check::report(tl() == before, "undo restores", __FILE__, line, what);
        project.undoStack()->redo();
        Check::report(tl() == after, "redo restores", __FILE__, line, what);
    }
};
#define CHECK_ONE_STEP(f, before, steps, what) (f).checkUndo((before), (steps), (what), __LINE__)

const char* kStart = "V1: a[0-100|0-99] b[100-160|0-59] c[160-240|0-79]  V2:  "
                     "A1: a[0-100|0-99] b[100-160|0-59] c[160-240|0-79]  A2:";

void testOff()
{
    // Switch off (the default): a gap stays, a drop overwrites, a move overwrites - as before
    Fixture f(false);
    CHECK(!f.editor.magneticMain());
    CHECK_EQ(f.d(), QString(kStart));
    f.sel.set({2, 12});
    f.editor.deleteSelection();
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[160-240|0-79]  V2:  A1: a[0-100|0-99] c[160-240|0-79]  A2:"));
    // Packing only on request (the phone's reorder, the switch turned on)
    CHECK(f.editor.packTrack(MainTrack::kMain));
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[100-180|0-79]  V2:  A1: a[0-100|0-99] c[100-180|0-79]  A2:"));
    f.editor.addMediaAt({"/x/x.mp4"}, 50);
    CHECK_EQ(f.tl().video[0].clips[0].length(), 50); // a overwritten from 50 on
    CHECK_EQ(f.tl().video[0].clips[1].start, 50);
}

void testDelete()
{
    Fixture f;
    const Timeline before = f.tl();
    const int steps = f.steps();
    f.sel.set({2, 12});
    f.editor.deleteSelection();
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[100-180|0-79]  V2:  A1: a[0-100|0-99] c[100-180|0-79]  A2:"));
    CHECK_EQ(f.project.undoStack()->text(f.project.undoStack()->index() - 1), T("Löschen"));
    CHECK_ONE_STEP(f, before, steps, "delete");
    // The first clip: everything moves to 0
    f.sel.set({1, 11});
    f.editor.deleteSelection();
    CHECK_EQ(f.d(), QString("V1: c[0-80|0-79]  V2:  A1: c[0-80|0-79]  A2:"));
}

void testOtherTracks()
{
    // A title on V2 and music on A2 (not linked) stay where they are; a clip on V2 linked to c goes along
    Fixture f;
    f.project.edit("others", [](Timeline& tl) {
        Clip t = mk(20, "t", 50, 30);
        t.kind = ClipKind::Title;
        tl.video[1].clips = {t, mk(21, "k", 170, 20, 3)}; // 21: a copy linked to c
        tl.audio[1].clips = {mk(22, "m", 0, 300)};
    });
    const Timeline before = f.tl();
    const int steps = f.steps();
    f.sel.set({2, 12});
    f.editor.deleteSelection();
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[100-180|0-79]  V2: T[50-80|0-29] k[110-130|0-19]  "
                            "A1: a[0-100|0-99] c[100-180|0-79]  A2: m[0-300|0-299]"));
    CHECK_ONE_STEP(f, before, steps, "delete with other tracks");
    // Moving the music (A2) leaves V1 alone, also its gaps: the switch acts on edits that touch V1
    f.project.edit("gap", [](Timeline& tl) { tl.video[1].clips.clear(); });
    f.editor.setMagneticMain(false);
    f.sel.set({3, 13});
    f.editor.deleteSelection(); // V1: a only, gap after it does not matter; put c back later
    f.project.undoStack()->undo();
    f.project.edit("a gap on V1 (switch off)", [](Timeline& tl) {
        TimelineOps::findClip(tl, 3)->start = 200;
        TimelineOps::findClip(tl, 13)->start = 200;
    });
    f.editor.setMagneticMain(true);
    f.editor.moveClips({22}, 10, TrackKind::Audio, 0);
    CHECK_EQ(TimelineOps::findClip(f.tl(), 22)->start, 10);
    CHECK_EQ(TimelineOps::findClip(f.tl(), 3)->start, 200); // V1 not touched: its gap stays
}

void testTrim()
{
    using TimelineOps::Edge;
    Fixture f;
    // Shorter at the end: the following clips move up
    Timeline before = f.tl();
    int steps = f.steps();
    f.editor.trimClip(1, Edge::End, -30);
    CHECK_EQ(f.d(), QString("V1: a[0-70|0-69] b[70-130|0-59] c[130-210|0-79]  V2:  "
                            "A1: a[0-70|0-69] b[70-130|0-59] c[130-210|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "trim end shorter");
    // Shorter at the start: the clip keeps its place, its head is gone, the rest moves up
    before = f.tl();
    steps = f.steps();
    f.editor.trimClip(2, Edge::Start, 10);
    CHECK_EQ(f.d(), QString("V1: a[0-70|0-69] b[70-120|10-59] c[120-200|0-79]  V2:  "
                            "A1: a[0-70|0-69] b[70-120|10-59] c[120-200|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "trim start shorter");
    // Longer at the end: pushes what follows (instead of stopping at or overwriting the neighbour)
    before = f.tl();
    steps = f.steps();
    CHECK_EQ(f.editor.clampTrim(1, Edge::End, 20, true), 20);
    const Timeline preview = f.editor.previewTrim(1, Edge::End, 20, true);
    f.editor.trimClip(1, Edge::End, 20, true);
    CHECK_EQ(f.d(), QString("V1: a[0-90|0-89] b[90-140|10-59] c[140-220|0-79]  V2:  "
                            "A1: a[0-90|0-89] b[90-140|10-59] c[140-220|0-79]  A2:"));
    CHECK(preview == f.tl()); // the drag's preview shows what the trim does
    CHECK_ONE_STEP(f, before, steps, "trim end longer");
    // Longer at the start: the head grows back, the clip and the rest move right
    before = f.tl();
    steps = f.steps();
    CHECK_EQ(f.editor.clampTrim(2, Edge::Start, -10), -10);
    f.editor.trimClip(2, Edge::Start, -10);
    CHECK_EQ(f.d(), QString("V1: a[0-90|0-89] b[90-150|0-59] c[150-230|0-79]  V2:  "
                            "A1: a[0-90|0-89] b[90-150|0-59] c[150-230|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "trim start longer");
    // Only as far as the material goes (a's file: 200 frames)
    CHECK_EQ(f.editor.clampTrim(1, Edge::End, 500), 110);
    f.editor.trimClip(1, Edge::End, 500);
    CHECK_EQ(f.tl().video[0].clips[0].length(), 200);
    CHECK_EQ(f.tl().video[0].clips[1].start, 200);
    CHECK_EQ(f.tl().audio[0].clips[1].start, 200);

    // Off: a longer clip stops at its neighbour (edge meets clip: stop)
    Fixture off(false);
    CHECK_EQ(off.editor.clampTrim(1, Edge::End, 20, false), 0);
}

void testMoveAlong()
{
    Fixture f;
    // a dropped with its middle past b's middle: b, a, c - nothing cut, the sound comes along
    Timeline before = f.tl();
    int steps = f.steps();
    f.editor.moveClips({1, 11}, 90, TrackKind::Video, 0);
    CHECK_EQ(f.d(), QString("V1: b[0-60|0-59] a[60-160|0-99] c[160-240|0-79]  V2:  "
                            "A1: b[0-60|0-59] a[60-160|0-99] c[160-240|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "reorder");
    // To the end
    f.editor.moveClips({2, 12}, 500, TrackKind::Video, 0);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[100-180|0-79] b[180-240|0-59]  V2:  "
                            "A1: a[0-100|0-99] c[100-180|0-79] b[180-240|0-59]  A2:"));
    // A small move that keeps the order changes nothing (no step)
    steps = f.steps();
    f.editor.moveClips({3, 13}, 5, TrackKind::Video, 0);
    CHECK_EQ(f.steps(), steps);
    // Fine positions (1/100 frame) on V1 go to whole frames
    f.editor.moveClipsFine({3, 13}, -12050, TrackKind::Video, 0);
    CHECK_EQ(f.d(), QString("V1: c[0-80|0-79] a[80-180|0-99] b[180-240|0-59]  V2:  "
                            "A1: c[0-80|0-79] a[80-180|0-99] b[180-240|0-59]  A2:"));
    // With a title on V2 in the selection: the title moves by the frames, V1 reorders
    f.project.edit("title", [](Timeline& tl) {
        Clip t = mk(20, "t", 0, 30);
        t.kind = ClipKind::Title;
        tl.video[1].clips = {t};
    });
    f.editor.moveClips({3, 13, 20}, 150, TrackKind::Video, 0);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[100-180|0-79] b[180-240|0-59]  V2: T[150-180|0-29]  "
                            "A1: a[0-100|0-99] c[100-180|0-79] b[180-240|0-59]  A2:"));
}

void testMoveOnto()
{
    Fixture f;
    f.project.edit("x on V2", [](Timeline& tl) {
        tl.video[1].clips = {mk(30, "x", 300, 40, 9)};
        tl.audio[1].clips = {mk(31, "x", 300, 40, 9)};
    });
    // Down onto V1 at 110 (inside b, nearer its start): in at 100, b and c with their sound make room
    Timeline before = f.tl();
    int steps = f.steps();
    f.editor.moveClips({30, 31}, -190, TrackKind::Video, -1);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] x[100-140|0-39] b[140-200|0-59] c[200-280|0-79]  V2:  "
                            "A1: a[0-100|0-99] x[100-140|0-39] b[140-200|0-59] c[200-280|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "move onto V1");
    // Up off V1: no gap stays there
    before = f.tl();
    steps = f.steps();
    f.editor.moveClips({2, 12}, 0, TrackKind::Video, 1);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] x[100-140|0-39] c[140-220|0-79]  V2: b[140-200|0-59]  "
                            "A1: a[0-100|0-99] x[100-140|0-39] c[140-220|0-79]  A2: b[140-200|0-59]"));
    CHECK_ONE_STEP(f, before, steps, "move off V1");
}

void testDropAndPaste()
{
    Fixture f;
    // A drop at 120 (inside b, nearer its start): in at 100, the rest 200 frames later
    Timeline before = f.tl();
    int steps = f.steps();
    f.editor.addMediaAt({"/x/x.mp4"}, 120, 0);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] x[100-300|0-199] b[300-360|0-59] c[360-440|0-79]  V2:  "
                            "A1: a[0-100|0-99] x[100-300|0-199] b[300-360|0-59] c[360-440|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "drop onto V1");
    f.project.undoStack()->undo();
    // Review 7: a picture and a sound-only file dropped together: the room on V1 is the picture's length only
    f.project.addMedia(MediaInfo{"/x/w.wav", "w.wav", 50, false, true, false});
    f.editor.addMediaAt({"/x/x.mp4", "/x/w.wav"}, 0, 0);
    CHECK_EQ(f.tl().video[0].clips.size(), 4);
    CHECK(f.tl().video[0].clips[0].length() == 200 && f.tl().video[0].clips[1].start == 200);
    CHECK_EQ(MainTrack::insertPoint(f.tl().video[0], 10000), 440); // (no gap on V1)
    f.project.undoStack()->undo();
    // Past the end: at the end of V1
    f.editor.addMediaAt({"/x/x.mp4"}, 600, 0);
    CHECK_EQ(TimelineOps::findClip(f.tl(), f.tl().video[0].clips.last().id)->start, 240);
    CHECK_EQ(f.tl().audio[0].clips.last().start, 240);
    f.project.undoStack()->undo();
    // Onto V2: overwrites as always, V1 untouched
    f.editor.addMediaAt({"/x/x.mp4"}, 20, 1);
    CHECK_EQ(f.tl().video[1].clips.size(), 1);
    CHECK_EQ(f.tl().video[1].clips[0].start, 20);
    CHECK_EQ(f.tl().video[0].clips[1].start, 100);
    f.project.undoStack()->undo();
    // From the viewer (placeSourceRange): 50 frames at 230 (inside c, nearer its end) -> after c
    CHECK_EQ(f.editor.placeSourceRange("/x/x.mp4", 0, 49, 230, 0), 50);
    CHECK_EQ(f.tl().video[0].clips.last().start, 240);
    f.project.undoStack()->undo();
    // Paste a (with its sound) at 150 (inside b, nearer its end): after b
    f.sel.set({1, 11});
    f.editor.copySelection();
    before = f.tl();
    steps = f.steps();
    f.editor.paste(150);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] b[100-160|0-59] a[160-260|0-99] c[260-340|0-79]  V2:  "
                            "A1: a[0-100|0-99] b[100-160|0-59] a[160-260|0-99] c[260-340|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "paste onto V1");
    // A title without a track goes above V1; one dropped onto V1 goes in
    f.editor.addTitle(10, -1, 30);
    CHECK_EQ(f.tl().video[1].clips.size(), 1);
    f.editor.addTitle(10, 0, 30);
    CHECK_EQ(f.d(), QString("V1: T[0-30|0-29] a[30-130|0-99] b[130-190|0-59] a[190-290|0-99] c[290-370|0-79]  "
                            "V2: T[10-40|0-29]  A1: a[30-130|0-99] b[130-190|0-59] a[190-290|0-99] c[290-370|0-79]  A2:"));
}

void testRange()
{
    // Cutting out a range without ripple: V1 (and its linked sound) closes, the rest keeps its gap
    Fixture f;
    f.project.edit("music", [](Timeline& tl) { tl.audio[1].clips = {mk(22, "m", 0, 300)}; });
    const Timeline before = f.tl();
    const int steps = f.steps();
    f.editor.deleteRange(110, 130, false);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] b[100-110|0-9] b[110-140|30-59] c[140-220|0-79]  V2:  "
                            "A1: a[0-100|0-99] b[100-110|0-9] b[110-140|30-59] c[140-220|0-79]  "
                            "A2: m[0-110|0-109] m[130-300|130-299]"));
    CHECK_ONE_STEP(f, before, steps, "range cut");
}

void testLocked()
{
    // The sound track locked: V1 cannot close (its sound would go out of sync) - the gap stays, the user is told
    Fixture f;
    f.editor.toggleTrackLock({TrackKind::Audio, 0});
    f.editor.takeNotice();
    f.sel.set({2});
    f.editor.deleteSelection();
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] c[160-240|0-79]  V2:  "
                            "A1: a[0-100|0-99] b[100-160|0-59] c[160-240|0-79]  A2:"));
    const Editor::Notice n = f.editor.takeNotice();
    CHECK(n.text.contains("A1"));
    // A drop that would push locked sound: refused, nothing placed, no step
    const int steps = f.steps();
    f.editor.addMediaAt({"/x/x.mp4"}, 10, 0);
    CHECK_EQ(f.steps(), steps);
    CHECK(f.editor.takeNotice().refused);
    f.editor.toggleTrackLock({TrackKind::Audio, 0});
    // V1 locked: nothing on it changes, nothing packs
    Fixture g(false);
    g.project.edit("gap", [](Timeline& tl) {
        TimelineOps::findClip(tl, 3)->start = 200;
        TimelineOps::findClip(tl, 13)->start = 200;
    });
    g.editor.setMagneticMain(true);
    g.editor.toggleTrackLock(MainTrack::kMain);
    CHECK(!g.editor.packTrack(MainTrack::kMain));
    g.sel.set({1, 11});
    g.editor.deleteSelection(); // (only A1's part is unlocked)
    CHECK_EQ(g.tl().video[0].clips.size(), 3);
    CHECK_EQ(TimelineOps::findClip(g.tl(), 3)->start, 200);
}

void testAnnotations()
{
    // Markers and subtitles follow like a ripple where the whole timeline moves; a marker in the closed gap lands on
    // the cut
    Fixture f;
    f.project.edit("marks", [](Timeline& tl) {
        tl.markers = {50, 130, 200};
        SubtitleTrack st;
        SubtitleCue c;
        c.id = 40;
        c.start = 170;
        c.end = 190;
        c.text = "hi";
        st.cues = {c};
        tl.subtitles = {st};
    });
    const Timeline before = f.tl();
    const int steps = f.steps();
    f.sel.set({2, 12});
    f.editor.deleteSelection();
    CHECK_EQ(f.tl().markers, (QVector<int>{50, 100, 140}));
    CHECK_EQ(f.tl().subtitles[0].cues[0].start, 110);
    CHECK_EQ(f.tl().subtitles[0].cues[0].end, 130);
    CHECK_ONE_STEP(f, before, steps, "markers follow");
    // Review 6: a subtitle in a closed gap is never deleted - it lands on the cut like a marker, keeps its length; the
    // one after it keeps its order, the earlier is shortened only as far as needed
    {
        Fixture g;
        g.project.edit("cues", [](Timeline& tl) {
            SubtitleTrack st;
            SubtitleCue a, b;
            a.id = 41, a.start = 120, a.end = 140, a.text = "in the gap";
            b.id = 42, b.start = 170, b.end = 190, b.text = "in c";
            st.cues = {a, b};
            tl.subtitles = {st};
        });
        g.sel.set({2, 12});
        g.editor.deleteSelection(); // b gone: 120-140 lies in the gap 100-160
        const auto& cues = g.tl().subtitles[0].cues;
        if (CHECK_EQ(cues.size(), 2)) {
            CHECK(cues[0].id == 41 && cues[0].start == 100 && cues[0].end == 110);
            CHECK(cues[1].id == 42 && cues[1].start == 110 && cues[1].end == 130);
        }
    }
    // An insert pushes them right
    f.editor.addMediaAt({"/x/x.mp4"}, 0, 0);
    CHECK_EQ(f.tl().markers, (QVector<int>{250, 300, 340}));
    CHECK_EQ(f.tl().subtitles[0].cues[0].start, 310);
    // A reorder leaves them (it moves pieces around, not time)
    const QVector<int> marks = f.tl().markers;
    f.editor.moveClips({1, 11}, -500, TrackKind::Video, 0);
    CHECK_EQ(f.tl().video[0].clips[0].id, 1);
    CHECK_EQ(f.tl().markers, marks);
}

void testTrackStructure()
{
    // Moving, adding or removing a video track never packs the track that becomes V1 (review 1)
    Fixture f(false);
    f.project.edit("V2 with gaps", [](Timeline& tl) { tl.video[1].clips = {mk(30, "x", 50, 20), mk(31, "x", 200, 20)}; });
    f.editor.setMagneticMain(true);
    f.editor.moveTrack(TrackKind::Video, 1, 0);
    CHECK_EQ(f.tl().video[0].clips[0].start, 50);
    CHECK_EQ(f.tl().video[0].clips[1].start, 200);
    f.project.undoStack()->undo();
    f.editor.removeTrack(MainTrack::kMain);
    CHECK_EQ(f.tl().video[0].clips[0].start, 50);
    CHECK_EQ(f.tl().video[0].clips[1].start, 200);
    f.project.undoStack()->undo();
    f.editor.addTrack(TrackKind::Video, 0);
    CHECK(f.tl().video[0].clips.isEmpty());
    CHECK_EQ(f.tl().video[2].clips[0].start, 50);
    // The track count changed: no edit of V1's clips
    Timeline a = f.tl(), b = f.tl();
    b.video.removeFirst();
    CHECK(!MainTrack::touched(a, b, MainTrack::kMain));
}

void testMulticam()
{
    // Inside a multicam sequence the angles lie in sync on purpose: nothing packs, a drop overwrites (review 2)
    Fixture f;
    f.project.editSequences("multicam", [](QVector<Sequence>& seqs, int& current) {
        for (Sequence& s : seqs)
            if (s.id == current) s.multicam = true;
    });
    CHECK(f.editor.magneticMain() && !f.project.magnetActive() && !f.editor.magnetActive());
    f.sel.set({2, 12});
    f.editor.deleteSelection();
    CHECK_EQ(TimelineOps::findClip(f.tl(), 3)->start, 160);
    // A hold (syncs, the AI's commands) does the same
    Fixture g;
    {
        const Project::MagnetHold hold(&g.project);
        CHECK(!g.project.magnetActive());
        g.sel.set({2, 12});
        g.editor.deleteSelection();
        CHECK_EQ(TimelineOps::findClip(g.tl(), 3)->start, 160);
    }
    CHECK(g.project.magnetActive());
}

void testNotBeforeZero()
{
    // Review 3: pasting a V1 clip with something that starts before it (a title 20 frames earlier) near frame 0 - the
    // insert point moves right far enough that nothing goes before frame 0
    Fixture f;
    f.project.edit("title", [](Timeline& tl) {
        Clip t = mk(20, "t", 80, 30);
        t.kind = ClipKind::Title;
        tl.video[1].clips = {t};
    });
    f.sel.set({2, 12, 20});
    f.editor.copySelection();
    const Timeline before = f.tl();
    const int steps = f.steps();
    f.editor.paste(5); // b's copy would go in at 0, the title at -20
    for (const Track& t : f.tl().video)
        for (const Clip& c : t.clips) CHECK(c.start >= 0);
    CHECK_EQ(f.d(), QString("V1: a[0-100|0-99] b[100-160|0-59] b[160-220|0-59] c[220-300|0-79]  V2: T[80-110|0-29]  " // (the copy replaced the title)
                            "A1: a[0-100|0-99] b[100-160|0-59] b[160-220|0-59] c[220-300|0-79]  A2:"));
    CHECK_ONE_STEP(f, before, steps, "paste not before 0");
    // Review 4: moving onto V1 together with a clip that limits the delta (sound on A2 that cannot go before 0): the
    // clips land in the room made for them, the pushed clip is not overwritten
    Fixture g;
    g.project.edit("x on V2, sound on A2", [](Timeline& tl) {
        tl.video[1].clips = {mk(30, "x", 300, 40)};
        tl.audio[1].clips = {mk(31, "m", 250, 20)};
    });
    g.editor.moveClips({30, 31}, -290, TrackKind::Video, -1); // x wants 10 (in at 0), m would go to -40
    CHECK_EQ(g.d(), QString("V1: a[0-100|0-99] x[100-140|0-39] b[140-200|0-59] c[200-280|0-79]  V2:  "
                            "A1: a[0-50|0-49] m[50-70|0-19] a[70-100|70-99] b[140-200|0-59] c[200-280|0-79]  A2:"));
    // insertPoint never before atLeast
    Track t;
    t.clips = {mk(1, "a", 0, 100), mk(2, "b", 100, 60)};
    CHECK_EQ(MainTrack::insertPoint(t, 10, {}, 20), 100);
    CHECK_EQ(MainTrack::insertPoint(t, 10, {}, 170), 170);
    CHECK_EQ(MainTrack::insertPoint(t, 130, {}, 100), 160);
}

void testCore()
{
    // MainTrack directly: insertPoint, openGap refusing, pack's ripple
    Track t;
    t.clips = {mk(1, "a", 0, 100), mk(2, "b", 100, 60)};
    CHECK_EQ(MainTrack::insertPoint(t, 30), 0);
    CHECK_EQ(MainTrack::insertPoint(t, 70), 100);
    CHECK_EQ(MainTrack::insertPoint(t, 125), 100);
    CHECK_EQ(MainTrack::insertPoint(t, 130), 160); // (halfway: after it)
    CHECK_EQ(MainTrack::insertPoint(t, 140), 160);
    CHECK_EQ(MainTrack::insertPoint(t, 900), 160);
    CHECK_EQ(MainTrack::insertPoint(t, 140, {2}), 100);
    Timeline tl;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
    tl.video[0].clips = {mk(1, "a", 0, 100, 1), mk(2, "b", 100, 60, 2)};
    tl.audio[0].clips = {mk(11, "a", 0, 100, 1), mk(12, "b", 100, 60, 2), mk(13, "m", 170, 30)};
    QString why;
    // b's sound pushed by 20 would run into the music at 170
    Timeline g = tl;
    CHECK(MainTrack::openGap(g, MainTrack::kMain, 100, 20, &why) == MainTrack::Result::Refused);
    CHECK(g == tl);
    CHECK(why.contains("A1"));
    CHECK(MainTrack::openGap(g, MainTrack::kMain, 100, 10) == MainTrack::Result::Moved);
    CHECK_EQ(dump(g), QString("V1: a[0-100|0-99] b[110-170|0-59]  A1: a[0-100|0-99] b[110-170|0-59] m[170-200|0-29]"));
    CHECK(MainTrack::pack(g, MainTrack::kMain) == MainTrack::Result::Moved);
    CHECK_EQ(dump(g), dump(tl));
    CHECK(MainTrack::pack(g, MainTrack::kMain) == MainTrack::Result::Unchanged);
    // touched: positions only
    Timeline h = tl;
    h.video[0].clips[0].volumeDb = -3;
    CHECK(!MainTrack::touched(tl, h, MainTrack::kMain));
    h.video[0].clips[1].start = 120;
    CHECK(MainTrack::touched(tl, h, MainTrack::kMain));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("main_track");
    I18n::install("de");
    testCore();
    testTrackStructure();
    testMulticam();
    testNotBeforeZero();
    testOff();
    testDelete();
    testOtherTracks();
    testTrim();
    testMoveAlong();
    testMoveOnto();
    testDropAndPaste();
    testRange();
    testLocked();
    testAnnotations();
    return Check::result();
}
