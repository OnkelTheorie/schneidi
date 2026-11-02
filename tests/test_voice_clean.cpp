// SPDX-License-Identifier: AGPL-3.0-only
// Voice cleaning ("Stimme säubern", effect "voiceclean", engine/VoiceClean with RNNoise): the processor keeps the sound
// in sync at 48/44.1/32/22.05 kHz (dry path = input delayed by exactly latency()), lowers the noise of a speech-like
// signal while the voice stays, stereo (same channels stay the same, different ones both cleaned), any block size
// gives the same output, CPU cost; the mixer compensates the latency (preview from the middle = export), the editor
// adds it in one undo step on audio clips only, project file round trip, CLI op clean_voice and `effects`.
#include "check.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/VoiceClean.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <vector>

namespace {

constexpr double kPeriod = 1.8; // seconds: 1.2 s of syllables, then 0.6 s pause
constexpr double kSpeech = 1.2;

// Speech-like sound: a glottal pulse train (110-140 Hz, harmonics to 3.8 kHz) through two vowel formants that change
// every syllable (0.2 s, sin² envelope), sentences of 1.2 s with 0.6 s pauses; RMS around -20 dBFS while speaking
std::vector<float> voice(int rate, double seconds)
{
    struct Vowel {
        double f1, f2;
    };
    const Vowel vowels[] = {{730, 1090}, {270, 2290}, {530, 1840}, {300, 870}, {570, 840}};
    const size_t n = size_t(seconds * rate);
    std::vector<float> out(n);
    double phase = 0;
    // Two resonators (bandpass, RBJ) per vowel; state per formant
    double z[2][2] = {};
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / rate;
        const double inPeriod = std::fmod(t, kPeriod);
        const double f0 = 125 + 15 * std::sin(2 * std::numbers::pi * 2.3 * t);
        phase += 2 * std::numbers::pi * f0 / rate;
        double src = 0;
        for (int k = 1; k * f0 < 3800 && k < 40; ++k) src += std::sin(k * phase) / k;
        const int syllable = int(t / 0.2);
        const Vowel& v = vowels[syllable % 5];
        double y = 0;
        for (int f = 0; f < 2; ++f) {
            const double fc = f ? v.f2 : v.f1, q = 6;
            const double w0 = 2 * std::numbers::pi * fc / rate, alpha = std::sin(w0) / (2 * q), a0 = 1 + alpha;
            const double b0 = alpha / a0, a1 = -2 * std::cos(w0) / a0, a2 = (1 - alpha) / a0;
            const double o = b0 * src + z[f][0];
            z[f][0] = -a1 * o + z[f][1];
            z[f][1] = -b0 * src - a2 * o;
            y += o * (f ? 0.6 : 1.0);
        }
        const double env = inPeriod < kSpeech ? std::pow(std::sin(std::numbers::pi * std::fmod(t, 0.2) / 0.2), 2) : 0.0;
        out[i] = float(0.5 * y * (0.25 + 0.75 * env) * (inPeriod < kSpeech ? 1.0 : 0.0));
    }
    return out;
}

// White noise at `db` dBFS RMS (fixed seed)
std::vector<float> noise(size_t n, double db, unsigned seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, float(std::pow(10.0, db / 20.0)));
    std::vector<float> out(n);
    for (float& x : out) x = g(rng);
    return out;
}

double rmsDb(const float* x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += double(x[i]) * x[i];
    return n && s > 0 ? 10 * std::log10(s / double(n)) : -200;
}

// RMS (dB) over the pauses (middle 0.4 s) or the speech (0.1-1.1 s) of every sentence after the first two, of x read
// `delay` samples late
double partDb(const std::vector<float>& x, int rate, int delay, bool pauses)
{
    double s = 0;
    size_t count = 0;
    for (double p = 2 * kPeriod; (p + kPeriod) * rate + delay < x.size(); p += kPeriod) {
        const double a = pauses ? p + kSpeech + 0.1 : p + 0.1, b = pauses ? p + kPeriod - 0.1 : p + kSpeech - 0.1;
        for (size_t i = size_t(a * rate); i < size_t(b * rate); ++i, ++count) {
            const double v = x[i + size_t(delay)];
            s += v * v;
        }
    }
    return count && s > 0 ? 10 * std::log10(s / double(count)) : -200;
}

