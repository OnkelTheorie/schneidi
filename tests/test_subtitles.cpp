// Test Untertitel: SRT lesen/schreiben (Kodierung, Formatierungen, kaputte Blöcke, Überlappungen, 29,97 fps),
// Überschreiben auf der Spur, Editor-Operationen mit Undo, Projektdatei, Formatwechsel, Einbrennen im Render
// (Pixel) und SRT-Datei beim Export.
#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/Subtitles.h"
#include "core/Timecode.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/RenderQueue.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cstring>

namespace {

QString cues(const QVector<SubtitleCue>& list)
{
    QStringList out;
    for (const SubtitleCue& c : list) out << QString("%1-%2:%3").arg(c.start).arg(c.end).arg(QString(c.text).replace('\n', '|'));
    return out.join(' ');
}

QString track(const Project& p, int i)
{
    return i < p.timeline().subtitles.size() ? cues(p.timeline().subtitles[i].cues) : QString("-");
}

void testSrt()
{
    // Nummern, CRLF, BOM, Formatierungen, Punkt statt Komma, Positionsangaben, Mehrzeiler
    const QByteArray srt = "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Hallo</i> Welt\r\n\r\n"
                           "2\r\n00:00:03.000 --> 00:00:04,000 X1:10 Y1:20\r\n{\\an8}Zeile eins\r\nZeile zwei\r\n\r\n";
    int skipped = -1;
    QVector<SubtitleCue> c = Subtitles::parseSrt(srt, 25, &skipped);
    CHECK_EQ(cues(c), QString("25-63:Hallo Welt 75-100:Zeile eins|Zeile zwei"));
    CHECK_EQ(skipped, 0);

    // Fehlende Nummer, kaputte Zeitzeile, leerer Text, unsortiert, Überlappung, doppelte Startzeit, LF, kein Schluss-Leerzeichen
    const QByteArray messy = "00:00:05,000 --> 00:00:06,000\nohne Nummer\n\n"
                             "7\n00:00:xx --> 00:00:09,000\nkaputt\n\n"
                             "8\n00:00:10,000 --> 00:00:11,000\n\n"
                             "3\n00:00:00,000 --> 00:00:05,500\nüberlappt\n\n"
                             "4\n00:00:00,000 --> 00:00:01,000\ngleicher Start";
    c = Subtitles::parseSrt(messy, 25, &skipped);
    CHECK_EQ(cues(c), QString("0-125:überlappt|gleicher Start 125-150:ohne Nummer"));
    CHECK_EQ(skipped, 2);

    // Windows-1252 (kein gültiges UTF-8): Umlaute und typografische Anführungszeichen
    c = Subtitles::parseSrt("1\r\n00:00:00,000 --> 00:00:01,000\r\nGr\xFC\xDF\x65 \x84zitiert\x93\r\n", 25);
    CHECK_EQ(c.value(0).text, QString::fromUtf8("Grüße „zitiert“"));

    // 29,97 fps: Frames aus Millisekunden gerundet, zurück wieder dieselben Zeiten
    const double ntsc = 30000.0 / 1001.0;
    c = Subtitles::parseSrt("1\n00:01:00,060 --> 00:01:02,000\nA\n", ntsc);
    CHECK_EQ(c.value(0).start, 1800); // 60,06 s * 29,97
    CHECK_EQ(c.value(0).end, 1858);

    // Schreiben -> Lesen ergibt dieselben Einträge (25 fps)
    const QVector<SubtitleCue> list{{1, 0, 30, "Eins"}, {2, 50, 75, "Zwei\nZeilen"}, {3, 90000, 90025, "Stunde"}};
    const QByteArray out = Subtitles::toSrt(list, 25);
    CHECK(out.startsWith("1\r\n00:00:00,000 --> 00:00:01,200\r\nEins\r\n\r\n2\r\n"));
    CHECK(out.contains("01:00:00,000 --> 01:00:01,000"));
    CHECK_EQ(cues(Subtitles::parseSrt(out, 25)), cues(list));
    // Bereich: Zeiten ab Bereichsanfang, angeschnittene gekürzt, Einträge außerhalb weg
    CHECK_EQ(cues(Subtitles::parseSrt(Subtitles::toSrt(list, 25, 10, 60), 25)), QString("0-20:Eins 40-50:Zwei|Zeilen"));
    CHECK_EQ(Subtitles::srtTime(3723004), QString("01:02:03,004"));

    // Timecode-Eingabe im Inspector (wie DaVinci)
    CHECK_EQ(Timecode::parse("00:00:10:05", 25), 255);
    CHECK_EQ(Timecode::parse("1:00", 25), 25);        // Teile von rechts
    CHECK_EQ(Timecode::parse("1000", 25), 250);       // Ziffern von rechts: 00:00:10:00
    CHECK_EQ(Timecode::parse("01;00;00;00", 30), 108000);
    CHECK_EQ(Timecode::parse("abc", 25), -1);
    CHECK_EQ(Timecode::parse("", 25), -1);
    CHECK_EQ(Timecode::parse("1:2:3:4:5", 25), -1);
}

void testPlace()
{
    SubtitleTrack t;
    t.cues = {{1, 0, 50, "a"}, {2, 60, 100, "b"}, {3, 120, 150, "c"}};
    Subtitles::place(t, {4, 40, 130, "neu"}); // a gekürzt, b verdeckt, c angeschnitten
    CHECK_EQ(cues(t.cues), QString("0-40:a 40-130:neu 130-150:c"));
    Subtitles::place(t, {5, 60, 70, "mitten"}); // teilt "neu": rechter Teil braucht eine neue id
    CHECK_EQ(cues(t.cues), QString("0-40:a 40-60:neu 60-70:mitten 70-130:neu 130-150:c"));
    CHECK_EQ(t.cues[1].id, 4);
    CHECK_EQ(t.cues[3].id, 0);
    CHECK_EQ(Subtitles::cueAt(t, 65), 2);
    CHECK_EQ(Subtitles::cueAt(t, 150), -1);
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();

    // Erster Untertitel legt ST1 an (sichtbar), 3 s lang, ist ausgewählt
    const int a = ed.addSubtitle(100);
    CHECK(a > 0);
    CHECK_EQ(p.timeline().subtitles.size(), 1);
    CHECK(p.timeline().subtitles[0].enabled);
    CHECK_EQ(track(p, 0), QString("100-175:Untertitel"));
    CHECK(sel.contains(a) && ed.isSubtitle(a));
    CHECK_EQ(undo->count(), 1);
    CHECK_EQ(ed.addSubtitle(120), 0); // dort liegt schon einer
    const int b = ed.addSubtitle(50); // vor dem nächsten gekürzt
    CHECK_EQ(track(p, 0), QString("50-100:Untertitel 100-175:Untertitel"));

    ed.setSubtitleText(a, "Hallo", "tippen");
    ed.setSubtitleText(a, "Hallo Welt", "tippen");
    CHECK_EQ(undo->count(), 3); // Tippen = ein Schritt
    CHECK_EQ(track(p, 0), QString("50-100:Untertitel 100-175:Hallo Welt"));
    undo->undo(); // Undo nimmt den Text wirklich zurück (Undo-Stand teilte sich früher die Daten)
    CHECK_EQ(track(p, 0), QString("50-100:Untertitel 100-175:Untertitel"));
    undo->redo();
    CHECK_EQ(track(p, 0), QString("50-100:Untertitel 100-175:Hallo Welt"));

    // Kanten: begrenzt auf den Nachbarn und mindestens 1 Frame
    CHECK_EQ(ed.clampSubtitleTrim(a, TimelineOps::Edge::Start, -30), 0);
    CHECK_EQ(ed.clampSubtitleTrim(b, TimelineOps::Edge::End, 10), 0);
    CHECK_EQ(ed.clampSubtitleTrim(b, TimelineOps::Edge::End, -100), -49);
    ed.trimSubtitle(b, TimelineOps::Edge::End, -20);
    ed.trimSubtitle(a, TimelineOps::Edge::Start, -10);
    CHECK_EQ(track(p, 0), QString("50-80:Untertitel 90-175:Hallo Welt"));
    ed.setSubtitleTiming(a, 60, 500); // Start stößt an b
    CHECK_EQ(track(p, 0), QString("50-80:Untertitel 80-500:Hallo Welt"));

    // Zweite Spur: nicht sichtbar (nur eine zugleich); einblenden schaltet ST1 aus
    CHECK_EQ(ed.addSubtitleTrack(), 1);
    CHECK(!p.timeline().subtitles[1].enabled);
    ed.setSubtitleTrackEnabled(1, true);
    CHECK(!p.timeline().subtitles[0].enabled && p.timeline().subtitles[1].enabled);
    const int c = ed.addSubtitle(0); // landet auf der sichtbaren Spur
    CHECK_EQ(track(p, 1), QString("0-75:Untertitel"));

    // Verschieben auf ST1 überschreibt dort (wie Clips)
    ed.moveSubtitles({c}, 40, -1);
    CHECK_EQ(track(p, 0), QString("40-115:Untertitel 115-500:Hallo Welt"));
    CHECK_EQ(track(p, 1), QString(""));
    CHECK(!ed.isSubtitle(b)); // ganz verdeckt
    CHECK_EQ(ed.clampSubtitleTrackDelta({c}, 5), 1);
    ed.moveSubtitles({c}, -100, 0); // nicht vor Frame 0
    CHECK_EQ(track(p, 0), QString("0-75:Untertitel 115-500:Hallo Welt"));

    // Gesperrte Spur: nichts änderbar, Auswahl darauf wird aufgehoben
    sel.set({a, c});
    ed.toggleSubtitleTrackLock(0);
    CHECK(sel.isEmpty());
    CHECK_EQ(ed.addSubtitle(80, 0), 0);
    const int n = undo->count();
    ed.setSubtitleText(a, "x");
    ed.moveSubtitles({a}, 10, 0);
    ed.trimSubtitle(a, TimelineOps::Edge::End, -5);
    CHECK_EQ(undo->count(), n);
    ed.toggleSubtitleTrackLock(0);

    // Löschen (auch zusammen mit Clips in der Auswahl)
    sel.set({a});
    ed.deleteSelection();
    CHECK_EQ(track(p, 0), QString("0-75:Untertitel"));
    CHECK(sel.isEmpty());

    // Import als neue Spur (Name aus Datei), ids neu vergeben
    const QVector<SubtitleCue> in = Subtitles::parseSrt("1\n00:00:01,000 --> 00:00:02,000\nimportiert\n", 25);
    CHECK_EQ(ed.importSubtitles(in, "film.de"), 2);
    CHECK_EQ(p.timeline().subtitles[2].name, QString("film.de"));
    CHECK_EQ(track(p, 2), QString("25-50:importiert"));
    CHECK(p.timeline().subtitles[2].cues[0].id > 0);
    CHECK(!p.timeline().subtitles[2].enabled); // ST2 ist sichtbar

    // Stil ändern (Ziehen = ein Schritt), umbenennen, Spur löschen
    const int before = undo->count();
    ed.setSubtitleStyle(0, "Größe", [](TitleStyle& s) { s.size = 60; }, "size");
    ed.setSubtitleStyle(0, "Größe", [](TitleStyle& s) { s.size = 70; }, "size");
    CHECK_EQ(undo->count(), before + 1);
    ed.renameSubtitleTrack(0, "Untertitel 1"); // Standardname -> bleibt leer
    CHECK(p.timeline().subtitles[0].name.isEmpty());
    ed.removeSubtitleTrack(2);
    CHECK_EQ(p.timeline().subtitles.size(), 2);

    // Alles zurück -> leer, alles vor -> gleich
    const QByteArray end = ProjectFile::toJson(p.data(), "/x/p.schneidi");
    while (undo->canUndo()) undo->undo();
    CHECK(p.timeline().subtitles.isEmpty());
    while (undo->canRedo()) undo->redo();
    CHECK_EQ(ProjectFile::toJson(p.data(), "/x/p.schneidi"), end);
}

// Ripple/Teilen: Untertitelspuren folgen den Clip-Spuren wie in DaVinci (gesperrte bleiben stehen),
// Strg+B/Klinge teilen Einträge. Jede Bearbeitung ist ein Undo-Schritt.
struct RippleFixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    int x = 0, y = 0, z = 0; // V1: x [0,50) y [50,100) z [100,150), nur Bild
    RippleFixture(const QVector<SubtitleCue>& st1, const QVector<SubtitleCue>& st2 = {}, bool lockSt2 = true)
    {
        p.addMedia({"/x/a.mp4", "a.mp4", 300, true, false, false});
        p.addMedia({"/x/b.mp4", "b.mp4", 50, true, false, false});
        p.edit("setup", [&](Timeline& tl) {
            int start = 0;
            for (int* id : {&x, &y, &z}) {
                Clip c;
                c.id = *id = p.newClipId();
                c.mediaPath = "/x/a.mp4";
                c.start = start;
                c.in = start;
                c.out = start + 49;
                tl.video[0].clips << c;
                start += 50;
            }
            for (const auto& list : {st1, st2}) {
                SubtitleTrack t;
                for (SubtitleCue c : list) {
                    c.id = p.newClipId();
                    t.cues << c;
                }
                tl.subtitles << t;
            }
            tl.subtitles[1].locked = lockSt2;
            tl.subtitles[1].enabled = false;
        });
        p.undoStack()->clear();
    }
    QString st(int i) const { return track(p, i); }
    QString v1() const
    {
        QStringList out;
        for (const Clip& c : p.timeline().video[0].clips) out << QString("%1-%2").arg(c.start).arg(c.end());
        return out.join(' ');
    }
    int cueId(int trackIndex, int i) const { return p.timeline().subtitles[trackIndex].cues[i].id; }
};

