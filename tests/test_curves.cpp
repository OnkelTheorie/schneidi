// Test Kurven-Editor (Keyframes mit Bezier-Griffen): Kurvenwerte, abgeleitete Griffe, weiche Griffe, Teilen ohne
// Formänderung, Projektdatei, Engine-Linearität, Einrasten und die Bedienung im Kurven-Editor der Timeline (offscreen).
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QUndoStack>
#include <cmath>

namespace {

bool close(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

Clip clip(int length)
{
    Clip c;
    c.id = 1;
    c.mediaPath = "/x/a.mp4";
    c.in = 0;
    c.out = length - 1;
    return c;
}

void testValues()
{
    // Bezier mit abgeleitetem Griff auf der Geraden = genau linear
    Clip c = clip(100);
    Keys::setKey(c, AnimParam::PosX, 0, 0);
    Keys::setKey(c, AnimParam::PosX, 30, 30);
    KeyTrack& k = c.keys[AnimParam::PosX];
    Keys::setHandle(k, 0, true, 10, 10);
    CHECK(k[0].ease == KeyEase::Bezier);
    for (double t : {0.0, 3.3, 15.0, 29.0, 30.0}) CHECK(close(Keys::valueAt(c, AnimParam::PosX, t), t, 1e-6));

    // flache Griffe: symmetrisches S (langsam los, langsam an)
    Keys::setHandle(k, 0, true, 15, 0, true);
    Keys::setHandle(k, 1, false, -15, 0, true);
    CHECK(close(Keys::valueAt(c, AnimParam::PosX, 15), 15, 1e-6));
    CHECK(Keys::valueAt(c, AnimParam::PosX, 3) < 3 * 0.5);
    CHECK(Keys::valueAt(c, AnimParam::PosX, 27) > 30 - 3 * 0.5);
    double last = -1;
    bool monotone = true;
    for (int t = 0; t <= 30; ++t) {
        const double v = Keys::valueAt(c, AnimParam::PosX, t);
        monotone &= v >= last;
        last = v;
    }
    CHECK(monotone);
    CHECK(!Keys::linearBetween(c, AnimParam::PosX, 0, 30));
    CHECK(Keys::linearBetween(c, AnimParam::PosX, 30, 60)); // nach dem letzten Keyframe konstant

    // zu lange Griffe werden auf den Abschnitt gekürzt: Endpunkte stimmen, Zeit läuft vorwärts, kein NaN
    Keys::setHandle(k, 0, true, 500, 0, true);
    Keys::setHandle(k, 1, false, -500, 0, true);
    CHECK(close(Keys::valueAt(c, AnimParam::PosX, 0), 0) && close(Keys::valueAt(c, AnimParam::PosX, 30), 30));
    CHECK(!std::isnan(Keys::valueAt(c, AnimParam::PosX, 12.5)));
    double dt, dv;
    CHECK(Keys::handle(k, 0, true, &dt, &dv) && close(dt, 30));
    CHECK(!Keys::handle(k, 0, false, &dt, &dv)); // kein Nachbar davor

    // Überschwingen erlaubt (Griff nach oben)
    Keys::setHandle(k, 0, true, 10, 40, true);
    bool over = false;
    for (int t = 0; t <= 30; ++t) over |= Keys::valueAt(c, AnimParam::PosX, t) > 30;
    CHECK(over);

    // Griffe bleiben auf einer Linie (ohne Alt), mit Alt getrennt
    Keys::setHandle(k, 1, false, -10, -5); // Steigung 0,5
    CHECK(close(k[1].outDv, k[1].outDt * 0.5));
    Keys::setHandle(k, 1, false, -10, -20, true);
    CHECK(close(k[1].outDv, k[1].outDt * 0.5)); // unverändert
    CHECK(close(k[1].inDv, -20));

    // waagerechte Bezier-Kurve gleicher Werte = linear
    Clip f = clip(50);
    Keys::setKey(f, AnimParam::Opacity, 0, 50);
    Keys::setKey(f, AnimParam::Opacity, 20, 50);
    Keys::setEase(f, {0, 20}, KeyEase::Bezier);
    CHECK(Keys::linearBetween(f, AnimParam::Opacity, 0, 20));

    // Farben: Bezier wie linear
    Clip col = clip(50);
    Keys::setKey(col, AnimParam::TitleColor, 0, Keys::fromColor(QColor(0, 0, 0)));
    Keys::setKey(col, AnimParam::TitleColor, 10, Keys::fromColor(QColor(200, 100, 0)));
    Keys::setEase(col, {0}, KeyEase::Bezier);
    CHECK_EQ(Keys::toColor(Keys::valueAt(col, AnimParam::TitleColor, 5)).red(), 100);
}

void testAutoHandles()
{
    Clip c = clip(100);
    for (auto [t, v] : {std::pair{0, 0.0}, {10, 10.0}, {20, 0.0}, {40, 30.0}, {60, 90.0}})
        Keys::setKey(c, AnimParam::PosY, t, v);
    Keys::setEase(c, {0, 10, 20, 40, 60}, KeyEase::Bezier);
    const KeyTrack& k = c.keys[AnimParam::PosY];
    CHECK(close(k[1].inDv, 0) && close(k[1].outDv, 0)); // Spitze: flach
    CHECK(close(k[0].outDv, 0));                         // Anfang: flach
    // 40: Steigung (90 − 0) / (60 − 20) = 2,25; Griffe ein Drittel der Nachbarabschnitte
    CHECK(close(k[3].inDt, -20.0 / 3) && close(k[3].outDt, 20.0 / 3));
    CHECK(close(k[3].outDv, 20.0 / 3 * 2.25) && close(k[3].inDv, -20.0 / 3 * 2.25));
    // Kurve geht durch alle Keyframes
    for (const Keyframe& key : k) CHECK(close(Keys::valueAt(c, AnimParam::PosY, key.frame), key.value));
    // schon Bezier: Verlauf erneut setzen lässt die Griffe stehen
    KeyTrack before = k;
    Keys::setEase(c, {40}, KeyEase::Bezier);
    CHECK(c.keys[AnimParam::PosY] == before);
}

// Teilen im Bezier-Abschnitt: beide Teile folgen genau der alten Kurve
void testSplit()
{
    auto checkSplit = [](const Clip& orig, int cut, AnimParam p) {
        Clip left = orig, right = orig;
        left.out = orig.in + cut - 1;
        right.in = orig.in + cut;
        Keys::split(orig, left, right);
        double worst = 0;
        for (int t = 0; t < cut; ++t)
            worst = std::max(worst, std::abs(Keys::valueAt(left, p, t) - Keys::valueAt(orig, p, t)));
        for (int t = cut; t < orig.length(); ++t)
            worst = std::max(worst, std::abs(Keys::valueAt(right, p, t - cut) - Keys::valueAt(orig, p, t)));
        return worst;
    };
    Clip c = clip(120);
    Keys::setKey(c, AnimParam::ZoomX, 0, 1.0);
    Keys::setKey(c, AnimParam::ZoomX, 50, 3.0);
    Keys::setKey(c, AnimParam::ZoomX, 110, 0.5);
    Keys::setEase(c, {0, 50, 110}, KeyEase::Bezier);
    Keys::setHandle(c.keys[AnimParam::ZoomX], 1, true, 25, 0.8, true);
    CHECK(checkSplit(c, 20, AnimParam::ZoomX) < 1e-6);
    CHECK(checkSplit(c, 71, AnimParam::ZoomX) < 1e-6);
    CHECK(checkSplit(c, 51, AnimParam::ZoomX) < 1e-6);
    // gemischt: linear -> Bezier
    Clip m = clip(80);
    Keys::setKey(m, AnimParam::Rotation, 5, 0);
    Keys::setKey(m, AnimParam::Rotation, 60, 90);
    Keys::setHandle(m.keys[AnimParam::Rotation], 1, false, -30, 0, true);
    CHECK(checkSplit(m, 33, AnimParam::Rotation) < 1e-6);
    // Schnitt hinter dem letzten Keyframe (stürzte ab) und vor dem ersten
    CHECK(checkSplit(m, 70, AnimParam::Rotation) < 1e-6);
    CHECK(checkSplit(m, 3, AnimParam::Rotation) < 1e-6);
    // vor dem ersten Keyframe mit schrägem Bezier-Griff: rechter Teil bleibt bis dahin ruhig (wackelte vorher)
    Clip b = clip(80);
    Keys::setKey(b, AnimParam::PosX, 30, 100);
    Keys::setKey(b, AnimParam::PosX, 70, 0);
    Keys::setEase(b, {30}, KeyEase::Bezier);
    Keys::setHandle(b.keys[AnimParam::PosX], 0, false, -10, -40, true);
    CHECK(checkSplit(b, 10, AnimParam::PosX) < 1e-6);
}

void testProjectFile(const QString& dir)
{
    Project project;
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 300, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    Clip c = clip(200);
    c.mediaPath = "/x/a.mp4";
    Keys::setKey(c, AnimParam::Volume, 0, -10);
    Keys::setKey(c, AnimParam::Volume, 100, 3);
    Keys::setEase(c, {0}, KeyEase::Bezier);
    Keys::setHandle(c.keys[AnimParam::Volume], 0, true, 17.5, 4.25, true);
    Keys::setHandle(c.keys[AnimParam::Volume], 0, false, -3, 1.5, true);
    tl.video[0].clips << c;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 1, 0});
    const QString path = dir + "/kurven.schneidi";
    QString err;
    CHECK(ProjectFile::save(project.data(), path, &err));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    const Clip& b = back.timeline.video[0].clips[0];
    CHECK(b.keys.value(AnimParam::Volume) == c.keys.value(AnimParam::Volume));
    CHECK(b.keys[AnimParam::Volume][0].ease == KeyEase::Bezier);
    CHECK(close(b.keys[AnimParam::Volume][0].outDv, 4.25));
}

