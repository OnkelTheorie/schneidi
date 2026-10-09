// Benchmark (not a CTest test): what an Inspector change costs in the real preview Engine.
// Paused: time from updateTimeline() to the next shown frame. Playing: shown frames per second while the
// timeline is updated at the rate of an Inspector drag (MainWindow bundles changes every 30 ms).
// Run: QT_QPA_PLATFORM=offscreen ./build-tests/tests/bench_engine [clips] [frei0r effects, -1 = no effects at all]
#include "check.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cstdio>

namespace {

Timeline make(const QString& media, int clips, const QStringList& frei0r, bool effects)
{
    Timeline tl;
    tl.video.resize(2);
    for (int i = 0; i < clips; ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = media;
        c.in = 0;
        c.out = 199;
        c.start = i * 200;
        tl.video[0].clips << c;
        if (!effects) continue;
        Clip& e = tl.video[0].clips.last();
        EffectRegistry::add(e, "color");
        EffectRegistry::add(e, "blur");
        EffectRegistry::add(e, "grade");
        for (const QString& id : frei0r) EffectRegistry::add(e, id);
        for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::FxBrightness, AnimParam::GradeSaturation}) {
            Keys::setKey(e, p, 0, Keys::staticValue(e, p));
            Keys::setKey(e, p, 199, Keys::staticValue(e, p) + (p == AnimParam::ZoomX || p == AnimParam::ZoomY ? 0.2 : 5));
        }
    }
    return tl;
}

// Wait until the Engine shows a frame (or the timeout runs out); returns the milliseconds waited
double waitFrame(Engine& e, int timeoutMs = 3000)
{
    QEventLoop loop;
    QElapsedTimer t;
    t.start();
    auto conn = QObject::connect(&e, &Engine::frameReady, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(conn);
    return t.nsecsElapsed() / 1e6;
}

void report(const char* name, QVector<double> ms)
{
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("%-34s mean %6.1f ms  median %6.1f  max %6.1f\n", name, sum / ms.size(), ms[ms.size() / 2], ms.last());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SDL_AUDIODRIVER", "dummy");
    QApplication app(argc, argv);
    Check::initApp("bench-engine");
    const int clips = argc > 1 ? atoi(argv[1]) : 5;
    const int nFrei0r = argc > 2 ? atoi(argv[2]) : 3;
    QTemporaryDir tmp;
    const QString media = Check::makeMedia(tmp.filePath("bars.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=25:duration=8", "-c:v",
                                            "libx264", "-preset", "ultrafast", "-g", "250", "-pix_fmt", "yuv420p"});
    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine init failed: %s\n", qPrintable(error));
        return 77;
    }
    QStringList frei0r;
    for (const EffectDescriptor& d : EffectRegistry::all())
        if (d.category == EffectRegistry::Frei0rCategory && d.video && frei0r.size() < nFrei0r
            && !d.id.contains("bluescreen") && !d.id.contains("facedetect"))
            frei0r << d.id;
    std::printf("clips %d, frei0r: %s\n", clips, qPrintable(frei0r.join(", ")));

    Timeline tl = make(media, clips, frei0r, nFrei0r >= 0);
    engine.updateTimeline(tl);
    engine.showTimeline(150);
    waitFrame(engine);

    // Paused: seek only (floor: one frame rendered) vs. Inspector change (rebuild)
    QVector<double> seekMs, changeMs;
    for (int i = 0; i < 15; ++i) {
        engine.seek(150 + i); // forward: no decoding from the previous keyframe
        seekMs << waitFrame(engine);
    }
    Clip& c = tl.video[0].clips[0];
    for (int i = 0; i < 15; ++i) {
        c.transform.rotation = i;
        QElapsedTimer t;
        t.start();
        engine.updateTimeline(tl);
        waitFrame(engine);
        changeMs << t.nsecsElapsed() / 1e6;
    }
    QVector<double> sameMs;
    for (int i = 0; i < 15; ++i) {
        engine.seek(engine.position()); // same frame again: refresh only
        sameMs << waitFrame(engine);
    }
    report("paused: seek (one frame)", seekMs);
    report("paused: same frame again", sameMs);
    report("paused: inspector change", changeMs);

    // Playing: frames shown per second, without and with changes every 30 ms
    auto playFps = [&](bool changes) {
        engine.seek(0);
        waitFrame(engine);
        int frames = 0;
        auto conn = QObject::connect(&engine, &Engine::frameReady, [&] { ++frames; });
        QTimer change;
        int n = 0;
        QObject::connect(&change, &QTimer::timeout, [&] {
            c.transform.rotation = ++n % 10;
            engine.updateTimeline(tl);
        });
        engine.play();
        if (changes) change.start(30);
        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        change.stop();
        engine.pause();
        QObject::disconnect(conn);
        return frames / 3.0;
    };
    std::printf("playing: %.1f fps without changes\n", playFps(false));
    std::printf("playing: %.1f fps with a change every 30 ms\n", playFps(true));
    return 0;
}
