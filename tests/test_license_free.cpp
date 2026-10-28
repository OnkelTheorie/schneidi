// No GPL in the bundled packages (0.14): Licenses::report() finds FFmpeg's license and the libraries in the process,
// schneidi loads none of MLT's GPL modules (normalize, resample, plus, …), and the own "volume" filter that replaces
// the normalize module's works like it (dB level, animated, ramp over a frame). A SCHNEIDI_NO_GPL build (or
// SCHNEIDI_NO_GPL=1 in the environment, for checking an installed bundle) fails on any GPL part.

#include "check.h"

#include "engine/Licenses.h"

#include <Mlt.h>
#include <QApplication>
#include <cmath>
#include <cstring>
#include <memory>
#include <algorithm>
#include <vector>

namespace {

// Audio of one frame of a 1 kHz tone (float, interleaved); filter = optional volume filter on the producer
std::vector<float> toneFrame(Mlt::Profile& profile, int position, const char* level)
{
    Mlt::Producer tone(profile, "tone");
    if (!tone.is_valid()) return {};
    tone.set("frequency", 1000);
    tone.set("level", -6.0); // dB: room upwards without clipping
    Mlt::Filter volume(profile, "volume");
    if (level) {
        volume.set("level", level);
        tone.attach(volume);
    }
    std::vector<float> out;
    // The ramp starts at the previous frame's gain only when the frames follow each other: render from 0 on
    for (int p = 0; p <= position; ++p) {
        tone.seek(p);
        std::unique_ptr<Mlt::Frame> frame(tone.get_frame());
        mlt_audio_format format = mlt_audio_f32le;
        int frequency = 48000, channels = 2, samples = 1920;
        auto* data = static_cast<float*>(frame->get_audio(format, frequency, channels, samples));
        if (p == position && data) out.assign(data, data + samples * channels);
    }
    return out;
}

double peak(const std::vector<float>& a, size_t from, size_t to)
{
    double m = 0;
    for (size_t i = from; i < std::min(to, a.size()); ++i) m = std::max(m, double(std::fabs(a[i])));
    return m;
}

void testVolume()
{
    Mlt::Profile profile;
    profile.set_frame_rate(25, 1);
    Mlt::Filter volume(profile, "volume");
    CHECK(volume.is_valid());
    const std::vector<float> plain = toneFrame(profile, 0, nullptr);
    const std::vector<float> half = toneFrame(profile, 0, "-6.0206");
    if (!CHECK(!plain.empty() && plain.size() == half.size())) return;
    CHECK(std::fabs(peak(half, 0, half.size()) / peak(plain, 0, plain.size()) - 0.5) < 0.01);
    const std::vector<float> plus6 = toneFrame(profile, 0, "6.0206");
    CHECK(std::fabs(peak(plus6, 0, plus6.size()) / peak(plain, 0, plain.size()) - 2.0) < 0.02);
    const std::vector<float> silent = toneFrame(profile, 0, "-200");
    CHECK(peak(silent, 0, silent.size()) < 1e-6);

    // Animated: 0 dB at frame 0, -6 dB from frame 1 -> frame 1 ramps from 1.0 down to 0.5 (no click), frame 2 is 0.5
    const char* anim = "0=0;1=-6.0206";
    const std::vector<float> ramp = toneFrame(profile, 1, anim);
    const std::vector<float> ref = toneFrame(profile, 1, nullptr);
    if (!CHECK(ramp.size() == ref.size() && ramp.size() > 400)) return;
    const size_t n = ramp.size();
    // 1 kHz at 48 kHz stereo: 96 values per period -> peak over two periods at the start and the end
    CHECK(std::fabs(peak(ramp, 0, 192) / peak(ref, 0, 192) - 1.0) < 0.03);
    CHECK(std::fabs(peak(ramp, n - 192, n) / peak(ref, n - 192, n) - 0.5) < 0.03);
    const std::vector<float> after = toneFrame(profile, 2, anim);
    const std::vector<float> ref2 = toneFrame(profile, 2, nullptr);
    CHECK(std::fabs(peak(after, 0, after.size()) / peak(ref2, 0, ref2.size()) - 0.5) < 0.01);
}

void testNames()
{
    CHECK(!Licenses::knownGplLicense("libx264.so.164").isEmpty());
    CHECK(!Licenses::knownGplLicense("x265-215.dll").isEmpty());
    CHECK(!Licenses::knownGplLicense("libmltnormalize.so").isEmpty());
    CHECK(!Licenses::knownGplLicense("libmltplus.so").isEmpty());
    CHECK(!Licenses::knownGplLicense("libpostproc.so.58").isEmpty());
    CHECK(Licenses::knownGplLicense("libmltplus.so").contains("GPL"));
    CHECK(Licenses::knownGplLicense("libopenh264.so.8").isEmpty());
    CHECK(Licenses::knownGplLicense("libavcodec.so.61.19.101").isEmpty());
    CHECK(Licenses::knownGplLicense("avcodec-61.dll").isEmpty());
    CHECK(Licenses::knownGplLicense("libmltavformat.so").isEmpty());
    CHECK(Licenses::knownGplLicense("libmltcore.so").isEmpty());
}

void testReport()
{
    const QVector<Licenses::Part> parts = Licenses::report();
    bool avcodec = false;
    for (const Licenses::Part& p : parts) {
        std::printf("  %-40s %-30s %s\n", qPrintable(p.name), qPrintable(p.license), p.gpl ? "GPL" : "");
        if (p.name.startsWith("libavcodec ")) avcodec = !p.license.isEmpty();
    }
    std::fflush(stdout);
    CHECK(avcodec);
    // schneidi's own choice in every build: none of MLT's GPL modules (xine stays allowed on a system MLT)
    for (const Licenses::Part& p : parts) {
        const bool mltGpl = p.gpl && p.name.startsWith("libmlt") && !p.name.startsWith("libmltxine");
        Check::report(!mltGpl, "GPL MLT module loaded", __FILE__, __LINE__, p.name);
    }
    if (Licenses::builtWithoutGpl() || qEnvironmentVariableIntValue("SCHNEIDI_NO_GPL") > 0)
        for (const Licenses::Part& p : Licenses::gplParts())
            Check::report(false, "GPL part in a SCHNEIDI_NO_GPL build", __FILE__, __LINE__, p.name + " " + p.license);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("license_free");
    Check::initMlt();
    testNames();
    testVolume();
    testReport();
    return Check::result();
}
