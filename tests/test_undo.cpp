// SPDX-License-Identifier: AGPL-3.0-only
// Test undo/redo chain: many editor operations in a row (each must create an undo step), then everything
// back = initial state, everything forward = final state; each intermediate step is compared via JSON (project file).
#include "check.h"
#include "timeline_guard.h"

#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QUndoStack>

namespace {

// State as project file JSON; counters for new IDs deliberately keep going (IDs are never reused)
QByteArray state(const Project& p)
{
    ProjectData d = p.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/projekt.schneidi");
}

int clipId(const Project& p, TrackKind k, int track, int index)
{
    const auto& tracks = p.timeline().tracks(k);
    if (track >= tracks.size() || index >= tracks[track].clips.size()) return 0;
    return tracks[track].clips[index].id;
}
int V(const Project& p, int track, int index) { return clipId(p, TrackKind::Video, track, index); }
int A(const Project& p, int track, int index) { return clipId(p, TrackKind::Audio, track, index); }

// A dissolve that loses its partner through an edit is released instead of silently turning into a
// fade out/in over black (trimming, speed)
void testBrokenDissolves()
{
    int fileLen = 250;
    auto len = [&](const Clip& c) { return c.retimedLength(fileLen); };
    auto fades = [&](const Project& p) {
        int n = 0;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : p.timeline().tracks(k))
                for (const auto& s : TimelineOps::transitions(t, len))
                    if (!s.leftId || !s.rightId) ++n;
        return n;
    };
    auto dissolves = [&](const Project& p) { return int(TimelineOps::transitions(p.timeline().video[0], len).size()); };
    auto setup = [&](Project& p, Selection& sel, Editor& ed) {
        p.addMedia({"/x/a.mp4", "a.mp4", fileLen, true, true, false});
        ed.addMediaAt({"/x/a.mp4"}, 0, 0);
        ed.bladeAt(V(p, 0, 0), 100);
        sel.clear();
        ed.addTransitions(100);
    };
    for (int scenario = 0; scenario < 3; ++scenario) {
        fileLen = scenario == 2 ? 130 : 250; // reversed: A (0..99) then has no material left behind it
        Project p;
        TimelineGuard guard_p{p};
        Selection sel;
        Editor ed(&p, &sel);
        setup(p, sel, ed);
        if (!CHECK_EQ(dissolves(p), 1)) return;
        CHECK_EQ(fades(p), 0);
        const int a = V(p, 0, 0), b = V(p, 0, 1);
        if (scenario == 0) ed.trimClip(b, TimelineOps::Edge::Start, 5);  // auseinandertrimmen
        if (scenario == 1) ed.trimClip(a, TimelineOps::Edge::End, -5);
        if (scenario == 2) ed.setClipSpeed({a}, {1.0, true, false, true}, false); // reversed: A without handles
        CHECK_EQ(fades(p), 0);
        CHECK_EQ(dissolves(p), 0);
        p.undoStack()->undo(); // undo brings the dissolve back
        CHECK_EQ(dissolves(p), 1);
    }

    // Copying only one side of a dissolve: the copy does not fade out over black
    {
        fileLen = 250;
        Project p;
        TimelineGuard guard_p{p};
        Selection sel;
        Editor ed(&p, &sel);
        setup(p, sel, ed);
        sel.set({V(p, 0, 0)});
        ed.copySelection();
        ed.paste(1000);
        CHECK_EQ(fades(p), 0);
        CHECK_EQ(dissolves(p), 1); // original unchanged

        // Cutting a subtitle: stays in place (clipboard only knows clips), clipboard unchanged
        const int cue = ed.addSubtitle(2000);
        sel.set({cue});
        const int steps = p.undoStack()->index();
        ed.cutSelection();
        CHECK_EQ(p.undoStack()->index(), steps);
        CHECK(ed.isSubtitle(cue));
    }
}

