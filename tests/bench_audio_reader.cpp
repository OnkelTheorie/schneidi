// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): the own audio reader (engine/audio/Reader, docs/archiv/plan-0.10-ton.md step 1) against
// MLT's avformat producer on real files. Per file and audio stream: time to open (until the first samples), decoding
// speed, time per jump, and whether both deliver the same samples (lag, difference).
// Run: QT_QPA_PLATFORM=offscreen ./build-tests/tests/bench_audio_reader <media files…> [--seconds 30] [--reps 5]
#include "check.h"

#include "engine/audio/Reader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <mlt++/Mlt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFps = 25;
constexpr int kBlock = kRate / kFps;

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}

std::unique_ptr<Mlt::Producer> mltOpen(Mlt::Profile& profile, const QString& path, int stream)
{
    auto p = std::make_unique<Mlt::Producer>(profile, path.toUtf8().constData());
    if (!p->is_valid()) return nullptr;
    int n = 0;
    for (int i = 0; i < p->get_int("meta.media.nb_streams"); ++i)
        if (QByteArray(p->get(QString("meta.media.%1.stream.type").arg(i).toUtf8().constData())) == "audio" && n++ == stream)
            p->set("audio_index", i);
    return p;
}

// Channel 0 of frame `pos` from MLT (at the current position), appended to `out`
void mltFrame(Mlt::Producer& p, int pos, std::vector<float>* out)
{
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    mlt_audio_format fmt = mlt_audio_float;
    int freq = kRate, ch = 2;
    int samples = mlt_audio_calculate_frame_samples(float(kFps), freq, pos);
    const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, ch, samples));
    if (out)
        for (int i = 0; i < samples; ++i) out->push_back(pcm && fmt == mlt_audio_float ? pcm[i] : 0.f);
}

void bench(const QString& path, int stream, double seconds, int reps)
{
    Mlt::Profile profile("atsc_1080p_25");
    profile.set_frame_rate(kFps, 1);
    std::vector<double> openOurs, openMlt;
    std::unique_ptr<Audio::Reader> r;
    for (int i = 0; i < reps; ++i) {
        QElapsedTimer t;
        t.start();
        r = Audio::Reader::open(path, stream, {kRate, 2});
        if (!r) {
            std::printf("  stream %d: cannot open\n", stream);
            return;
        }
        std::vector<float> l(kBlock), rr(kBlock);
        float* ptr[] = {l.data(), rr.data()};
        r->read(0, kBlock, ptr);
        openOurs.push_back(t.nsecsElapsed() / 1e6);
        t.restart();
        auto m = mltOpen(profile, path, stream);
        if (!m) return;
        m->seek(0);
        mltFrame(*m, 0, nullptr);
        openMlt.push_back(t.nsecsElapsed() / 1e6);
    }
    const Audio::StreamInfo& info = r->info();
    const int frames = int(std::min<double>(seconds, double(info.end) / kRate) * kFps);

    // Sequential from position 0
    std::vector<float> ours, mlt;
    QElapsedTimer t;
    t.start();
    {
        auto seq = Audio::Reader::open(path, stream, {kRate, 2});
        std::vector<float> l(kBlock + 2), rr(kBlock + 2);
        float* ptr[] = {l.data(), rr.data()};
        int64_t at = 0;
        for (int pos = 0; pos < frames; ++pos) {
            const int n = mlt_audio_calculate_frame_samples(float(kFps), kRate, pos);
            seq->read(at, n, ptr);
            ours.insert(ours.end(), l.begin(), l.begin() + n);
            at += n;
        }
    }
    const double seqOurs = t.nsecsElapsed() / 1e6;
    t.restart();
    {
        auto m = mltOpen(profile, path, stream);
        m->seek(0);
        for (int pos = 0; pos < frames; ++pos) mltFrame(*m, pos, &mlt);
    }
    const double seqMlt = t.nsecsElapsed() / 1e6;

    // Jumps (like clicking around in the timeline)
    QRandomGenerator rng(17);
    std::vector<int> targets;
    for (int i = 0; i < 40; ++i) targets.push_back(int(rng.bounded(quint32(std::max(1, frames - 1)))));
    t.restart();
    {
        std::vector<float> l(kBlock), rr(kBlock);
        float* ptr[] = {l.data(), rr.data()};
        for (int f : targets) r->read(int64_t(f) * kBlock, kBlock, ptr);
    }
    const double jumpOurs = t.nsecsElapsed() / 1e6 / double(targets.size());
    t.restart();
    {
        auto m = mltOpen(profile, path, stream);
        for (int f : targets) {
            m->seek(f);
            mltFrame(*m, f, nullptr);
        }
    }
    const double jumpMlt = t.nsecsElapsed() / 1e6 / double(targets.size());

    // Same samples? Lag of MLT against ours (within ±0.1 s), difference after that
    const size_t n = std::min(ours.size(), mlt.size());
    int lag = 0;
    double best = 1e300;
    for (int l = -4800; l <= 4800 && n > 20000; ++l) {
        double e = 0;
        for (size_t i = 9600; i + 9600 < std::min<size_t>(n, 200000); i += 5) e += std::abs(ours[i] - mlt[size_t(int64_t(i) + l)]);
        if (e < best) {
            best = e;
            lag = l;
        }
    }
    double diff = 0;
    for (size_t i = 4800; i + 4800 < n; ++i) diff = std::max(diff, double(std::abs(ours[i] - mlt[size_t(int64_t(i) + lag)])));

    const Audio::Reader::Stats& st = r->stats();
    std::printf("  stream %d: %s %d Hz %d ch, starts %+.2f ms, %.1f s\n", stream, qPrintable(info.codec), info.sourceRate,
                info.sourceChannels, info.start * 1000.0 / kRate, double(info.end) / kRate);
    std::printf("    open:  %7.2f ms   (MLT %7.2f ms)\n", median(openOurs), median(openMlt));
    std::printf("    %4.0f s: %7.1f ms   (MLT %7.1f ms) = %.0fx / %.0fx real time\n", frames / double(kFps), seqOurs, seqMlt,
                frames / double(kFps) / seqOurs * 1000, frames / double(kFps) / seqMlt * 1000);
    std::printf("    jump:  %7.2f ms   (MLT %7.2f ms)\n", jumpOurs, jumpMlt);
    std::printf("    MLT lag %d samples (%.2f ms), largest difference %.3g; gaps %d, overlaps %d, errors %d\n", lag,
                lag * 1000.0 / kRate, diff, st.gaps, st.overlaps, st.decodeErrors);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("bench_audio_reader");
    Check::initMlt();
    double seconds = 30;
    int reps = 5;
    QStringList files;
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size()) seconds = args[++i].toDouble();
        else if (args[i] == "--reps" && i + 1 < args.size()) reps = args[++i].toInt();
        else files << args[i];
    }
    if (files.isEmpty()) {
        std::printf("usage: bench_audio_reader <media files…> [--seconds 30] [--reps 5]\n");
        return 1;
    }
    for (const QString& f : files) {
        std::printf("%s\n", qPrintable(f));
        for (int s = 0; s < 8; ++s) {
            if (!Audio::Reader::open(f, s)) break;
            bench(f, s, seconds, reps);
        }
    }
    return 0;
}
