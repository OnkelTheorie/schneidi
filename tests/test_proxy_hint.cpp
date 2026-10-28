// SPDX-License-Identifier: AGPL-3.0-only
// Proxy hint on slow playback (core/PlaybackMeter.h, MainWindow::updateProxyHint): the meter tells a machine that
// cannot keep up (pictures shown per second against the frame rate, after a warmup) from one that can; the main
// window then shows a hint above the picture with "Generate proxy media" – and generates nothing by itself. The
// hint follows the proxy queue and goes away once the proxies are there; closed, it stays away.
// PROXY_HINT_DUMP=<png>: picture of the viewer with the hint.

#include "check.h"

#include "app/MainWindow.h"
#include "core/PlaybackMeter.h"
#include "engine/Engine.h"
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("proxy-hint");
    QTemporaryDir dir;
    testMeter();
    testMainWindow(dir.path());
    return Check::result();
}