// Fade out and fade in that bump together by moving/closing a gap stay two transitions (like DaVinci);
// only a transition deliberately put on the cut turns them into a dissolve
void testFadesStayApart()
{
    auto len = [](const Clip& c) { return c.retimedLength(250); };
    auto spans = [&](const Project& p) { return TimelineOps::transitions(p.timeline().video[0], len); };
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, false, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.bladeAt(V(p, 0, 0), 100);
    const int a = V(p, 0, 0), b = V(p, 0, 1);
    ed.moveClips({b}, 50, TrackKind::Video, 0); // gap 100..150
    sel.set({a});
    ed.addTransitions(0); // A: fade in and out
    sel.set({b});
    ed.addTransitions(0); // B: fade in and out
    if (!CHECK_EQ(int(spans(p).size()), 4)) return;
    ed.moveClips({b}, -50, TrackKind::Video, 0); // close the gap
    auto s = spans(p);
    CHECK_EQ(int(s.size()), 4);
    for (const auto& x : s) CHECK(!x.isDissolve());
    // Saving/loading keeps that
    ProjectData d;
    CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &d, nullptr));
    CHECK(!TimelineOps::isDissolve(d.timeline.video[0].clips[0], d.timeline.video[0].clips[1]));
    // apart again: marking is dropped, transitions stay
    ed.moveClips({b}, 20, TrackKind::Video, 0);
    CHECK(!TimelineOps::findClip(p.timeline(), a)->transOutAlone && !TimelineOps::findClip(p.timeline(), b)->transInAlone);
    CHECK_EQ(int(spans(p).size()), 4);
    p.undoStack()->undo();
    // Transition deliberately on the cut: dissolve
    sel.clear();
    ed.addTransitions(100);
    s = spans(p);
    CHECK_EQ(int(s.size()), 3);
    CHECK(s.size() == 3 && s[1].isDissolve() && s[1].leftId == a && s[1].rightId == b);
    // Undo: two separate transitions again
    p.undoStack()->undo();
    CHECK_EQ(int(spans(p).size()), 4);
    // A dissolve stays a dissolve when moved together
    p.undoStack()->redo();
    ed.moveClips({a, b}, 10, TrackKind::Video, 0);
    CHECK_EQ(int(spans(p).size()), 3);

    // Compound clip from B: A keeps no "standalone" without a partner (otherwise every ineffective
    // edit afterwards would create an empty undo step)
    p.undoStack()->undo();
    p.undoStack()->undo(); // two separate transitions again
    sel.set({b});
    const int seq = ed.createCompoundClip();
    CHECK(!TimelineOps::findClip(p.timeline(), a)->transOutAlone);
    if (const Sequence* s = p.sequence(seq); CHECK(s && !s->timeline.video[0].clips.isEmpty()))
        CHECK(!s->timeline.video[0].clips[0].transInAlone);
    const int steps = p.undoStack()->count();
    p.edit("nichts", [](Timeline&) {});
    CHECK_EQ(p.undoStack()->count(), steps);
}

// Fade longer than the (afterwards shortened) clip: the other fade never becomes negative
void testFadeAfterTrim()
{
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, false, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int a = V(p, 0, 0);
    ed.setClipFade(a, TimelineOps::Edge::End, 80);
    ed.trimClip(a, TimelineOps::Edge::End, -210); // 40 frames long, fade out 80
    ed.setClipFade(a, TimelineOps::Edge::Start, 10);
    CHECK(TimelineOps::findClip(p.timeline(), a)->fadeIn >= 0);
}

