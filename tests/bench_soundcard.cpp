// Measuring program (not a CTest test): plays through Audio::Player on the real sound card and records what the
// system really sends to it (WASAPI loopback of the default output, so Windows only), then counts clicks and checks
// the level – the Windows test of the own audio engine with WASAPI and DirectSound (docs/dev/testen.md).
// The sound is a 440-Hz sine at -40 dBFS (quiet). Scenarios: steady play, jumps, pause/play, 2x, backwards.
// Run: ./build-win/tests/bench_soundcard [backend: wasapi|directsound|""] [buffer] [seconds of steady play]
//      Env BENCH_SOUNDCARD_KEEP=<file.raw>: keeps the recording (float, channels and rate as printed)
#include "check.h"

#include "engine/audio/Miniaudio.h"
#include "engine/audio/Player.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr double kAmp = 0.01; // -40 dBFS
constexpr double kFreq = 440;

struct Recorder {
    std::mutex lock;
    std::vector<float> data; // interleaved
    ma_uint32 channels = 0, rate = 0;
    ma_device device{};
    bool open = false;

    static void callback(ma_device* d, void*, const void* in, ma_uint32 frames)
    {
        auto* self = static_cast<Recorder*>(d->pUserData);
        const auto* f = static_cast<const float*>(in);
        std::lock_guard<std::mutex> g(self->lock);
        self->data.insert(self->data.end(), f, f + size_t(frames) * self->channels);
    }
    bool start()
    {
        ma_backend wasapi = ma_backend_wasapi;
        ma_device_config c = ma_device_config_init(ma_device_type_loopback);
        c.capture.format = ma_format_f32;
        c.capture.channels = 0; // native
        c.sampleRate = 0;
        c.dataCallback = callback;
        c.pUserData = this;
        static ma_context context;
        if (ma_context_init(&wasapi, 1, nullptr, &context) != MA_SUCCESS) return false;
        if (ma_device_init(&context, &c, &device) != MA_SUCCESS) return false;
        channels = device.capture.channels;
        rate = device.sampleRate;
        open = ma_device_start(&device) == MA_SUCCESS;
        return open;
    }
    void stop()
    {
        if (open) ma_device_uninit(&device);
        open = false;
    }
    size_t frames()
    {
        std::lock_guard<std::mutex> g(lock);
        return data.size() / channels;
    }
};

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    Check::initApp("bench-soundcard");
    const QStringList args = app.arguments().mid(1);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QString backend = args.value(0);
    const int buffer = args.size() > 1 ? args[1].toInt() : 2048;
    const int steady = args.size() > 2 ? args[2].toInt() : 8;

    QTemporaryDir tmp;
    const QString tone = Check::makeMedia(
        tmp.filePath("tone.wav"),
        {"-f", "lavfi", "-i", QString("aevalsrc=%1*sin(2*PI*%2*t)|0:s=48000:d=60").arg(kAmp).arg(kFreq), "-c:a",
         "pcm_f32le"}); // left only: the right channel must stay silent (stereo kept, no downmix)
    if (tone.isEmpty()) return 1;
    Timeline tl;
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = tone;
    c.out = 59 * 30;
    tl.audio[0].clips << c;

    Recorder rec;
    if (!rec.start()) {
        std::printf("Loopback-Mitschnitt geht nicht (nur WASAPI)\n");
        return 1;
    }
    Audio::Player::Options opt;
    opt.backend = backend;
    opt.buffer = buffer;
    Audio::Player player(opt);
    player.setTimeline(tl, FrameRate{30, 1});
    sleepMs(300);

    // Scenario marks in recorded frames
    std::vector<std::pair<size_t, const char*>> marks;
    auto mark = [&](const char* what) { marks.emplace_back(rec.frames(), what); };
    mark("Start");
    player.play(0, 1.0);
    player.release();
    sleepMs(500);
    const size_t steadyFrom = rec.frames();
    sleepMs(steady * 1000);
    const size_t steadyTo = rec.frames();
    std::printf("Gerät: %s%s\n", qPrintable(player.deviceName()),
                player.deviceError().isEmpty() ? "" : qPrintable(" – " + player.deviceError()));
    mark("Sprünge");
    for (int i = 0; i < 6; ++i) {
        player.play(48000.0 * (5 + 3 * i) + 1234 * i, 1.0);
        player.release();
        sleepMs(600);
    }
    mark("Pause/Play");
    for (int i = 0; i < 4; ++i) {
        player.pause();
        sleepMs(300);
        player.play(player.position(), 1.0);
        player.release();
        sleepMs(500);
    }
    mark("2x");
    player.play(player.position(), 2.0);
    player.release();
    sleepMs(1000);
    mark("rückwärts");
    player.play(player.position(), -1.0);
    player.release();
    sleepMs(1500);
    mark("Stopp");
    player.pause();
    sleepMs(500);
    const Audio::Player::Stats stats = player.stats();
    rec.stop();

    const std::vector<float>& d = rec.data;
    const size_t frames = d.size() / rec.channels;
    std::printf("Mitschnitt: %zu Kanäle, %u Hz, %.2f s; Treiber %s, Puffer %d\n", size_t(rec.channels), rec.rate,
                frames / double(rec.rate), backend.isEmpty() ? "automatisch" : qPrintable(backend), buffer);
    if (const QString keep = qEnvironmentVariable("BENCH_SOUNDCARD_KEEP"); !keep.isEmpty()) {
        QFile f(keep);
        if (f.open(QIODevice::WriteOnly)) f.write(reinterpret_cast<const char*>(d.data()), qint64(d.size() * 4));
    }
    if (frames < 1000) return 1;

    // Level per channel in the steady part against the sine's: left 0 dB, everything else silent
    std::printf("Pegel (gleichmäßig, dB gegen den Sinus, links soll 0 sein, Rest still):");
    for (ma_uint32 ch = 0; ch < rec.channels; ++ch) {
        double sum = 0;
        for (size_t i = steadyFrom; i < steadyTo; ++i) sum += double(d[i * rec.channels + ch]) * d[i * rec.channels + ch];
        const double rms = std::sqrt(sum / double(std::max<size_t>(1, steadyTo - steadyFrom)));
        const double db = 20 * std::log10(std::max(rms, 1e-12) / (kAmp / std::sqrt(2.0)));
        std::printf(db < -120 ? " still" : " %.2f", db);
    }
    std::printf("\n");

    // Clicks: second difference of a sine is tiny (A·(2πf/R)²); a step, gap or dropout makes it about A.
    // The 5-ms fades at jumps/stops stay far below the threshold.
    const double w = 2 * M_PI * kFreq / rec.rate, smooth = kAmp * w * w, threshold = std::max(20 * smooth, kAmp * 0.05);
    auto scenarioOf = [&](size_t at) {
        const char* s = "";
        for (const auto& [from, name] : marks)
            if (at >= from) s = name;
        return s;
    };
    int clicks = 0, steadyClicks = 0;
    double largest = 0;
    size_t lastClick = 0;
    for (size_t i = 1; i + 1 < frames; ++i) {
        const double x = d[(i - 1) * rec.channels], y = d[i * rec.channels], z = d[(i + 1) * rec.channels];
        const double dd = std::abs(x - 2 * y + z);
        largest = std::max(largest, i >= steadyFrom && i < steadyTo ? dd : 0.0);
        if (dd > threshold) {
            if (i - lastClick > rec.rate / 100) { // one per 10 ms
                ++clicks;
                if (i >= steadyFrom && i < steadyTo) ++steadyClicks;
                if (clicks <= 15)
                    std::printf("  Knack bei %.4f s (%s): %.5f\n", i / double(rec.rate), scenarioOf(i), dd);
            }
            lastClick = i;
        }
    }
    std::printf("Knacken: %d gesamt, %d im gleichmäßigen Teil (Schwelle %.5f, größte 2. Differenz dort %.6f, Sinus "
                "selbst %.6f)\n",
                clicks, steadyClicks, threshold, largest, smooth);
    std::printf("Player: %d Aussetzer, %d Ausblendungen, %d× neu geöffnet\n", stats.underruns, stats.fadeOuts,
                stats.reopens);
    return 0;
}
