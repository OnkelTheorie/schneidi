// Test undo/redo chain: many editor operations in a row (each must create an undo step), then everything
// back = initial state, everything forward = final state; each intermediate step is compared via JSON (project file).
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QUndoStack>

namespace {

// State as project file JSON; counters for new IDs deliberately keep going (IDs are never reused)
QByteArray state(const Project& p)
{
    ProjectData d = p.data();
    d.lastClipId = d.lastLinkId = 0;
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("undo");
    testFadesStayApart();
    testFadeAfterTrim();

    Project p;
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
    return Check::result();
}
