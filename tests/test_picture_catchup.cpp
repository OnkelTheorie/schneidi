// Picture follows sound on a machine too slow for the material (docs/plan-0.10-ton.md › Zielbild: "a frame that is
// too late is skipped, the sound runs on"). Measured on one core (2026-10-23): the picture fell further and further
// behind the sound clock and never caught up (15 s: sound at frame 843, picture at 434). Here a test filter makes
// rendering slow without needing CPU (it sleeps, so parallel load does not matter) and behaves like a long-GOP decoder:
// a rendered frame costs 40 ms plus 20 ms of decoding for every frame since the last rendered one (MLT skipping images
// saves no decoding), a jump (backwards, or more than 12 frames ahead – where MLT's avformat seeks instead of decoding
// on) 150 ms more for decoding from the keyframe before it. Played in order: 16.7 frames per second for 25 fps.
// The real Engine (own audio engine only) plays: first a fast clip (nothing may jump there), then the slow one
// forward, a seek while playing, then backwards. The shown frame must stay within a bound of the sound clock (a
// slideshow in sync), the playhead never runs back, the sound (file device as the speaker) has no dropout.
#include "check.h"

#include "core/EffectRegistry.h"
#include "engine/Engine.h"
#include "engine/audio/Path.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace {

constexpr int kFps = 25;
constexpr int kSlowFrom = 50;     // timeline frame where the slow clip starts
constexpr int kLength = 500;      // 20 s
constexpr double kRenderMs = 40, kDecodeMs = 20, kSeekMs = 150; // costs of the slow clip, see above
// The picture may be this far behind the sound once the cost of a jump is known: a jump in flight included (the
// picture stands meanwhile; backwards a jump waits for the frame being rendered, 210 ms, and then costs as much
// again), the first jumps learn the cost. Without keeping up: 1.7 s after 5 s forward, 2.8 s after 3.5 s backwards.
constexpr double kBoundSeconds = 1.0;
constexpr double kMeanBoundSeconds = 0.6;

std::atomic<int> g_last{-1000}; // the slow filter's last rendered frame

int slowImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    const int error = mlt_frame_get_image(frame, image, format, width, height, writable);
    const int pos = mlt_frame_get_position(frame);
    const int since = pos - g_last.exchange(pos);
    const double ms = kRenderMs + (since >= 1 && since <= 12 ? since * kDecodeMs : kSeekMs + kDecodeMs);
    std::this_thread::sleep_for(std::chrono::microseconds(int64_t(ms * 1000)));
    return error;
}

mlt_frame slowProcess(mlt_filter, mlt_frame frame)
{
    mlt_frame_push_get_image(frame, slowImage);
    return frame;
}

void* slowInit(mlt_profile, mlt_service_type, const char*, const void*)
{
    mlt_filter filter = mlt_filter_new();
    if (filter) filter->process = slowProcess;
    return filter;
}

void registerSlowEffect()
{
    mlt_repository_register(mlt_factory_repository(), mlt_service_filter_type, "schneidi_test_slow",
                            reinterpret_cast<mlt_register_callback>(slowInit));
    EffectDescriptor d;
    d.id = "test_slow";
    d.name = "Test: slow";
    d.mltService = "schneidi_test_slow";
    EffectRegistry::registerEffects({d});
}

Clip mk(int id, const QString& path, int start, int in, int out)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Plays for `ms` and samples every 20 ms how far the shown frame is behind the sound (seconds); also every new
// position (the playhead) for its direction
struct Watch {
    double worst = 0, sum = 0, last = 0;
    int samples = 0, backwards = 0, positions = 0, largestStep = 0;
};

Watch watch(Engine& engine, int ms, int direction)
{
    Watch w;
    int previous = engine.position();
    QMetaObject::Connection c = QObject::connect(&engine, &Engine::positionChanged, &engine, [&](int f) {
        if (direction * (f - previous) < 0) ++w.backwards;
        w.largestStep = std::max(w.largestStep, direction * (f - previous));
        previous = f;
        ++w.positions;
    });
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        wait(20);
        const double sound = engine.soundFrame();
        if (std::isnan(sound)) continue;
        // forward frame f is due at clock f, backward at clock f + 1
        const double behind = (direction > 0 ? sound - engine.position() : engine.position() + 1 - sound) / kFps;
        w.worst = std::max(w.worst, behind);
        w.sum += behind;
        w.last = behind;
        ++w.samples;
    }
    QObject::disconnect(c);
    return w;
}

