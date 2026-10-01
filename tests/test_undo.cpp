// Test Undo/Redo-Kette: viele Editor-Operationen nacheinander (jede muss einen Undo-Schritt anlegen), dann alles
// zurück = Ausgangszustand, alles vor = Endzustand; jeder Zwischenschritt wird per JSON (Projektdatei) verglichen.
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

// Zustand als Projektdatei-JSON; Zähler für neue IDs laufen absichtlich weiter (IDs werden nie wiederverwendet)
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

// Überblendung, die durch eine Bearbeitung ihren Partner verliert, wird gelöst statt stillschweigend zum
// Aus-/Einblenden über Schwarz zu werden (Trimmen, Tempo)
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
        fileLen = scenario == 2 ? 130 : 250; // rückwärts: A (0..99) hat danach kein Material mehr hinter sich
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        setup(p, sel, ed);
        if (!CHECK_EQ(dissolves(p), 1)) return;
        CHECK_EQ(fades(p), 0);
        const int a = V(p, 0, 0), b = V(p, 0, 1);
        if (scenario == 0) ed.trimClip(b, TimelineOps::Edge::Start, 5);  // auseinandertrimmen
        if (scenario == 1) ed.trimClip(a, TimelineOps::Edge::End, -5);
        if (scenario == 2) ed.setClipSpeed({a}, {1.0, true, false, true}, false); // rückwärts: A ohne Handles
        CHECK_EQ(fades(p), 0);
        CHECK_EQ(dissolves(p), 0);
        p.undoStack()->undo(); // Undo bringt die Überblendung zurück
        CHECK_EQ(dissolves(p), 1);
    }

    // Kopieren nur einer Seite einer Überblendung: Kopie blendet nicht über Schwarz aus
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
        CHECK_EQ(dissolves(p), 1); // Original unverändert

        // Ausschneiden eines Untertitels: bleibt liegen (Zwischenablage kennt nur Clips), Zwischenablage unverändert
        const int cue = ed.addSubtitle(2000);
        sel.set({cue});
        const int steps = p.undoStack()->index();
        ed.cutSelection();
        CHECK_EQ(p.undoStack()->index(), steps);
        CHECK(ed.isSubtitle(cue));
    }
}

// Aus- und Einblenden, die durch Verschieben/Lücke schließen aneinanderstoßen, bleiben zwei Übergänge (wie DaVinci);
// erst ein bewusst auf den Schnitt gesetzter Übergang macht daraus eine Überblendung
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
    ed.moveClips({b}, 50, TrackKind::Video, 0); // Lücke 100..150
    sel.set({a});
    ed.addTransitions(0); // A: Ein- und Ausblenden
    sel.set({b});
    ed.addTransitions(0); // B: Ein- und Ausblenden
    if (!CHECK_EQ(int(spans(p).size()), 4)) return;
    ed.moveClips({b}, -50, TrackKind::Video, 0); // Lücke schließen
    auto s = spans(p);
    CHECK_EQ(int(s.size()), 4);
    for (const auto& x : s) CHECK(!x.isDissolve());
    // Speichern/Laden behält das
    ProjectData d;
    CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &d, nullptr));
    CHECK(!TimelineOps::isDissolve(d.timeline.video[0].clips[0], d.timeline.video[0].clips[1]));
    // wieder auseinander: Markierung fällt weg, Übergänge bleiben
    ed.moveClips({b}, 20, TrackKind::Video, 0);
    CHECK(!TimelineOps::findClip(p.timeline(), a)->transOutAlone && !TimelineOps::findClip(p.timeline(), b)->transInAlone);
    CHECK_EQ(int(spans(p).size()), 4);
    p.undoStack()->undo();
    // Übergang bewusst auf den Schnitt: Überblendung
    sel.clear();
    ed.addTransitions(100);
    s = spans(p);
    CHECK_EQ(int(s.size()), 3);
    CHECK(s.size() == 3 && s[1].isDissolve() && s[1].leftId == a && s[1].rightId == b);
    // Undo: wieder zwei getrennte Übergänge
    p.undoStack()->undo();
    CHECK_EQ(int(spans(p).size()), 4);
    // Überblendung bleibt beim gemeinsamen Verschieben eine Überblendung
    p.undoStack()->redo();
    ed.moveClips({a, b}, 10, TrackKind::Video, 0);
    CHECK_EQ(int(spans(p).size()), 3);

    // Compound Clip aus B: A behält kein „eigenständig“ ohne Partner (sonst legte danach jede wirkungslose
    // Bearbeitung einen leeren Undo-Schritt an)
    p.undoStack()->undo();
    p.undoStack()->undo(); // wieder zwei getrennte Übergänge
    sel.set({b});
    const int seq = ed.createCompoundClip();
    CHECK(!TimelineOps::findClip(p.timeline(), a)->transOutAlone);
    if (const Sequence* s = p.sequence(seq); CHECK(s && !s->timeline.video[0].clips.isEmpty()))
        CHECK(!s->timeline.video[0].clips[0].transInAlone);
    const int steps = p.undoStack()->count();
    p.edit("nichts", [](Timeline&) {});
    CHECK_EQ(p.undoStack()->count(), steps);
}