// A cut where both clips are used to their ends gets no transition (DaVinci "insufficient handles");
// trimClips ripple-trims both sides (with the linked audio) so it fits, in one undo step
void testTransitionTrimClips()
{
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, true, false});
    p.addMedia({"/x/b.mp4", "b.mp4", 150, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/b.mp4"}, 250, 0);
    sel.clear();
    const int a = V(p, 0, 0), b = V(p, 0, 1);
    const int fps = p.fps();
    auto len = [&](const Clip& c) { return c.retimedLength(p.mediaInfo(c.mediaPath)->length); };
    auto dissolve = [&](TrackKind k) {
        for (const auto& s : TimelineOps::transitions(p.timeline().tracks(k)[0], len))
            if (s.leftId && s.rightId) return s.length();
        return 0;
    };
    CHECK_EQ(ed.transitionsLackingHandles(250).size(), 2); // video and audio cut
    const int steps = p.undoStack()->index();
    CHECK_EQ(ed.addTransitions(250).size(), 2);
    CHECK(!ed.addTransitionAt(a, b, {}));
    CHECK_EQ(p.undoStack()->index(), steps); // nothing fits -> no undo step
    CHECK(ed.previewTransitionAt(a, b, {}, true).has_value());
    CHECK_EQ(p.undoStack()->index(), steps);

    CHECK(ed.addTransitions(250, {}, {}, true).isEmpty());
    CHECK_EQ(p.undoStack()->index(), steps + 1);
    const Clip* ca = TimelineOps::findClip(p.timeline(), a);
    const Clip* cb = TimelineOps::findClip(p.timeline(), b);
    CHECK_EQ(ca->out, 249 - (fps - fps / 2));
    CHECK_EQ(cb->in, fps / 2);
    CHECK_EQ(cb->start, ca->end());
    CHECK_EQ(dissolve(TrackKind::Video), fps);
    CHECK_EQ(dissolve(TrackKind::Audio), fps);
    CHECK_EQ(A(p, 0, 1) ? TimelineOps::findClip(p.timeline(), A(p, 0, 1))->start : -1, cb->start); // audio in sync

    // Drop onto the cut (Effects Library), aligned to the start of the cut: only the left clip needs material
    p.undoStack()->undo();
    CHECK_EQ(TimelineOps::findClip(p.timeline(), a)->out, 249);
    TransitionStyle start;
    start.align = TransitionAlign::Start;
    CHECK(ed.addTransitionAt(a, b, start, true));
    CHECK_EQ(TimelineOps::findClip(p.timeline(), a)->out, 249 - fps);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), b)->in, 0);
    CHECK_EQ(dissolve(TrackKind::Video), fps);
}