void testRippleAndSplit()
{
    using TimelineOps::Edge;
    using TimelineOps::TrimKind;
    { // Löschen mit Ripple: ST1 rückt um den gelöschten Bereich, gesperrte ST2 bleibt stehen
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}, {0, 150, 160, "c"}}, {{0, 110, 120, "fest"}});
        f.sel.set({f.y});
        f.ed.rippleDeleteSelection();
        CHECK_EQ(f.v1(), QString("0-50 50-100"));
        CHECK_EQ(f.st(0), QString("10-40:a 60-90:b 100-110:c"));
        CHECK_EQ(f.st(1), QString("110-120:fest"));
        CHECK_EQ(f.p.undoStack()->count(), 1);
        f.p.undoStack()->undo();
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-150"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-140:b 150-160:c"));
    }
    { // Nicht gesperrte ST2 rückt ebenfalls; Eintrag über dem gelöschten Clip: Spur bliebe nicht überschneidungsfrei
      // -> sie bleibt ganz stehen (wie die Clip-Spuren, nie überschreiben)
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}}, {{0, 60, 90, "drüber"}, {0, 120, 130, "danach"}}, false);
        f.sel.set({f.y});
        f.ed.rippleDeleteSelection();
        CHECK_EQ(f.st(0), QString("10-40:a 60-90:b"));
        CHECK_EQ(f.st(1), QString("60-90:drüber 120-130:danach"));
    }
    { // Löschen mit Ripple vorne: b würde unter den stehenbleibenden Eintrag rutschen -> Spur bleibt stehen
        RippleFixture f({{0, 20, 30, "früh"}, {0, 60, 70, "b"}});
        f.sel.set({f.x});
        f.ed.rippleDeleteSelection();
        CHECK_EQ(f.v1(), QString("0-50 50-100"));
        CHECK_EQ(f.st(0), QString("20-30:früh 60-70:b"));
    }
    { // Strg+B ohne Auswahl: Clip und Eintrag unter dem Playhead, gesperrte Spur nicht; ein Undo-Schritt
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}}, {{0, 110, 125, "fest"}});
        const int b = f.cueId(0, 1);
        f.ed.splitAtPlayhead(120);
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-120 120-150"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 120-140:b"));
        CHECK_EQ(f.cueId(0, 1), b); // links behält die id
        CHECK(f.cueId(0, 2) > 0 && f.cueId(0, 2) != b && !TimelineOps::findClip(f.p.timeline(), f.cueId(0, 2)));
        CHECK_EQ(f.st(1), QString("110-125:fest"));
        CHECK_EQ(f.p.undoStack()->count(), 1);
        f.p.undoStack()->undo();
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-150"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-140:b"));
        f.ed.splitAtPlayhead(110); // genau am Anfang: nichts zu teilen beim Eintrag
        CHECK_EQ(f.st(0), QString("10-40:a 110-140:b"));
    }
    { // Strg+B mit Auswahl: nur ausgewählte Einträge bzw. Clips
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}}, {{0, 110, 125, "zwei"}}, false);
        f.ed.setSplitOnSelectedTracks(false);
        f.sel.set({f.cueId(0, 1)});
        f.ed.splitAtPlayhead(120);
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-150"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 120-140:b"));
        CHECK_EQ(f.st(1), QString("110-125:zwei"));
        f.sel.set({f.z});
        f.ed.splitAtPlayhead(130);
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-130 130-150"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 120-140:b"));
        // „Auf Spuren der Auswahl“: Eintrag einer anderen Stelle derselben Spur ist ausgewählt
        f.sel.set({f.cueId(0, 0)});
        f.ed.splitAtPlayhead(135); // aus: a liegt nicht unter dem Playhead
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 120-140:b"));
        f.ed.setSplitOnSelectedTracks(true);
        f.ed.splitAtPlayhead(135);
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 120-135:b 135-140:b"));
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-130 130-150"));
        CHECK_EQ(f.st(1), QString("110-125:zwei"));
    }
    { // Klinge auf einen Eintrag: nur dieser; gesperrte Spur / Kante: nichts
        RippleFixture f({{0, 110, 140, "b"}}, {{0, 110, 125, "fest"}});
        f.ed.bladeAt(f.cueId(0, 0), 130);
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-150"));
        CHECK_EQ(f.st(0), QString("110-130:b 130-140:b"));
        CHECK_EQ(f.p.undoStack()->count(), 1);
        f.ed.bladeAt(f.cueId(1, 0), 115);
        f.ed.bladeAt(f.cueId(0, 0), 110);
        f.ed.bladeAt(f.cueId(0, 0), 130);
        CHECK_EQ(f.st(1), QString("110-125:fest"));
        CHECK_EQ(f.st(0), QString("110-130:b 130-140:b"));
        CHECK_EQ(f.p.undoStack()->count(), 1);
    }
    { // Einfügen (F9): Eintrag über dem Einfügepunkt wird geteilt, Rest rückt mit; gesperrte Spur bleibt
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}, {0, 140, 150, "c"}}, {{0, 110, 125, "fest"}});
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        const int n = f.p.undoStack()->count();
        CHECK_EQ(f.ed.sourceEdit(Editor::SourceEditMode::Insert, "/x/b.mp4", 0, 120), 140);
        CHECK_EQ(f.v1(), QString("0-50 50-100 100-120 120-140 140-170"));
        CHECK_EQ(f.st(0), QString("10-40:a 110-120:b 140-160:b 160-170:c"));
        CHECK_EQ(f.st(1), QString("110-125:fest"));
        CHECK_EQ(f.p.undoStack()->count(), n + 1);
        f.p.undoStack()->undo();
        CHECK_EQ(f.st(0), QString("10-40:a 110-140:b 140-150:c"));
    }
    { // Ripple-Überschreiben: y (50) durch 20 Frames ersetzen -> Rest rückt um -30
        RippleFixture f({{0, 10, 40, "a"}, {0, 110, 140, "b"}});
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        f.ed.sourceEdit(Editor::SourceEditMode::RippleOverwrite, "/x/b.mp4", 0, 60);
        CHECK_EQ(f.v1(), QString("0-50 50-70 70-120"));
        CHECK_EQ(f.st(0), QString("10-40:a 80-110:b"));
    }
    { // Ripple-Trimmen: Verlängern schiebt ST1, Verkürzen nur so weit, wie ST1 nachrücken kann
        RippleFixture f({{0, 90, 95, "davor"}, {0, 100, 120, "b"}}, {{0, 99, 101, "fest"}});
        const auto e = f.ed.trimEdit(TrimKind::Ripple, f.y, Edge::End);
        f.ed.applyTrimEdit(e, 10);
        CHECK_EQ(f.v1(), QString("0-50 50-110 110-160"));
        CHECK_EQ(f.st(0), QString("90-95:davor 110-130:b"));
        CHECK_EQ(f.st(1), QString("99-101:fest")); // gesperrt: zählt auch nicht als Hindernis
        CHECK_EQ(f.ed.clampTrimEdit(e, -40), -15); // b darf bis an "davor" (95)
        f.ed.applyTrimEdit(e, -40);
        CHECK_EQ(f.v1(), QString("0-50 50-95 95-145"));
        CHECK_EQ(f.st(0), QString("90-95:davor 95-115:b"));
        CHECK_EQ(f.p.undoStack()->count(), 2);
        f.p.undoStack()->undo();
        f.p.undoStack()->undo();
        CHECK_EQ(f.st(0), QString("90-95:davor 100-120:b"));
        // ST1 gesperrt: kein Hindernis mehr, bleibt stehen
        f.ed.toggleSubtitleTrackLock(0);
        CHECK_EQ(f.ed.clampTrimEdit(e, -40), -40);
        f.ed.applyTrimEdit(e, -40);
        CHECK_EQ(f.v1(), QString("0-50 50-60 60-110"));
        CHECK_EQ(f.st(0), QString("90-95:davor 100-120:b"));
    }
    { // Reine Datenfunktionen
        SubtitleTrack t;
        t.cues = {{1, 0, 10, "a"}, {2, 20, 30, "b"}};
        int next = 10;
        CHECK_EQ(Subtitles::splitAt(t, 20, [&] { return next++; }), 0);
        CHECK_EQ(Subtitles::splitAt(t, 25, [&] { return next++; }), 10);
        CHECK_EQ(cues(t.cues), QString("0-10:a 20-25:b 25-30:b"));
        CHECK_EQ(Subtitles::rippleRoom(t, 20), 10);
        CHECK_EQ(Subtitles::rippleRoom(t, 5), 10); // a liegt über `from`, bleibt stehen
        CHECK_EQ(Subtitles::rippleRoom(t, 22), 0);
        CHECK_EQ(Subtitles::rippleRoom(t, 40), -1);
        CHECK(!Subtitles::ripple(t, {{20, -11}}));
        CHECK(Subtitles::ripple(t, {{20, -10}, {25, 3}}));
        CHECK_EQ(cues(t.cues), QString("0-10:a 10-15:b 18-23:b"));
        Subtitles::insertGap(t, 5, 4, [&] { return next++; });
        CHECK_EQ(cues(t.cues), QString("0-5:a 9-14:a 14-19:b 22-27:b"));
    }
}