// Fade länger als der (danach gekürzte) Clip: der andere Fade wird nie negativ
void testFadeAfterTrim()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 250, true, false, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int a = V(p, 0, 0);
    ed.setClipFade(a, TimelineOps::Edge::End, 80);
    ed.trimClip(a, TimelineOps::Edge::End, -210); // 40 Frames lang, Ausblenden 80
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
    ed.setSourceMarkIn("/x/b.mp4", 20); // Quell-Marken gehören nicht zum Undo -> vorher setzen
    ed.setSourceMarkOut("/x/b.mp4", 59);

    QVector<QByteArray> states{state(p)};
    QStringList names;
    // Führt eine Operation aus; sie muss genau einen Undo-Schritt anlegen
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
    // Schnitt bei 100 (a|a, genug Material auf beiden Seiten); IDs merken, Indizes ändern sich später
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

    // Alle Schritte haben etwas geändert
    for (int i = 1; i < states.size(); ++i)
        if (states[i] == states[i - 1]) Check::report(false, "Schritt ändert nichts", __FILE__, __LINE__, names[i - 1]);

    const int n = int(states.size()) - 1;
    CHECK_EQ(p.undoStack()->index(), n);
    // alles zurück, jeder Zwischenstand gleich
    for (int i = n; i > 0; --i) {
        p.undoStack()->undo();
        if (state(p) != states[i - 1])
            Check::report(false, "Undo", __FILE__, __LINE__, "Zustand vor „" + names[i - 1] + "“ stimmt nicht");
    }
    CHECK(state(p) == states.first());
    CHECK(!p.undoStack()->canUndo());
    // alles vor
    for (int i = 1; i <= n; ++i) {
        p.undoStack()->redo();
        if (state(p) != states[i])
            Check::report(false, "Redo", __FILE__, __LINE__, "Zustand nach „" + names[i - 1] + "“ stimmt nicht");
    }
    CHECK(state(p) == states.last());
    CHECK(!p.undoStack()->canRedo());

    // Hälfte zurück, dann neue Operation: Redo-Zweig verworfen, Undo führt wieder zum Zwischenstand
    for (int i = 0; i < n / 2; ++i) p.undoStack()->undo();
    const QByteArray mid = state(p);
    ed.toggleMarker(777);
    CHECK(!p.undoStack()->canRedo());
    p.undoStack()->undo();
    CHECK(state(p) == mid);

    // Zusammengefasste Änderungen (mergeKey, z. B. Regler ziehen) = ein Undo-Schritt
    const int before = p.undoStack()->index();
    const QByteArray unchanged = state(p);
    const int id = V(p, 0, 0);
    for (int v = 1; v <= 5; ++v)
        ed.modifyClips({id}, "Deckkraft", [v](Clip& c) { c.transform.opacity = 100 - v * 10; }, "opacity");
    p.closeMerge();
    CHECK_EQ(p.undoStack()->index(), before + 1);
    p.undoStack()->undo();
    CHECK(state(p) == unchanged);

    // Bearbeitung ohne Änderung (Teilen, wo kein Clip liegt; gleicher Wert) legt keinen Undo-Schritt an
    const int idle = p.undoStack()->index();
    ed.splitAtPlayhead(100000);
    ed.modifyClips({id}, "Deckkraft", [](Clip&) {});
    CHECK_EQ(p.undoStack()->index(), idle);
    CHECK(state(p) == unchanged);
    testBrokenDissolves();
    return Check::result();
}
