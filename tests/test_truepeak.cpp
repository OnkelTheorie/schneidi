// Test True Peak (ITU-R BS.1770-4 Annex 2, dBTP): TruePeakMeter mit synthetischen Signalen (fs/4 mit 45°
// Phasenversatz: Sample Peak 3 dB unter dem echten Spitzenwert), Häppchen-Verarbeitung, und die Messung am
// Master-Pegelmesser der Vorschau (_true_peak.N und SharedLoudness).
#include "check.h"

#include "core/Loudness.h"
#include "core/TimelineOps.h"
#include "core/TruePeak.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Stereo-Sinus (verschachtelt), Amplitude linear, Frequenz in Hz, Phase in Bogenmaß
std::vector<float> sine(double freq, double amp, double phase, int frames, int rate, int channels = 2)
{
    std::vector<float> v(size_t(frames) * channels);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            v[size_t(i) * channels + c] = float(amp * std::sin(2 * kPi * freq * i / rate + phase));
    return v;
}

double samplePeakDb(const std::vector<float>& v)
{
    double p = 0;
    for (float x : v) p = std::max(p, double(std::abs(x)));
    return TruePeakMeter::toDb(p);
}

// Stereo-Signal einspeisen, Spitzenwerte aber erst nach dem Einschwingen messen: der abrupte Einsatz aus der
// Stille schwingt im Interpolationsfilter über (echtes Intersample-Überschwingen, hier nicht gemeint)
void feedSteady(TruePeakMeter& m, const std::vector<float>& v, int rate)
{
    const int frames = int(v.size() / 2), warm = 100;
    m.addInterleaved(v.data(), warm, 2, rate);
    m.resetPeaks();
    m.addInterleaved(v.data() + 2 * warm, frames - warm, 2, rate);
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("truepeak");

    // --- Filter: jede Phase hat Gleichanteil-Verstärkung ~1 (Interpolation, keine Pegeländerung)
    for (int p = 0; p < TruePeakMeter::kPhases; ++p) {
        double sum = 0;
        for (double c : TruePeakMeter::kCoef[p]) sum += c;
        CHECK(near(sum, 1.0, 0.03));
    }
    // Phasen 0/3 und 1/2 sind gespiegelt (symmetrisches 48-Tap-FIR)
    for (int k = 0; k < TruePeakMeter::kTaps; ++k) {
        CHECK_EQ(TruePeakMeter::kCoef[0][k], TruePeakMeter::kCoef[3][TruePeakMeter::kTaps - 1 - k]);
        CHECK_EQ(TruePeakMeter::kCoef[1][k], TruePeakMeter::kCoef[2][TruePeakMeter::kTaps - 1 - k]);
    }

    // --- Leer/Stille
    {
        TruePeakMeter m;
        CHECK_EQ(m.maxDb(), TruePeakMeter::kSilence);
        const std::vector<float> zero(4800 * 2, 0.f);
        m.addInterleaved(zero.data(), 4800, 2, 48000);
        CHECK_EQ(m.peakDb(0), TruePeakMeter::kSilence);
        CHECK_EQ(m.channels(), 2);
    }

    // --- fs/4 mit 45° Phase (EBU Tech 3341, Fall 15–18 sinngemäß): alle Samples liegen bei ±A·0,707, der Sinus
    // erreicht dazwischen A. Sample Peak -3,01 dB unter dem True Peak; Toleranz der Norm -0,4/+0,2 dB.
    for (int rate : {48000, 44100}) {
        for (double ampDb : {0.0, -6.0, -20.0}) {
            const double amp = std::pow(10.0, ampDb / 20);
            const auto v = sine(rate / 4.0, amp, kPi / 4, rate, rate);
            TruePeakMeter m;
            feedSteady(m, v, rate);
            const double sp = samplePeakDb(v), tp = m.maxDb();
            CHECK(near(sp, ampDb - 3.01, 0.01));
            if (!CHECK(tp >= ampDb - 0.4 && tp <= ampDb + 0.2))
                std::printf("       %d Hz, %.0f dB: TP %.3f dBTP, SP %.3f dBFS\n", rate, ampDb, tp, sp);
            CHECK(near(m.peakDb(0), m.peakDb(1), 1e-9));
            CHECK(tp > sp + 2.5); // Sample Peak unterschätzt deutlich
        }
    }
    // Over-Anzeige: Sample Peak -3 dBFS sieht harmlos aus, True Peak liegt über -1 dBTP
    {
        const auto v = sine(12000, 1.0, kPi / 4, 48000, 48000);
        TruePeakMeter m;
        feedSteady(m, v, 48000);
        CHECK(samplePeakDb(v) < -1.0);
        CHECK(m.maxDb() > -1.0);
    }

    // --- fs/4 ohne Phasenversatz: Samples treffen den Gipfel, True Peak = Sample Peak (0 dB)
    {
        const auto v = sine(12000, 1.0, 0.0, 48000, 48000);
        TruePeakMeter m;
        feedSteady(m, v, 48000);
        if (!CHECK(near(m.maxDb(), 0.0, 0.01))) std::printf("       TP %.4f\n", m.maxDb());
    }

    // --- Tiefe Frequenz (1 kHz, -6 dBFS): Interpolation ändert praktisch nichts
    {
        const auto v = sine(997, 0.5, 0.3, 48000, 48000);
        TruePeakMeter m;
        feedSteady(m, v, 48000);
        if (!CHECK(near(m.maxDb(), -6.02, 0.05))) std::printf("       TP %.3f\n", m.maxDb());
        CHECK(m.maxDb() >= samplePeakDb(v));
    }

    // --- Kanäle getrennt, planar
    {
        std::vector<float> planar(2 * 4800, 0.f);
        const auto s = sine(12000, 0.5, kPi / 4, 4800, 48000, 1);
        std::copy(s.begin(), s.end(), planar.begin()); // nur links
        TruePeakMeter m;
        m.addPlanar(planar.data(), 4800, 2, 48000);
        CHECK(near(m.peakDb(0), -6.02, 0.3));
        CHECK_EQ(m.peakDb(1), TruePeakMeter::kSilence);
        CHECK(near(m.maxDb(), m.peakDb(0), 1e-12));
    }

    // --- Häppchen = ein Strom: dasselbe Ergebnis wie am Stück; resetPeaks behält den Filterverlauf
    {
        const int n = 48000;
        const auto v = sine(9000, 0.8, 1.0, n, 48000);
        TruePeakMeter whole, parts, perChunk;
        whole.addInterleaved(v.data(), n, 2, 48000);
        double chunkMin = 1e9;
        for (int i = 0; i < n;) {
            const int len = std::min(n - i, 1 + (i * 7) % 1601); // ungleich große Häppchen
            parts.addInterleaved(v.data() + size_t(i) * 2, len, 2, 48000);
            perChunk.resetPeaks();
            perChunk.addInterleaved(v.data() + size_t(i) * 2, len, 2, 48000);
            if (i > 0 && len >= 64) chunkMin = std::min(chunkMin, perChunk.maxDb());
            i += len;
        }
        CHECK_EQ(parts.maxDb(), whole.maxDb());
        // jedes längere Häppchen sieht den vollen Spitzenwert (kein Einschwingen an der Grenze)
        if (!CHECK(chunkMin > whole.maxDb() - 0.3)) std::printf("       min %.3f / %.3f\n", chunkMin, whole.maxDb());
        whole.reset();
        CHECK_EQ(whole.maxDb(), TruePeakMeter::kSilence);
    }

    // --- Vorschau: Master-Pegelmesser setzt _true_peak.N, Live-Messung (SharedLoudness) merkt sich das Maximum
    Check::initMlt();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir tmp;
    const QString wav = Check::makeMedia(
        tmp.filePath("fs4.wav"),
        {"-f", "lavfi", "-i", "aevalsrc=0.5*sin(2*PI*12000*t+PI/4)|0.5*sin(2*PI*12000*t+PI/4):s=48000:d=4",
         "-c:a", "pcm_f32le"});
    CHECK(!wav.isEmpty());
    if (wav.isEmpty()) return Check::result();
    ProjectFormat fmt;
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.audio[0].clips << mk(1, wav, 0, 0, 74);
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    MixerHooks hooks;
    auto tr = b.build(tl, &hooks);
    CHECK(hooks.master.meter != nullptr && hooks.tracks.size() == 1);
    if (!hooks.master.meter || hooks.tracks.size() != 1) return Check::result();
    SharedLoudness live;
    hooks.master.meter->set("_loudness", &live, 0);
    live.active = true;
    tr->seek(10);
    double tpMin = 1e9, spMax = -1e9, trackTp = -1e9;
    for (int pos = 10; pos < 60; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format afmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        f->get_audio(afmt, freq, channels, samples);
        tpMin = std::min(tpMin, hooks.master.meter->get_double("_true_peak.0"));
        spMax = std::max(spMax, hooks.master.meter->get_double("_audio_level.0"));
        trackTp = std::max(trackTp, hooks.tracks[0].meter->get_double("_true_peak.1"));
    }
    if (!CHECK(near(tpMin, -6.02, 0.4) && near(spMax, -9.03, 0.1)))
        std::printf("       master TP %.2f dBTP, SP %.2f dBFS\n", tpMin, spMax);
    CHECK(near(trackTp, -6.02, 0.4));
    {
        std::lock_guard<std::mutex> lock(live.mutex);
        if (!CHECK(near(live.truePeak.maxDb(), -6.02, 0.4))) std::printf("       live %.2f\n", live.truePeak.maxDb());
    }
    return Check::result();
}