void testProjectFile()
{
    ProjectData d;
    d.format = {1920, 1080, {25, 1}};
    Clip clip;
    clip.id = 5;
    clip.kind = ClipKind::Title;
    clip.out = 99;
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << clip;
    SubtitleTrack t;
    t.name = "Deutsch";
    t.locked = true;
    t.style.color = QColor(255, 255, 0);
    t.style.boxOn = true;
    t.cues = {{7, 0, 25, "Eins"}, {8, 30, 60, "Zwei\nZeilen"}};
    SubtitleTrack t2;
    t2.enabled = false;
    t2.cues = {{5, 0, 10, "gleiche id wie der Clip"}, {0, 20, 30, "ohne id"}};
    d.timeline.subtitles = {t, t2};
    d.lastClipId = 8;
    const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
    CHECK_EQ(back.timeline.subtitles.size(), 2);
    const SubtitleTrack& b = back.timeline.subtitles[0];
    CHECK_EQ(b.name, QString("Deutsch"));
    CHECK(b.enabled && b.locked);
    CHECK(b.style == t.style);
    CHECK_EQ(cues(b.cues), cues(t.cues));
    CHECK_EQ(b.cues[0].id, 7);
    // doppelte/fehlende ids werden neu vergeben
    const SubtitleTrack& b2 = back.timeline.subtitles[1];
    CHECK(!b2.enabled);
    CHECK(b2.cues[0].id > 8 && b2.cues[1].id > 8 && b2.cues[0].id != b2.cues[1].id);
    CHECK(back.lastClipId >= b2.cues[1].id);
    // ohne Stilangaben: Untertitel-Standard (nicht Titel-Standard)
    QByteArray old = json;
    ProjectData d2 = d;
    d2.timeline.subtitles[0].style = subtitleBaseStyle();
    CHECK(ProjectFile::fromJson(ProjectFile::toJson(d2, "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
    CHECK(back.timeline.subtitles[0].style == subtitleBaseStyle());
    // Speichern -> Laden -> Speichern identisch
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
    ProjectData again;
    CHECK(ProjectFile::fromJson(ProjectFile::toJson(back, "/x/p.schneidi"), "/x/p.schneidi", &again, &err));
    CHECK_EQ(ProjectFile::toJson(again, "/x/p.schneidi"), ProjectFile::toJson(back, "/x/p.schneidi"));

    // Formatwechsel rechnet den Spurstil mit um; Standardstil folgt dem Format
    Timeline tl = d.timeline;
    scaleTimeline(tl, {1920, 1080}, {3840, 2160});
    CHECK_EQ(tl.subtitles[0].style.size, t.style.size * 2);
    CHECK_EQ(tl.subtitles[0].style.posY, t.style.posY * 2);
    CHECK_EQ(Subtitles::defaultStyle({3840, 2160}).size, 96.0);
    CHECK_EQ(Subtitles::defaultStyle({1080, 1920}).size, 48.0); // Hochformat: kürzere Kante
    CHECK_EQ(Subtitles::defaultStyle({1080, 1920}).posY, 107.0); // 60 * 1920 / 1080, gerundet
}

QImage render(const Timeline& tl, const ProjectFormat& fmt, int pos, bool subtitles = true)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    b.setSubtitles(subtitles);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof->width(), h = prof->height();
    const uint8_t* data = f->get_image(ifmt, w, h);
    if (!data) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    return img.copy();
}

// Helle Pixel (Text weiß auf Schwarz) in einem Bereich (relativ)
int bright(const QImage& img, QRectF rel)
{
    int n = 0;
    for (int y = int(rel.top() * img.height()); y < int(rel.bottom() * img.height()); ++y)
        for (int x = int(rel.left() * img.width()); x < int(rel.right() * img.width()); ++x)
            if (qGray(img.pixel(x, y)) > 160) ++n;
    return n;
}

int testRender(const QString& dir)
{
    const QString black = Check::makeMedia(dir + "/black.mp4", {"-f", "lavfi", "-i", "color=black:size=640x360:rate=25:duration=4",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!black.isEmpty())) return 1;
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt{640, 360, {25, 1}};
    {
        auto prof = makeProfile(fmt);
        Mlt::Filter qt(*prof, "qtext");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = black;
    c.out = 99;
    tl.video[0].clips << c;
    SubtitleTrack st;
    st.style = Subtitles::defaultStyle(fmt.size());
    st.style.bold = true; // bei 360p (16 px) sonst zu dünn für eine klare Helligkeitsschwelle
    st.cues = {{2, 10, 40, "Ein ziemlich langer Untertitel, der auf zwei Zeilen umbrochen werden muss"}};
    tl.subtitles << st;

    const QRectF bottom(0, 0.6, 1, 0.4), top(0, 0, 1, 0.5);
    const QImage on = render(tl, fmt, 20);
    if (!CHECK(!on.isNull())) return 1;
    if (qEnvironmentVariableIsSet("SUBTITLE_DUMP")) on.save(qEnvironmentVariable("SUBTITLE_DUMP"));
    CHECK(bright(on, bottom) > 100);
    CHECK_EQ(bright(on, top), 0);
    CHECK_EQ(bright(on, QRectF(0, 0.95, 1, 0.05)), 0); // Abstand zum unteren Rand
    CHECK_EQ(bright(on, QRectF(0, 0.6, 0.03, 0.4)), 0); // umbrochen: nichts am linken Rand
    CHECK_EQ(bright(render(tl, fmt, 5), bottom), 0);   // vor dem Eintrag
    CHECK_EQ(bright(render(tl, fmt, 40), bottom), 0);  // Ende exklusiv
    CHECK_EQ(bright(render(tl, fmt, 20, false), bottom), 0); // Export ohne Einbrennen
    Timeline hidden = tl;
    hidden.subtitles[0].enabled = false;
    CHECK_EQ(bright(render(hidden, fmt, 20), bottom), 0);
    // Position: höher gesetzt -> Text weiter oben
    Timeline high = tl;
    high.subtitles[0].style.posY = 150;
    const QImage hi = render(high, fmt, 20);
    CHECK_EQ(bright(hi, QRectF(0, 0.75, 1, 0.25)), 0);
    CHECK(bright(hi, QRectF(0, 0.3, 1, 0.4)) > 100);

    // Export: SRT daneben (Bereich 20..29 -> Zeiten ab 0, Ende am Bereichsende), Fehler ohne Untertitel
    RenderJob job;
    job.path = dir + "/out.mp4";
    job.settings.subtitles = RenderSettings::SrtFile;
    job.inOut = true;
    job.from = 20;
    job.to = 29;
    ExportSettings s = RenderQueue::exportSettings(job, fmt);
    CHECK_EQ(s.subtitlePath, dir + "/out.srt");
    CHECK(!s.burnSubtitles);
    Exporter ex;
    QString err;
    CHECK(ex.start(tl, s, &err));
    ex.cancel();
    QFile srt(dir + "/out.srt");
    CHECK(srt.open(QIODevice::ReadOnly));
    CHECK_EQ(QString::fromUtf8(srt.readAll()), QString("1\r\n00:00:00,000 --> 00:00:00,400\r\n") + st.cues[0].text + "\r\n\r\n");
    Timeline none = tl;
    none.subtitles.clear();
    CHECK(!ex.start(none, s, &err));
    CHECK(err.contains("Untertitel"));
    job.settings.subtitles = RenderSettings::BurnSubtitles;
    s = RenderQueue::exportSettings(job, fmt);
    CHECK(s.burnSubtitles && s.subtitlePath.isEmpty());
    CHECK(job.settings.summary({640, 360}).contains("Untertitel"));
    CHECK(RenderSettings::fromJson(job.settings.toJson()) == job.settings);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("subtitles");
    testSrt();
    testPlace();
    testEditor();
    testRippleAndSplit();
    testProjectFile();
    if (!Check::haveFfmpeg()) {
        Check::result();
        return Check::skip("ffmpeg nicht gefunden (Render-Teil)");
    }
    QTemporaryDir tmp;
    if (CHECK(tmp.isValid()))
        if (const int r = testRender(tmp.path()); r == Check::kSkip) {
            Check::result();
            return r;
        }
    return Check::result();
}