// trim:true with a wanted length takes off only the frames missing for it, and never from a still (any length);
// it used to trim for the default 1 s on both sides and then shorten the dissolve
void testTransitionTrimWantedLength()
{
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, true, false});
    p.addMedia({"/x/s.png", "s.png", 125, true, false, true});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/s.png"}, 250, 0);
    sel.clear();
    const int a = V(p, 0, 0), st = V(p, 0, 1);
    const int stillLen = TimelineOps::findClip(p.timeline(), st)->length();
    const int end = TimelineOps::endFrame(p.timeline());
    CHECK_EQ(end, 250 + stillLen);
    auto dissolve = [&] {
        for (const auto& s : ed.transitions({TrackKind::Video, 0}))
            if (s.leftId == a && s.rightId == st) return s.length();
        return 0;
    };
    // Centred 12 frames: 6 after the cut are missing in a (it ends at the end of its recording), the still has any.
    // (A centred dissolve may also move to the end of the cut, where the still has the material: then nothing is
    // trimmed; either way the still keeps its start and a loses at most the 6 missing frames.)
    CHECK(ed.addTransitions(250, {}, {}, true, 12).isEmpty());
    const Clip* ca = TimelineOps::findClip(p.timeline(), a);
    const Clip* cs = TimelineOps::findClip(p.timeline(), st);
    CHECK(ca->out == 249 - 6 || ca->out == 249);
    CHECK_EQ(cs->in, 0);
    CHECK_EQ(cs->start, ca->end());
    CHECK_EQ(cs->length(), stillLen);
    CHECK_EQ(dissolve(), 12);
    CHECK_EQ(TimelineOps::endFrame(p.timeline()), end - (249 - ca->out));
    // Aligned to the start of the cut: all 12 frames come from a, the still keeps its start
    p.undoStack()->undo();
    CHECK(ed.addTransitions(250, {}, {}, true, 12, TransitionAlign::Start).isEmpty());
    CHECK_EQ(TimelineOps::findClip(p.timeline(), a)->out, 249 - 12);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), st)->in, 0);
    CHECK_EQ(dissolve(), 12);
    // Aligned to the end: the still has the material before the cut, nothing is trimmed
    p.undoStack()->undo();
    CHECK(ed.addTransitions(250, {}, {}, true, 12, TransitionAlign::End).isEmpty());
    CHECK_EQ(TimelineOps::findClip(p.timeline(), a)->out, 249);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), st)->in, 0);
    CHECK_EQ(dissolve(), 12);
    CHECK_EQ(TimelineOps::endFrame(p.timeline()), end);
    // Default length (1 s) centred: only a is trimmed, by half of it
    p.undoStack()->undo();
    const int fps = p.fps();
    CHECK(ed.addTransitions(250, {}, {}, true).isEmpty());
    const int outA = TimelineOps::findClip(p.timeline(), a)->out;
    CHECK(outA == 249 - (fps - fps / 2) || outA == 249);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), st)->in, 0);
    CHECK_EQ(dissolve(), fps);

    // Two clips without spare material, centred 12 frames: 6 frames off each, the timeline 12 frames shorter
    Project q;
    TimelineGuard guard_q{q};
    Selection sel2;
    Editor ed2(&q, &sel2);
    q.addMedia({"/x/a.mp4", "a.mp4", 250, true, false, false});
    q.addMedia({"/x/b.mp4", "b.mp4", 150, true, false, false});
    ed2.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed2.addMediaAt({"/x/b.mp4"}, 250, 0);
    sel2.clear();
    const int qa = V(q, 0, 0), qb = V(q, 0, 1);
    CHECK(ed2.addTransitions(250, {}, {}, true, 12).isEmpty());
    CHECK_EQ(TimelineOps::findClip(q.timeline(), qa)->out, 249 - 6);
    CHECK_EQ(TimelineOps::findClip(q.timeline(), qb)->in, 6);
    CHECK_EQ(TimelineOps::endFrame(q.timeline()), 400 - 12);
    int qlen = 0;
    for (const auto& s : ed2.transitions({TrackKind::Video, 0}))
        if (s.leftId == qa && s.rightId == qb) qlen = s.length();
    CHECK_EQ(qlen, 12);
}

