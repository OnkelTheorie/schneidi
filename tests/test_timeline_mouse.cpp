// Test Maus-Bedienung der Timeline (offscreen): Auswahlrahmen, Auto-Scroll am Rand, Spuren umsortieren;
// dazu Auswahl ab Playhead (Y / Strg+Y / Alt+Y).
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTest>
#include <cmath>

namespace {

Clip clip(int id, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QApplication::sendEvent(&w, &e);
}

void drag(QWidget& w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
{
    mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, to, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    TimelineView tv{&editor};

    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 3000, true, false, false});
        Timeline tl;
        tl.video.resize(2);
        tl.audio.resize(1);
        tl.video[0].clips << clip(1, 0, 100) << clip(2, 150, 100); // V1
        tl.video[1].clips << clip(3, 50, 70);                       // V2
        tl.audio[0].clips << clip(4, 0, 100);                       // A1
        project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 5, 0});
        tv.resize(1200, 500);
        tv.show();
        QApplication::processEvents();
    }
    // Zeilen: V2 ab y 30, V1 ab 94 (je 64 hoch), Trennfuge 6, A1 ab 164
    int x(double frame) const { return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * tv.view().pxPerFrame)); }
};

void testRubber()
{
    Fixture f;
    // Rahmen von V2 (Frame 130, leer) nach V1 (Frame 95): berührt 3 und 1 – müssen nicht ganz drin sein
    drag(f.tv, {f.x(130), 40}, {f.x(95), 120});
    CHECK(f.sel.ids() == (QSet<int>{1, 3}));
    CHECK(f.project.timeline().video[1].clips[0].start == 50); // nichts verschoben

    // Klick auf leere Stelle hebt die Auswahl auf
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.x(130), 120}, Qt::NoButton);
    CHECK(f.sel.isEmpty());

    // Rahmen ohne Treffer -> nichts; nach oben/links aufgezogen geht genauso
    drag(f.tv, {f.x(140), 120}, {f.x(110), 100});
    CHECK(f.sel.isEmpty());
    drag(f.tv, {f.x(130), 200}, {f.x(90), 170});
    CHECK(f.sel.ids() == QSet<int>{4});

    // Strg bzw. Shift: zur bestehenden Auswahl hinzufügen
    drag(f.tv, {f.x(140), 120}, {f.x(160), 110}, Qt::ControlModifier);
    CHECK(f.sel.ids() == (QSet<int>{2, 4}));
    drag(f.tv, {f.x(125), 40}, {f.x(115), 50}, Qt::ShiftModifier);
    CHECK(f.sel.ids() == (QSet<int>{2, 3, 4}));

    // Gesperrte Spur: nichts darauf wird ausgewählt
    f.editor.toggleTrackLock({TrackKind::Video, 0});
    drag(f.tv, {f.x(125), 40}, {f.x(95), 120});
    CHECK(f.sel.ids() == QSet<int>{3});
}

void testAutoScroll()
{
    Fixture f;
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    // Rahmen ab Frame 120 in V1, Maus über den rechten Rand hinaus: Timeline scrollt mit, Rahmenanfang bleibt
    // an Frame 120 (Clip 1 endet bei 100 -> nicht drin)
    const QPoint from(f.x(120), 120);
    mouse(f.tv, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, from + QPoint(20, 0), Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.tv.width() + 10, 120}, Qt::LeftButton);
    QTest::qWait(300);
    const double scrolled = f.tv.view().leftFrame;
    CHECK(scrolled > 20);
    mouse(f.tv, QEvent::MouseButtonRelease, {f.tv.width() + 10, 120}, Qt::NoButton);
    CHECK(f.sel.ids() == QSet<int>{2});
    QTest::qWait(100);
    CHECK_EQ(f.tv.view().leftFrame, scrolled); // nach dem Loslassen kein Weiterscrollen

    // zurück an den linken Rand: scrollt bis Frame 0 und nicht weiter
    CHECK(scrolled < 120); // Frame 130 (leer in V1) noch sichtbar
    mouse(f.tv, QEvent::MouseButtonPress, {f.x(130), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {f.x(120), 120}, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, {TimelineView::kHeaderW - 40, 120}, Qt::LeftButton);
    QTest::qWait(1500);
    CHECK_EQ(f.tv.view().leftFrame, 0.0);
    mouse(f.tv, QEvent::MouseButtonRelease, {TimelineView::kHeaderW - 40, 120}, Qt::NoButton);
    CHECK(f.sel.ids() == QSet<int>{1}); // Rahmen reicht jetzt von Frame 0 bis 130

    // Clip verschieben über den rechten Rand: landet um den gescrollten Betrag weiter rechts
    const QPoint grab(f.x(200), 120);
    mouse(f.tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
    mouse(f.tv, QEvent::MouseMove, grab + QPoint(10, 0), Qt::LeftButton);
    const QPoint edge(f.tv.width() - 5, 120);
    mouse(f.tv, QEvent::MouseMove, edge, Qt::LeftButton);
    QTest::qWait(300);
    mouse(f.tv, QEvent::MouseButtonRelease, edge, Qt::NoButton);
    const double left = f.tv.view().leftFrame;
    CHECK(left > 0);
    const Clip* c = TimelineOps::findClip(f.project.timeline(), 2);
    const int expect = 150 + int(std::lround((edge.x() - grab.x()) / f.tv.view().pxPerFrame + left));
    CHECK(c && std::abs(c->start - expect) <= 1);
}

void testTrackMove()
{
    Fixture f;
    // Spurkopf V2 auf V1 ziehen: V2 wird zu V1 (Clip 3 auf Spur 0)
    drag(f.tv, {100, 50}, {100, 130});
    const Timeline& tl = f.project.timeline();
    CHECK(tl.video[0].clips.size() == 1 && tl.video[0].clips[0].id == 3);
    CHECK_EQ(tl.video[1].clips.size(), 2);
    // weit über die Audiospuren hinaus: nächste Videospur (V1) ist das Ziel, ohne Änderung
    drag(f.tv, {100, 120}, {100, 400});
    CHECK(f.project.timeline().video[0].clips[0].id == 3);
}

void testSelectFromPlayhead()
{
    Fixture f;
    // ohne Auswahl: Zielspuren V1 + A1; Clip unter dem Playhead gehört dazu
    f.editor.selectFromPlayhead(60, true, false);
    CHECK(f.sel.ids() == (QSet<int>{1, 2, 4}));
    f.editor.selectFromPlayhead(60, false, false);
    CHECK(f.sel.ids() == (QSet<int>{1, 4}));
    // alle Spuren
    f.editor.selectFromPlayhead(110, true, true);
    CHECK(f.sel.ids() == (QSet<int>{2, 3}));
    f.editor.selectFromPlayhead(110, false, true);
    CHECK(f.sel.ids() == (QSet<int>{1, 3, 4}));
    // mit Auswahl: deren Spuren (hier V2)
    f.sel.set({3});
    f.editor.selectFromPlayhead(0, true, false);
    CHECK(f.sel.ids() == QSet<int>{3});
    // gesperrte Spur bleibt außen vor
    f.editor.toggleTrackLock({TrackKind::Audio, 0});
    f.editor.selectFromPlayhead(0, true, true);
    CHECK(f.sel.ids() == (QSet<int>{1, 2, 3}));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline_mouse");
    I18n::install("de");
    Theme::apply(app);
    testRubber();
    testAutoScroll();
    testTrackMove();
    testSelectFromPlayhead();
    return Check::result();
}
