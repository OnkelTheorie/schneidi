// Multicam viewer (docs/archiv/plan-0.8.md): grid layout (auto 1x1/2x2/3x3/4x4, fixed, pages, cells tile the area, hit
// test), the grid widget (click while playing = cut, while stopped = switch, Alt+click = cut, number keys, modes
// video+audio / video / audio, pages, the grid follows the active angle's page, red frame on the active angle,
// nothing to do without a multicam clip, the proxy hint above many cells), the engine (a grid rebuilt while playing
// goes on where playback is) and the main window (multicam mode, keys 1–9 / Alt+1–9 only there, Alt+N = target track
// otherwise, flatten action). Everything offscreen.
#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/TimelineBuilder.h"
#include "ui/MulticamGrid.h"
#include "ui/MulticamView.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>

namespace {

using Mode = Multicam::Mode;

MediaInfo media(const QString& path, int length, bool video = true, bool audio = true)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

QString mdump(const Timeline& tl, TrackKind kind = TrackKind::Video)
{
    QString s;
    for (const Clip& c : tl.tracks(kind).value(0).clips)
        s += QString("%1M%2[%3-%4]").arg(s.isEmpty() ? "" : " ").arg(c.angle + 1).arg(c.start).arg(c.end());
    return s;
}

// ---- Layout ----

void testGridLayout()
{
    using namespace MulticamGrid;
    CHECK_EQ(columns(0), 1);
    CHECK_EQ(columns(1), 1);
    CHECK_EQ(columns(2), 2);
    CHECK_EQ(columns(4), 2);
    CHECK_EQ(columns(5), 3);
    CHECK_EQ(columns(9), 3);
    CHECK_EQ(columns(10), 4); // 5v5 tournament: all ten players on one page
    CHECK_EQ(columns(16), 4);
    CHECK_EQ(columns(17), 4); // more than 16: pages of 4x4
    CHECK_EQ(pages(16), 1);
    CHECK_EQ(pages(17), 2);
    CHECK_EQ(pages(0), 1);
    CHECK_EQ(perPage(10), 16);
    // Fixed layout
    CHECK_EQ(columns(10, 2), 2);
    CHECK_EQ(perPage(10, 2), 4);
    CHECK_EQ(pages(10, 2), 3);
    CHECK_EQ(pages(3, 3), 1);
    CHECK_EQ(columns(3, 9), 4); // out of range -> 4x4 at most
    CHECK_EQ(columns(3, 1), 1);
}

void testCellsTileTheArea()
{
    // Odd sizes: cells cover the area exactly, no overlap, sizes differ by at most one pixel
    for (const QRect area : {QRect(0, 0, 320, 180), QRect(7, 3, 641, 359), QRect(0, 0, 100, 100)})
        for (int n : {1, 3, 7, 12, 16}) {
            const int per = MulticamGrid::perPage(n);
            QRegion all;
            int pixels = 0, minW = 1 << 20, maxW = 0;
            for (int i = 0; i < per; ++i) {
                const QRect c = MulticamGrid::cell(area, n, i);
                pixels += c.width() * c.height();
                all += c;
                minW = std::min(minW, c.width());
                maxW = std::max(maxW, c.width());
            }
            CHECK(all == QRegion(area));
            CHECK_EQ(pixels, area.width() * area.height());
            CHECK(maxW - minW <= 1);
        }
    // Order: left to right, then top to bottom
    const QRect a(0, 0, 300, 300);
    CHECK_EQ(MulticamGrid::cell(a, 9, 0), QRect(0, 0, 100, 100));
    CHECK_EQ(MulticamGrid::cell(a, 9, 1), QRect(100, 0, 100, 100));
    CHECK_EQ(MulticamGrid::cell(a, 9, 3), QRect(0, 100, 100, 100));
    CHECK(MulticamGrid::cell(a, 9, 9).isNull()); // beyond the page
}

void testAngleAtHitTest()
{
    const QRect a(10, 10, 400, 400);
    // 10 angles: 4x4, 100 px cells
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(60, 60)), 0);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(160, 60)), 1);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(60, 160)), 4);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(160, 260)), 9);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(260, 260)), -1); // empty cell
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(5, 60)), -1);    // outside
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 0, QPoint(410, 60)), -1);
    // Fixed 2x2, page 3 of 10 angles: cells are angles 8 and 9
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 2, QPoint(60, 60), 2), 8);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 2, QPoint(260, 60), 2), 9);
    CHECK_EQ(MulticamGrid::angleAt(a, 10, 2, QPoint(60, 260), 2), -1);
    CHECK_EQ(MulticamGrid::angleAt(a, 0, 0, QPoint(60, 60)), -1);
}