void print(const char* what, const Watch& w)
{
    std::printf("       %-28s hinter dem Ton: max %.2f s, Mittel %.2f s, zuletzt %.2f s; %d Positionen, größter Schritt %d\n",
                what, w.worst, w.samples ? w.sum / w.samples : 0.0, w.last, w.positions, w.largestStep);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QTemporaryDir tmp;
    const QString speaker = tmp.filePath("speaker.raw");
    qputenv("SCHNEIDI_AUDIO_DEVICE", ("file:" + QDir::toNativeSeparators(speaker)).toLocal8Bit());
    QApplication app(argc, argv);
    Check::initApp("picture_catchup");
    if (!Audio::ownPath()) return Check::skip("nur mit der eigenen Ton-Maschine (SCHNEIDI_AUDIO=own)");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString media = Check::makeMedia(
        tmp.filePath("media.mov"),
        {"-f", "lavfi", "-i", QString("testsrc2=size=320x180:rate=%1:duration=20").arg(kFps), "-f", "lavfi", "-i",
         "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*440*t):s=48000:d=20", "-c:v", "libx264", "-preset", "ultrafast",
         "-g", "25", "-c:a", "pcm_s16le"});
    if (!CHECK(!media.isEmpty())) return Check::result();

    Engine engine;
    QString error;
    if (!engine.init(&error)) {
        std::printf("Engine: %s\n", qPrintable(error));
        return Check::skip("Engine startet nicht");
    }
    registerSlowEffect();
    engine.setFormat(ProjectFormat{320, 180, {kFps, 1}});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip slow = mk(12, media, kSlowFrom, kSlowFrom, kLength - 1);
    CHECK(EffectRegistry::add(slow, "test_slow"));
    tl.video[0].clips << mk(11, media, 0, 0, kSlowFrom - 1) << slow;
    tl.audio[0].clips << mk(1, media, 0, 0, kLength - 1);
    engine.updateTimeline(tl);
    engine.showTimeline(0);
    wait(800);

    // Fast clip: in time, every frame, nothing jumps
    engine.play();
    const Watch fast = watch(engine, 1600, 1);
    print("schneller Clip:", fast);
    CHECK(fast.worst < 0.1);
    CHECK(fast.largestStep <= 2);

    // Slow clip: behind by at most the bound, at the end too (without keeping up: 1.7 s after 5 s and growing)
    const Watch learn = watch(engine, 1500, 1);
    print("langsamer Clip, Anfang:", learn);
    CHECK_EQ(learn.backwards, 0);
    const Watch forward = watch(engine, 4000, 1);
    print("langsamer Clip vorwärts:", forward);
    CHECK(forward.worst <= kBoundSeconds);
    CHECK(forward.last <= kBoundSeconds);
    CHECK(forward.sum / std::max(1, forward.samples) <= kMeanBoundSeconds);
    CHECK_EQ(forward.backwards, 0);
    CHECK(forward.positions >= 5); // a slideshow, not a still

    // A seek while it plays (and keeps up): goes there, stays in sync
    engine.seek(300);
    wait(100);
    const Watch seek = watch(engine, 2000, 1);
    print("nach Sprung auf 300:", seek);
    CHECK(engine.position() >= 300);
    CHECK(seek.last <= kBoundSeconds);
    CHECK_EQ(seek.backwards, 0);

    // Backwards (every frame is a jump for a long-GOP decoder)
    engine.pause();
    wait(300);
    const int paused = engine.position();
    CHECK(std::isnan(engine.soundFrame()));
    engine.seek(450);
    wait(300);
    engine.setSpeed(-1);
    const Watch learnBack = watch(engine, 1500, -1);
    print("rückwärts, Anfang:", learnBack);
    CHECK_EQ(learnBack.backwards, 0);
    const Watch backward = watch(engine, 2500, -1);
    print("rückwärts:", backward);
    CHECK(backward.worst <= kBoundSeconds);
    CHECK(backward.last <= kBoundSeconds);
    CHECK(backward.sum / std::max(1, backward.samples) <= kMeanBoundSeconds);
    CHECK_EQ(backward.backwards, 0);
    CHECK(backward.positions >= 3);
    engine.pause();
    wait(400);
    std::printf("       angehalten bei %d (vorher bei %d)\n", engine.position(), paused);

    // The sound ran on without a dropout: no step in the sine (one would be > 5000, a 440-Hz sine at 0.5 changes its
    // slope by < 80 per sample); fades at play/seek/pause are smooth
    QFile f(speaker);
    if (!CHECK(f.open(QIODevice::ReadOnly))) return Check::result();
    const QByteArray raw = f.readAll();
    const auto* s = reinterpret_cast<const int16_t*>(raw.constData());
    const qsizetype frames = raw.size() / 4; // s16 stereo
    int steps = 0;
    for (qsizetype i = 2; i < frames; ++i) {
        const int d2 = std::abs(s[i * 2] - 2 * s[(i - 1) * 2] + s[(i - 2) * 2]);
        if (d2 > 1500) {
            if (steps < 5) std::printf("       Sprung in der Welle bei %.3f s: %d\n", i / 48000.0, d2);
            ++steps;
            i += 480;
        }
    }
    std::printf("       %.2f s Ton\n", frames / 48000.0);
    CHECK(frames / 48000.0 > 10);
    CHECK_EQ(steps, 0);
    return Check::result();
}