// Geschwindigkeit ändern streckt die Kurve samt Griffen, rückwärts spiegelt sie
void testSpeed()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 400, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    Clip c = clip(100);
    Keys::setKey(c, AnimParam::PosX, 10, 0);
    Keys::setKey(c, AnimParam::PosX, 60, 100);
    Keys::setKey(c, AnimParam::PosX, 90, 20);
    Keys::setEase(c, {10, 60, 90}, KeyEase::Bezier);
    Keys::setHandle(c.keys[AnimParam::PosX], 0, true, 30, 0, true);
    tl.video[0].clips << c;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 1, 0});
    auto now = [&] { return *TimelineOps::findClip(project.timeline(), 1); };
    editor.setClipSpeed({1}, Editor::Retime{0.5, false, false, true}, true);
    const Clip slow = now();
    CHECK_EQ(slow.length(), 200);
    double worst = 0;
    for (int t = 0; t < 100; ++t)
        worst = std::max(worst, std::abs(Keys::valueAt(slow, AnimParam::PosX, 2.0 * t) - Keys::valueAt(c, AnimParam::PosX, t)));
    CHECK(worst < 1e-6);
    project.undoStack()->undo();
    editor.setClipSpeed({1}, Editor::Retime{1.0, true, false, true}, true);
    const Clip rev = now();
    worst = 0;
    for (int t = 0; t < 100; ++t)
        worst = std::max(worst, std::abs(Keys::valueAt(rev, AnimParam::PosX, 99 - t) - Keys::valueAt(c, AnimParam::PosX, t)));
    CHECK(worst < 1e-6);
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QApplication::sendEvent(&w, &e);
}