// Undo memory: speed (with ripple), speed points and delete range write only the tracks they change - the others share
// their clips (and the subtitle tracks their cues) with the step before; a copy of every track per step cost 0.3 MB
// instead of 40 KB at 500 clips (tests/bench_undo). Undo and redo still give exactly the states.
void testUndoSharesUntouchedTracks()
{
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 400, true, true, false});
    p.addMedia({"/x/m.wav", "m.wav", 900, false, true, false});
    ed.addSubtitleTrack();
    int v = 0;
    p.edit("setup", [&](Timeline& tl) {
        tl.subtitles[0].cues = {SubtitleCue{p.newClipId(), 0, 40, "before everything"}};
        while (tl.audio.size() < 3) tl.audio << Track{TrackKind::Audio};
        Clip c;
        c.id = v = p.newClipId();
        c.mediaPath = "/x/a.mp4";
        c.start = 100;
        c.out = 99;
        c.linkId = p.newLinkId();
        tl.video[0].clips << c;
        c.id = p.newClipId();
        tl.audio[0].clips << c;
        Clip m;
        m.id = p.newClipId();
        m.mediaPath = "/x/m.wav";
        m.out = 349; // [0, 350): across the edit points, its track cannot move (stays as it is)
        tl.audio[1].clips << m;
        m.id = p.newClipId();
        m.out = 49; // [0, 50): before everything
        tl.audio[2].clips << m;
    });
    // A2 is untouched by the speed changes only (a delete range cuts it)
    const auto shared = [](const Timeline& a, const Timeline& b, const char* what, bool a2) {
        const bool ok = (!a2 || a.audio[1].clips.constData() == b.audio[1].clips.constData())
                        && a.audio[2].clips.constData() == b.audio[2].clips.constData()
                        && a.subtitles[0].cues.constData() == b.subtitles[0].cues.constData();
        if (!ok) std::printf("       %s: untouched tracks copied\n", what);
        return ok;
    };
    const auto exact = [&](const Timeline& before, const char* what) {
        const Timeline after = p.timeline();
        CHECK(!(after == before));
        p.undoStack()->undo();
        if (!CHECK(p.timeline() == before)) std::printf("       %s: undo\n", what);
        p.undoStack()->redo();
        if (!CHECK(p.timeline() == after)) std::printf("       %s: redo\n", what);
    };
    const auto check = [&](const char* what, bool a2, const std::function<void()>& op) {
        const Timeline before = p.timeline();
        op();
        CHECK(shared(before, p.timeline(), what, a2));
        exact(before, what);
    };
    check("speed with ripple", true, [&] { ed.setClipSpeed({v}, {2.0, false, false, true}, true); });
    check("speed point", true, [&] { ed.addSpeedPoint(v, 120); });
    check("speed of a section", true, [&] { ed.setSegmentSpeed(v, 1, 0.5); });
    check("delete range", false, [&] { ed.deleteRange(110, 115, false); });
    check("delete range with ripple", false, [&] { ed.deleteRange(125, 130, true); });
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("undo");
    testFadesStayApart();
    testFadeAfterTrim();
    testTransitionTrimClips();
    testTransitionTrimWantedLength();
    testUndoSharesUntouchedTracks();

    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    using namespace TimelineOps;
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, true, false});
    p.addMedia({"/x/b.mp4", "b.mp4", 150, true, true, false});
    p.addMedia({"/x/m.wav", "m.wav", 300, false, true, false});
    ed.setSourceMarkIn("/x/b.mp4", 20); // source marks are not part of undo -> set beforehand
    ed.setSourceMarkOut("/x/b.mp4", 59);

    QVector<QByteArray> states{state(p)};
    QStringList names;
    // Runs an operation; it must create exactly one undo step
    auto step = [&](const char* name, const std::function<void()>& fn) {
        const int before = p.undoStack()->index();
        fn();
        const int after = p.undoStack()->index();
        Check::report(after == before + 1, name, __FILE__, __LINE__,
                      QString("Undo-Schritte: %1 statt 1").arg(after - before));
        if (after != before + 1) return;
        states << state(p);
        names << name;
    };

    step("Medien ablegen a", [&] { ed.addMediaAt({"/x/a.mp4"}, 0, 0); });
    step("Medien ablegen b", [&] { ed.addMediaAt({"/x/b.mp4"}, 250, 0); });
    step("Medien ablegen m auf A2", [&] { ed.addMediaAt({"/x/m.wav"}, 30, 1); });
    step("Titel", [&] { ed.addTitle(50); });
    step("Klinge", [&] { ed.bladeAt(V(p, 0, 0), 100); });
    step("Teilen am Playhead", [&] { sel.clear(); ed.splitAtPlayhead(180); });
    // Cut at 100 (a|a, enough material on both sides); remember IDs, indices change later
    const int transL = V(p, 0, 0), transR = V(p, 0, 1);
    step("Übergang Strg+T", [&] { sel.clear(); ed.addTransitions(95); });
    step("Übergangsart", [&] {
        TransitionStyle s;
        s.type = TransitionType::WipeDown;
        s.softness = 20;
        ed.setTransitionStyle(transL, transR, s);
    });
    step("Übergangslänge", [&] { ed.setTransitionLength(transL, transR, 10); });
    step("Übergang entfernen", [&] { ed.removeTransition(transL, transR); });
    step("Übergang wieder Strg+T", [&] { sel.clear(); ed.addTransitions(95); });
    step("Fade", [&] { ed.setClipFade(V(p, 0, 0), Edge::Start, 12); });
    step("Lautstärke", [&] { ed.setClipVolume(A(p, 0, 0), -6.5); });
    step("Transform", [&] {
        ed.modifyClips({V(p, 0, 1)}, "Zoom", [](Clip& c) {
            c.transform.zoomX = c.transform.zoomY = 1.5;
            c.transform.rotation = 10;
        });
    });
    const int fxClip = V(p, 0, 1);
    step("Effekt", [&] { ed.addEffect({fxClip}, "color"); });
    step("Effekt-Wert", [&] {
        ed.modifyClips({V(p, 0, 1)}, "Temp", [](Clip& c) { EffectRegistry::instance(c, "color")->params["tint"] = 12.0; });
    });
    step("Keyframe", [&] { ed.setKeyframes({V(p, 0, 1)}, {AnimParam::Opacity, AnimParam::ZoomX}, 110, true); });
    step("Keyframe-Wert", [&] {
        ed.modifyClips({V(p, 0, 1)}, "Deckkraft", [](Clip& c) { Keys::setKey(c, AnimParam::Opacity, 60, 20); });
    });
    step("Keyframe-Verlauf", [&] { ed.setKeyframeEase({V(p, 0, 1)}, {}, 110, KeyEase::EaseIn); });
    step("Keyframes verschieben", [&] { ed.moveKeyframes(V(p, 0, 1), {10}, 5); });
    step("Marker", [&] { ed.toggleMarker(30); });
    step("In", [&] { ed.setMarkIn(10); });
    step("Out", [&] { ed.setMarkOut(400); });
    step("Spur umbenennen", [&] { ed.renameTrack({TrackKind::Audio, 1}, "Musik"); });
    step("Spurfarbe", [&] { ed.setTrackColor({TrackKind::Video, 1}, "orange"); });
    step("Stumm", [&] { ed.toggleTrackMute({TrackKind::Audio, 0}); });
    step("Ausblenden", [&] { ed.toggleTrackHidden({TrackKind::Video, 1}); });
    step("Speed", [&] { ed.setClipSpeed({V(p, 0, 1)}, {0.5, false, false, true}, true); });
    step("Trimmen", [&] { ed.trimClip(V(p, 0, 0), Edge::End, -20); });
    step("Verschieben", [&] { ed.moveClips({V(p, 1, 0)}, 40, TrackKind::Video, 0); });
    step("Nudge", [&] { sel.set({V(p, 1, 0)}); ed.nudgeSelection(-5); });
    step("Deaktivieren", [&] { ed.toggleSelectionEnabled(); });
    step("Kopieren+Einfügen", [&] { ed.copySelection(); ed.paste(900); });
    step("Trim-Modus Roll", [&] {
        const auto e = ed.trimEdit(TrimKind::Roll, V(p, 0, 1), Edge::End);
        ed.applyTrimEdit(e, 8);
    });
    step("Trim-Modus Slip", [&] {
        const auto e = ed.trimEdit(TrimKind::Slip, V(p, 0, 0));
        ed.applyTrimEdit(e, 5);
    });
    step("Marken löschen", [&] { ed.clearMarks(); });
    step("Quelle einfügen F9", [&] { ed.sourceEdit(Editor::SourceEditMode::Insert, "/x/b.mp4", 0, 20); });
    step("Quelle überschreiben F10", [&] { ed.sourceEdit(Editor::SourceEditMode::Overwrite, "/x/a.mp4", 0, 600); });
    // Only the picture of a linked clip (its sound on A1 blocks A1's ripple): the clips after it come apart from their
    // sound on purpose here; the guard checks the structure from now on
    guard_p.stopSyncCheck();
    step("Ripple-Löschen", [&] { sel.set({V(p, 0, 1)}); ed.rippleDeleteSelection(); });
    step("Löschen", [&] { sel.set({A(p, 1, 0)}); ed.deleteSelection(); });
    step("Verknüpfen lösen", [&] { sel.set({V(p, 0, 0)}); ed.toggleLinkSelection(); });
    step("Spur sperren", [&] { ed.toggleTrackLock({TrackKind::Video, 1}); });
    step("Auflösung", [&] {
        ProjectFormat f = p.format();
        f.width = 1280;
        f.height = 720;
        p.setFormat(f);
    });
    step("Effekt entfernen", [&] { ed.removeEffect({fxClip}, "color"); });

    // All steps changed something
    for (int i = 1; i < states.size(); ++i)
        if (states[i] == states[i - 1]) Check::report(false, "Schritt ändert nichts", __FILE__, __LINE__, names[i - 1]);

    const int n = int(states.size()) - 1;
    CHECK_EQ(p.undoStack()->index(), n);
    // everything back, every intermediate state equal
    for (int i = n; i > 0; --i) {
        p.undoStack()->undo();
        if (state(p) != states[i - 1])
            Check::report(false, "Undo", __FILE__, __LINE__, "Zustand vor „" + names[i - 1] + "“ stimmt nicht");
    }
    CHECK(state(p) == states.first());
    CHECK(!p.undoStack()->canUndo());
    // everything forward
    for (int i = 1; i <= n; ++i) {
        p.undoStack()->redo();
        if (state(p) != states[i])
            Check::report(false, "Redo", __FILE__, __LINE__, "Zustand nach „" + names[i - 1] + "“ stimmt nicht");
    }
    CHECK(state(p) == states.last());
    CHECK(!p.undoStack()->canRedo());

    // Half back, then a new operation: redo branch discarded, undo leads to the intermediate state again
    for (int i = 0; i < n / 2; ++i) p.undoStack()->undo();
    const QByteArray mid = state(p);
    ed.toggleMarker(777);
    CHECK(!p.undoStack()->canRedo());
    p.undoStack()->undo();
    CHECK(state(p) == mid);

    // Merged changes (mergeKey, e.g. dragging a slider) = one undo step
    const int before = p.undoStack()->index();
    const QByteArray unchanged = state(p);
    const int id = V(p, 0, 0);
    for (int v = 1; v <= 5; ++v)
        ed.modifyClips({id}, "Deckkraft", [v](Clip& c) { c.transform.opacity = 100 - v * 10; }, "opacity");
    p.closeMerge();
    CHECK_EQ(p.undoStack()->index(), before + 1);
    p.undoStack()->undo();
    CHECK(state(p) == unchanged);

    // Edit without change (split where no clip lies; same value) creates no undo step
    const int idle = p.undoStack()->index();
    ed.splitAtPlayhead(100000);
    ed.modifyClips({id}, "Deckkraft", [](Clip&) {});
    CHECK_EQ(p.undoStack()->index(), idle);
    CHECK(state(p) == unchanged);
    testBrokenDissolves();

    // Many markers (an AI edit with tens of thousands of marker ops): each toggle finds its place, the list stays
    // sorted, toggling again removes; markers that came unsorted (an imported timeline) are sorted first
    {
        Project q;
        TimelineGuard guard_q{q};
        q.undoStack()->setUndoLimit(1); // as in schneidi-cli (every step keeps a snapshot of all markers)
        Selection s;
        Editor e(&q, &s);
        QElapsedTimer t;
        t.start();
        for (int i = 40000; i > 0; --i) e.toggleMarker(i * 2);
        CHECK(t.elapsed() < 10000);
        const QVector<int>& m = q.timeline().markers;
        CHECK_EQ(m.size(), 40000);
        CHECK(std::is_sorted(m.cbegin(), m.cend()) && m.first() == 2 && m.last() == 80000);
        e.toggleMarker(4);
        e.toggleMarker(5);
        CHECK_EQ(q.timeline().markers.size(), 40000);
        CHECK(!q.timeline().markers.contains(4) && q.timeline().markers.value(1) == 5);
        q.edit("Unsorted", [](Timeline& tl) { tl.markers = {30, 10, 20}; });
        e.toggleMarker(10);
        e.toggleMarker(25);
        CHECK(q.timeline().markers == QVector<int>({20, 25, 30}));
    }
    return Check::result();
}