void testFitKeepsAspect()
{
    CHECK_EQ(MulticamGrid::fit(QRect(0, 0, 160, 90), QSize(1920, 1080)), QRect(0, 0, 160, 90));
    CHECK_EQ(MulticamGrid::fit(QRect(0, 0, 160, 90), QSize(1440, 1080)), QRect(20, 0, 120, 90));  // 4:3 pillarbox
    CHECK_EQ(MulticamGrid::fit(QRect(0, 0, 160, 90), QSize(1080, 1920)).height(), 90);           // phone: tall
    CHECK_EQ(MulticamGrid::fit(QRect(0, 0, 160, 90), QSize(1920, 800)), QRect(0, 11, 160, 67));  // scope: letterbox
    CHECK(MulticamGrid::fit(QRect(0, 0, 160, 90), QSize()).isNull());
}

// ---- Widget ----

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    MulticamView view{&p, &sel, &ed};
    int seq = 0;
    explicit Fixture(int angles = 3)
    {
        QVector<Multicam::Angle> list;
        for (int i = 0; i < angles; ++i) {
            const QString path = QString("/x/pov%1.mp4").arg(i + 1);
            p.addMedia(media(path, 1000));
            list << Multicam::Angle{QString("P%1").arg(i + 1), {{path, 0}}};
        }
        seq = ed.createMulticamClip("Match", list);
        ed.addSequenceAt(seq, 0, 0);
        view.resize(640, 400);
        view.show();
        view.setPosition(100);
    }
    QPoint centerOf(int angle) const
    {
        const int per = MulticamGrid::perPage(view.angleCount(), view.columns());
        return MulticamGrid::cell(view.gridRect(), view.angleCount(), angle % per, view.columns()).center();
    }
    void click(int angle, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QTest::mouseClick(&view, Qt::LeftButton, mods, centerOf(angle));
    }
};

void testFollowsMulticamClipAtPlayhead()
{
    Fixture f;
    CHECK(f.view.currentClip() && f.view.currentClip()->sequenceId == f.seq);
    CHECK_EQ(f.view.angleCount(), 3);
    CHECK_EQ(f.view.activeAngle(), 0);
    CHECK(f.view.message().isEmpty());
    f.ed.multicamCut(300, 2, Mode::Both);
    f.view.setPosition(400);
    CHECK_EQ(f.view.activeAngle(), 2);
    // Audio mode shows the angle of the sound part
    f.ed.multicamCut(500, 1, Mode::Video);
    f.view.setPosition(600);
    CHECK_EQ(f.view.activeAngle(), 1);
    f.view.setMode(Mode::Audio);
    CHECK_EQ(f.view.activeAngle(), 2);
}

void testClickWhileStoppedSwitchesAngle()
{
    Fixture f;
    f.view.setPlaying(false);
    const int steps = f.p.undoStack()->count();
    f.click(1);
    CHECK_EQ(f.p.undoStack()->count(), steps + 1);
    CHECK_EQ(mdump(f.p.timeline()), QString("M2[0-1000]")); // whole segment, no cut
    CHECK_EQ(mdump(f.p.timeline(), TrackKind::Audio), QString("M2[0-1000]"));
    f.click(1); // already: nothing
    CHECK_EQ(f.p.undoStack()->count(), steps + 1);
}

