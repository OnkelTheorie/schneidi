// SPDX-License-Identifier: AGPL-3.0-only
// The preview resolution the user chooses (engine/PreviewResolution; Workspace › Preview Resolution, the viewer's
// right-click, Settings): automatic / 360p … 2160p / the project's, separately for the main window and the second
// monitor. Checked where it counts: the size of the frame the viewer's picture widget gets (FrameView::frame) for
// each choice, a 1080p and a 4K project; the picture with 1-px lines looks different at 360p and at the project size;
// the notice over the picture; menu (disabled above the project, tooltips with the size); stored, old values
// migrated; the larger wish of main window and monitor wins (one preview).
// Runs on the CPU path (test_preview_resolution) and with the GPU deciding by itself (test_preview_resolution_gpu:
// checks that the card composited the frames, says so when there is none).
// PREVIEW_RES_SHOTS=<dir>: saves the viewer at 360p and at the project size as PNGs.
// PREVIEW_RES_BENCH=1: rough ms per preview frame of the 4K clip at 1920 vs. 3840 wide (a busy PC varies ±15 %).

#include "check.h"

#include "app/MainWindow.h"
#include "app/SettingsDialog.h"
#include "engine/Engine.h"
#include "engine/PreviewResolution.h"
#include "ui/FrameView.h"
#include "ui/PreviewOutput.h"
#include "ui/Viewer.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMenu>
#include <QScreen>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <climits>
#include <clocale>
#include <cstdlib>

namespace {

using PreviewResolution::Choice;
using PreviewResolution::Target;

bool gpuWanted()
{
    return qgetenv("SCHNEIDI_GPU") != "0";
}

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

void testRules()
{
    using namespace PreviewResolution;
    const QSize uhd(3840, 2160), hd(1920, 1080), portrait(1080, 1920);
    // Fixed sizes: the shorter edge, the longer follows the aspect
    CHECK_EQ(resultSize(Choice::P360, 0, hd), QSize(640, 360));
    CHECK_EQ(resultSize(Choice::P540, 0, hd), QSize(960, 540));
    CHECK_EQ(resultSize(Choice::P720, 0, hd), QSize(1280, 720));
    CHECK_EQ(resultSize(Choice::P1080, 0, hd), hd);
    CHECK_EQ(resultSize(Choice::P1080, 0, portrait), portrait);
    CHECK_EQ(resultSize(Choice::P720, 0, portrait), QSize(720, 1280));
    CHECK_EQ(resultSize(Choice::P1440, 0, uhd), QSize(2560, 1440));
    CHECK_EQ(resultSize(Choice::P2160, 0, uhd), uhd);
    CHECK_EQ(resultSize(Choice::Project, 0, uhd), uhd);
    // Never larger than the project; those are flagged for the menu
    CHECK_EQ(resultSize(Choice::P2160, 0, hd), hd);
    CHECK(largerThanProject(Choice::P1440, hd));
    CHECK(!largerThanProject(Choice::P1080, hd));
    CHECK(!largerThanProject(Choice::P1080, portrait));
    CHECK(!largerThanProject(Choice::Project, hd));
    // Automatic: what the display asks for, the engine's default without
    CHECK_EQ(edge(Choice::Auto, 0, uhd), 0);
    CHECK_EQ(resultSize(Choice::Auto, 0, hd), QSize(960, 540));
    CHECK_EQ(resultSize(Choice::Auto, 704, hd), QSize(704, 396));
    CHECK_EQ(edge(Choice::Project, 0, hd), INT_MAX);
    // Stored per target, automatic when nothing is stored
    for (const Target t : {Target::Viewer, Target::Monitor}) {
        QSettings().remove(settingKey(t));
        CHECK(stored(t) == Choice::Auto);
        for (const Choice c : kChoices) {
            store(t, c);
            CHECK(stored(t, uhd) == c);
        }
    }
    CHECK(QSettings().value("viewer/monitorResolution").toString() == "project");
    store(Target::Viewer, Choice::P720);
    CHECK(QSettings().value("viewer/previewResolution").toString() == "720");
    // Values of 0.20's first version: full = the project, half = the fixed size nearest to half the project
    CHECK(fromStored("full", hd) == Choice::Project);
    CHECK(fromStored("half", hd) == Choice::P540);
    CHECK(fromStored("half", uhd) == Choice::P1080);
    CHECK(fromStored("half", QSize(2560, 1440)) == Choice::P720);
    CHECK(fromStored("half", portrait) == Choice::P540);
    CHECK(fromStored("nonsense", hd) == Choice::Auto);
    store(Target::Viewer, Choice::Auto);
    store(Target::Monitor, Choice::Auto);
}

// A clip of a grid of 1-px dark lines (every third pixel): what a low preview loses
QString makePattern(const QString& path, QSize size)
{
    return Check::makeMedia(path,
        {"-f", "lavfi", "-i",
         QString("nullsrc=size=%1x%2:rate=25:duration=1,format=gray,"
                 "geq=lum='if(lt(mod(X\\,3)\\,1)+lt(mod(Y\\,3)\\,1)\\,20\\,235)'")
             .arg(size.width()).arg(size.height()),
         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
}

// Waits until the viewer's picture widget got a frame of `size` (what the engine hands over, on whichever path)
bool waitForFrame(FrameView* view, const QSize& size)
{
    for (int i = 0; i < 400 && view->frame().size() != size; ++i) QTest::qWait(10);
    return view->frame().size() == size;
}

// Mean brightness difference of two screenshots of the viewer in the middle of the picture (below the notice)
double difference(const QImage& a, const QImage& b)
{
    const QImage ga = a.convertToFormat(QImage::Format_Grayscale8), gb = b.convertToFormat(QImage::Format_Grayscale8);
    if (ga.size() != gb.size()) return 255;
    const QRect r(ga.width() / 4, ga.height() / 4, ga.width() / 2, ga.height() / 2);
    double sum = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) sum += std::abs(ga.constScanLine(y)[x] - gb.constScanLine(y)[x]);
    return sum / (double(r.width()) * r.height());
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
            engine.seek((i * 7) % 20);
            for (int w = 0; w < 5000 && spy.count() == before; w += 1) QTest::qWait(1);
        }
        std::printf("preview %dx%d: %.1f ms per frame (seek + render)\n", engine.previewSize().width(),
                    engine.previewSize().height(), double(t.elapsed()) / kFrames);
    }
}