// Runs a processor over stereo input in blocks of `block`; returns the output
std::vector<float> run(int rate, const VoiceClean::Params& p, std::vector<float> l, std::vector<float> r, int block,
                       std::vector<float>* rightOut = nullptr)
{
    VoiceClean::Processor proc(rate);
    proc.setParams(p);
    for (size_t i = 0; i < l.size(); i += size_t(block)) {
        const int n = int(std::min(l.size() - i, size_t(block)));
        float* d[2] = {l.data() + i, r.data() + i};
        proc.process(d, n);
    }
    if (rightOut) *rightOut = std::move(r);
    return l;
}

// Best lag (samples) of y against x within ±maxLag, by normalized cross-correlation over the middle of the signal
int bestLag(const std::vector<float>& x, const std::vector<float>& y, int maxLag)
{
    int best = 0;
    double bestC = -2;
    const size_t a = x.size() / 4, b = x.size() * 3 / 4;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double c = 0, ex = 0, ey = 0;
        for (size_t i = a; i < b; ++i) {
            const double xv = x[i], yv = y[size_t(int64_t(i) + lag)];
            c += xv * yv;
            ex += xv * xv;
            ey += yv * yv;
        }
        c /= std::sqrt(ex * ey) + 1e-30;
        if (c > bestC) {
            bestC = c;
            best = lag;
        }
    }
    return best;
}

bool writeWav(const QString& path, const std::vector<float>& mono, int rate)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 data = quint32(mono.size() * 4);
    auto le32 = [&](quint32 v) {
        char b[4];
        qToLittleEndian(v, b);
        f.write(b, 4);
    };
    auto le16 = [&](quint16 v) {
        char b[2];
        qToLittleEndian(v, b);
        f.write(b, 2);
    };
    f.write("RIFF");
    le32(36 + data);
    f.write("WAVEfmt ");
    le32(16);
    le16(3); // float
    le16(1);
    le32(quint32(rate));
    le32(quint32(rate) * 4);
    le16(4);
    le16(32);
    f.write("data");
    le32(data);
    for (float s : mono) le32(qFromLittleEndian<quint32>(&s));
    return true;
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

// The mixed sound (left) of samples [from, from + n) at `rate`, rendered in one go starting at `from`
std::vector<float> mix(const Timeline& tl, int rate, int64_t from, int n)
{
    Audio::Mixer::Options o;
    o.rate = rate;
    Audio::Mixer m(tl, ProjectFormat{}.rate, o);
    std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
    for (int done = 0; done < n;) {
        const int k = std::min(n - done, 4800);
        float* p[] = {l.data() + done, r.data() + done};
        m.render(from + done, k, p);
        done += k;
    }
    return l;
}

QString g_path;