void testClickWhilePlayingCutsAtFrame()
{
    Fixture f;
    f.view.setPlaying(true);
    f.view.setPosition(250);
    f.click(2);
    f.view.setPosition(400);
    f.click(1);
    CHECK_EQ(mdump(f.p.timeline()), QString("M1[0-250] M3[250-400] M2[400-1000]"));
    CHECK_EQ(mdump(f.p.timeline(), TrackKind::Audio), QString("M1[0-250] M3[250-400] M2[400-1000]"));
}

void testAltClickWhileStoppedCuts()
{
    Fixture f;
    f.view.setPlaying(false);
    f.view.setPosition(300);
    f.click(2, Qt::AltModifier);
    CHECK_EQ(mdump(f.p.timeline()), QString("M1[0-300] M3[300-1000]"));
}

void testModeButtons()
{
    Fixture f;
    auto* both = f.view.findChild<QToolButton*>("multicam_mode_both");
    auto* video = f.view.findChild<QToolButton*>("multicam_mode_video");
    auto* audio = f.view.findChild<QToolButton*>("multicam_mode_audio");
    if (!CHECK(both && video && audio)) return;
    CHECK(f.view.mode() == Mode::Both && both->isChecked());
    video->click();
    CHECK(f.view.mode() == Mode::Video && video->isChecked() && !both->isChecked());
    f.view.setPlaying(true);
    f.view.setPosition(200);
    f.click(1);
    CHECK_EQ(mdump(f.p.timeline()), QString("M1[0-200] M2[200-1000]"));
    CHECK_EQ(mdump(f.p.timeline(), TrackKind::Audio), QString("M1[0-200] M1[200-1000]"));
    audio->click();
    CHECK(f.view.mode() == Mode::Audio);
    f.view.setPosition(500);
    f.click(2);
    CHECK_EQ(mdump(f.p.timeline(), TrackKind::Audio), QString("M1[0-200] M1[200-500] M3[500-1000]"));
}

void testCutAndSwitchSlots()
{
    // What the number keys call (absolute angle, 0-based); invalid angles do nothing
    Fixture f;
    f.view.setPosition(200);
    f.view.cutToAngle(1);
    f.view.setPosition(600);
    f.view.cutToAngle(2);
    f.view.setPosition(300);
    f.view.switchToAngle(0);
    CHECK_EQ(mdump(f.p.timeline()), QString("M1[0-200] M1[200-600] M3[600-1000]"));
    const int steps = f.p.undoStack()->count();
    f.view.cutToAngle(3);
    f.view.cutToAngle(-1);
    f.view.switchToAngle(9);
    CHECK_EQ(f.p.undoStack()->count(), steps);
}

void testNoMulticamClipAtPlayhead()
{
    Fixture f;
    f.view.setPosition(5000);
    CHECK(!f.view.currentClip());
    CHECK_EQ(f.view.angleCount(), 0);
    CHECK(!f.view.message().isEmpty());
    const int steps = f.p.undoStack()->count();
    QTest::mouseClick(&f.view, Qt::LeftButton, Qt::NoModifier, f.view.gridRect().center());
    f.view.cutToAngle(0);
    CHECK_EQ(f.p.undoStack()->count(), steps);
}

void testActiveAngleFramedAndNamed()
{
    Fixture f;
    f.view.setPlaying(false);
    QImage grid(640, 360, QImage::Format_RGB32);
    grid.fill(Qt::darkGray);
    f.view.setFrame(grid);
    f.ed.multicamSwitch(100, 1, Mode::Both);
    f.view.setPosition(100);
    const QImage shot = f.view.grab().toImage();
    const QRect active = MulticamGrid::cell(f.view.gridRect(), 3, 1);
    const QRect other = MulticamGrid::cell(f.view.gridRect(), 3, 0);
    CHECK_EQ(QColor(shot.pixel(active.center().x(), active.top() + 1)).rgb(), Theme::multicamActive.rgb());
    CHECK(QColor(shot.pixel(other.center().x(), other.top() + 1)).rgb() != Theme::multicamActive.rgb());
    // Angle names are drawn: the cells differ from the plain picture in their label corner
    int changed = 0;
    for (int y = other.top() + 4; y < other.top() + 24; ++y)
        for (int x = other.left() + 4; x < other.left() + 60; ++x) changed += QColor(shot.pixel(x, y)) != QColor(Qt::darkGray);
    CHECK(changed > 20);
    CHECK_EQ(f.view.angleName(1), QString("P2"));
    f.ed.renameMulticamAngle(f.seq, 1, "Theo");
    CHECK_EQ(f.view.angleName(1), QString("Theo"));
}