// Every choice of the main window: the frame the viewer gets, the notice, the menu
void testViewerChoices(MainWindow& w, Engine& engine, const QSize& project, const QString& shots)
{
    using namespace PreviewResolution;
    Viewer* viewer = w.findChild<Viewer*>();
    FrameView* view = viewer ? viewer->screen() : nullptr;
    if (!CHECK(view)) return;
    const char* name = project.width() > 1920 ? "4k" : "1080p";
    std::printf("%s project, viewer picture widget %dx%d (%s)\n", name, view->width(), view->height(),
                view->backend() == FrameView::Backend::Rhi ? "GPU" : "painter");
    // Automatic: the pixels the picture covers in the viewer, a step of 64 up
    CHECK(w.previewResolution(Target::Viewer) == Choice::Auto);
    const int autoEdge = viewer->wantedPreviewEdge();
    CHECK(autoEdge > 0 && autoEdge <= kAutoCap && autoEdge % 64 == 0);
    const QSize fitted = project.scaled(view->size() * view->devicePixelRatioF(), Qt::KeepAspectRatio);
    CHECK(autoEdge >= fitted.width() && autoEdge < fitted.width() + 64);
    const QSize autoSize = sizeFor(project, autoEdge);
    CHECK_EQ(engine.previewSize(), autoSize);
    CHECK(waitForFrame(view, autoSize));

    const QSize expected[] = {autoSize,
                              QSize(640, 360),
                              QSize(960, 540),
                              QSize(1280, 720),
                              QSize(1920, 1080),
                              project.width() > 1920 ? QSize(2560, 1440) : project,
                              project,
                              project};
    QMenu* menu = w.findChild<QMenu*>("preview_resolution_menu");
    QList<QAction*> entries;
    if (CHECK(menu) && CHECK_EQ(menu->actions().size(), 2)) entries = menu->actions()[0]->menu()->actions();
    CHECK_EQ(entries.size(), kChoiceCount);
    QImage low, full;
    for (const Choice c : kChoices) {
        const bool larger = largerThanProject(c, project);
        if (int(c) < entries.size()) {
            QAction* a = entries[int(c)];
            CHECK_EQ(a->isEnabled(), !larger);
            if (larger) CHECK_EQ(a->text(), T("%1 (größer als das Projekt)").arg(label(c)));
            else if (c != Choice::Auto) CHECK(a->toolTip().contains(sizeText(expected[int(c)])));
            if (c == Choice::Auto) CHECK(a->toolTip().contains(sizeText(autoSize)));
            if (!larger) a->trigger(); // as the user does
            else w.setPreviewResolution(Target::Viewer, c); // a stored one stays usable: the project's size
        } else {
            w.setPreviewResolution(Target::Viewer, c);
        }
        CHECK(w.previewResolution(Target::Viewer) == c);
        const QSize want = expected[int(c)];
        CHECK_EQ(engine.previewSize(), want);
        const int gpuBefore = engine.timelineFramesOnGpu();
        if (!CHECK(waitForFrame(view, want)))
            std::printf("  %s: viewer got %dx%d, wanted %dx%d\n", qPrintable(label(c)), view->frame().width(),
                        view->frame().height(), want.width(), want.height());
        else
            std::printf("  %-34s -> viewer frame %dx%d\n", qPrintable(label(c)), want.width(), want.height());
        if (c != Choice::Auto) CHECK_EQ(viewer->notice(), T("Vorschau: %1").arg(sizeText(engine.previewSize())));
        // A seek renders anew: on the GPU run the card must have drawn it
        engine.seek((engine.position() + 1) % 20);
        QTest::qWait(150);
        if (gpuWanted() && engine.timelineFramesOnGpu() > 0) CHECK(engine.timelineFramesOnGpu() > gpuBefore);
        if (c == Choice::P360) low = view->grab().toImage();
        if (c == Choice::Project) full = view->grab().toImage();
    }
    // The notice goes after ~2 s; the same choice again shows none (nothing changed)
    QTest::qWait(2100);
    CHECK(viewer->notice().isEmpty());
    w.setPreviewResolution(Target::Viewer, Choice::Project);
    CHECK(viewer->notice().isEmpty());

    // What the eye sees: 360p loses the 1-px lines, the project's size shows them (scaled into the viewer)
    if (CHECK(!low.isNull() && !full.isNull())) {
        const double diff = difference(low, full);
        std::printf("  viewer screenshots 360p vs project: mean difference %.1f of 255\n", diff);
        CHECK(diff > 20);
        if (!shots.isEmpty()) {
            low.save(QDir(shots).filePath(QString("viewer-%1-360p.png").arg(name)));
            full.save(QDir(shots).filePath(QString("viewer-%1-project.png").arg(name)));
        }
    }

    // Automatic follows the viewer's size (a larger window, a bigger picture)
    w.setPreviewResolution(Target::Viewer, Choice::Auto);
    const int before = viewer->wantedPreviewEdge();
    w.resize(w.width() + 400, w.height() + 250);
    QTest::qWait(400);
    const int after = viewer->wantedPreviewEdge();
    std::printf("  automatic: longer edge %d -> %d after the window grew\n", before, after);
    if (after != before) {
        CHECK_EQ(engine.previewSize(), sizeFor(project, after));
        CHECK(waitForFrame(view, sizeFor(project, after)));
    }
    w.resize(w.width() - 400, w.height() - 250);
    QTest::qWait(400);
}

