// SPDX-License-Identifier: AGPL-3.0-only
// The preview resolution the user chooses (engine/PreviewResolution; Workspace › Preview Resolution, the viewer's
// right-click, Settings): automatic / half / full, separately for the main window and the second monitor. The choice
// -> Engine::previewSize; stored; the larger wish wins (one preview); the main window's wish counts only while its
// viewer is there.
// PREVIEW_RES_BENCH=1: rough ms per preview frame of a 4K clip at 1920 vs. 3840 wide (a busy PC varies ±15 %).

#include "check.h"

#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "engine/Engine.h"
#include "engine/PreviewResolution.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMenu>
#include <QScreen>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <climits>
#include <clocale>

namespace {

using PreviewResolution::Choice;
using PreviewResolution::Target;

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

void testEdge()
{
    const QSize uhd(3840, 2160), hd(1920, 1080), portrait(1080, 1920);
    CHECK_EQ(PreviewResolution::edge(Choice::Auto, 0, uhd), 0);
    CHECK_EQ(PreviewResolution::edge(Choice::Auto, 2560, uhd), 2560);
    CHECK_EQ(PreviewResolution::edge(Choice::Half, 2560, uhd), 1920);
    CHECK_EQ(PreviewResolution::edge(Choice::Half, 0, hd), 960);
    CHECK_EQ(PreviewResolution::edge(Choice::Half, 0, portrait), 960);
    CHECK_EQ(PreviewResolution::edge(Choice::Full, 0, hd), INT_MAX);
    // Stored per target, automatic when nothing is stored
    for (const Target t : {Target::Viewer, Target::Monitor}) {
        QSettings().remove(PreviewResolution::settingKey(t));
        CHECK(PreviewResolution::stored(t) == Choice::Auto);
        PreviewResolution::store(t, Choice::Full);
        CHECK(PreviewResolution::stored(t) == Choice::Full);
        PreviewResolution::store(t, Choice::Half);
        CHECK(PreviewResolution::stored(t) == Choice::Half);
    }
    CHECK(QSettings().value("viewer/previewResolution").toString() == "half");
    PreviewResolution::store(Target::Viewer, Choice::Auto);
    PreviewResolution::store(Target::Monitor, Choice::Auto);
    CHECK(QSettings().value("viewer/monitorResolution").toString() == "auto");
}

void bench(Engine& engine)
{
    if (qEnvironmentVariable("PREVIEW_RES_BENCH").isEmpty()) return;
    for (const int edge : {1920, INT_MAX}) {
        engine.setPreviewEdge(edge);
        settle();
        QSignalSpy spy(&engine, &Engine::frameReady);
        QElapsedTimer t;
        t.start();
        constexpr int kFrames = 40;
        for (int i = 0; i < kFrames; ++i) {
            const int before = int(spy.count());
            engine.seek((i * 7) % 45);
            for (int w = 0; w < 5000 && spy.count() == before; w += 1) QTest::qWait(1);
        }
        std::printf("preview %dx%d: %.1f ms per frame (seek + render)\n", engine.previewSize().width(),
                    engine.previewSize().height(), double(t.elapsed()) / kFrames);
        // Playback: pictures per second shown (25 = all of them)
        engine.seek(0);
        settle();
        spy.clear();
        engine.setSpeed(1.0);
        QTest::qWait(1500);
        engine.pause();
        std::printf("preview %dx%d: playback %.1f of 25 pictures/s\n", engine.previewSize().width(),
                    engine.previewSize().height(), spy.count() / 1.5);
    }
    engine.setPreviewEdge(0);
}

void testApp(MainWindow& w, Engine& engine)
{
    const QSize project = engine.frameSize();
    if (!CHECK_EQ(project, QSize(3840, 2160))) return; // the 4K clip set the format
    // Automatic: the viewer's default (longer edge 960)
    CHECK(w.previewResolution(Target::Viewer) == Choice::Auto);
    CHECK_EQ(engine.previewSize().width(), 960);
    w.setPreviewResolution(Target::Viewer, Choice::Half);
    CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
    w.setPreviewResolution(Target::Viewer, Choice::Full);
    CHECK_EQ(engine.previewSize(), project);
    // The menu shows the choice and sets it
    if (QMenu* menu = w.findChild<QMenu*>("preview_resolution_menu"); CHECK(menu)) {
        QList<QAction*> subs = menu->actions();
        if (CHECK_EQ(subs.size(), 2)) {
            const QList<QAction*> viewerChoices = subs[0]->menu()->actions();
            if (CHECK_EQ(viewerChoices.size(), 3)) {
                CHECK(viewerChoices[2]->isChecked()); // full
                viewerChoices[1]->trigger();
                CHECK(w.previewResolution(Target::Viewer) == Choice::Half);
                CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
                CHECK(viewerChoices[1]->isChecked());
            }
        }
    }
    w.setPreviewResolution(Target::Viewer, Choice::Auto);
    CHECK_EQ(engine.previewSize().width(), 960);

    // The second monitor: automatic = its pixels (no 1920 cap: ScreenOutput); full = the project; the larger wins
    w.showPreviewOn(QGuiApplication::primaryScreen());
    settle();
    if (!CHECK(w.previewOutput())) return;
    const int wanted = w.previewOutput()->wantedPreviewEdge();
    CHECK(wanted > 0);
    CHECK_EQ(engine.previewEdge(), wanted);
    w.setPreviewResolution(Target::Monitor, Choice::Full);
    CHECK_EQ(engine.previewSize(), project);
    w.setPreviewResolution(Target::Monitor, Choice::Half);
    CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
    w.setPreviewResolution(Target::Viewer, Choice::Full); // the main window's wish is larger
    CHECK_EQ(engine.previewSize(), project);
    // "Hide the preview area": the main window shows nothing, its wish does not count
    w.setSecondScreenMode(SecondScreenPlan::Mode::HideViewer);
    settle();
    CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
    w.setSecondScreenMode(SecondScreenPlan::Mode::KeepArea);
    w.showPreviewOn(nullptr);
    settle();
    CHECK_EQ(engine.previewSize(), project); // the viewer's full choice alone
    w.setPreviewResolution(Target::Viewer, Choice::Auto);
    w.setPreviewResolution(Target::Monitor, Choice::Auto);
    CHECK_EQ(engine.previewEdge(), 0);

    // Settings: both choices there, automatic first
    {
        SettingsDialog dlg(AiSettings{});
        for (const char* name : {"viewer_resolution", "monitor_resolution"}) {
            auto* combo = dlg.findChild<QComboBox*>(name);
            if (!CHECK(combo)) continue;
            CHECK_EQ(combo->count(), 3);
            CHECK_EQ(combo->itemData(0).toInt(), int(Choice::Auto));
        }
        dlg.findChild<QComboBox*>("monitor_resolution")->setCurrentIndex(2);
        CHECK(dlg.previewResolution(Target::Monitor) == Choice::Full);
        CHECK(dlg.previewResolution(Target::Viewer) == Choice::Auto);
    }
    bench(engine);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("preview-resolution");
    testEdge();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    const QString file = Check::makeMedia(dir.path() + "/uhd.mp4",
        {"-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=25:duration=2", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p"});
    if (!CHECK(!file.isEmpty())) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    CHECK(QTest::qWaitForWindowExposed(&w));
    w.importFiles({file}, true);
    settle();
    QTest::qWait(300);
    testApp(w, engine);
    return Check::result();
}
