// Test Spuren: hinzufügen/löschen, sperren, umbenennen, Farbe, Ripple über alle nicht gesperrten Spuren, Spurkopf-Bedienung (offscreen)
#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "app/Theme.h"
#include "engine/Engine.h"
#include "ui/Mixer.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTest>
#include <QTimer>

static Clip mk(int id, int start, int len, int link = 0, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id; c.start = start; c.in = 0; c.out = len - 1; c.linkId = link; c.mediaPath = path;
    return c;
}

static const Clip* F(const Project& p, int id) { return TimelineOps::findClip(p.timeline(), id); }

// V1: 1[0,100) 2[100,200) 3[200,300); A1 Partner 11,12,13; A2: 21[0,50) 22[250,350)
static void setup(Project& p)
{
    p.reset();
    p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
    p.addMedia({"/x/m.mp3", "m.mp3", 1000, false, true, false});
    p.edit("setup", [](Timeline& tl) {
        tl.video[0].clips = {mk(1, 0, 100, 1), mk(2, 100, 100, 2), mk(3, 200, 100, 3)};
        tl.audio[0].clips = {mk(11, 0, 100, 1), mk(12, 100, 100, 2), mk(13, 200, 100, 3)};
        tl.audio[1].clips = {mk(21, 0, 50, 0, "/x/m.mp3"), mk(22, 250, 100, 0, "/x/m.mp3")};
    });
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("tracks");
    Project p;
    Selection sel;
    Editor ed(&p, &sel);

    // --- Ripple-Löschen über alle Spuren
    setup(p);
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 3)->start == 100 && F(p, 13)->start == 100);
    CHECK(F(p, 22)->start == 150);
    // gesperrte A2 bleibt stehen
    setup(p);
    ed.toggleTrackLock({TrackKind::Audio, 1});
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 3)->start == 100 && F(p, 22)->start == 250);
    // Spur, auf der Nachrücken überschreiben würde, bleibt stehen
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.audio[1].clips = {mk(21, 0, 150, 0, "/x/m.mp3"), mk(22, 160, 100, 0, "/x/m.mp3")}; });
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 22)->start == 160);

    // --- Trim-Modus Ripple
    using namespace TimelineOps;
    setup(p);
    auto e = ed.trimEdit(TrimKind::Ripple, 1, Edge::End);
    CHECK(e.ids.size() == 2);
    ed.applyTrimEdit(e, -30);
    CHECK(F(p, 1)->end() == 70 && F(p, 2)->start == 70 && F(p, 12)->start == 70 && F(p, 22)->start == 220);
    // Platz auf A2 begrenzt das Verkürzen
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.audio[1].clips = {mk(21, 0, 120, 0, "/x/m.mp3"), mk(22, 130, 100, 0, "/x/m.mp3")}; });
    e = ed.trimEdit(TrimKind::Ripple, 1, Edge::End);
    CHECK(ed.clampTrimEdit(e, -30) == -10);
    ed.toggleTrackLock({TrackKind::Audio, 1}); // gesperrt -> begrenzt nicht mehr, bleibt stehen
    CHECK(ed.clampTrimEdit(e, -30) == -30);
    ed.applyTrimEdit(e, -30);
    CHECK(F(p, 22)->start == 130 && F(p, 2)->start == 70);
    // Anfang (Clip bleibt, Inhalt wandert) verlängert: alles rückt nach rechts
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.video[0].clips[1].in = 50; tl.video[0].clips[1].out = 149; tl.audio[0].clips[1].in = 50; tl.audio[0].clips[1].out = 149; });
    e = ed.trimEdit(TrimKind::Ripple, 2, Edge::Start);
    ed.applyTrimEdit(e, -20);
    CHECK(F(p, 2)->length() == 120 && F(p, 3)->start == 220 && F(p, 22)->start == 270);

    // --- Speed mit Ripple
    setup(p);
    ed.setClipSpeed({1}, {0.5, false, false, true}, true);
    CHECK(F(p, 1)->length() == 200 && F(p, 2)->start == 200 && F(p, 22)->start == 350);

    // --- Sperre: nichts änderbar, nicht auswählbar
    setup(p);
    ed.toggleTrackLock({TrackKind::Video, 0});
    CHECK(ed.withLinked({11}) == QVector<int>{11});
    ed.selectAll();
    CHECK(!sel.contains(1) && sel.contains(11));
    sel.set({1, 2});
    ed.deleteSelection();
    CHECK(F(p, 1) && F(p, 2));
    ed.modifyClips({1}, "x", [](Clip& c) { c.enabled = false; });
    CHECK(F(p, 1)->enabled);
    ed.bladeAt(1, 50);
    CHECK(F(p, 1)->length() == 100);
    ed.setClipFade(1, Edge::Start, 10);
    CHECK(F(p, 1)->fadeIn == 0);
    ed.addTransitionAt(1, 2, {});
    CHECK(F(p, 1)->transOut == 0);
    ed.trimClip(1, Edge::End, -10);
    CHECK(F(p, 1)->length() == 100);
    // Partner auf A1 lässt sich allein trimmen
    ed.trimClip(11, Edge::End, -10);
    CHECK(F(p, 11)->length() == 90 && F(p, 1)->length() == 100);
    // Titel: gesperrte V1 wird übersprungen
    ed.addTitle(500);
    CHECK(p.timeline().video[0].clips.size() == 3 && p.timeline().video[1].clips.size() == 1);
    // Verschieben von V2 auf gesperrte V1 geht nicht
    const int title = p.timeline().video[1].clips[0].id;
    ed.moveClips({title}, 0, TrackKind::Video, -1);
    CHECK(p.timeline().video[1].clips.size() == 1);
    // Einfügen auf gesperrte Spur nicht
    sel.set({2});
    ed.copySelection(); // V1 gesperrt -> Kopieren geht (liest nur)
    ed.paste(600);
    CHECK(p.timeline().video[0].clips.size() == 3);
    // Medien auf gesperrte V1 ablegen: nur Ton landet auf A1
    ed.addMediaAt({"/x/a.mp4"}, 700, 0);
    CHECK(p.timeline().video[0].clips.size() == 3 && p.timeline().audio[0].clips.size() == 4);
    // Spur sperren wählt Clips der Spur ab
    ed.toggleTrackLock({TrackKind::Video, 0});
    sel.set({1, 11});
    ed.toggleTrackLock({TrackKind::Video, 0});
    CHECK(sel.ids() == QSet<int>{11});
    // Undo nimmt Sperre zurück
    p.undoStack()->undo();
    CHECK(!p.timeline().video[0].locked);

    // --- Umbenennen / Farbe
    ed.renameTrack({TrackKind::Audio, 1}, "  Musik  ");
    CHECK(p.timeline().audio[1].name == "Musik");
    CHECK(trackDisplayName(p.timeline().audio[0], {TrackKind::Audio, 0}) == "Audio 1");
    ed.renameTrack({TrackKind::Audio, 1}, "Audio 2"); // Standardname = automatisch
    CHECK(p.timeline().audio[1].name.isEmpty());
    ed.renameTrack({TrackKind::Audio, 1}, "Musik");
    ed.setTrackColor({TrackKind::Audio, 1}, "violet");
    ed.setTrackColor({TrackKind::Video, 0}, "nix");
    CHECK(p.timeline().audio[1].color == "violet" && p.timeline().video[0].color.isEmpty());
    ed.toggleTrackLock({TrackKind::Video, 1});

    // --- Speichern/Laden
    ProjectData d = p.data();
    const QByteArray json = ProjectFile::toJson(d, "/x/projekt.schneidi");
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/projekt.schneidi", &back, &err));
    CHECK(back.timeline.audio[1].name == "Musik" && back.timeline.audio[1].color == "violet");
    CHECK(back.timeline.video[1].locked && !back.timeline.video[0].locked && back.timeline.video[0].name.isEmpty());
    CHECK(json.contains("\"name\": \"A2\"")); // ältere Versionen sehen das Kürzel
    // alte Datei: nur "name": "V1"
    QByteArray old = json;
    old.replace("\"label\": \"Musik\",", "");
    CHECK(ProjectFile::fromJson(old, "/x/projekt.schneidi", &back, &err) && back.timeline.audio[1].name.isEmpty());

    // --- Spuren hinzufügen/löschen (Rechtsklick auf den Spurkopf)
    {
        Engine engine;
        Mixer mixer(&p, &engine);
        // Mixer-Kanalzüge: Kürzel und Spurname (Tooltip) in Spurreihenfolge
        auto stripNames = [&] {
            QStringList out;
            for (QLabel* l : mixer.findChildren<QLabel*>())
                if (QRegularExpression("^A\\d+$").match(l->text()).hasMatch()) out << l->text() + "=" + l->toolTip();
            out.sort(); // A1, A2, A3 (findChildren-Reihenfolge ist nicht festgelegt)
            return out;
        };
        setup(p);
        ed.renameTrack({TrackKind::Audio, 1}, "Musik");
        p.edit("x", [](Timeline& tl) { tl.audio[1].volumeDb = -6; tl.audio[1].solo = true; tl.audio[0].muted = true; });
        ed.setTargetTracks(1, 1);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        // Audio: oberhalb von A1 = Index 0 -> alte Spuren rücken eins weiter
        ed.addTrack(TrackKind::Audio, 0);
        const Timeline& t1 = p.timeline();
        CHECK(t1.audio.size() == 3 && t1.audio[0].clips.isEmpty() && t1.audio[0].name.isEmpty() && !t1.audio[0].muted);
        CHECK(t1.audio[1].muted && t1.audio[1].clips.size() == 3 && t1.audio[1].clips[0].id == 11);
        CHECK(t1.audio[2].name == "Musik" && t1.audio[2].volumeDb == -6 && t1.audio[2].solo);
        CHECK(ed.targetAudioTrack() == 2 && ed.targetVideoTrack() == 1);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Audio 2", "A3=Musik"}));
        // ein Undo-Schritt
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() == 2 && p.timeline().audio[1].name == "Musik"
              && p.timeline().audio[0].clips.size() == 3);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        p.undoStack()->redo();
        CHECK(p.timeline().audio.size() == 3 && p.timeline().audio[2].name == "Musik");
        // Video oberhalb von V2 = ganz oben (Index 2); Zielspur V2 bleibt
        ed.addTrack(TrackKind::Video, 2);
        CHECK(p.timeline().video.size() == 3 && p.timeline().video[0].clips.size() == 3 && ed.targetVideoTrack() == 1);
        // Projektdatei-Rundlauf
        ProjectData back2;
        QString err2;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &back2, &err2));
        CHECK(back2.timeline.video.size() == 3 && back2.timeline.audio.size() == 3);
        CHECK(back2.timeline.audio[2].name == "Musik" && back2.timeline.audio[2].solo && back2.timeline.audio[1].muted);

        // Löschen: Clips der Spur gehen mit, Auswahl darauf wird aufgehoben
        sel.set({1, 11, 21});
        ed.removeTrack({TrackKind::Audio, 1}); // die Spur mit 11,12,13
        CHECK(p.timeline().audio.size() == 2 && !F(p, 11) && F(p, 1) && p.timeline().audio[1].name == "Musik");
        CHECK(sel.ids() == QSet<int>({1, 21}));
        CHECK(ed.targetAudioTrack() == 1); // folgt „Musik“
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() == 3 && F(p, 11) && p.timeline().audio[1].muted);
        p.undoStack()->redo();
        CHECK(!F(p, 11));
        // gesperrte Spur und letzte Spur eines Typs nicht löschbar
        ed.toggleTrackLock({TrackKind::Audio, 1});
        CHECK(!ed.canRemoveTrack({TrackKind::Audio, 1}));
        ed.removeTrack({TrackKind::Audio, 1});
        CHECK(p.timeline().audio.size() == 2);
        ed.toggleTrackLock({TrackKind::Audio, 1});
        ed.removeTrack({TrackKind::Audio, 1});
        CHECK(p.timeline().audio.size() == 1 && !ed.canRemoveTrack({TrackKind::Audio, 0}));
        ed.removeTrack({TrackKind::Audio, 0});
        CHECK(p.timeline().audio.size() == 1 && ed.targetAudioTrack() == 0);
        CHECK(!ed.canRemoveTrack({TrackKind::Video, 5}));
    }

    // --- UI: Spurkopf, Klicks
    setup(p);
    ed.setTrackColor({TrackKind::Audio, 1}, "violet");
    ed.renameTrack({TrackKind::Video, 1}, "Titel und Grafik");
    sel.clear();
    TimelineView view(&ed);
    view.resize(1400, 320);
    view.show();
    QApplication::processEvents();
    auto click = [&](QPoint pt) {
        QMouseEvent pr(QEvent::MouseButtonPress, pt, view.mapToGlobal(pt), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(&view, &pr);
        QMouseEvent rl(QEvent::MouseButtonRelease, pt, view.mapToGlobal(pt), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(&view, &rl);
    };
    const int kH = TimelineView::kHeaderW;
    // V1 Zeile y 94..158, Schloss bei x kH-54..kH-34, y+6
    click({kH - 44, 94 + 12});
    CHECK(p.timeline().video[0].locked);
    click({kH + 50 * 4, 120}); // Clip 1 auf V1
    CHECK(sel.isEmpty());
    click({kH + 50 * 4, 190}); // Clip 11 auf A1
    CHECK(sel.ids() == QSet<int>{11});
    // Doppelklick auf den Namen von A1 (y 164)
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPoint(60, 164 + 12), view.mapToGlobal(QPoint(60, 176)), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(&view, &dbl);
    QLineEdit* le = view.findChild<QLineEdit*>();
    if (!CHECK(le && le->isVisible() && le->text() == "Audio 1")) return Check::result();
    le->setText("Dialog");
    QTest::keyClick(le, Qt::Key_Return);
    CHECK(p.timeline().audio[0].name == "Dialog" && !le->isVisible());
    // Esc bricht ab
    QApplication::sendEvent(&view, &dbl);
    le->setText("Quatsch");
    QTest::keyClick(le, Qt::Key_Escape);
    CHECK(p.timeline().audio[0].name == "Dialog" && !le->isVisible());

    // Rechtsklick auf den Spurkopf: Menüpunkt auslösen, sobald das Menü offen ist
    auto headerMenu = [&](QPoint pt, const QString& text) {
        bool found = false, enabled = false;
        QTimer::singleShot(0, [&] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : m->actions())
                    if (a->text() == text) {
                        found = true;
                        enabled = a->isEnabled();
                        if (enabled) a->trigger();
                    }
                m->close();
            }
        });
        QContextMenuEvent ce(QContextMenuEvent::Mouse, pt, view.mapToGlobal(pt));
        QApplication::sendEvent(&view, &ce);
        return found && enabled;
    };
    // A1 (y 164): unterhalb = neue A2, „Musik“ rückt auf A3 (Audio zählt von oben)
    ed.renameTrack({TrackKind::Audio, 1}, "Musik");
    CHECK(headerMenu({60, 170}, "Spur unterhalb hinzufügen"));
    CHECK(p.timeline().audio.size() == 3 && p.timeline().audio[0].name == "Dialog" && p.timeline().audio[1].clips.isEmpty()
          && p.timeline().audio[2].name == "Musik");
    // V1 (y 94): oberhalb = neue V2, „Titel und Grafik“ rückt auf V3 (Video zählt von unten)
    CHECK(headerMenu({60, 100}, "Spur oberhalb hinzufügen"));
    CHECK(p.timeline().video.size() == 3 && p.timeline().video[1].clips.isEmpty()
          && p.timeline().video[2].name == "Titel und Grafik");
    // Spur löschen: V1 ist gesperrt -> Menüpunkt aus
    CHECK(!headerMenu({60, 94 + 64 + 10}, "Spur löschen"));
    CHECK(p.timeline().video.size() == 3);

    return Check::result();
}
