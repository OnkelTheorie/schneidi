// SPDX-License-Identifier: AGPL-3.0-only
// Proxy hint on slow playback (core/PlaybackMeter.h, MainWindow::updateProxyHint): the meter tells a machine that
// cannot keep up (pictures shown per second against the frame rate, after a warmup) from one that can; the main
// window then shows a hint above the picture with "Generate proxy media" – and generates nothing by itself. The
// hint follows the proxy queue and goes away once the proxies are there; closed, it stays away.
// A preview the user raised above the automatic size, or one that stays slow although every file has its proxy, gets
// "Vorschau verkleinern" instead (PreviewResolution::smaller: one step down, reversible in the resolution menu):
// proxies only make decoding cheaper, effects and compositing cost per preview pixel.
// PROXY_HINT_DUMP=<png>: picture of the viewer with the hint (LOWER_HINT_DUMP=<png>: with "Vorschau verkleinern").

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/PlaybackMeter.h"
#include "engine/Engine.h"
#include "engine/PreviewResolution.h"
#include "engine/ProxyManager.h"
#include "ui/Viewer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>
#include <clocale>

namespace {

// Pictures at `rate` per second from 0 to `ms`; true if the meter reported too slow
bool play(PlaybackMeter& m, double rate, qint64 ms, double* reported = nullptr)
{
    bool slow = false;
    for (qint64 t = 0; t <= ms; t = qint64(t + 1000.0 / rate))
        if (m.frame(t)) {
            slow = true;
            if (reported) *reported = m.rate();
        }
    return slow;
}

// Pictures at `rate` per second, but from `dipFrom` for `dipMs` at `dipRate` (0 = nothing shown: a stall)
bool playWithDip(PlaybackMeter& m, double rate, qint64 ms, qint64 dipFrom, qint64 dipMs, double dipRate)
{
    bool slow = false;
    for (qint64 t = 0; t <= ms;) {
        const bool dip = t >= dipFrom && t < dipFrom + dipMs;
        if (dip && dipRate <= 0) {
            t = dipFrom + dipMs;
            continue;
        }
        if (m.frame(t)) slow = true;
        t = qint64(t + 1000.0 / (dip ? dipRate : rate));
    }
    return slow;
}

// A short dip (an expensive transition between two 1080p60 recordings: ~30 of 60 pictures/s for two seconds, the pump
// jumping ahead) is no reason for proxies, even when it pulls the window's rate below kSlow; slow most of the time is.
// The window opens with the first picture after the warmup (1.5 s) and lasts 4 s.
void testMeterShortDip()
{
    const qint64 window = PlaybackMeter::kWarmupMs + 2 * PlaybackMeter::kWindowMs + 200;
    PlaybackMeter m;
    m.start(60, 0);
    CHECK(!playWithDip(m, 60, window, 3000, 1500, 5)); // window rate 39 of 60
    m.start(60, 0);
    CHECK(!playWithDip(m, 60, window, 2600, 1600, 0)); // a stall of 1.6 s (window rate 36 of 60)
    m.start(60, 0);
    CHECK(!playWithDip(m, 60, window, 2500, 2000, 10)); // two slow seconds of four (window rate 35)
    // Slow throughout, or most of the window: reported
    m.start(60, 0);
    CHECK(play(m, 35, 12000)); // 35 of 60 every second
    m.start(60, 0);
    CHECK(playWithDip(m, 60, window, 2500, 3000, 20)); // three slow seconds of four
}

void testMeter()
{
    PlaybackMeter m;
    CHECK(!play(m, 2, 10000)); // not started: nothing
    m.start(30, 0);
    CHECK(!play(m, 30, 12000)); // keeps up
    m.start(30, 0);
    CHECK(!play(m, 25, 12000)); // a few pictures dropped: still fine
    m.start(60, 0);
    double rate = 0;
    CHECK(play(m, 8, 12000, &rate)); // a Pi with 1080p originals
    CHECK(rate > 6 && rate < 10);
    // The warmup and the first window do not report yet
    m.start(30, 0);
    CHECK(!play(m, 5, PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs - 500));
    m.stop();
    CHECK(!m.running());
    CHECK(!play(m, 1, 20000)); // stopped (pause, another speed)
}

// Emits what the engine emits while playing: speed 1, then a picture every `stepMs` for `ms`
void fakePlayback(Engine& engine, int stepMs, int ms)
{
    emit engine.speedChanged(1.0);
    QElapsedTimer t;
    t.start();
    int frame = 0;
    while (t.elapsed() < ms) {
        QThread::msleep(stepMs);
        emit engine.positionChanged(++frame);
        QApplication::processEvents();
    }
    emit engine.speedChanged(0.0);
}

void testMainWindow(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=4",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!a.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({a}, true);
    ProxyManager* proxies = engine.proxies();
    proxies->remove({a});
    auto* viewer = w.findChild<Viewer*>();
    auto* button = w.findChild<QToolButton*>("viewer_hint_button");
    auto* close = w.findChild<QToolButton*>("viewer_hint_close");
    if (!CHECK(viewer && button && close)) return;
    CHECK(viewer->hint().isEmpty());

    // Smooth playback: no hint
    fakePlayback(engine, 30, PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs + 600);
    CHECK(viewer->hint().isEmpty());

    // 2 of 25 pictures/s: hint with the button, nothing generated yet
    fakePlayback(engine, 500, PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs + 1200);
    CHECK(!viewer->hint().isEmpty());
    CHECK(viewer->hint().contains("25"));
    CHECK(button->isVisible());
    CHECK(!proxies->isPending(a) && !proxies->hasProxy(a));
    if (const QString dump = qEnvironmentVariable("PROXY_HINT_DUMP"); !dump.isEmpty()) viewer->grab().save(dump);

    // The button generates the proxy; the hint follows the queue and goes away when it is done
    button->click();
    CHECK(proxies->isPending(a));
    CHECK(!button->isVisible());
    QElapsedTimer t;
    t.start();
    while (!proxies->hasProxy(a) && t.elapsed() < 60000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
    CHECK(proxies->hasProxy(a));
    CHECK(viewer->hint().isEmpty());

    // Slow again without a proxy: hint again; closed, it stays away for the session
    proxies->remove({a});
    fakePlayback(engine, 500, PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs + 1200);
    CHECK(!viewer->hint().isEmpty());
    close->click();
    CHECK(viewer->hint().isEmpty());
    fakePlayback(engine, 500, PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs + 1200);
    CHECK(viewer->hint().isEmpty());
    CHECK(!proxies->isPending(a));
}

// One step down: back to automatic when the user raised the preview above it, else the next fixed size that is
// noticeably smaller (at most 3/4 of the pixels), never below 360p
void testSmaller()
{
    using PreviewResolution::Choice;
    using PreviewResolution::smaller;
    const QSize fhd(1920, 1080);
    CHECK(smaller(Choice::Project, 960, fhd) == Choice::Auto);
    CHECK(smaller(Choice::P1080, 960, fhd) == Choice::Auto);
    CHECK(smaller(Choice::P2160, 960, fhd) == Choice::Auto); // (larger than the project: the project's size)
    CHECK(smaller(Choice::P1080, 1900, fhd) == Choice::P720); // automatic would be hardly smaller
    CHECK(smaller(Choice::Project, 0, fhd) == Choice::Auto);  // not shown yet: automatic is the engine's 960
    CHECK(smaller(Choice::Auto, 1920, fhd) == Choice::P720);
    CHECK(smaller(Choice::Auto, 1280, fhd) == Choice::P540);
    CHECK(smaller(Choice::Auto, 960, fhd) == Choice::P360);
    CHECK(smaller(Choice::P720, 1920, fhd) == Choice::P540);
    CHECK(smaller(Choice::P540, 1920, fhd) == Choice::P360);
    CHECK(smaller(Choice::P360, 1920, fhd) == Choice::P360); // nothing smaller
    CHECK(smaller(Choice::Auto, 600, fhd) == Choice::Auto);  // already below 360p
    // A small project: nothing is smaller than the project itself
    CHECK(smaller(Choice::Project, 960, QSize(320, 180)) == Choice::Project);
    CHECK(smaller(Choice::Auto, 960, QSize(320, 180)) == Choice::Auto);
    // 9:16: lines are the shorter edge
    CHECK(smaller(Choice::P1080, 1920, QSize(1080, 1920)) == Choice::P720);
    CHECK(smaller(Choice::Project, 960, QSize(1080, 1920)) == Choice::Auto);
}

void testLowerPreview(const QString& dir)
{
    if (!Check::haveFfmpeg()) return;
    const QString hd = Check::makeMedia(dir + "/hd.mp4", {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=1",
                                                          "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!hd.isEmpty())) return;
    using PreviewResolution::Choice;
    using PreviewResolution::Target;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({hd}, true);
    QApplication::processEvents();
    ProxyManager* proxies = engine.proxies();
    proxies->remove({hd});
    auto* viewer = w.findChild<Viewer*>();
    auto* button = w.findChild<QToolButton*>("viewer_hint_button");
    if (!CHECK(viewer && button)) return;
    const auto pixels = [](QSize s) { return qint64(s.width()) * s.height(); };
    const int slowFor = PlaybackMeter::kWarmupMs + PlaybackMeter::kWindowMs + 1200;

    // The preview raised to the project's size: slow playback offers the smaller preview first (proxies would leave
    // the picture as large), the hint names both sizes
    w.setPreviewResolution(Target::Viewer, Choice::Project);
    CHECK(engine.previewSize() == QSize(1920, 1080));
    fakePlayback(engine, 500, slowFor);
    CHECK(viewer->hint().contains(QStringLiteral("1920×1080")));
    CHECK(button->isVisible());
    CHECK_EQ(button->text(), T("Vorschau verkleinern"));
    if (const QString dump = qEnvironmentVariable("LOWER_HINT_DUMP"); !dump.isEmpty()) viewer->grab().save(dump);
    button->click();
    CHECK(w.previewResolution(Target::Viewer) == Choice::Auto);
    CHECK(pixels(engine.previewSize()) < pixels(QSize(1920, 1080)));
    CHECK(viewer->hint().isEmpty());
    CHECK(!proxies->isPending(hd) && !proxies->hasProxy(hd)); // nothing generated

    // Slow again at the automatic size, the file without a proxy: proxies as before
    fakePlayback(engine, 500, slowFor);
    CHECK_EQ(button->text(), T("Proxy-Medien erzeugen"));
    button->click();
    QElapsedTimer t;
    t.start();
    while (!proxies->hasProxy(hd) && t.elapsed() < 60000) QApplication::processEvents(QEventLoop::AllEvents, 50);
    QApplication::processEvents();
    CHECK(proxies->hasProxy(hd));
    CHECK(viewer->hint().isEmpty()); // the proxies are there: no other offer before a new measurement

    // Still slow although every file has its proxy: never proxies again, the next smaller preview. A large viewer
    // (automatic = the pixels it shows) so that a fixed size noticeably below it exists
    w.resize(3200, 1800);
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 800) QApplication::processEvents(QEventLoop::AllEvents, 20);
    const QSize before = engine.previewSize();
    CHECK(before.width() > 854); // (precondition: larger than 480p)
    fakePlayback(engine, 500, slowFor);
    CHECK(!viewer->hint().isEmpty());
    CHECK_EQ(button->text(), T("Vorschau verkleinern"));
    CHECK(viewer->hint().contains(PreviewResolution::sizeText(before)));
    button->click();
    const Choice lowered = w.previewResolution(Target::Viewer);
    CHECK(lowered != Choice::Auto && PreviewResolution::lines(lowered) >= 360);
    CHECK(pixels(engine.previewSize()) * 4 <= pixels(before) * 3);
    CHECK(viewer->hint().isEmpty());

    // At the smallest size with all proxies there: nothing left to offer, no hint
    w.setPreviewResolution(Target::Viewer, Choice::P360);
    fakePlayback(engine, 500, slowFor);
    CHECK(viewer->hint().isEmpty());

    // Zoomed to 100 % (the engine renders the project's size whatever the choice): lowering would change nothing
    w.setPreviewResolution(Target::Viewer, Choice::Project);
    engine.setPreviewFullSize(true);
    fakePlayback(engine, 500, slowFor);
    CHECK(viewer->hint().isEmpty());
    engine.setPreviewFullSize(false);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("proxy-hint");
    QTemporaryDir dir;
    testMeter();
    testMeterShortDip();
    testSmaller();
    testMainWindow(dir.path());
    testLowerPreview(dir.path());
    return Check::result();
}