void drag(QWidget& w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = {})
{
    mouse(w, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
    const QPoint mid = (from + to) / 2;
    mouse(w, QEvent::MouseMove, mid, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseMove, to, Qt::LeftButton, mods);
    mouse(w, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
}

void testUi()
{
    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 300, true, false, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c = clip(100);
    Keys::setKey(c, AnimParam::Opacity, 10, 20);
    Keys::setKey(c, AnimParam::Opacity, 80, 80);
    Keys::setKey(c, AnimParam::PosX, 50, 7); // anderer Parameter: bleibt beim Löschen stehen
    tl.video[0].clips << c;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, 1, 0});
    auto opacity = [&] { return TimelineOps::findClip(project.timeline(), 1)->keys.value(AnimParam::Opacity); };

    TimelineView tv(&editor);
    tv.resize(1200, 500);
    tv.show();
    QApplication::processEvents();
    const int h0 = tv.contentHeight();
    tv.setCurveEditor({1}, true, AnimParam::Opacity);
    CHECK(tv.curveEditorShown(1));
    CHECK_EQ(tv.contentHeight(), h0 + 120);
    CHECK(!tv.grab().isNull());

    // Lage wie in TimelineView/TimelineCurves: V1 ab y 30, Höhe 64 + 120; Kurvenfläche y 113..203 (91 px)
    const double ppf = tv.view().pxPerFrame;
    const int kH = TimelineView::kHeaderW;
    const double lo = 20 - 60 * 0.12, hi = 80 + 60 * 0.12;
    auto yOf = [&](double v) { return int(std::lround(203 - (v - lo) / (hi - lo) * 91)); };
    auto xOf = [&](double t) { return int(std::lround(kH + t * ppf)); };

    // Punkt bei 80 um 10 Frames nach rechts und 20 px nach oben: ein Undo-Schritt, Wert steigt
    const int undo0 = project.undoStack()->count();
    drag(tv, {xOf(80), yOf(80)}, {xOf(80) + int(10 * ppf), yOf(80) - 20});
    KeyTrack k = opacity();
    CHECK_EQ(k.size(), 2);
    CHECK_EQ(k.last().frame, 90);
    CHECK(k.last().value > 95 && k.last().value < 98);
    CHECK_EQ(project.undoStack()->count(), undo0 + 1);
    CHECK(sel.keyClip() == 1 && sel.keyParam() == int(AnimParam::Opacity) && sel.keyTimes() == QSet<int>{90});
    project.undoStack()->undo();
    CHECK_EQ(opacity().last().frame, 80);
    CHECK(close(opacity().last().value, 80));

    // Shift = nur eine Achse; Einrasten am Keyframe 50 des anderen Parameters
    drag(tv, {xOf(80), yOf(80)}, {xOf(51) + 1, yOf(80) - 3}, Qt::ShiftModifier);
    k = opacity();
    CHECK_EQ(k.last().frame, 50);
    CHECK(close(k.last().value, 80));
    project.undoStack()->undo();

    // Doppelklick in eine leere Stelle: neuer Keyframe mit dem Wert unter der Maus, Entf löscht nur diesen Parameter
    const QPoint empty(xOf(45), yOf(50));
    mouse(tv, QEvent::MouseButtonPress, empty, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, empty, Qt::NoButton);
    mouse(tv, QEvent::MouseButtonDblClick, empty, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, empty, Qt::NoButton);
    k = opacity();
    CHECK_EQ(k.size(), 3);
    CHECK(k.size() == 3 && k[1].frame == 45 && std::abs(k[1].value - 50) < 1.0);
    CHECK(sel.keyTimes() == QSet<int>{45});
    editor.deleteSelection();
    CHECK_EQ(opacity().size(), 2);
    CHECK_EQ(TimelineOps::findClip(project.timeline(), 1)->keys.value(AnimParam::PosX).size(), 1);

    // Griff ziehen: Punkt 10 auswählen, Aus-Griff nach oben -> Bezier, Wert in der Mitte steigt
    const double mid = Keys::valueAt(*TimelineOps::findClip(project.timeline(), 1), AnimParam::Opacity, 30);
    mouse(tv, QEvent::MouseButtonPress, {xOf(10), yOf(20)}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {xOf(10), yOf(20)}, Qt::NoButton);
    double dt, dv;
    CHECK(Keys::handle(opacity(), 0, true, &dt, &dv));
    const QPoint grip(xOf(10 + dt), yOf(20 + dv));
    drag(tv, grip, grip + QPoint(0, -30));
    k = opacity();
    CHECK(k[0].ease == KeyEase::Bezier);
    CHECK(k[0].outDv > dv + 5);
    CHECK(Keys::valueAt(*TimelineOps::findClip(project.timeline(), 1), AnimParam::Opacity, 30) > mid + 3);

    // Zuklappen: Spur wieder so hoch wie vorher
    tv.setCurveEditor({1}, false);
    CHECK(!tv.curveEditorShown(1));
    CHECK_EQ(tv.contentHeight(), h0);

    // Keyframe-Spur (Raute unten rechts im Clip aufklappen): Rauten rasten beim Ziehen am Playhead ein
    const QPoint icon(xOf(100) - 1 - 7, 30 + 64 - 3 - 6);
    mouse(tv, QEvent::MouseButtonPress, icon, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, icon, Qt::NoButton);
    CHECK_EQ(tv.contentHeight(), h0 + 16);
    tv.setPlayhead(33);
    const int laneY = 30 + 80 - 16 - 2 + 7;
    drag(tv, {xOf(10), laneY}, {xOf(31) + 1, laneY});
    CHECK_EQ(opacity().first().frame, 33);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("curves");
    I18n::install("de");
    Theme::apply(app);
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testValues();
    testAutoHandles();
    testSplit();
    testProjectFile(tmp.path());
    testSpeed();
    testUi();
    return Check::result();
}