void testPagesAndLayoutChoice()
{
    Fixture f(10);
    auto* layout = f.view.findChild<QComboBox*>("multicam_layout");
    auto* next = f.view.findChild<QToolButton*>("multicam_page_next");
    auto* prev = f.view.findChild<QToolButton*>("multicam_page_prev");
    if (!CHECK(layout && next && prev)) return;
    CHECK_EQ(f.view.columns(), 0); // auto: 4x4, one page
    CHECK_EQ(f.view.pageCount(), 1);
    CHECK(!next->isEnabled() && !prev->isEnabled());
    layout->setCurrentIndex(1); // 2x2
    CHECK_EQ(f.view.columns(), 2);
    CHECK_EQ(f.view.pageCount(), 3);
    CHECK(next->isEnabled() && !prev->isEnabled());
    next->click();
    next->click();
    CHECK_EQ(f.view.page(), 2);
    CHECK(!next->isEnabled() && prev->isEnabled());
    // Clicking the first cell of page 3 = angle 9
    f.view.setPlaying(false);
    f.click(8);
    CHECK_EQ(f.view.activeAngle(), 8);
    // The active angle's page is shown when it changes elsewhere (key, menu)
    f.ed.multicamSwitch(100, 0, Mode::Both);
    f.view.setPosition(100);
    CHECK_EQ(f.view.page(), 0);
    // Fewer angles than the page: back to a valid page
    layout->setCurrentIndex(0);
    CHECK_EQ(f.view.page(), 0);
    CHECK_EQ(f.view.pageCount(), 1);
}

void testProxyHintForManyCells()
{
    Fixture f(10);
    auto* button = f.view.findChild<QToolButton*>("multicam_proxies");
    auto* layout = f.view.findChild<QComboBox*>("multicam_layout");
    if (!CHECK(button && layout)) return;
    QStringList files;
    for (int i = 1; i <= 10; ++i) files << QString("/x/pov%1.mp4").arg(i);
    CHECK_EQ(f.view.angleFiles(), files);
    CHECK(f.view.proxyHint().isEmpty()); // nothing fed yet = no proxies missing
    const QRect full = f.view.gridRect();

    // 10 cells decoded at once, two files without a proxy: hint + button, the grid makes room
    f.view.setProxyState({files[2], files[7]}, 0);
    CHECK(!f.view.proxyHint().isEmpty());
    CHECK(button->isVisibleTo(&f.view));
    CHECK(f.view.gridRect().top() > full.top());
    const QRect withHint = f.view.gridRect();
    QSignalSpy spy(&f.view, &MulticamView::proxiesRequested);
    button->click();
    if (CHECK_EQ(spy.size(), 1)) CHECK_EQ(spy[0][0].toStringList(), QStringList({files[2], files[7]}));

    // Being generated: hint without the button; done: no hint, full grid again
    f.view.setProxyState({}, 2);
    CHECK(f.view.proxyHint().contains("2"));
    CHECK(!button->isVisibleTo(&f.view));
    CHECK_EQ(f.view.gridRect(), withHint); // the hint keeps its height without the button
    f.view.setProxyState({}, 0);
    CHECK(f.view.proxyHint().isEmpty());
    CHECK_EQ(f.view.gridRect(), full);

    // 2x2 = four decoders: smooth without proxies, no hint (page 3 has only two cells)
    f.view.setProxyState({files[0]}, 0);
    CHECK(!f.view.proxyHint().isEmpty());
    layout->setCurrentIndex(1);
    CHECK(f.view.proxyHint().isEmpty());
    f.view.setPage(2);
    CHECK(f.view.proxyHint().isEmpty());
    layout->setCurrentIndex(0);
    CHECK(!f.view.proxyHint().isEmpty());

    // Four angles: never a hint
    Fixture four(4);
    four.view.setProxyState(four.view.angleFiles(), 0);
    CHECK_EQ(four.view.angleFiles().size(), 4);
    CHECK(four.view.proxyHint().isEmpty());
}

