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
        // BENCH_FX=blur,grade: only these effects (to find the expensive one)
        const QString only = qEnvironmentVariable("BENCH_FX");
        for (const QString& id : only.isEmpty() ? QStringList{"color", "blur", "grade"} + frei0r : only.split(','))
            EffectRegistry::add(e, id);
        // BENCH_KEYS=0: no keyframes, zoom: only zoom, fx: only effect parameters
        const QString keys = qEnvironmentVariable("BENCH_KEYS");
        if (keys == "0") continue;
        for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY, AnimParam::FxBrightness, AnimParam::GradeSaturation}) {
            if ((keys == "zoom") != (p == AnimParam::ZoomX || p == AnimParam::ZoomY) && !keys.isEmpty()) continue;
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
                                           {"-f", "lavfi", "-i", QString("testsrc2=size=1920x1080:rate=%1:duration=8").arg(qEnvironmentVariable("BENCH_RATE", "25")), "-c:v",
                                            "libx264", "-preset", "ultrafast", "-g", "250", "-pix_fmt", "yuv420p"});
    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine init failed: %s\n", qPrintable(error));
        return 77;
    }
    if (const int rate = qEnvironmentVariableIntValue("BENCH_RATE"); rate > 0) { // BENCH_RATE=60: 60 fps project
        ProjectFormat f;
        f.rate = {rate, 1};
        engine.setFormat(f);
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
        QElapsedTimer wall;
        wall.start();
        double maxGap = 0, last = -1, first = 0;
        int firstPos = 0;
        auto gapConn = QObject::connect(&engine, &Engine::frameReady, [&] {
            const double now = wall.nsecsElapsed() / 1e6;
            if (last < 0) first = now, firstPos = engine.position();
            else maxGap = std::max(maxGap, now - last);
            last = now;
        });
        engine.play();
        if (changes) change.start(30);
        QEventLoop loop;
        QTimer::singleShot(3000, &loop, &QEventLoop::quit);
        loop.exec();
        change.stop();
        engine.pause();
        QObject::disconnect(conn);
        QObject::disconnect(gapConn);
        // Playback speed after the first frame below 1: the audio clock stalled (audible stutter); longest gap between shown frames
        std::printf("  first frame %.0f ms, speed %.2f, longest gap %.0f ms\n", first, (engine.position() - firstPos) / qEnvironmentVariable("BENCH_RATE", "25").toDouble() / ((last - first) / 1000.0), maxGap);
        return frames / 3.0;
    };
    std::printf("playing: %.1f fps without changes\n", playFps(false));
    std::printf("playing: %.1f fps with a change every 30 ms\n", playFps(true));
    return 0;
}
