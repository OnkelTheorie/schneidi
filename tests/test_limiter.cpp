// SPDX-License-Identifier: AGPL-3.0-only
// Test master limiter (PeakLimiter): holds the true peak ceiling, leaves quiet audio unchanged (only delayed),
// same result in any block size and after a state copy; in the timeline the export path (integer samples like the
// avformat consumer requests) and the measurement (float) give the same level.
#include "check.h"

#include "core/Limiter.h"
#include "core/TruePeak.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Planar stereo: loud bursts with intersample peaks (fs/4 at 45°) and a low tone, then quiet
std::vector<float> testSignal(int frames)
{
    std::vector<float> v(size_t(frames) * 2);
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / kRate;
        const bool burst = std::fmod(t, 0.25) < 0.08;
        const double x = (burst ? 1.8 * std::sin(2 * kPi * i / 4.0 + kPi / 4) : 0.0) + 0.6 * std::sin(2 * kPi * 80 * t);
        v[size_t(i)] = float(x);
        v[size_t(frames + i)] = float(0.9 * x);
    }
    return v;
}

void processInBlocks(PeakLimiter& lim, std::vector<float>& planar, int frames, const std::vector<int>& sizes)
{
    // The limiter works on one planar block at a time: copy each block out and back
    std::vector<float> block;
    for (int pos = 0, k = 0; pos < frames; ++k) {
        const int n = std::min(sizes[size_t(k) % sizes.size()], frames - pos);
        block.assign(size_t(n) * 2, 0.f);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) block[size_t(c * n + i)] = planar[size_t(c * frames + pos + i)];
        lim.processPlanar(block.data(), n, 2, kRate);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) planar[size_t(c * frames + pos + i)] = block[size_t(c * n + i)];
        pos += n;
    }
}

struct Levels {
    double peakDb = -200, rmsDb = -200;
};

// Timeline audio [from, to] as float (the measurement) or as interleaved s32 (what the export's consumer asks for)
Levels renderLevels(const Timeline& tl, int from, int to, mlt_audio_format want)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl, nullptr);
    tr->seek(from);
    double peak = 0, sum = 0;
    long count = 0;
    for (int pos = from; pos <= to; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format fmt = want;
        int freq = kRate, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        const void* data = f->get_audio(fmt, freq, channels, samples);
        if (!data || fmt != want) continue;
        for (int i = 0; i < samples * channels; ++i) {
            const double x = want == mlt_audio_s32le ? static_cast<const int32_t*>(data)[i] / 2147483648.0
                                                     : static_cast<const float*>(data)[i];
            peak = std::max(peak, std::abs(x));
            sum += x * x;
            ++count;
        }
    }
    Levels l;
    if (peak > 1e-10) l.peakDb = 20 * std::log10(peak);
    if (count && sum > 0) l.rmsDb = 10 * std::log10(sum / count);
    return l;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("limiter");

    const int frames = kRate * 2;
    const std::vector<float> input = testSignal(frames);

    // --- Holds the true peak ceiling (-1 dBTP), measured like the loudness meter
    for (double ceiling : {-1.0, -3.0}) {
        std::vector<float> out = input;
        PeakLimiter lim;
        lim.setCeilingDb(ceiling);
        processInBlocks(lim, out, frames, {800});
        TruePeakMeter tp;
        tp.addPlanar(out.data(), frames, 2, kRate);
        if (!CHECK(tp.maxDb() <= ceiling + 0.01 && tp.maxDb() > ceiling - 1.0))
            std::printf("       ceiling %.1f: true peak %.2f dBTP\n", ceiling, tp.maxDb());
    }

    // --- Block size does not matter (the state carries over)
    {
        std::vector<float> a = input, b = input;
        PeakLimiter la, lb;
        la.setCeilingDb(-1.0);
        lb.setCeilingDb(-1.0);
        processInBlocks(la, a, frames, {800});
        processInBlocks(lb, b, frames, {1, 37, 801, 4096, 250});
        double maxDiff = 0;
        for (size_t i = 0; i < a.size(); ++i) maxDiff = std::max(maxDiff, double(std::abs(a[i] - b[i])));
        CHECK(maxDiff < 1e-6);
    }
    {
        // Same frame again (preview): a copy of the state before it gives the identical frame
        std::vector<float> first = input, again = input;
        PeakLimiter lim;
        lim.setCeilingDb(-1.0);
        const PeakLimiter before = lim;
        lim.processPlanar(first.data(), frames, 2, kRate);
        PeakLimiter redo = before;
        redo.processPlanar(again.data(), frames, 2, kRate);
        CHECK(first == again);
    }

    // --- Quiet audio passes unchanged, only delayed by latency()
    {
        std::vector<float> quiet(size_t(frames) * 2);
        for (int i = 0; i < frames; ++i)
            quiet[size_t(i)] = quiet[size_t(frames + i)] = float(0.3 * std::sin(2 * kPi * 440.0 * i / kRate));
        std::vector<float> out = quiet;
        PeakLimiter lim;
        lim.setCeilingDb(-1.0);
        processInBlocks(lim, out, frames, {800});
        const int d = lim.latency();
        CHECK(d > 0 && d < kRate / 100); // under 10 ms
        double maxDiff = 0;
        for (int c = 0; c < 2; ++c)
            for (int i = d; i < frames; ++i)
                maxDiff = std::max(maxDiff, double(std::abs(out[size_t(c * frames + i)] - quiet[size_t(c * frames + i - d)])));
        CHECK(maxDiff < 1e-6);
    }

    // --- Timeline: +12 dB over full scale; export (s32) and measurement (float) agree, the ceiling holds
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString tone = Check::makeMedia(tmp.filePath("tone.wav"),
                                          {"-f", "lavfi", "-i",
                                           "aevalsrc=0.5*sin(2*PI*440*t)*(0.6+0.4*sin(2*PI*3*t))|0.5*sin(2*PI*440*t)*(0.6+0.4*sin(2*PI*3*t)):s=48000:d=3",
                                           "-c:a", "pcm_f32le"});
    if (!CHECK(!tone.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = tone;
    c.start = 0;
    c.in = 0;
    c.out = 74;
    c.volumeDb = 12;
    tl.audio[0].clips << c;
    tl.masterLimiter = true;
    tl.masterLimiterDb = -1.0;
    const Levels measured = renderLevels(tl, 5, 70, mlt_audio_float);
    const Levels exported = renderLevels(tl, 5, 70, mlt_audio_s32le);
    if (!CHECK(near(measured.rmsDb, exported.rmsDb, 0.05)))
        std::printf("       RMS float %.2f, s32 %.2f dB\n", measured.rmsDb, exported.rmsDb);
    CHECK(measured.peakDb <= -0.99 && exported.peakDb <= -0.99);
    CHECK(measured.peakDb > -3.0);
    // Off: the clip is louder than full scale (float keeps it, nothing clipped before the master)
    tl.masterLimiter = false;
    CHECK(renderLevels(tl, 5, 70, mlt_audio_float).peakDb > 3.0);
    return Check::result();
}
