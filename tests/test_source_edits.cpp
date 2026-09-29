// Test der Quell-Bearbeitungen wie DaVinci (F9 Einfügen, F10 Überschreiben, F11 Ersetzen, F12 Oben platzieren,
// Ripple-Überschreiben, Anhängen), Quell-In/Out in der Projektdatei – ohne UI
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include "check.h"

#include <QCoreApplication>
#include <cmath>

using SE = Editor::SourceEditMode;
using Check::dump;

// Vergleich mit Namen (Ausgabe nur bei Abweichung)
static void check(const char* name, const QString& got, const QString& want)
{
    Check::report(got == want, name, __FILE__, __LINE__, QString("ist: %1\n       erwartet: %2").arg(got, want));
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    Fixture()
    {
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        p.addMedia({"/x/b.mp4", "b.mp4", 50, true, true, false});
        p.addMedia({"/x/m.wav", "m.wav", 30, false, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v; v.id = p.newClipId(); v.mediaPath = "/x/a.mp4"; v.start = 0; v.in = 0; v.out = 99; v.linkId = p.newLinkId();
            Clip a = v; a.id = p.newClipId();
            tl.video[0].clips << v;
            tl.audio[0].clips << a;
        });
    }
};

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("source-edits");
    {
        Fixture f;
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        const int end = f.ed.sourceEdit(SE::Insert, "/x/b.mp4", 0, 40);
        check("Insert", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: a[0-40|0-39] b[40-60|10-29] a[60-120|40-99]  V2:  A1: a[0-40|0-39] b[40-60|10-29] a[60-120|40-99]  A2: end=60");
        // verknüpft: V1-b und A1-b gleiche linkId, rechte Teile von a ebenfalls
        const Timeline& tl = f.p.timeline();
        const bool linked = tl.video[0].clips[1].linkId && tl.video[0].clips[1].linkId == tl.audio[0].clips[1].linkId
                            && tl.video[0].clips[2].linkId == tl.audio[0].clips[2].linkId
                            && tl.video[0].clips[2].linkId != tl.video[0].clips[0].linkId;
        check("Insert verknüpft", linked ? "ja" : "nein", "ja");
        f.p.undoStack()->undo();
        check("Insert Undo", dump(f.p.timeline()), "V1: a[0-100|0-99]  V2:  A1: a[0-100|0-99]  A2:");
    }
    {
        Fixture f;
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        const int end = f.ed.sourceEdit(SE::Overwrite, "/x/b.mp4", 0, 40);
        check("Overwrite", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: a[0-40|0-39] b[40-60|10-29] a[60-100|60-99]  V2:  A1: a[0-40|0-39] b[40-60|10-29] a[60-100|60-99]  A2: end=60");
    }
    {
        Fixture f; // Timeline-In+Out begrenzt die Länge, Marken werden verbraucht
        f.ed.setMarkIn(10);
        f.ed.setMarkOut(14);
        const int end = f.ed.sourceEdit(SE::Overwrite, "/x/b.mp4", 0, 70);
        check("Overwrite In/Out", dump(f.p.timeline()) + QString(" end=%1 marks=%2/%3").arg(end)
                  .arg(f.p.timeline().markIn).arg(f.p.timeline().markOut),
              "V1: a[0-10|0-9] b[10-15|0-4] a[15-100|15-99]  V2:  A1: a[0-10|0-9] b[10-15|0-4] a[15-100|15-99]  A2: end=15 marks=-1/-1");
    }
    {
        Fixture f; // nur Timeline-Out: rückwärts
        f.ed.setMarkOut(30);
        f.ed.setSourceMarkIn("/x/b.mp4", 5);
        f.ed.setSourceMarkOut("/x/b.mp4", 14);
        const int end = f.ed.sourceEdit(SE::Overwrite, "/x/b.mp4", 0, 70);
        check("Overwrite nur Out", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: a[0-21|0-20] b[21-31|5-14] a[31-100|31-99]  V2:  A1: a[0-21|0-20] b[21-31|5-14] a[31-100|31-99]  A2: end=31");
    }
    {
        Fixture f; // Replace: Quell-Playhead 30 deckt sich mit Timeline-Playhead 20
        const int end = f.ed.sourceEdit(SE::Replace, "/x/b.mp4", 30, 20);
        check("Replace zu wenig Material", QString::number(end), "-1");
        f.ed.bladeAt(1, 30); // a 0..30 | 30..100
        const int end2 = f.ed.sourceEdit(SE::Replace, "/x/b.mp4", 30, 20);
        check("Replace", dump(f.p.timeline()) + QString(" end=%1").arg(end2),
              "V1: b[0-30|10-39] a[30-100|30-99]  V2:  A1: b[0-30|10-39] a[30-100|30-99]  A2: end=20");
    }
    {
        Fixture f;
        const int end = f.ed.sourceEdit(SE::PlaceOnTop, "/x/b.mp4", 0, 20);
        check("Place on Top", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: a[0-100|0-99]  V2: b[20-70|0-49]  A1: a[0-100|0-99]  A2: b[20-70|0-49] end=70");
    }
    {
        Fixture f; // Ripple Overwrite: a (100) durch b-Bereich (20) ersetzen, dahinter liegender Clip rückt nach
        f.ed.sourceEdit(SE::AppendAtEnd, "/x/m.wav", 0, 0); // m.wav auf A1 bei 100..130
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        const int end = f.ed.sourceEdit(SE::RippleOverwrite, "/x/b.mp4", 0, 50);
        check("Ripple Overwrite", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: b[0-20|10-29]  V2:  A1: b[0-20|10-29] m[20-50|0-29]  A2: end=20");
    }
    {
        Fixture f; // Einfügen nur Ton
        const int end = f.ed.sourceEdit(SE::Insert, "/x/m.wav", 0, 50);
        check("Insert Ton", dump(f.p.timeline()) + QString(" end=%1").arg(end),
              "V1: a[0-50|0-49] a[80-130|50-99]  V2:  A1: a[0-50|0-49] m[50-80|0-29] a[80-130|50-99]  A2: end=80");
    }
    {
        Fixture f; // Speichern/Laden der Quell-In/Out
        f.ed.setSourceMarkIn("/x/b.mp4", 7);
        f.ed.setSourceMarkOut("/x/b.mp4", 3); // vor In -> In fällt weg
        ProjectData d;
        const QString ok0 = QString("%1/%2").arg(f.p.mediaInfo("/x/b.mp4")->markIn).arg(f.p.mediaInfo("/x/b.mp4")->markOut);
        check("Out vor In", ok0, "-1/3");
        f.ed.setSourceMarkIn("/x/b.mp4", 2);
        ProjectFile::fromJson(ProjectFile::toJson(f.p.data(), "/x/p.schneidi"), "/x/p.schneidi", &d, nullptr);
        QString got;
        for (const MediaInfo& m : d.media) got += QString("%1:%2/%3 ").arg(m.name).arg(m.markIn).arg(m.markOut);
        check("Speichern/Laden", got.trimmed(), "a.mp4:-1/-1 b.mp4:2/3 m.wav:-1/-1");
    }
    {   // Insert rückt alle nicht gesperrten Spuren mit (wie DaVinci), gesperrte bleiben
        Fixture f;
        f.p.edit("m", [&](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 2);
            Clip m; m.id = f.p.newClipId(); m.mediaPath = "/x/m.wav"; m.start = 70; m.in = 0; m.out = 29;
            tl.audio[1].clips << m;
        });
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        f.ed.sourceEdit(SE::Insert, "/x/b.mp4", 0, 40);
        check("Insert alle Spuren", dump(f.p.timeline()),
              "V1: a[0-40|0-39] b[40-60|10-29] a[60-120|40-99]  V2:  A1: a[0-40|0-39] b[40-60|10-29] a[60-120|40-99]  A2: m[90-120|0-29]");
        f.p.undoStack()->undo();
        f.ed.toggleTrackLock({TrackKind::Audio, 0});
        f.ed.sourceEdit(SE::Insert, "/x/b.mp4", 0, 40);
        check("Insert A1 gesperrt", dump(f.p.timeline()),
              "V1: a[0-40|0-39] b[40-60|10-29] a[60-120|40-99]  V2:  A1: a[0-100|0-99]  A2: m[90-120|0-29]");
        f.ed.toggleTrackLock({TrackKind::Video, 0});
        const int r = f.ed.sourceEdit(SE::Overwrite, "/x/b.mp4", 0, 0);
        check("Beide gesperrt -> nichts", QString::number(r), "-1");
    }
    {
        // Fit to Fill (Shift+F11): 20 Quell-Frames (10..29) auf 40 Timeline-Frames (40..79) = 50 %, überschreibt,
        // in/out im umgerechneten Material, Marken verbraucht
        Fixture f;
        f.ed.setSourceMarkIn("/x/b.mp4", 10);
        f.ed.setSourceMarkOut("/x/b.mp4", 29);
        f.ed.setMarkIn(40);
        f.ed.setMarkOut(79);
        const int end = f.ed.sourceEdit(SE::FitToFill, "/x/b.mp4", 0, 0);
        CHECK_EQ(end, 80);
        const Timeline& tl = f.p.timeline();
        check("FitToFill", dump(tl), "V1: a[0-40|0-39] b[40-80|20-59] a[80-100|80-99]  V2:  A1: a[0-40|0-39] b[40-80|20-59] a[80-100|80-99]  A2:");
        for (const Track* t : {&tl.video[0], &tl.audio[0]})
            CHECK(std::abs(t->clips[1].speed - 0.5) < 1e-9);
        CHECK(tl.markIn < 0 && tl.markOut < 0);
        f.p.undoStack()->undo();
        CHECK_EQ(f.p.timeline().markIn, 40); // ein Undo-Schritt, Marken wieder da
    }
    {
        // Schneller: 40 Quell-Frames auf 10 = 400 %; ohne Timeline-Out bzw. mit zu wenig Material nichts
        Fixture f;
        f.ed.setSourceMarkIn("/x/b.mp4", 0);
        f.ed.setSourceMarkOut("/x/b.mp4", 39);
        f.ed.setMarkIn(0);
        CHECK_EQ(f.ed.sourceEdit(SE::FitToFill, "/x/b.mp4", 0, 0), -1);
        f.ed.setMarkOut(9);
        CHECK_EQ(f.ed.sourceEdit(SE::FitToFill, "/x/b.mp4", 0, 0), 10);
        const Clip& c = f.p.timeline().video[0].clips[0];
        CHECK(std::abs(c.speed - 4.0) < 1e-9);
        CHECK_EQ(c.in, 0);
        CHECK_EQ(c.length(), 10);
    }
    return Check::result();
}