void testMonitor(MainWindow& w, Engine& engine, const QSize& project)
{
    // The second monitor: automatic = its pixels (no cap: ScreenOutput); fixed sizes and the project; the larger wins
    w.setPreviewResolution(Target::Viewer, Choice::Auto);
    w.showPreviewOn(QGuiApplication::primaryScreen());
    settle();
    if (!CHECK(w.previewOutput())) return;
    const int wanted = w.previewOutput()->wantedPreviewEdge();
    CHECK(wanted > 0);
    CHECK_EQ(engine.previewEdge(), std::max(wanted, w.findChild<Viewer*>()->wantedPreviewEdge()));
    w.setPreviewResolution(Target::Monitor, Choice::Project);
    CHECK_EQ(engine.previewSize(), project);
    w.setPreviewResolution(Target::Monitor, Choice::P1080);
    CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
    w.setPreviewResolution(Target::Viewer, Choice::Project); // the main window's wish is larger
    CHECK_EQ(engine.previewSize(), project);
    // "Hide the preview area": the main window shows nothing, its wish does not count
    w.setSecondScreenMode(SecondScreenPlan::Mode::HideViewer);
    settle();
    CHECK_EQ(engine.previewSize(), QSize(1920, 1080));
    w.setSecondScreenMode(SecondScreenPlan::Mode::KeepArea);
    w.showPreviewOn(nullptr);
    settle();
    CHECK_EQ(engine.previewSize(), project); // the viewer's choice alone
    w.setPreviewResolution(Target::Viewer, Choice::Auto);
    w.setPreviewResolution(Target::Monitor, Choice::Auto);

    // Settings: every choice there, automatic first; above the project disabled
    SettingsDialog dlg(AiSettings{});
    dlg.setPreviewProject(project);
    for (const char* name : {"viewer_resolution", "monitor_resolution"}) {
        auto* combo = dlg.findChild<QComboBox*>(name);
        if (!CHECK(combo)) continue;
        CHECK_EQ(combo->count(), PreviewResolution::kChoiceCount);
        CHECK_EQ(combo->itemData(0).toInt(), int(Choice::Auto));
        auto* model = qobject_cast<QStandardItemModel*>(combo->model());
        if (CHECK(model)) {
            CHECK_EQ(model->item(int(Choice::P1440))->isEnabled(), project.height() >= 1440);
            CHECK(model->item(int(Choice::P720))->isEnabled());
        }
    }
    dlg.findChild<QComboBox*>("monitor_resolution")->setCurrentIndex(int(Choice::P720));
    CHECK(dlg.previewResolution(Target::Monitor) == Choice::P720);
    CHECK(dlg.previewResolution(Target::Viewer) == Choice::Auto);
}