QJsonObject cli(const QString& name, const QJsonObject& raw0, QString* error = nullptr)
{
    QJsonObject raw = raw0;
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (error) *error = e.code;
        return {};
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("voice-clean");

    VoiceClean::Params dry; // dry path only: the input delayed by latency()
    dry.amount = 0;
    dry.gate = dry.clicks = dry.shape = false;

    // Sync: the dry path is the input exactly `latency()` late at 48 kHz, and close to it (band-limited, lag 0) at
    // the other rates
    {
        CHECK_EQ(VoiceClean::latencyFor(48000), 2750);
        for (int rate : {48000, 44100, 32000, 22050}) {
            VoiceClean::Processor p(rate);
            CHECK_EQ(p.latency(), VoiceClean::latencyFor(rate));
            const std::vector<float> in = voice(rate, 4.0);
            std::vector<float> noisy = noise(in.size(), -40, 3);
            for (size_t i = 0; i < in.size(); ++i) noisy[i] += in[i];
            const int L = VoiceClean::latencyFor(rate);
            const std::vector<float> out = run(rate, dry, noisy, noisy, 1024);
            std::vector<float> aligned(out.begin() + L, out.end()), ref(noisy.begin(), noisy.end() - L);
            const int lag = bestLag(ref, aligned, rate / 100);
            CHECK(std::abs(lag) <= rate / 1000); // ±1 ms
            CHECK_EQ(lag, 0);
            double err = 0;
            for (size_t i = ref.size() / 4; i < ref.size() * 3 / 4; ++i) err = std::max(err, double(std::abs(ref[i] - aligned[i])));
            if (rate == 48000) CHECK(err == 0.0);
            else CHECK(rmsDb(aligned.data() + ref.size() / 4, ref.size() / 2) - rmsDb(ref.data() + ref.size() / 4, ref.size() / 2) < 0.5);
            std::printf("sync %d Hz: latency %d samples (%.1f ms), lag %d, max dry error %.5f\n", rate, L,
                        L * 1000.0 / rate, lag, err);
        }
    }

    // Noise reduction on speech under noise (-40 dBFS white noise, voice ~-20 dBFS)
    for (int rate : {48000, 44100}) {
        const std::vector<float> v = voice(rate, 12.0);
        std::vector<float> noisy = noise(v.size(), -40, 7);
        for (size_t i = 0; i < v.size(); ++i) noisy[i] += v[i];
        const int L = VoiceClean::latencyFor(rate);
        const double noiseIn = partDb(noisy, rate, 0, true), voiceIn = partDb(noisy, rate, 0, false);

        VoiceClean::Params suppressOnly = dry;
        suppressOnly.amount = 1;
        const std::vector<float> a = run(rate, suppressOnly, noisy, noisy, 4800);
        const double noiseA = partDb(a, rate, L, true), voiceA = partDb(a, rate, L, false);
        const std::vector<float> b = run(rate, VoiceClean::Params{}, noisy, noisy, 4800);
        const double noiseB = partDb(b, rate, L, true), voiceB = partDb(b, rate, L, false);
        std::printf("%d Hz: in noise %.1f dB voice %.1f dB | RNNoise only: noise %.1f (-%.1f dB) voice %.1f (%+.1f dB) | "
                    "defaults: noise %.1f (-%.1f dB) voice %.1f (%+.1f dB)\n",
                    rate, noiseIn, voiceIn, noiseA, noiseIn - noiseA, voiceA, voiceA - voiceIn, noiseB, noiseIn - noiseB,
                    voiceB, voiceB - voiceIn);
        CHECK(noiseIn - noiseA > 10);
        CHECK(std::abs(voiceA - voiceIn) < 4);
        CHECK(noiseIn - noiseB > 20);
        CHECK(std::abs(voiceB - voiceIn) < 8);
        // The voice stays in sync after the full chain too
        std::vector<float> vb(b.begin() + L, b.end()), vref(v.begin(), v.end() - L);
        CHECK(std::abs(bestLag(vref, vb, rate / 100)) <= rate / 1000);
    }

    // Stereo: the same sound on both channels stays the same; different noise on each side is lowered on both; any
    // block size gives the same output
    {
        const int rate = 48000;
        const std::vector<float> v = voice(rate, 8.0);
        std::vector<float> l = noise(v.size(), -40, 11), r = noise(v.size(), -40, 12);
        for (size_t i = 0; i < v.size(); ++i) l[i] += v[i];
        std::vector<float> rOut;
        const std::vector<float> same = run(rate, VoiceClean::Params{}, l, l, 4096, &rOut);
        CHECK(same == rOut);
        const std::vector<float> lOut = run(rate, VoiceClean::Params{}, l, r, 4096, &rOut);
        const int L = VoiceClean::latencyFor(rate);
        CHECK(partDb(l, rate, 0, true) - partDb(lOut, rate, L, true) > 15);
        CHECK(partDb(r, rate, 0, true) - partDb(rOut, rate, L, true) > 15);
        CHECK(std::abs(partDb(lOut, rate, L, false) - partDb(l, rate, 0, false)) < 8);
        bool finite = true;
        for (float x : lOut) finite = finite && std::isfinite(x);
        CHECK(finite);
        for (int rate2 : {48000, 44100}) {
            const std::vector<float> w = voice(rate2, 2.0);
            CHECK(run(rate2, VoiceClean::Params{}, w, w, 7) == run(rate2, VoiceClean::Params{}, w, w, 4096));
        }
    }

    // CPU: seconds of processing per second of stereo sound
    {
        for (int rate : {48000, 44100}) {
            const std::vector<float> v = voice(rate, 10.0);
            std::vector<float> l = v, r = noise(v.size(), -40, 5);
            QElapsedTimer t;
            t.start();
            run(rate, VoiceClean::Params{}, l, l, 1024);
            const double mono = t.nsecsElapsed() / 1e9 / 10.0;
            t.restart();
            run(rate, VoiceClean::Params{}, l, r, 1024);
            const double stereo = t.nsecsElapsed() / 1e9 / 10.0;
            std::printf("CPU %d Hz: %.1f ms per second of sound (same channels), %.1f ms (stereo)\n", rate,
                        mono * 1000, stereo * 1000);
            CHECK(stereo < 0.5); // far within real time even on a slow machine
        }
    }

    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    // Mixer: the effect's latency is compensated - with only the dry path the mix is the source itself, from the
    // start and from a position inside the clip (preview after a jump); defaults keep the voice in place
    for (int rate : {48000, 44100}) {
        const QString wav = tmp.filePath(QString("noisy-%1.wav").arg(rate));
        const std::vector<float> v = voice(rate, 8.0);
        std::vector<float> noisy = noise(v.size(), -40, 9);
        for (size_t i = 0; i < v.size(); ++i) noisy[i] += v[i];
        CHECK(writeWav(wav, noisy, rate));
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.audio[0].clips << mk(1, wav, 0, 0, 149); // 6 s at 25 fps
        EffectInstance fx{"voiceclean", {{"amount", 0.0}, {"gate", false}, {"clicks", false}, {"shape", false}}, true};
        tl.audio[0].clips[0].effects << fx;
        const int n = rate * 5;
        const std::vector<float> whole = mix(tl, rate, 0, n);
        // A mono file plays on both channels at -3 dB
        double err = 0;
        for (int i = rate / 10; i < n; ++i)
            err = std::max(err, double(std::abs(whole[size_t(i)] - noisy[size_t(i)] * float(std::numbers::sqrt2 / 2))));
        std::printf("mixer %d Hz, dry path: max difference to the source %.7f\n", rate, err);
        CHECK(err < (rate == 48000 ? 1e-6 : 0.02)); // other rates: through the 48 kHz resampling and back
        const std::vector<float> later = mix(tl, rate, rate * 2, rate);
        double jump = 0;
        for (int i = 0; i < rate; ++i) jump = std::max(jump, double(std::abs(later[size_t(i)] - whole[size_t(rate * 2 + i)])));
        CHECK(jump < 1e-6);

        tl.audio[0].clips[0].effects = {EffectInstance{"voiceclean", {}, true}};
        const std::vector<float> clean = mix(tl, rate, 0, n);
        std::vector<float> ref(v.begin(), v.begin() + n);
        CHECK(std::abs(bestLag(ref, clean, rate / 100)) <= rate / 1000);
        CHECK(partDb(noisy, rate, 0, true) - partDb(clean, rate, 0, true) > 20);
        // Playback starting inside the clip sounds like the export after the warm-up
        const std::vector<float> from3 = mix(tl, rate, rate * 3, rate);
        double diff = 0;
        for (int i = 0; i < rate; ++i) diff += std::abs(from3[size_t(i)] - clean[size_t(rate * 3 + i)]);
        std::printf("mixer %d Hz, defaults: mean difference after a jump %.5f\n", rate, diff / rate);
    }

    // Editor: one undo step, audio clips only (a video clip's audio stays as it is), values; project round trip
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.edit("setup", [&](Timeline& tl) {
            tl.video[0].clips << mk(1, "/x/a.mp4", 0, 0, 99);
            tl.audio[0].clips << mk(11, "/x/a.mp4", 0, 0, 99) << mk(12, "/x/b.wav", 100, 0, 49);
        });
        const int undo = p.undoStack()->count();
        CHECK_EQ(ed.cleanVoice({1, 11}, {{"gate", false}}), QVector<int>{11});
        CHECK_EQ(p.undoStack()->count(), undo + 1);
        const Clip* a = TimelineOps::findClip(p.timeline(), 11);
        CHECK(a && EffectRegistry::has(*a, "voiceclean") && !EffectRegistry::value(*a, "voiceclean", "gate").toBool()
              && EffectRegistry::value(*a, "voiceclean", "amount").toDouble() == 100.0);
        CHECK(!EffectRegistry::has(*TimelineOps::findClip(p.timeline(), 1), "voiceclean"));
        CHECK(Editor::effectKind("voiceclean") == TrackKind::Audio);
        CHECK_EQ(ed.audioTrackClips(0), (QVector<int>{11, 12}));
        ed.cleanVoice(ed.audioTrackClips(0), {{"amount", 60.0}});
        a = TimelineOps::findClip(p.timeline(), 11);
        CHECK(a && EffectRegistry::value(*a, "voiceclean", "amount").toDouble() == 60.0
              && !EffectRegistry::value(*a, "voiceclean", "gate").toBool()); // kept
        CHECK(EffectRegistry::has(*TimelineOps::findClip(p.timeline(), 12), "voiceclean"));
        p.undoStack()->undo();
        CHECK(!EffectRegistry::has(*TimelineOps::findClip(p.timeline(), 12), "voiceclean"));

        const QString path = tmp.filePath("p.schneidi");
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
        ProjectData back;
        CHECK(ProjectFile::load(path, &back, &error));
        const Clip* b = TimelineOps::findClip(back.timeline, 11);
        CHECK(b && *b == *TimelineOps::findClip(p.timeline(), 11));
    }

    // CLI: op clean_voice on a track and on clips, values, remove, errors; `effects` lists it as audio
    {
        g_path = tmp.filePath("cli.schneidi");
        Project p;
        ProjectData d;
        d.timeline = emptyTimeline();
        p.load(d);
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v = mk(p.newClipId(), "/x/a.mp4", 0, 0, 99);
            v.linkId = p.newLinkId();
            Clip a = v;
            a.id = p.newClipId();
            tl.video[0].clips << v;
            tl.audio[0].clips << a;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
        const int video = p.timeline().video[0].clips[0].id, audio = p.timeline().audio[0].clips[0].id;
        auto edit = [&](const QString& ops, QString* err = nullptr) {
            return cli("edit", {{"project", g_path}, {"ops", ops}}, err);
        };
        auto loaded = [&] {
            ProjectData x;
            QString e;
            ProjectFile::load(g_path, &x, &e);
            return x.timeline;
        };
        const QJsonObject r = edit(R"([{"op":"clean_voice","track":"A1"}])");
        CHECK(QJsonDocument(r).toJson().contains("voice cleaning on 1 audio clip"));
        Timeline tl = loaded();
        CHECK(EffectRegistry::has(*TimelineOps::findClip(tl, audio), "voiceclean"));
        edit(QString(R"([{"op":"clean_voice","clips":[%1],"amount":70,"gate":false}])").arg(video));
        tl = loaded();
        const Clip* a = TimelineOps::findClip(tl, audio);
        CHECK(a && EffectRegistry::value(*a, "voiceclean", "amount").toDouble() == 70.0
              && !EffectRegistry::value(*a, "voiceclean", "gate").toBool());
        CHECK(!EffectRegistry::has(*TimelineOps::findClip(tl, video), "voiceclean"));
        QString err;
        edit(R"([{"op":"clean_voice","track":"V1"}])", &err);
        CHECK_EQ(err, QString("BAD_ARGUMENT"));
        err.clear();
        edit(R"([{"op":"clean_voice","track":"A1","gate":"yes"}])", &err);
        CHECK_EQ(err, QString("BAD_ARGUMENT"));
        err.clear();
        edit(R"([{"op":"clean_voice","track":"A1","amount":150}])", &err);
        CHECK_EQ(err, QString("BAD_ARGUMENT"));
        edit(R"([{"op":"clean_voice","track":"A1","remove":true}])");
        CHECK(!EffectRegistry::has(*TimelineOps::findClip(loaded(), audio), "voiceclean"));
        // The generic effect op works as well
        edit(QString(R"([{"op":"effect","clips":[%1],"effect":"voiceclean","params":{"shape":false}}])").arg(audio));
        CHECK(!EffectRegistry::value(*TimelineOps::findClip(loaded(), audio), "voiceclean", "shape").toBool());
        bool listed = false;
        for (const QJsonValue& e : cli("effects", {})["effects"].toArray())
            if (e["effect"].toString() == "voiceclean") listed = e["kind"].toString() == "audio";
        CHECK(listed);
    }

    return Check::result();
}
