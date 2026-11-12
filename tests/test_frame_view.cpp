// SPDX-License-Identifier: AGPL-3.0-only
// FrameView (ui/FrameView): the viewer's picture widget.
// - Backend choice: painter when asked (SCHNEIDI_VIEWER=painter), without RHI in the build, on headless platforms, without
//   a device or on a software device (unless the GPU is forced); GPU otherwise. Offscreen = painter
// - Geometry: fit (whole pixels, centred), zoom 50/100/200 % of the project frame, panning clamped to the picture,
//   widget <-> frame mapping, the overlay gets the same frame rectangle in paint and mouse events
// - Mouse: middle drag pans, left drag pans only when zoomed in and nothing can be dragged out; Ctrl+wheel zooms
// - Drawing: bilinear below 200 %, sharp pixels from 200 %; the bars in the letterbox colour
// - Viewer: the zoom actions (Workspace › Viewer-Zoom, fit on Alt+0) set the zoom, 100 % asks the engine for full-size
//   frames, the zoom button shows the step
// FRAME_VIEW_DUMP=<folder>: pictures of the zoom states. FRAME_VIEW_BENCH=<n>: CPU time per displayed 1080p frame
// (FRAME_VIEW_POS=x,y places the window, e.g. on a second monitor; also run it under Xvfb with SCHNEIDI_VIEWER=gpu to measure the GPU path).

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "ui/FrameView.h"
#include "ui/Viewer.h"
#include "ui/ViewerOverlay.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QWheelEvent>
#include <clocale>
#include <ctime>

namespace {

QString dumpDir() { return qEnvironmentVariable("FRAME_VIEW_DUMP"); }

void dump(QWidget* w, const QString& name)
{
    if (!dumpDir().isEmpty()) w->grab().save(dumpDir() + "/" + name + ".png");
}

// Remembers the frame rectangle it was given
class RecordingOverlay : public ViewerOverlay {
public:
    using ViewerOverlay::ViewerOverlay;
    QRectF painted, pressed;
    QPointF pressPos;
    bool take = false;
    void paint(QPainter& p, const QRectF& frame) override
    {
        painted = frame;
        p.setPen(QPen(Qt::yellow, 2));
        p.drawRect(frame.adjusted(frame.width() * 0.1, frame.height() * 0.1, -frame.width() * 0.1, -frame.height() * 0.1));
    }
    bool mousePress(QMouseEvent* e, const QRectF& frame) override
    {
        pressed = frame;
        pressPos = e->position();
        return take;
    }
};

// Test card: 8x8 checker of 1-px cells in the top-left corner, colour bands elsewhere
QImage card(QSize size)
{
    QImage img(size, QImage::Format_RGBA8888);
    QPainter p(&img);
    const QColor bands[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue};
    for (int i = 0; i < 7; ++i) p.fillRect(QRectF(size.width() * i / 7.0, 0, size.width() / 7.0 + 1, size.height()), bands[i]);
    p.end();
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) img.setPixelColor(x, y, (x + y) % 2 ? Qt::black : Qt::white);
    return img;
}