void testProject(const QSize& size, const QString& dir, const QString& shots, bool doMonitor)
{
    const QString file = makePattern(dir + QString("/lines-%1.mp4").arg(size.height()), size);
    if (!CHECK(!file.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    CHECK(QTest::qWaitForWindowExposed(&w));
    w.importFiles({file}, true);
    settle();
    QTest::qWait(400);
    if (!CHECK_EQ(engine.frameSize(), size)) return; // the clip set the format
    testViewerChoices(w, engine, size, shots);
    if (doMonitor) testMonitor(w, engine, size);
    if (gpuWanted())
        std::printf("GPU run: %d timeline frames composited on the graphics card%s\n", engine.timelineFramesOnGpu(),
                    engine.timelineFramesOnGpu() == 0 ? " (no usable GPU here: CPU path checked only)" : "");
    if (size.width() > 1920) bench(engine);
}

} // namespace

int main(int argc, char** argv)
{
    // The CPU run is registered with SCHNEIDI_GPU=0; the _gpu run lets the GPU decide
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("preview-resolution");
    std::setlocale(LC_NUMERIC, "C");
    testRules();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    const QString shots = qEnvironmentVariable("PREVIEW_RES_SHOTS");
    if (!shots.isEmpty()) QDir().mkpath(shots);
    testProject(QSize(1920, 1080), dir.path(), shots, false);
    testProject(QSize(3840, 2160), dir.path(), shots, true);
    return Check::result();
}
