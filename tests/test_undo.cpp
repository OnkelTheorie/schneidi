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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("undo");

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
    step("Übergang entfernen", [&] { ed.removeTransition(transL, transR); });
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
    return Check::result();
}