void testPageFollowRebuildsGrid()
{
    // Following the active angle to its page also changes the picture: the grid tractor has to follow (before, only
    // the names and the red frame moved to the new page while the picture stayed on the old one)
    Fixture f(10);
    f.view.setColumns(2);
    QSignalSpy spy(&f.view, &MulticamView::gridChanged);
    f.ed.multicamSwitch(100, 8, Mode::Both);
    f.view.setPosition(100);
    CHECK_EQ(f.view.page(), 2);
    CHECK_EQ(spy.count(), 1);
    spy.clear();
    f.ed.multicamSwitch(100, 0, Mode::Both);
    f.view.setPosition(100);
    CHECK_EQ(f.view.page(), 0);
    CHECK_EQ(spy.count(), 1);
    // Another angle of the same page: no rebuild
    spy.clear();
    f.ed.multicamSwitch(100, 1, Mode::Both);
    f.view.setPosition(100);
    CHECK_EQ(f.view.page(), 0);
    CHECK_EQ(spy.count(), 0);
}

void testGridRebuildWhilePlayingKeepsPosition(const QString& dir)
{
    // Building a grid takes a while (one decoder per angle) and the old grid plays on meanwhile: the new one starts
    // where playback is, not at the position asked for before building (the playhead jumped back)
    if (!Check::haveFfmpeg()) return;
    QStringList files;
    for (const char* color : {"red", "lime", "blue"})
        files << Check::makeMedia(dir + QString("/rb_%1.mp4").arg(color),
                                  {"-f", "lavfi", "-i", QString("color=%1:size=320x180:rate=25:duration=20").arg(color),
                                   "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!files.contains(QString()))) return;
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    QVector<Multicam::Angle> list;
    for (const QString& f : files) {
        p.addMedia(media(f, 500, true, false));
        list << Multicam::Angle{QFileInfo(f).baseName(), {{f, 0}}};
    }
    const int seq = ed.createMulticamClip("RGB", list);
    if (!CHECK(seq > 0)) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    engine.setFormat(p.format());
    const Timeline content = p.renderTimeline(seq);
    const QVector<MulticamAngle> angles = p.sequence(seq)->angles;
    MulticamGridOptions o;
    if (!CHECK(engine.showMulticam(content, angles, o, 0, 0))) return;
    engine.play();
    CHECK(QTest::qWaitFor([&] { return engine.position() >= 50; }, 10000));
    const int playing = engine.position();
    o.columns = 4; // another layout: rebuilt
    CHECK(engine.showMulticam(content, angles, o, 0, 10));
    CHECK(engine.position() >= playing);
    engine.pause();
}

// ---- Main window ----

void testMainWindowMulticamMode(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "color=red:size=320x180:rate=25:duration=8",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    const QString b = Check::makeMedia(dir + "/b.mp4", {"-f", "lavfi", "-i", "color=lime:size=320x180:rate=25:duration=8",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!a.isEmpty() && !b.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({a, b}, false);
    auto* ed = w.findChild<Editor*>();
    auto* p = w.findChild<Project*>();
    auto* sel = w.findChild<Selection*>();
    if (!CHECK(ed && p && sel)) return;
    const int seq = ed->createMulticamClip("AB", {{"A", {{a, 0}}}, {"B", {{b, 0}}}});
    if (!CHECK(seq > 0)) return;
    ed->addSequenceAt(seq, 0, 0);

    InputBindings& in = InputBindings::instance();
    auto action = [&](const QString& id) { return in.action(id); };
    // Registered with DaVinci's keys (customizable like every action)
    for (int n = 1; n <= 9; ++n) {
        QKeySequence cut, sw;
        for (const auto& e : in.entries()) {
            if (e.id == QString("multicam_cut_%1").arg(n)) cut = e.defaultKey;
            if (e.id == QString("multicam_switch_%1").arg(n)) sw = e.defaultKey;
        }
        CHECK_EQ(cut, QKeySequence(QString::number(n)));
        CHECK_EQ(sw, QKeySequence(QString("Alt+%1").arg(n)));
    }
    QAction* mode = action("viewer_multicam");
    if (!CHECK(mode && action("multicam_cut_2") && action("multicam_switch_1") && action("target_video_2")
               && action("multicam_flatten")))
        return;
    // Outside the multicam view: number keys do nothing, Alt+N is the video target track
    CHECK(mode->isCheckable() && !mode->isChecked());
    CHECK(!action("multicam_cut_2")->isEnabled() && !action("multicam_switch_1")->isEnabled());
    CHECK(action("target_video_2")->isEnabled());
    auto* view = w.findChild<MulticamView*>();
    CHECK(!view || !view->isVisible());

    mode->trigger();
    view = w.findChild<MulticamView*>();
    CHECK(view && view->isVisible());
    CHECK_EQ(view->angleFiles(), QStringList({a, b}));
    CHECK(view->proxyHint().isEmpty()); // two angles play smoothly from the originals
    CHECK(action("multicam_cut_2")->isEnabled() && action("multicam_switch_1")->isEnabled());
    CHECK(!action("target_video_2")->isEnabled()); // otherwise Alt+2 would be ambiguous
    engine.seek(50);
    QApplication::processEvents();
    action("multicam_cut_2")->trigger();
    CHECK_EQ(mdump(p->timeline()), QString("M1[0-50] M2[50-200]"));
    engine.seek(20);
    QApplication::processEvents();
    action("multicam_switch_2")->trigger();
    CHECK_EQ(mdump(p->timeline()), QString("M2[0-50] M2[50-200]"));
    action("multicam_cut_9")->trigger(); // no angle 9: nothing
    CHECK_EQ(mdump(p->timeline()), QString("M2[0-50] M2[50-200]"));

    // Flatten the selection (Timeline → Multicam menu)
    sel->set({p->timeline().video[0].clips.value(0).id});
    action("multicam_flatten")->trigger();
    CHECK(!p->timeline().video[0].clips.value(0).isMulticam());
    CHECK(p->timeline().video[0].clips.value(1).isMulticam());

    // Leaving the mode gives Alt+N back to the target tracks
    mode->trigger();
    CHECK(!mode->isChecked());
    CHECK(!action("multicam_cut_2")->isEnabled());
    CHECK(action("target_video_2")->isEnabled());
    action("target_video_2")->trigger();
    CHECK_EQ(ed->targetVideoTrack(), 1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("multicam-viewer");
    QTemporaryDir dir;

    testGridLayout();
    testCellsTileTheArea();
    testAngleAtHitTest();
    testFitKeepsAspect();
    testFollowsMulticamClipAtPlayhead();
    testClickWhileStoppedSwitchesAngle();
    testClickWhilePlayingCutsAtFrame();
    testAltClickWhileStoppedCuts();
    testModeButtons();
    testCutAndSwitchSlots();
    testNoMulticamClipAtPlayhead();
    testActiveAngleFramedAndNamed();
    testPagesAndLayoutChoice();
    testProxyHintForManyCells();
    testPageFollowRebuildsGrid();
    testGridRebuildWhilePlayingKeepsPosition(dir.path());
    testMainWindowMulticamMode(dir.path());
    return Check::result();
}
