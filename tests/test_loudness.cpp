// Test loudness (ITU-R BS.1770-4 / EBU R128): LoudnessMeter with synthetic signals, comparison with ffmpeg ebur128,
// clip measurement (AudioAnalysis::clipLoudness), normalizing to LUFS (editor, relative via common loudness),
// live measurement at the master level meter of the preview.
#include "check.h"

#include "app/NormalizeDialog.h"

#include "core/Editor.h"
#include "core/Loudness.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

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

// Stereo sine (same signal on both channels), amplitude in dBFS, in chunks like a player
void feedSine(LoudnessMeter& m, double freq, double dbfs, double seconds, int rate, int channels = 2)
{
    const double amp = std::pow(10.0, dbfs / 20.0);
    const int total = int(seconds * rate), chunk = 1920;
    std::vector<float> buf;
    for (int start = 0; start < total; start += chunk) {
        const int n = std::min(chunk, total - start);
        buf.assign(size_t(n) * channels, 0.f);
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
                buf[size_t(i) * channels + c] = float(amp * std::sin(2 * kPi * freq * (start + i) / rate));
        m.addInterleaved(buf.data(), n, channels, rate);
    }
}

// Integrated loudness according to ffmpeg ebur128 (summary "I: -23.0 LUFS"); NaN on error
double ffmpegIntegrated(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-nostdin", "-nostats", "-hide_banner", "-i", path, "-filter_complex", "ebur128", "-f", "null", "-"});
    if (!p.waitForFinished(120000)) return std::nan("");
    const QString err = QString::fromUtf8(p.readAllStandardError());
    const int summary = err.lastIndexOf("Summary:");
    const auto m = QRegularExpression(R"(I:\s+(-?[0-9.]+) LUFS)").match(err, std::max(0, summary));
    return m.hasMatch() ? m.captured(1).toDouble() : std::nan("");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("loudness");

    // --- Meter: reference values of the standard
    {
        // EBU Tech 3341: 1 kHz stereo sine with -23 dBFS per channel = -23 LUFS
        LoudnessMeter m;
        CHECK_EQ(m.integrated(), LoudnessMeter::kSilence);
        CHECK_EQ(m.momentary(), LoudnessMeter::kSilence);
        feedSine(m, 1000, -23, 20, 48000);
        if (!CHECK(near(m.integrated(), -23.0, 0.1))) std::printf("       I = %.2f\n", m.integrated());
        CHECK(near(m.momentary(), -23.0, 0.1));
        CHECK(near(m.shortTerm(), -23.0, 0.1));
        CHECK(near(m.range(), 0.0, 0.1)); // equally loud: LRA 0
        CHECK(near(m.measuredSeconds(), 20.0, 0.01));
        // Other sample rate, same loudness
        LoudnessMeter m44;
        feedSine(m44, 1000, -23, 10, 44100);
        CHECK(near(m44.integrated(), -23.0, 0.1));
        // Mono: half the energy = 3 dB quieter
        LoudnessMeter mono;
        feedSine(mono, 1000, -23, 10, 48000, 1);
        CHECK(near(mono.integrated(), -26.0, 0.1));
        // K-filter: 100 Hz counts clearly less than 1 kHz, 5 kHz more (high shelf)
        LoudnessMeter low, high;
        feedSine(low, 100, -23, 5, 48000);
        feedSine(high, 5000, -23, 5, 48000);
        CHECK(low.integrated() < -23.5 && high.integrated() > -21.0);
        // Too short (< 400 ms): no measurement yet
        LoudnessMeter shortM;
        feedSine(shortM, 1000, -10, 0.3, 48000);
        CHECK_EQ(shortM.integrated(), LoudnessMeter::kSilence);
        // Reset
        m.reset();
        CHECK_EQ(m.integrated(), LoudnessMeter::kSilence);
        CHECK_EQ(m.measuredSeconds(), 0.0);
    }
    // Gates (EBU Tech 3341 case 3 in spirit): 10 s -36 dBFS, 60 s -23, 10 s -36 -> gate omits the quiet parts
    {
        LoudnessMeter m;
        feedSine(m, 1000, -36, 10, 48000);
        feedSine(m, 1000, -23, 60, 48000);
        feedSine(m, 1000, -36, 10, 48000);
        CHECK(near(m.integrated(), -23.0, 0.1));
        // Silence is never counted (absolute gate)
        LoudnessMeter s;
        feedSine(s, 1000, -23, 10, 48000);
        feedSine(s, 1000, -120, 30, 48000);
        CHECK(near(s.integrated(), -23.0, 0.1));
        // LRA: two levels 10 dB apart (EBU Tech 3342 case 2: -20/-30 -> 10 LU)
        LoudnessMeter r;
        feedSine(r, 1000, -20, 20, 48000);
        feedSine(r, 1000, -30, 20, 48000);
        if (!CHECK(near(r.range(), 10.0, 0.2))) std::printf("       LRA = %.2f\n", r.range());
        // Common measurement from blocks of two parts = measurement in one piece
        LoudnessMeter a, b, ab;
        feedSine(a, 1000, -20, 5, 48000);
        feedSine(b, 1000, -26, 5, 48000);
        feedSine(ab, 1000, -20, 5, 48000);
        feedSine(ab, 1000, -26, 5, 48000);
        std::vector<double> blocks = a.blocks();
        blocks.insert(blocks.end(), b.blocks().begin(), b.blocks().end());
        CHECK(near(LoudnessMeter::integratedOf(blocks), ab.integrated(), 0.1));
        CHECK_EQ(LoudnessMeter::integratedOf({}), LoudnessMeter::kSilence);
    }

    // --- Normalizing to LUFS (editor): measured values are given
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.edit("setup", [](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
            tl.audio[0].clips = {mk(1, "/x/a.wav", 0, 0, 99), mk(2, "/x/b.wav", 100, 0, 99)};
        });
        auto vol = [&](int id) { return TimelineOps::findClip(p.timeline(), id)->volumeDb; };
        const int before = p.undoStack()->index();
        ed.normalizeAudio({{1, -20.0}, {2, -30.0}}, -14.0, false);
        CHECK(near(vol(1), 6.0, 1e-9) && near(vol(2), 12.0, 1e-9)); // +16 is limited to +12
        CHECK_EQ(p.undoStack()->index(), before + 1);
        p.undoStack()->undo();
        // Relative via the common loudness: both by the same amount, reference -24 LUFS -> +10 dB
        ed.normalizeAudio({{1, -20.0}, {2, -30.0}}, -14.0, true, -24.0);
        CHECK(near(vol(1), 10.0, 1e-9) && near(vol(2), 10.0, 1e-9));
        // Measured jointly silent: do nothing
        const int n = p.undoStack()->index();
        ed.normalizeAudio({{1, -20.0}}, -14.0, true, LoudnessMeter::kSilence);
        CHECK_EQ(p.undoStack()->index(), n);
    }

    // --- Dialog: switching mode remembers the target value per unit, preset sets the value, settings stay
    {
        QSettings().remove("audio"); // test settings (QStandardPaths test mode) from the last run
        NormalizeDialog dlg(2);
        auto* mode = dlg.findChildren<QComboBox*>().value(0);
        auto* preset = dlg.findChildren<QComboBox*>().value(1);
        auto* target = dlg.findChild<QDoubleSpinBox*>();
        if (CHECK(mode && preset && target)) {
            CHECK(dlg.mode() == NormalizeDialog::Mode::SamplePeak);
            CHECK_EQ(dlg.targetDb(), -9.0);
            mode->setCurrentIndex(1);
            CHECK(dlg.mode() == NormalizeDialog::Mode::Loudness);
            CHECK_EQ(dlg.targetDb(), -14.0);
            CHECK_EQ(target->suffix(), QString(" LUFS"));
            preset->activated(2); // EBU R128
            CHECK_EQ(dlg.targetDb(), -23.0);
            target->setValue(-20.0);
            CHECK_EQ(preset->currentIndex(), preset->count() - 1); // own value
            mode->setCurrentIndex(0);
            CHECK_EQ(dlg.targetDb(), -9.0);
            mode->setCurrentIndex(1);
            CHECK_EQ(dlg.targetDb(), -20.0);
            dlg.accept();
        }
        NormalizeDialog again(1);
        CHECK(again.mode() == NormalizeDialog::Mode::Loudness);
        CHECK_EQ(again.targetDb(), -20.0);
    }

    // --- With real media (ffmpeg)
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Pink noise + sine with level jump (broadband, K-filter acts) - comparison with ffmpeg ebur128
    const QString mix = Check::makeMedia(
        tmp.filePath("mix.wav"),
        {"-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.2:duration=8:seed=7:sample_rate=48000",
         "-f", "lavfi", "-i", "aevalsrc=if(lt(t\\,4)\\,0.05\\,0.3)*sin(2*PI*220*t):s=48000:d=8",
         "-filter_complex", "[0][1]amix=inputs=2:normalize=0,pan=stereo|c0=c0|c1=c0", "-c:a", "pcm_f32le"});
    if (!CHECK(!mix.isEmpty())) return Check::result();
    const double ref = ffmpegIntegrated(mix);
    CHECK(!std::isnan(ref));

    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt; // 25 fps
    const auto l = AudioAnalysis::clipLoudness(fmt, mk(1, mix, 0, 0, 199));
    CHECK(l.has_value());
    if (l && !CHECK(near(l->integrated, ref, 0.3)))
        std::printf("       schneidi %.2f LUFS, ffmpeg %.2f LUFS\n", l->integrated, ref);
    if (l) CHECK(l->range > 1.0 && l->peakDb < 0.0 && !l->blocks.empty());
    // Title / missing file / abort
    Clip title = mk(1, {}, 0, 0, 49);
    title.kind = ClipKind::Title;
    CHECK(!AudioAnalysis::clipLoudness(fmt, title));
    CHECK(!AudioAnalysis::clipLoudness(fmt, mk(1, tmp.filePath("fehlt.wav"), 0, 0, 49)));
    int calls = 0;
    CHECK(!AudioAnalysis::clipLoudness(fmt, mk(1, mix, 0, 0, 199), [&](double) { return ++calls < 2; }));

    // After normalizing (+x dB clip volume) the timeline measures the target: live measurement at the master level meter
    // (like the loudness meter in the mixer), only when active
    if (l) {
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        Clip c = mk(1, mix, 0, 0, 199);
        c.volumeDb = -14.0 - l->integrated;
        tl.audio[0].clips << c;
        auto prof = makeProfile(fmt);
        TimelineBuilder b(*prof);
        MixerHooks hooks;
        auto tr = b.build(tl, &hooks);
        SharedLoudness live;
        CHECK(hooks.master.meter != nullptr);
        if (hooks.master.meter) hooks.master.meter->set("_loudness", &live, 0);
        auto pull = [&](int from, int to) {
            tr->seek(from);
            for (int pos = from; pos <= to; ++pos) {
                std::unique_ptr<Mlt::Frame> f(tr->get_frame());
                mlt_audio_format afmt = mlt_audio_float;
                int freq = 48000, channels = 2;
                int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
                f->get_audio(afmt, freq, channels, samples);
            }
        };
        pull(0, 49); // not active: nothing measured
        CHECK_EQ(live.meter.measuredSeconds(), 0.0);
        live.active = true;
        pull(0, 199);
        if (!CHECK(near(live.meter.integrated(), -14.0, 0.3)))
            std::printf("       live: %.2f LUFS\n", live.meter.integrated());
        CHECK(near(live.meter.measuredSeconds(), 8.0, 0.15));
    }
    return Check::result();
}