void send(QWidget* w, QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pos, w->mapToGlobal(pos), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

// The viewer's API on Linux/BSD: the environment, then the setting (OpenGL after a Vulkan failure), else Vulkan on X11
void testApiChoice()
{
    CHECK(FrameView::preferVulkan("", "", "xcb", true));
    CHECK(!FrameView::preferVulkan("", "", "wayland", true)); // not measured there: OpenGL as before
    CHECK(!FrameView::preferVulkan("", "", "xcb", false));    // Qt or the build without Vulkan
    CHECK(!FrameView::preferVulkan("", "opengl", "xcb", true)); // Vulkan failed here once
    CHECK(FrameView::preferVulkan("vulkan", "opengl", "xcb", true)); // the environment wins
    CHECK(!FrameView::preferVulkan("OpenGL", "", "xcb", true));
    CHECK(FrameView::preferVulkan("", "vulkan", "wayland", true));
    CHECK(!FrameView::preferVulkan("vulkan", "", "xcb", false)); // never without it
}

// X11 start (FRAME_VIEW_PLATFORM=xcb under Xvfb, SCHNEIDI_VIEWER=gpu for its software device): views of the stored
// check paint with the painter until the shown window is switched to GPU composition in place, and end on the GPU -
// also one made between prepareWindow and the switch (it switches with the others instead of finding no QRhi)
void testDeferredAttach()
{
    if (QGuiApplication::platformName() != QLatin1String("xcb")) return;
    QSettings().setValue("viewer/gpuCheck", "1|0|Test GPU");
    FrameView::setUseCachedGpuCheck(true);
    FrameView::warmUpGpu();
    QWidget window;
    window.resize(640, 360);
    auto* first = new FrameView(&window);
    first->setGeometry(0, 0, 320, 360);
    if (!CHECK(first->surfaceDeferred())) return;
    FrameView::prepareWindow(&window);
    window.show();
    if (!CHECK(QTest::qWaitForWindowExposed(&window))) return;
    auto* later = new FrameView(&window);
    later->setGeometry(320, 0, 320, 360);
    later->show();
    CHECK(later->surfaceDeferred());
    CHECK(first->backend() == FrameView::Backend::Rhi);
    CHECK(!first->gpuSurfaceReady()); // the painter until the switch
    FrameView::attachDeferredSurfaces();
    for (FrameView* v : {first, later}) {
        CHECK(!v->surfaceDeferred());
        (void)QTest::qWaitFor([v] { return v->gpuSurfaceReady() || v->backend() != FrameView::Backend::Rhi; }, 10000);
        CHECK(v->backend() == FrameView::Backend::Rhi);
        CHECK(v->gpuSurfaceReady());
    }
    FrameView::checkCachedGpu(); // answered by the start's check or the window's QRhi
    CHECK(!QSettings().value("viewer/gpuCheck").toString().isEmpty());
    QSettings().remove("viewer/gpuCheck");
    FrameView::setUseCachedGpuCheck(false);
}

void testChoice()
{
    using B = FrameView::Backend;
    const auto hw = [] { return FrameView::Probe{true, false, "GPU"}; };
    const auto sw = [] { return FrameView::Probe{true, true, "llvmpipe"}; };
    const auto none = [] { return FrameView::Probe{}; };
    CHECK(FrameView::chooseBackend("", "", "xcb", true, hw) == B::Rhi);
    CHECK(FrameView::chooseBackend("", "", "windows", true, hw) == B::Rhi);
    CHECK(FrameView::chooseBackend("painter", "", "xcb", true, hw) == B::Painter);
    CHECK(FrameView::chooseBackend("", "painter", "xcb", true, hw) == B::Painter); // the setting
    CHECK(FrameView::chooseBackend("gpu", "painter", "xcb", true, hw) == B::Rhi);  // the environment wins
    CHECK(FrameView::chooseBackend("", "", "xcb", false, hw) == B::Painter);      // built without RHI
    CHECK(FrameView::chooseBackend("", "", "offscreen", true, hw) == B::Painter);
    CHECK(FrameView::chooseBackend("gpu", "", "offscreen", true, hw) == B::Painter); // no window system: never
    CHECK(FrameView::chooseBackend("", "", "xcb", true, none) == B::Painter);
    CHECK(FrameView::chooseBackend("", "", "xcb", true, nullptr) == B::Painter);
    QString why;
    CHECK(FrameView::chooseBackend("", "", "xcb", true, sw, &why) == B::Painter);
    CHECK(why.contains("llvmpipe"));
    CHECK(FrameView::chooseBackend("gpu", "", "xcb", true, sw) == B::Rhi); // forced for testing
    // The probe is not even tried when the answer is clear
    bool probed = false;
    FrameView::chooseBackend("painter", "", "xcb", true, [&] { probed = true; return FrameView::Probe{}; });
    FrameView::chooseBackend("", "", "offscreen", true, [&] { probed = true; return FrameView::Probe{}; });
    CHECK(!probed);

    // Here (offscreen tests): the painter, with the same API
    FrameView view;
    if (QGuiApplication::platformName() == "offscreen") CHECK(view.backend() == B::Painter);
    CHECK(!view.backendInfo().isEmpty());
}

void testGeometry()
{
    FrameView view;
    view.resize(960, 600);
    view.show();
    CHECK(view.frameRect().isEmpty()); // nothing to show
    view.setFrame(card({1920, 1080}));
    CHECK_EQ(view.frameSize(), QSize(1920, 1080));
    CHECK_EQ(view.frameRect(), QRectF(0, 30, 960, 540));
    CHECK(view.smoothScaling());

    // A smaller preview picture of the same project: the zoom refers to the project frame
    view.setFrameSize({3840, 2160});
    view.setFrame(card({960, 540}));
    CHECK_EQ(view.frameRect(), QRectF(0, 30, 960, 540));
    CHECK_EQ(view.widgetToFrame({480, 300}), QPointF(1920, 1080));
    CHECK_EQ(view.frameToWidget({3840, 2160}), QPointF(960, 570));
    view.setFrameSize({1920, 1080});

    // 100 %: one frame pixel per screen pixel (device pixel ratio 1 offscreen), centred
    QList<double> zooms;
    QObject::connect(&view, &FrameView::zoomChanged, [&](double z) { zooms << z; });
    view.setZoom(1.0);
    CHECK_EQ(zooms, QList<double>({1.0}));
    CHECK_EQ(view.frameRect(), QRectF(-480, -240, 1920, 1080));
    CHECK_EQ(view.widgetToFrame({0, 0}), QPointF(480, 240));
    // Panning stops where the picture's edge reaches the widget's edge
    view.setPan({1000, -1000});
    CHECK_EQ(view.pan(), QPointF(480, -240));
    CHECK_EQ(view.frameRect(), QRectF(0, -480, 1920, 1080));
    CHECK_EQ(view.widgetToFrame({0, 0}), QPointF(0, 480));
    // 200 %: the pan grows with the picture (the same point stays at the centre), pixels sharp
    view.setPan({100, 50});
    const QPointF centre = view.widgetToFrame({480, 300});
    view.setZoom(2.0);
    CHECK_EQ(view.widgetToFrame({480, 300}), centre);
    CHECK_EQ(view.frameRect().size(), QSizeF(3840, 2160));
    CHECK(!view.smoothScaling());
    // 50 %: smaller than the widget in width -> centred on that axis, no pan there
    view.setZoom(0.5);
    CHECK_EQ(view.frameRect(), QRectF(0, 30, 960, 540));
    CHECK_EQ(view.pan(), QPointF());
    view.setPan({50, 50});
    CHECK_EQ(view.pan(), QPointF());
    // Back to fit: no pan left over
    view.setZoom(1.0);
    view.setPan({200, 100});
    view.setZoom(FrameView::kFit);
    CHECK_EQ(view.pan(), QPointF());
    CHECK_EQ(view.frameRect(), QRectF(0, 30, 960, 540));
    CHECK_EQ(zooms.size(), 5);
    view.setZoom(FrameView::kFit); // no change, no signal
    CHECK_EQ(zooms.size(), 5);

    // Resize while zoomed: the pan stays inside the new limits
    view.setZoom(1.0);
    view.setPan({480, 240});
    view.resize(1600, 1000);
    CHECK_EQ(view.pan(), QPointF(160, 40));
}

void testOverlayAndMouse()
{
    FrameView view;
    view.resize(800, 600);
    view.show();
    view.setFrame(card({1600, 900}));
    RecordingOverlay overlay;
    view.setOverlay(&overlay);
    CHECK(view.overlay() == &overlay);
    view.grab();
    CHECK_EQ(overlay.painted, view.frameRect());

    // Zoomed and panned: the overlay draws and hits in the same coordinates as the picture
    view.setZoom(1.0);
    view.setPan({300, -100});
    view.grab();
    CHECK_EQ(overlay.painted, view.frameRect());
    CHECK_EQ(overlay.painted, QRectF(-100, -250, 1600, 900));
    overlay.take = true;
    send(&view, QEvent::MouseButtonPress, {400, 300}, Qt::LeftButton, Qt::LeftButton);
    send(&view, QEvent::MouseButtonRelease, {400, 300}, Qt::LeftButton, Qt::NoButton);
    CHECK_EQ(overlay.pressed, view.frameRect());
    CHECK_EQ(view.widgetToFrame(overlay.pressPos), QPointF(500, 550));
    CHECK_EQ(view.pan(), QPointF(300, -100)); // the overlay took it: no pan

    // Middle drag pans (the overlay leaves the event)
    overlay.take = false;
    send(&view, QEvent::MouseButtonPress, {400, 300}, Qt::MiddleButton, Qt::MiddleButton);
    send(&view, QEvent::MouseMove, {350, 320}, Qt::NoButton, Qt::MiddleButton);
    send(&view, QEvent::MouseButtonRelease, {350, 320}, Qt::MiddleButton, Qt::NoButton);
    CHECK_EQ(view.pan(), QPointF(250, -80));
    // Left drag pans too while zoomed in and nothing can be dragged out …
    send(&view, QEvent::MouseButtonPress, {400, 300}, Qt::LeftButton, Qt::LeftButton);
    send(&view, QEvent::MouseMove, {420, 300}, Qt::NoButton, Qt::LeftButton);
    send(&view, QEvent::MouseButtonRelease, {420, 300}, Qt::LeftButton, Qt::NoButton);
    CHECK_EQ(view.pan(), QPointF(270, -80));
    // … but not when the picture can be dragged into the timeline (source viewer)
    view.dragData = [] { return nullptr; };
    send(&view, QEvent::MouseButtonPress, {400, 300}, Qt::LeftButton, Qt::LeftButton);
    send(&view, QEvent::MouseMove, {440, 300}, Qt::NoButton, Qt::LeftButton);
    send(&view, QEvent::MouseButtonRelease, {440, 300}, Qt::LeftButton, Qt::NoButton);
    CHECK_EQ(view.pan(), QPointF(270, -80));
    view.dragData = nullptr;
    // … and not in fit
    view.setZoom(FrameView::kFit);
    send(&view, QEvent::MouseButtonPress, {400, 300}, Qt::LeftButton, Qt::LeftButton);
    send(&view, QEvent::MouseMove, {440, 300}, Qt::NoButton, Qt::LeftButton);
    send(&view, QEvent::MouseButtonRelease, {440, 300}, Qt::LeftButton, Qt::NoButton);
    CHECK_EQ(view.pan(), QPointF());

    // Ctrl+wheel: fit (here 50 %) -> 100 % -> 200 %, back down to fit; without Ctrl nothing
    const auto wheel = [&](int dy, Qt::KeyboardModifiers mods) {
        QWheelEvent e(QPointF(400, 300), view.mapToGlobal(QPointF(400, 300)), QPoint(), QPoint(0, dy), Qt::NoButton,
                      mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(&view, &e);
    };
    wheel(120, Qt::NoModifier);
    CHECK_EQ(view.zoom(), FrameView::kFit);
    wheel(120, Qt::ControlModifier);
    CHECK_EQ(view.zoom(), 1.0);
    wheel(120, Qt::ControlModifier);
    CHECK_EQ(view.zoom(), 2.0);
    wheel(-120, Qt::ControlModifier);
    CHECK_EQ(view.zoom(), 1.0);
    wheel(-120, Qt::ControlModifier);
    CHECK_EQ(view.zoom(), FrameView::kFit); // 50 % = the fit size: fit
}

void testDrawing()
{
    FrameView view;
    view.setBackground(QColor(10, 20, 30));
    view.resize(400, 300);
    view.show();
    const QImage img = card({320, 180});
    view.setFrame(img);
    // Fit: bars in the letterbox colour above and below, the picture between
    QImage shot = view.grab().toImage();
    CHECK_EQ(shot.pixelColor(200, 5), QColor(10, 20, 30));
    CHECK_EQ(shot.pixelColor(200, 295), QColor(10, 20, 30));
    CHECK_EQ(view.frameRect(), QRectF(0, 37, 400, 225));
    dump(&view, "fit");

    // 200 % at the top-left corner: every frame pixel is a sharp 2x2 square (checker stays pure black/white)
    view.setZoom(2.0);
    view.setPan({1000, 1000});
    CHECK_EQ(view.frameRect().topLeft(), QPointF(0, 0));
    shot = view.grab().toImage();
    bool sharp = true;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const QColor want = (x / 2 + y / 2) % 2 ? Qt::black : Qt::white;
            if (shot.pixelColor(x, y) != want) sharp = false;
        }
    CHECK(sharp);
    dump(&view, "zoom-200-corner");
    view.setPan({});
    dump(&view, "zoom-200-centre");
    view.setZoom(1.0);
    dump(&view, "zoom-100");
    view.setZoom(0.5);
    dump(&view, "zoom-50");
}

// A frame wider than the GPU's texture size limit: still shown on the GPU, scaled down to the limit (before: OpenGL took
// the texture without complaint and the viewer stayed black). GPU path only: FRAME_VIEW_PLATFORM=xcb
// SCHNEIDI_VIEWER=gpu under Xvfb (llvmpipe: 16384)
void testTextureTooLarge()
{
    FrameView view;
    if (view.backend() != FrameView::Backend::Rhi) {
        std::fprintf(stderr, "  texture too large: skipped (no GPU viewer here)\n");
        return;
    }
    view.setBackground(Qt::black);
    view.resize(400, 300);
    view.show();
    QImage wide(QSize(17000, 1200), QImage::Format_RGBA8888); // over 16384, 80 MB; ~28 px high in the view
    wide.fill(Qt::red);
    view.setFrame(wide);
    const QImage shot = view.grab().toImage();
    CHECK(view.backend() == FrameView::Backend::Rhi);
    const QColor mid = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    CHECK(mid.red() > 200 && mid.green() < 60 && mid.blue() < 60);
}

// CPU time per displayed 1080p frame: a new frame each time (as during playback), drawn into a 1280x720 view
void bench(int frames)
{
    FrameView view;
    view.resize(1280, 720);
    // FRAME_VIEW_POS=x,y: where the window opens on a real display (e.g. a second monitor), without taking the focus
    if (const QStringList pos = QString::fromLocal8Bit(qgetenv("FRAME_VIEW_POS")).split(','); pos.size() == 2)
        view.move(pos[0].toInt(), pos[1].toInt());
    view.setAttribute(Qt::WA_ShowWithoutActivating);
    view.show();
    QTest::qWait(200);
    QList<QImage> images;
    for (int i = 0; i < 8; ++i) {
        QImage img = card({1920, 1080});
        img.fill(QColor::fromHsv(i * 40, 200, 200));
        images << img;
    }
    for (double zoom : {FrameView::kFit, 1.0}) {
        view.setZoom(zoom);
        for (int i = 0; i < 10; ++i) { // warm up
            view.setFrame(images[i % images.size()].copy());
            view.window()->repaint();
            QCoreApplication::processEvents();
        }
        const std::clock_t cpu0 = std::clock();
        QElapsedTimer wall;
        wall.start();
        for (int i = 0; i < frames; ++i) {
            // A fresh copy: a new cacheKey every frame like the engine's frames (the upload is part of the cost)
            QImage frame = images[i % images.size()];
            frame.detach();
            view.setFrame(frame);
            view.window()->repaint();
            QCoreApplication::processEvents();
        }
        const double cpuMs = 1000.0 * double(std::clock() - cpu0) / CLOCKS_PER_SEC / frames;
        const double wallMs = double(wall.nsecsElapsed()) / 1e6 / frames;
        qInfo().noquote() << QString("bench %1 zoom %2: %3 ms CPU, %4 ms wall per frame (%5)")
                                 .arg(view.backend() == FrameView::Backend::Rhi ? "rhi" : "painter")
                                 .arg(zoom == FrameView::kFit ? QString("fit") : QString::number(zoom))
                                 .arg(cpuMs, 0, 'f', 2)
                                 .arg(wallMs, 0, 'f', 2)
                                 .arg(view.backendInfo());
    }
}

void testViewer(const QString& clipDir)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    if (Check::haveFfmpeg()) {
        const QString file = Check::makeMedia(clipDir + "/clip.mp4",
            {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=25:duration=2", "-c:v", "libx264", "-preset",
             "ultrafast", "-pix_fmt", "yuv420p"});
        if (CHECK(!file.isEmpty())) w.importFiles({file}, true);
    }
    auto* viewer = w.findChild<Viewer*>();
    auto* button = viewer ? viewer->findChild<QToolButton*>("viewer_zoom") : nullptr;
    QAction* fit = InputBindings::instance().action("viewer_zoom_fit");
    QAction* z100 = InputBindings::instance().action("viewer_zoom_100");
    QAction* z200 = InputBindings::instance().action("viewer_zoom_200");
    QAction* z50 = InputBindings::instance().action("viewer_zoom_50");
    if (!CHECK(viewer && button && fit && z100 && z200 && z50)) return;
    CHECK_EQ(InputBindings::instance().defaultInputs("viewer_zoom_fit").value(0), QString("Alt+0"));
    CHECK_EQ(InputBindings::instance().defaultInputs("viewer_zoom_100").value(0), QString()); // Alt+1 = target V1
    CHECK_EQ(InputBindings::instance().defaultInputs("viewer_zoom_200").value(0), QString());
    CHECK(fit->isChecked() && viewer->zoom() == FrameView::kFit);
    CHECK(!engine.previewFullSize());
    engine.seek(10);
    // Waits for what is to be seen, not a fixed time (a loaded machine renders later; an empty picture passed before)
    CHECK(QTest::qWaitFor([&] { return !viewer->picture().isNull(); }, 10000));
    const QSize small = viewer->picture().size();
    CHECK(!small.isEmpty() && small.width() <= 960);
    dump(&w, "viewer-fit");

    z100->trigger();
    CHECK_EQ(viewer->zoom(), 1.0);
    CHECK(z100->isChecked() && !fit->isChecked());
    CHECK_EQ(button->text(), QString("100 %"));
    CHECK(engine.previewFullSize());
    QTest::qWaitFor([&] { return viewer->picture().size() == engine.frameSize(); }, 10000);
    CHECK_EQ(viewer->picture().size(), engine.frameSize()); // full-size frames for 100 %
    dump(&w, "viewer-100");
    if (!dumpDir().isEmpty()) viewer->picture().save(dumpDir() + "/picture-100.png");
    z200->trigger();
    CHECK_EQ(viewer->zoom(), 2.0);
    CHECK(engine.previewFullSize());
    dump(&w, "viewer-200");
    z50->trigger();
    CHECK(!engine.previewFullSize()); // 50 %: the small preview is enough
    fit->trigger();
    CHECK_EQ(viewer->zoom(), FrameView::kFit);
    CHECK_EQ(button->text(), QString("Einpassen"));
    // Back to the small preview: "Automatic" follows the viewer's own size (since 0.20), which may differ from the
    // first picture (rendered before the viewer had a size) - smaller than the frame, never above 960
    const auto smallAgain = [&] {
        const QSize s = viewer->picture().size();
        return !s.isEmpty() && s != engine.frameSize() && s.width() <= 960;
    };
    QTest::qWaitFor(smallAgain, 10000);
    CHECK(smallAgain());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    // FRAME_VIEW_PLATFORM=xcb under Xvfb: the GPU path (with SCHNEIDI_VIEWER=gpu on a software device)
    if (qEnvironmentVariableIsSet("FRAME_VIEW_PLATFORM")) qputenv("QT_QPA_PLATFORM", qgetenv("FRAME_VIEW_PLATFORM"));
    QApplication app(argc, argv);
    Check::initApp("frame-view");
    testDeferredAttach(); // first: the start's path needs a GPU check not made yet in this process
    qInfo().noquote() << "FrameView backend:" << (FrameView().backend() == FrameView::Backend::Rhi ? "rhi" : "painter")
                      << FrameView().backendInfo();
    if (const int frames = qEnvironmentVariableIntValue("FRAME_VIEW_BENCH"); frames > 0) {
        bench(frames);
        return 0;
    }
    testChoice();
    testApiChoice();
    testGeometry();
    testOverlayAndMouse();
    testDrawing();
    testTextureTooLarge();
    QTemporaryDir dir;
    testViewer(dir.path());
    return Check::result();
}
