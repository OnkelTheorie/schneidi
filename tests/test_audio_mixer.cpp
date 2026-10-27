// Own audio engine, step 2 (docs/archiv/plan-0.10-ton.md): Audio::Mixer mixes a timeline offline.
// Retimed clips against generated sources (always): bursts at known times land where the clip's RetimeMap puts
// them (sample-exact without pitch correction, within a few ms with it), the pitch follows the speed (tape) or stays
// (own stretcher, engine/audio/Stretch), level kept, reverse at 100 % = the samples backwards, block size does not matter.
// Against the sound references of step 0 (test_audio_reference, private docs folder; skipped without it):
// - MLT compatibility mode (curves frame-late like MLT, no latency compensation): every case the same samples as the
//   reference rendered through MLT (2 LSB at 24 bit), except where MLT itself is wrong or works differently
//   (listed per case with the reason and its own check)
// - exact mode (the engine as it will be used): difference to the reference printed, checked against a bound per case
// - block size: rendering in odd block sizes gives the same samples as in one go

#include "check.h"

#include "core/Project.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/Retime.h"
#include "core/Fft.h"
#include "engine/audio/Mixer.h"
#include "engine/audio/Reader.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#ifndef SCHNEIDI_AUDIO_REFERENCE
#define SCHNEIDI_AUDIO_REFERENCE ""
#endif

namespace {

constexpr int kRate = 48000;
// Same samples: 2 LSB at 16 bit. The MLT path rounds to 16 bit at the end (render()); where only one track sounds, its
// declick ramp also works on 16-bit samples (cut toward zero twice)
constexpr double kExact = 2.02 / 32768;

QString g_dir;

// Interleaved stereo float samples of a reference
std::vector<float> decode(const QString& path)
{
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-f", "f32le", "-ac", "2", "-ar", QString::number(kRate), "-"});
    p.waitForFinished(60000);
    const QByteArray d = p.readAllStandardOutput();
    const auto* s = reinterpret_cast<const float*>(d.constData());
    return std::vector<float>(s, s + d.size() / qsizetype(sizeof(float)));
}

struct Loaded {
    Timeline timeline;
    FrameRate fps;
    QHash<QString, int> lengths;
};

bool load(const QString& path, Loaded* out)
{
    ProjectData d;
    QString error;
    if (!ProjectFile::load(path, &d, &error)) {
        std::printf("       %s: %s\n", qPrintable(path), qPrintable(error));
        return false;
    }
    Project project;
    project.load(d);
    out->timeline = project.renderTimeline();
    out->fps = d.format.rate;
    for (const MediaInfo& m : project.media()) out->lengths.insert(m.path, m.length);
    return true;
}

// Whole timeline in blocks of `block` samples (0 = varying odd sizes), interleaved stereo, quantized like the
// references: the MLT export path writes its "24 bit" WAV with 16-bit samples, cut toward zero (the frames reach the
// consumer as s16)
std::vector<float> render(const Loaded& l, bool compat, int block, qint64* ms = nullptr, Audio::Mixer::Stats* stats = nullptr)
{
    Audio::Mixer::Options o;
    o.rate = kRate;
    o.mltCompat = compat;
    o.fileLength = [&](const QString& p) { return l.lengths.value(p); };
    QElapsedTimer t;
    t.start();
    Audio::Mixer mixer(l.timeline, l.fps, o);
    const int64_t n = mixer.length();
    std::vector<float> out(size_t(n) * 2);
    std::vector<float> L, R;
    static const int sizes[] = {1, 1919, 4096, 333, 1920, 7, 12000};
    int k = 0;
    for (int64_t pos = 0; pos < n;) {
        const int count = int(std::min<int64_t>(n - pos, block > 0 ? block : sizes[k++ % 7]));
        L.resize(size_t(count));
        R.resize(size_t(count));
        float* buf[2] = {L.data(), R.data()};
        mixer.render(pos, count, buf);
        for (int i = 0; i < count; ++i) {
            out[size_t(pos + i) * 2] = L[size_t(i)];
            out[size_t(pos + i) * 2 + 1] = R[size_t(i)];
        }
        pos += count;
    }
    if (ms) *ms = t.elapsed();
    if (stats) *stats = mixer.stats();
    for (float& v : out) v = float(std::trunc(std::clamp(double(v) * 32768, -32768.0, 32767.0)) / 32768);
    return out;
}

double spectralDiff(const std::vector<float>& got, const std::vector<float>& ref, const QVector<QPair<double, double>>& skip,
                    qsizetype* at);
double alignedShare(const std::vector<float>& got, const std::vector<float>& ref, double residualDb);

// How a render is compared with its reference
struct Match {
    double bound = kExact; // largest difference allowed
    double rmsDb = 0;      // instead: bound on the RMS of the difference (dBFS), for intended differences
    int64_t shift = 0;     // got[i] is compared with ref[i + shift] (latency the MLT path does not compensate)
    int maxLag = 0;        // each sample may also match up to this many samples away
    int outliers = 0;      // this many samples may differ more
    QVector<QPair<double, double>> skip; // seconds left out (from, to)
    double spectralDb = 0; // instead: bound on spectralDiff() (retimed clips)
    double alignedShare = 0; // instead: at least this share of 0.2-s windows within -60 dB after shifting (alignedShare())
};

struct Result {
    double maxDiff = 0; // after the outliers
    qsizetype maxAt = 0;
    double rmsDb = -200;
    double spectral = -1;
    double share = -1;
    bool ok = false;
};

Result measure(const std::vector<float>& got, const std::vector<float>& ref, const Match& m)
{
    Result r;
    const qsizetype n = qsizetype(got.size() / 2), nr = qsizetype(ref.size() / 2);
    std::vector<std::pair<double, qsizetype>> worst(size_t(m.outliers) + 1, {0.0, 0}); // the largest, ascending
    double sum = 0;
    qsizetype counted = 0;
    for (qsizetype i = 0; i < n; ++i) {
        if (std::any_of(m.skip.begin(), m.skip.end(), [&](const auto& r) { return i >= r.first * kRate && i < r.second * kRate; }))
            continue;
        const qsizetype j0 = i + m.shift;
        if (j0 < 0 || j0 >= nr) continue;
        double best = 1e9;
        for (qsizetype j = std::max<qsizetype>(0, j0 - m.maxLag); j <= std::min(nr - 1, j0 + m.maxLag); ++j)
            best = std::min(best, std::max(std::abs(double(got[size_t(i) * 2]) - ref[size_t(j) * 2]),
                                           std::abs(double(got[size_t(i) * 2 + 1]) - ref[size_t(j) * 2 + 1])));
        for (int c = 0; c < 2; ++c) {
            const double e = double(got[size_t(i) * 2 + c]) - ref[size_t(j0) * 2 + c];
            sum += e * e;
        }
        ++counted;
        if (best > worst[0].first) {
            worst[0] = {best, i};
            std::sort(worst.begin(), worst.end());
        }
    }
    r.maxDiff = worst[0].first;
    r.maxAt = worst.back().second;
    if (counted) r.rmsDb = 10 * std::log10(sum / double(2 * counted) + 1e-30);
    r.ok = m.rmsDb != 0 ? r.rmsDb <= m.rmsDb : r.maxDiff <= m.bound;
    if (m.spectralDb != 0) {
        r.spectral = spectralDiff(got, ref, m.skip, &r.maxAt);
        r.ok = r.spectral <= m.spectralDb;
    }
    if (m.alignedShare != 0) {
        r.share = alignedShare(got, ref, -60);
        r.ok = r.share >= m.alignedShare;
    }
    return r;
}

// Sound comparison for retimed clips, where sample-wise comparison means nothing (another stretcher, another
// resampler delay): energy in third-octave bands (100 Hz - 16 kHz) per window of 85 ms, compared where the reference
// is not silent. Returns the largest mean band difference of a window (dB, weighted by the band energy).
double spectralDiff(const std::vector<float>& got, const std::vector<float>& ref, const QVector<QPair<double, double>>& skip,
                    qsizetype* at)
{
    constexpr int w = 4096;
    const qsizetype n = qsizetype(std::min(got.size(), ref.size()) / 2);
    std::vector<double> bandEdge;
    for (double f = 100; f < 16000; f *= std::pow(2.0, 1.0 / 3)) bandEdge.push_back(f);
    auto bands = [&](const std::vector<float>& x, qsizetype a) {
        std::vector<double> re(w), im(w, 0.0);
        for (int i = 0; i < w; ++i)
            re[size_t(i)] = (x[size_t(a + i) * 2] + x[size_t(a + i) * 2 + 1]) * (0.5 - 0.5 * std::cos(2 * M_PI * i / w));
        Fft::transform(re, im, false);
        std::vector<double> e(bandEdge.size() - 1, 1e-20);
        for (int k = 1; k < w / 2; ++k) {
            const double f = double(k) * kRate / w;
            for (size_t b = 0; b + 1 < bandEdge.size(); ++b)
                if (f >= bandEdge[b] && f < bandEdge[b + 1]) e[b] += re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
        }
        return e;
    };
    double worst = 0;
    for (qsizetype a = 0; a + w <= n; a += w / 2) {
        if (std::any_of(skip.begin(), skip.end(), [&](const auto& r) { return a + w > r.first * kRate && a < r.second * kRate; }))
            continue;
        double level = 0;
        for (int i = 0; i < w; ++i) level += double(ref[size_t(a + i) * 2]) * ref[size_t(a + i) * 2];
        if (10 * std::log10(level / w + 1e-30) < -50) continue;
        const std::vector<double> g = bands(got, a), r = bands(ref, a);
        double sum = 0, weight = 0;
        for (size_t b = 0; b < r.size(); ++b) {
            const double wgt = r[b] + g[b];
            sum += wgt * std::abs(10 * std::log10(g[b] / r[b]));
            weight += wgt;
        }
        const double d = weight > 0 ? sum / weight : 0;
        if (d > worst) {
            worst = d;
            if (at) *at = a;
        }
    }
    return worst;
}

// Per window of 0.2 s: the shift against the reference with the best correlation (mono sum, up to 0.15 s either way),
// how far apart the two are after shifting (dB below the reference) and the reference's level (dBFS)
struct Aligned {
    double at;
    qsizetype lag;
    double residualDb, levelDb;
};

std::vector<Aligned> align(const std::vector<float>& got, const std::vector<float>& ref)
{
    std::vector<Aligned> out;
    const qsizetype n = qsizetype(std::min(got.size(), ref.size()) / 2), w = kRate / 5, reach = kRate * 15 / 100;
    const size_t size = Fft::size(size_t(w + 2 * reach) * 2);
    for (qsizetype a = 0; a + w <= n; a += w) {
        std::vector<double> gr(size, 0), gi(size, 0), rr(size, 0), ri(size, 0);
        for (qsizetype i = 0; i < w; ++i) gr[size_t(i)] = got[size_t(a + i) * 2] + got[size_t(a + i) * 2 + 1];
        for (qsizetype i = -reach; i < w + reach; ++i)
            if (a + i >= 0 && a + i < n) rr[size_t(i + reach)] = ref[size_t(a + i) * 2] + ref[size_t(a + i) * 2 + 1];
        Fft::transform(gr, gi, false);
        Fft::transform(rr, ri, false);
        for (size_t k = 0; k < size; ++k) { // ref * conj(got)
            const double re = rr[k] * gr[k] + ri[k] * gi[k], im = ri[k] * gr[k] - rr[k] * gi[k];
            rr[k] = re;
            ri[k] = im;
        }
        Fft::transform(rr, ri, true);
        qsizetype best = 0;
        double bestV = -1e300;
        for (qsizetype lag = -reach; lag <= reach; ++lag)
            if (rr[size_t(lag + reach)] > bestV) {
                bestV = rr[size_t(lag + reach)];
                best = lag;
            }
        double e = 0, p = 0;
        for (qsizetype i = 0; i < w; ++i) {
            const qsizetype j = a + i + best;
            if (j < 0 || j >= n) continue;
            for (int c = 0; c < 2; ++c) {
                const double r = ref[size_t(j) * 2 + c], g = got[size_t(a + i) * 2 + c];
                e += (g - r) * (g - r);
                p += r * r;
            }
        }
        out.push_back({double(a) / kRate, best, p > 0 ? 10 * std::log10(e / p + 1e-30) : 0.0,
                       10 * std::log10(p / double(2 * w) + 1e-30)});
    }
    return out;
}

// Diagnosis (AUDIO_MIXER_ALIGN=1)
void alignReport(const std::vector<float>& got, const std::vector<float>& ref)
{
    for (const Aligned& a : align(got, ref))
        std::printf("       %6.2f s  Versatz %+6lld  Rest %6.1f dB  Pegel %6.1f dBFS\n", a.at, qlonglong(a.lag), a.residualDb,
                    a.levelDb);
}

// Share of the windows (where the reference is not silent) that are the same as the reference after shifting
double alignedShare(const std::vector<float>& got, const std::vector<float>& ref, double residualDb)
{
    int counted = 0, same = 0;
    for (const Aligned& a : align(got, ref)) {
        if (a.levelDb < -50) continue;
        ++counted;
        if (a.residualDb <= residualDb) ++same;
    }
    return counted ? double(same) / counted : 0;
}

// Expectations per case (default: both modes the same samples as the reference)
struct Expect {
    Match compat, exact;
    const char* why = nullptr;
    int64_t lengthTol = 0;
};

QHash<QString, Expect> expectations()
{
    // Gain/pan curves in the exact mode: a frame's value at its start (MLT: at its end, the first frame of every
    // excerpt constant), fades/transitions reach 0 at the edge
    const Match curves{kExact, -30};
    const char* curvesWhy = "exact mode: curves on time (the MLT path is a frame late)";
    Match retimed;
    retimed.spectralDb = 3;
    Match real;
    real.alignedShare = 0.9;
    auto gaps = [&](std::initializer_list<QPair<double, double>> ranges) {
        Match m = retimed;
        for (const auto& r : ranges) m.skip.push_back(r);
        return m;
    };
    const char* retimedWhy = "retimed clips: other resampler/stretcher than MLT, compared by third-octave bands";
    return {
        {"fades", {{}, curves, curvesWhy}},
        {"pan", {{}, curves, curvesWhy}},
        {"volume_keys", {{}, curves, curvesWhy}},
        {"transition_fades", {{}, curves, curvesWhy}},
        {"crossfades", {{}, curves, "exact mode: curves on time; no 4 ms dip to silence where a dissolve starts on the edit"}},
        {"subframe", {{}, curves, "exact mode: the offset moves the whole clip (MLT cuts its end and with it the declick)"}},
        {"ntsc", {{kExact, 0, 0, 2, 200}, curves,
                  "MLT's mix transition delays the mix by a sample when the tracks' frames bring different sample counts "
                  "(29.97 fps, as the clip on A2 ends; its declick ramp moves against A1), its last sample is missing; "
                  "exact mode: curves and sub-frame offset", 1}},
        // Noise reduction ~10.7 ms, limiter 5 ms + 6 samples: exact = reference that much later (edges aside)
        {"denoise", {{}, {kExact, 0, 512, 0, 2400}, "exact mode: noise reduction latency compensated (512 samples)"}},
        {"master", {{}, {kExact, 0, 246, 0, 300}, "exact mode: limiter latency compensated (246 samples)"}},
        // Real material: Clipi clips are Matroska, where the MLT path drops 48 samples after clip starts
        // (test_audio_regression), and they carry speed changes and transitions -> compared by sound
        {"tiktok", {real, real, "Matroska: the MLT path drops samples after clip starts and jumps inside clips whose "
                                "timestamps overlap (Clipi); compared by windows after shifting"}},
        // Where MLT reads wrong (test_audio_reader, test_av_sync): left out / looser
        {"late_start", {{kExact, 0, 0, 0, 0, {{0, 4.0}}}, {kExact, 0, 0, 0, 0, {{0, 4.0}}},
                        "MLT plays a sound stream that starts after the picture 65 ms early when the clip starts at the "
                        "file start (first clip left out)"}},
        {"rate44", {{kExact, -55, 17}, {kExact, -55, 17}, "MLT's resampler puts 44.1 kHz sources 17 samples late"}},
        // Retimed: MLT resamples frame by frame (32 samples late at 50 %) and pitches with Rubber Band after its
        // resampler; the own engine reads the source position directly. Compared by sound
        {"speed_nopitch", {retimed, retimed, retimedWhy}},
        {"reverse", {retimed, retimed, retimedWhy}},
        // MLT's pitch correction starts every section with ~50 ms of silence, a fade and an overshoot (its stretcher
        // starts empty, the delay is not compensated), and plays smooth ramp transitions in steps without pitch
        // correction, after which its 200 % section runs 35 ms early (the synthetic test above puts the own engine
        // within 4 ms there): left out
        {"speed_pitch", {gaps({{0.0, 0.3}, {3.95, 4.3}}), gaps({{0.0, 0.3}, {3.95, 4.3}}), retimedWhy}},
        {"ramp", {gaps({{0.0, 0.3}, {0.95, 1.3}, {3.55, 99}}), gaps({{0.0, 0.3}, {0.95, 1.3}, {3.55, 99}}), retimedWhy}},
    };
}


// ---- Retimed clips against generated sources ----

constexpr int kFps = 25;
constexpr double kSpf = double(kRate) / kFps;
constexpr double kSynthSeconds = 12;
constexpr int kBursts = 25;
double burstTime(int k) { return 0.5 + 0.41 * k; }

struct Synth {
    QString tone, bursts;
};

bool makeSynth(const QString& dir, Synth* s)
{
    s->tone = dir + "/tone.wav";
    s->bursts = dir + "/bursts.wav";
    // 4 ms bursts of 3 kHz under a Hann window
    QString expr = "0";
    for (int k = 0; k < kBursts; ++k)
        expr += QString("+between(t\\,%1\\,%2)*0.5*sin(2*PI*3000*t)*(0.5-0.5*cos(2*PI*(t-%1)/0.004))")
                    .arg(burstTime(k) - 0.002, 0, 'f', 4)
                    .arg(burstTime(k) + 0.002, 0, 'f', 4);
    return !Check::makeMedia(s->tone, {"-f", "lavfi", "-i", QString("aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=48000:d=%1").arg(kSynthSeconds),
                                       "-c:a", "pcm_f32le"}).isEmpty()
        && !Check::makeMedia(s->bursts, {"-f", "lavfi", "-i", QString("aevalsrc=%1|%1:s=48000:d=%2").arg(expr).arg(kSynthSeconds),
                                         "-c:a", "pcm_f32le"}).isEmpty();
}

// Left channel of clips on A1, rendered in odd block sizes (or `block`)
std::vector<float> renderClips(const QVector<Clip>& clips, int block = 0)
{
    Timeline tl;
    tl.audio.resize(1);
    tl.audio[0].kind = TrackKind::Audio;
    tl.audio[0].clips = clips;
    Audio::Mixer::Options o;
    o.fileLength = [](const QString&) { return int(kSynthSeconds * kFps); };
    Audio::Mixer mixer(tl, FrameRate{kFps, 1}, o);
    std::vector<float> out(size_t(mixer.length())), r(size_t(mixer.length()));
    static const int sizes[] = {4096, 1, 1919, 333, 12000, 7, 1920};
    int k = 0;
    for (int64_t pos = 0; pos < mixer.length();) {
        const int n = int(std::min<int64_t>(mixer.length() - pos, block > 0 ? block : sizes[k++ % 7]));
        float* buf[2] = {out.data() + pos, r.data() + pos};
        mixer.render(pos, n, buf);
        pos += n;
    }
    return out;
}

std::vector<float> renderClip(const Clip& c, int block = 0) { return renderClips({c}, block); }

Clip synthClip(const QString& path, int in, int frames, double speed, bool reverse, bool pitch)
{
    Clip c;
    c.id = 1;
    c.mediaPath = path;
    c.in = in;
    c.out = in + frames - 1;
    c.speed = speed;
    c.reverse = reverse;
    c.keepPitch = pitch;
    c.fadeOut = 0;
    return c;
}

// Energy centroids of the bursts (samples)
std::vector<double> centroids(const std::vector<float>& x)
{
    std::vector<double> out;
    double sum = 0, weighted = 0;
    qsizetype last = -100000;
    for (qsizetype i = 0; i < qsizetype(x.size()); ++i) {
        const double e = double(x[size_t(i)]) * x[size_t(i)];
        if (e < 1e-6) continue;
        if (i - last > kRate / 50 && sum > 0) { // a new burst after 20 ms of quiet
            out.push_back(weighted / sum);
            sum = weighted = 0;
        }
        sum += e;
        weighted += e * double(i);
        last = i;
    }
    if (sum > 0) out.push_back(weighted / sum);
    return out;
}

// Frequency from zero crossings over [a, b) samples
double frequency(const std::vector<float>& x, qsizetype a, qsizetype b)
{
    int crossings = 0;
    double first = -1, lastCross = -1;
    for (qsizetype i = std::max<qsizetype>(1, a); i < std::min<qsizetype>(b, qsizetype(x.size())); ++i)
        if (x[size_t(i - 1)] < 0 && x[size_t(i)] >= 0) {
            const double t = double(i - 1) + x[size_t(i - 1)] / (x[size_t(i - 1)] - x[size_t(i)]);
            if (first < 0) first = t;
            lastCross = t;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) / (lastCross - first) * kRate : 0;
}

double rmsDb(const std::vector<float>& x, qsizetype a, qsizetype b)
{
    double sum = 0;
    for (qsizetype i = a; i < b; ++i) sum += double(x[size_t(i)]) * x[size_t(i)];
    return 10 * std::log10(sum / double(std::max<qsizetype>(1, b - a)) + 1e-30);
}

// Where the clip puts a source time (seconds in the file): timeline sample
double expectedAt(const Clip& c, double sourceSeconds)
{
    const RetimeMap map(c, int(kSynthSeconds * kFps));
    const double fileSamples = kSynthSeconds * kRate;
    // playback index: forward = file sample, reverse = counted back from the file end (sample p -> L - 1 - p)
    const double play = c.reverse ? fileSamples - 1 - sourceSeconds * kRate : sourceSeconds * kRate;
    const double mu = map.materialAt(play / kSpf) * kSpf;
    return mu - c.in * kSpf + c.start * kSpf;
}

// Exact mode: curves on time. A frame's gain runs linearly from its value at the frame start to the one at its end:
// fades are 0 at the clip edge and 1 where they end, keyframes hold at the start of their frame, the two sides of a
// linear dissolve add up to 1 on every sample
void checkCurves(const Synth& src)
{
    auto reader = Audio::Reader::open(src.tone);
    std::vector<float> tone(size_t(kSynthSeconds * kRate)), r2(tone.size());
    float* buf[2] = {tone.data(), r2.data()};
    reader->read(0, int(tone.size()), buf);
    const qsizetype spf = qsizetype(kSpf);
    // Largest |got - tone * gain| over [a, b); gain(u) from boundary values g(k) at frames k
    auto deviation = [&](const std::vector<float>& got, int in, qsizetype a, qsizetype b, const std::function<double(int)>& g) {
        double worst = 0;
        for (qsizetype u = a; u < b; ++u) {
            const int k = int(u / spf);
            const double x = double(u - k * spf) / spf, gain = g(k) + (g(k + 1) - g(k)) * x;
            worst = std::max(worst, std::abs(got[size_t(u)] - tone[size_t(u + in * spf)] * gain));
        }
        return worst;
    };
    const int length = 100;
    {
        Clip c = synthClip(src.tone, 0, length, 1.0, false, false);
        c.fadeIn = 10;
        c.fadeOut = 20;
        const std::vector<float> got = renderClip(c);
        const double d = deviation(got, 0, 0, qsizetype(got.size()), [&](int k) {
            const double ramp = std::clamp(std::min({1.0, k / 10.0, (length - k) / 20.0}), 0.0, 1.0);
            return std::sin(ramp * M_PI / 2);
        });
        std::printf("  Fades genau: Abweichung %.2e, erstes Sample %.2e, letztes %.2e\n", d, got.front(), got.back());
        CHECK(d < 1e-6);
        CHECK(got.front() == 0.f);
        CHECK(std::abs(got.back()) < 0.25 / (20 * kSpf) * 1.01);
    }
    {
        Clip c = synthClip(src.tone, 5, length, 1.0, false, false);
        c.keys[AnimParam::Volume] = KeyTrack{{5 + 10, -20}, {5 + 30, 0, KeyEase::EaseInOut}, {5 + 60, -6}};
        const std::vector<float> got = renderClip(c);
        // declick ramps at both edges (hard cuts) left out
        const double d = deviation(got, 5, qsizetype(0.005 * kRate), qsizetype(got.size()) - qsizetype(0.005 * kRate),
                                   [&](int k) { return std::pow(10.0, Keys::valueAt(c, AnimParam::Volume, k) / 20); });
        std::printf("  Lautstärke-Keyframes genau: Abweichung %.2e\n", d);
        CHECK(d < 1e-6);
    }
    {
        // Linear dissolve between two excerpts of the same tone in phase (whole frames = whole periods of 1 kHz):
        // the sum is the tone itself, no dip anywhere
        Clip a = synthClip(src.tone, 20, 50, 1.0, false, false), b = synthClip(src.tone, 120, 50, 1.0, false, false);
        b.id = 2;
        b.start = 50;
        a.transOut = b.transIn = 12;
        a.transOutStyle.audio = b.transInStyle.audio = AudioCurve::Zero;
        const std::vector<float> got = renderClips({a, b});
        double worst = 0;
        for (qsizetype u = 44 * spf; u < 56 * spf; ++u) worst = std::max(worst, double(std::abs(got[size_t(u)] - tone[size_t(u)])));
        std::printf("  Überblendung linear: Summe weicht %.2e vom Ton ab\n", worst);
        CHECK(worst < 1e-6);
    }
}

// A jump gives the same samples as a mixer that starts there (sources keep their readers, filters start over)
void checkJumps(const Synth& src)
{
    Timeline tl;
    tl.audio.resize(2);
    for (Track& t : tl.audio) t.kind = TrackKind::Audio;
    Clip a = synthClip(src.tone, 10, 80, 1.0, false, false);
    a.fadeIn = 5;
    a.effects << EffectInstance{"denoise", {}, true};
    Clip b = synthClip(src.bursts, 0, 100, 0.7, false, true);
    b.id = 2;
    b.start = 20;
    tl.audio[0].clips << a;
    tl.audio[1].clips << b;
    tl.audio[1].eq.on = true;
    tl.audio[1].eq.bands[3].gainDb = 6;
    tl.masterLimiter = true;
    Audio::Mixer::Options o;
    o.fileLength = [](const QString&) { return int(kSynthSeconds * kFps); };
    auto take = [&](Audio::Mixer& m, int64_t pos, int n) {
        std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
        float* buf[2] = {l.data(), r.data()};
        m.render(pos, n, buf);
        return l;
    };
    Audio::Mixer jumping(tl, FrameRate{kFps, 1}, o);
    take(jumping, 0, 3 * kRate);
    const std::vector<float> afterJump = take(jumping, kRate * 3 / 2, kRate);
    const std::vector<float> back = take(jumping, kRate / 3, kRate / 2);
    Audio::Mixer fresh(tl, FrameRate{kFps, 1}, o), fresh2(tl, FrameRate{kFps, 1}, o);
    const bool same = afterJump == take(fresh, kRate * 3 / 2, kRate) && back == take(fresh2, kRate / 3, kRate / 2);
    std::printf("  Sprünge vor/zurück: %s (%d Neustarts)\n", same ? "gleich wie frisch gestartet" : "ANDERS",
                jumping.stats().restarts);
    CHECK(same);
    CHECK_EQ(jumping.stats().restarts, 2);
}

// Live values (preview, Mixer::updateMix): fader, pan and mute glide within 10 ms to what a mixer built with the new
// values gives (then sample-exact), without a step; a track that was silent when planned or a changed clip needs a
// new mixer. Level meters per track after fader/pan.
void checkLive(const Synth& src)
{
    Timeline tl;
    tl.audio.resize(2);
    for (Track& t : tl.audio) t.kind = TrackKind::Audio;
    tl.audio[0].clips << synthClip(src.tone, 0, 200, 1.0, false, false);
    Clip b = synthClip(src.tone, 0, 200, 1.0, false, false);
    b.id = 2;
    tl.audio[1].clips << b;
    Audio::Mixer::Options o;
    o.fileLength = [](const QString&) { return int(kSynthSeconds * kFps); };
    auto take = [&](Audio::Mixer& m, int64_t pos, int n, std::vector<float>* right = nullptr) {
        std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
        float* buf[2] = {l.data(), r.data()};
        m.render(pos, n, buf);
        if (right) *right = r;
        return l;
    };
    Audio::Mixer live(tl, FrameRate{kFps, 1}, o);
    live.setMetering(true);
    take(live, 0, kRate);
    const std::vector<float> peaks = live.trackPeaks();
    if (CHECK_EQ(int(peaks.size()), 4))
        CHECK(std::abs(peaks[0] - 0.25f) < 0.001f && std::abs(peaks[3] - 0.25f) < 0.001f);

    Timeline changed = tl;
    changed.audio[0].volumeDb = -6;
    changed.audio[1].pan = 50;
    changed.masterVolumeDb = -3;
    bool ok = CHECK(live.updateMix(changed));
    std::vector<float> rLive, rFresh;
    const std::vector<float> glide = take(live, kRate, kRate, &rLive);
    Audio::Mixer fresh(changed, FrameRate{kFps, 1}, o);
    const std::vector<float> want = take(fresh, kRate, kRate, &rFresh);
    float step = 0;
    for (size_t i = 1; i < glide.size(); ++i) step = std::max(step, std::abs(glide[i] - glide[i - 1]));
    const bool after = std::equal(glide.begin() + 480, glide.end(), want.begin() + 480)
                       && std::equal(rLive.begin() + 480, rLive.end(), rFresh.begin() + 480);
    // Two 1-kHz tracks at 0.25 change by up to 0.5 * 2 pi * 1000 / 48000 = 0.0654 per sample on their own
    std::printf("  Live-Werte: nach 10 ms %s wie neu gebaut, größter Schritt %.4f (die Welle selbst: 0,0654)\n",
                after ? "gleich" : "ANDERS", step);
    ok = CHECK(after) && CHECK(step < 0.0655f) && ok;
    take(live, 2 * kRate - 4800, 4800); // levels of a block after the glide
    if (CHECK_EQ(int(live.trackPeaks().size()), 4)) // A1 at -6 dB: 0.125; A2 panned right: left at half
        CHECK(std::abs(live.trackPeaks()[0] - 0.125f) < 0.001f && std::abs(live.trackPeaks()[2] - 0.125f) < 0.002f);

    Timeline muted = changed;
    muted.audio[1].muted = true;
    CHECK(live.updateMix(muted));
    const std::vector<float> m = take(live, 2 * kRate, kRate / 2);
    Audio::Mixer freshMuted(muted, FrameRate{kFps, 1}, o);
    CHECK(std::equal(m.begin() + 480, m.end(), take(freshMuted, 2 * kRate, kRate / 2).begin() + 480));
    Audio::Mixer silentPlanned(muted, FrameRate{kFps, 1}, o);
    CHECK(!silentPlanned.updateMix(changed)); // A2 has no planned voices
    Timeline cut = changed;
    cut.audio[0].clips[0].volumeDb = -1;
    CHECK(!live.updateMix(cut));
    CHECK(!live.updateMix(Timeline{}));
}

void checkRetimed(const Synth& src)
{
    struct T {
        const char* name;
        double speed;
        bool reverse, pitch;
        int in = 0;
        QVector<SpeedPoint> ramp = {};
    };
    const T all[] = {
        {"50 %", 0.5, false, false, 10},
        {"200 %", 2.0, false, false},
        {"150 %", 1.5, false, false, 20},
        {"50 % Tonhöhe", 0.5, false, true, 10},
        {"200 % Tonhöhe", 2.0, false, true},
        {"150 % Tonhöhe", 1.5, false, true, 20},
        {"rückwärts", 1.0, true, false, 5},
        {"rückwärts 50 %", 0.5, true, false},
        {"rückwärts 200 % Tonhöhe", 2.0, true, true},
        // 100 % up to source 1.52 s, 50 % (hard) up to 3 s, 200 % (smooth over 1 s) - timeline 0-1.52, 1.52-4, 5-6 s
        {"Ramp", 1.0, false, false, 0, {{38, 0.5, 0}, {75, 2.0, 25}}},
        {"Ramp Tonhöhe", 1.0, false, true, 0, {{38, 0.5, 0}, {75, 2.0, 25}}},
    };
    std::printf("  %-26s %-28s %-24s %s\n", "Tempo", "Piep-Lage (Samples)", "Ton (Hz, erwartet)", "Pegel");
    for (const T& t : all) {
        Clip c = synthClip(src.bursts, t.in, 150, t.speed, t.reverse, t.pitch);
        c.ramp = t.ramp;
        const std::vector<float> got = renderClip(c);
        const std::vector<double> found = centroids(got);
        // Expected bursts inside the clip (not at its edges, where the declick ramps cut into them)
        std::vector<double> want;
        for (int k = 0; k < kBursts; ++k) {
            const double at = expectedAt(c, burstTime(k));
            if (at > 0.02 * kRate && at < 150 * kSpf - 0.02 * kRate) want.push_back(at);
        }
        std::sort(want.begin(), want.end());
        double worst = 0;
        bool countOk = found.size() == want.size();
        QString offsets;
        if (countOk)
            for (size_t k = 0; k < want.size(); ++k) {
                worst = std::max(worst, std::abs(found[k] - want[k]));
                offsets += QString(" %1").arg(found[k] - want[k], 0, 'f', 0);
            }
        // Pitch and level on the tone, in the middle of a section of constant speed
        Clip tone = c;
        tone.mediaPath = src.tone;
        const std::vector<float> sound = renderClip(tone);
        const qsizetype a = qsizetype(0.3 * kRate), b = qsizetype(1.4 * kRate); // ramp: before the first point
        const double speedThere = t.ramp.isEmpty() ? t.speed : 1.0;
        const double hz = frequency(sound, a, b), wantHz = t.pitch ? 1000 : 1000 * speedThere;
        const double level = rmsDb(sound, a, b), wantLevel = 20 * std::log10(0.25 / std::sqrt(2.0));
        // Interpolation: a sample. The own stretcher places stretched bursts exactly when slower; faster, a frame plays
        // the source unstretched around its centre, so a burst lands up to ~3.5 ms early (Rubber Band, used up to
        // 0.13: 7 ms early at 50 %, 1 ms late at 200 %, +-1.5 ms jitter). The spread checks that nothing drifts.
        const double posTol = t.pitch ? 0.015 * kRate : 1.0;
        double lo = 1e9, hi = -1e9;
        if (countOk)
            for (size_t k = 0; k < want.size(); ++k) {
                lo = std::min(lo, found[k] - want[k]);
                hi = std::max(hi, found[k] - want[k]);
            }
        if (t.pitch && t.ramp.isEmpty()) CHECK(hi - lo <= 0.005 * kRate);
        std::printf("  %-26s %3zu/%-3zu max %8.2f %s   %8.2f (%6.1f) %s  %+5.2f dB\n", t.name, found.size(), want.size(), worst,
                    countOk && worst <= posTol ? "  " : "<-", hz, wantHz, std::abs(hz / wantHz - 1) < 0.002 ? "  " : "<-",
                    level - wantLevel);
        if (qEnvironmentVariableIsSet("AUDIO_MIXER_OFFSETS")) std::printf("       Versatz:%s\n", qPrintable(offsets));
        CHECK(countOk);
        CHECK(worst <= posTol);
        CHECK(std::abs(hz / wantHz - 1) < 0.002);
        CHECK(std::abs(level - wantLevel) < (t.pitch ? 1.0 : 0.05));
        // Ramp: the other sections' pitch
        if (!t.ramp.isEmpty()) {
            for (const auto& [from, to, v] : {std::tuple{2.0, 3.9, 0.5}, std::tuple{5.3, 5.95, 2.0}}) {
                const double f = frequency(sound, qsizetype(from * kRate), qsizetype(to * kRate)), w = t.pitch ? 1000 : 1000 * v;
                std::printf("  %-26s %-28s %8.2f (%6.1f) %s\n", "", QString("  %1-%2 s").arg(from).arg(to).toUtf8().constData(),
                            f, w, std::abs(f / w - 1) < 0.002 ? "  " : "<-");
                CHECK(std::abs(f / w - 1) < 0.002);
            }
        }
        // Block size does not matter
        CHECK(renderClip(tone, 1920) == sound);
    }
    // Reverse at 100 %: exactly the samples backwards
    {
        Clip c = synthClip(src.tone, 5, 150, 1.0, true, false);
        const std::vector<float> got = renderClip(c);
        auto reader = Audio::Reader::open(src.tone);
        std::vector<float> fwd(size_t(kSynthSeconds * kRate)), r2(fwd.size());
        float* buf[2] = {fwd.data(), r2.data()};
        reader->read(0, int(fwd.size()), buf);
        bool same = true;
        // clip material sample mu = u + 5 frames; playback index = mu -> file sample L - 1 - mu (declick at the edges)
        for (qsizetype u = qsizetype(0.01 * kRate); u < qsizetype(got.size()) - qsizetype(0.01 * kRate); ++u)
            same = same && got[size_t(u)] == fwd[size_t(qsizetype(fwd.size()) - 1 - (u + qsizetype(5 * kSpf)))];
        std::printf("  rückwärts 100 %%: Samples rückwärts %s\n", same ? "gleich" : "ANDERS");
        CHECK(same);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("audio_mixer");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QStringList only = qEnvironmentVariable("AUDIO_MIXER_CASES").split(',', Qt::SkipEmptyParts);
    if (only.isEmpty() || only.contains("synth")) {
        QTemporaryDir tmp;
        Synth src;
        if (CHECK(tmp.isValid()) && CHECK(makeSynth(tmp.path(), &src))) {
            checkCurves(src);
            checkJumps(src);
            checkLive(src);
            checkRetimed(src);
        }
    }
    g_dir = qEnvironmentVariable("AUDIO_REFERENCE_DIR", SCHNEIDI_AUDIO_REFERENCE);
    if (g_dir.isEmpty() || !QFileInfo::exists(g_dir + "/manifest.json")) {
        std::printf("Referenzen übersprungen: keine Ton-Referenzen (docs/testdaten/ton-referenz)\n");
        return Check::result();
    }
    const QHash<QString, Expect> expect = expectations();

    std::printf("  %-18s %-24s %-24s %s\n", "Fall", "MLT-Modus max (dB)", "genau max / RMS (dB)", "ms");
    QVector<QPair<QString, QString>> cases; // project, reference
    for (const QString& file : QDir(g_dir + "/projekte").entryList({"*.schneidi"}, QDir::Files, QDir::Name))
        cases.push_back({g_dir + "/projekte/" + file, g_dir + "/referenz/" + QFileInfo(file).completeBaseName() + ".flac"});
    // The user's real project (Clipi clips in ~/Videos/clipi): sound and the time of a whole sound export
    cases.push_back({g_dir + "/echt/tiktok.schneidi", g_dir + "/echt/tiktok.flac"});
    for (const auto& [project, reference] : cases) {
        const QString name = QFileInfo(project).completeBaseName();
        if (!only.isEmpty() && !only.contains(name)) continue;
        if (!QFileInfo::exists(project)) continue;
        Loaded l;
        if (!CHECK(load(project, &l))) continue;
        qint64 ms = 0, exactMs = 0;
        Audio::Mixer::Stats stats;
        const std::vector<float> compat = render(l, true, 1920, &ms, &stats);
        if (stats.missing) {
            std::printf("  %-18s übersprungen: %d Quellen fehlen\n", qPrintable(name), stats.missing);
            continue;
        }
        const std::vector<float> exact = render(l, false, 0, &exactMs);
        const std::vector<float> ref = decode(reference);
        const Expect e = expect.value(name);
        const Result rc = measure(compat, ref, e.compat), re = measure(exact, ref, e.exact);
        const bool lenOk = std::abs(qsizetype(compat.size()) - qsizetype(ref.size())) <= 2 * e.lengthTol
            && std::abs(qsizetype(exact.size()) - qsizetype(ref.size())) <= 2 * e.lengthTol;
        auto db = [](double v) { return v > 0 ? 20 * std::log10(v) : -200.0; };
        if (rc.share >= 0 || re.share >= 0)
            std::printf("       nach Ausrichten gleich (< -60 dB): MLT-Modus %.1f %%, genau %.1f %% der Abschnitte\n",
                        rc.share * 100, re.share * 100);
        if (rc.spectral >= 0 || re.spectral >= 0)
            std::printf("       Klang (Terzbänder): MLT-Modus %.2f dB @%.3fs, genau %.2f dB @%.3fs\n", rc.spectral,
                        double(rc.maxAt) / kRate, re.spectral, double(re.maxAt) / kRate);
        std::printf("  %-18s %7.1f @%-6.3fs %s  %7.1f / %6.1f @%-6.3fs %s %5lld%s\n", qPrintable(name), db(rc.maxDiff),
                    double(rc.maxAt) / kRate, rc.ok ? "  " : "<-", db(re.maxDiff), re.rmsDb, double(re.maxAt) / kRate,
                    re.ok ? "  " : "<-", ms, lenOk ? "" : "  Länge!");
        if (!lenOk)
            std::printf("       Länge: MLT-Modus %zu, genau %zu, Referenz %zu Samples\n", compat.size() / 2, exact.size() / 2,
                        ref.size() / 2);
        if (name == "tiktok") {
            // Acceptance of step 2: the whole sound faster than the MLT path's sound export (8.7 s when measured)
            std::printf("       Ton von %.1f s gemischt in %.2f s (MLT-Modus %.2f s), Abnahme: unter 8,7 s\n",
                        double(compat.size() / 2) / kRate, exactMs / 1000.0, ms / 1000.0);
            CHECK(exactMs < 8700);
        }
        if (e.why && (!rc.ok || !re.ok)) std::printf("       %s\n", e.why);
        // AUDIO_MIXER_KEEP=<dir>: renders as raw interleaved float (<case>-mlt.f32, <case>-genau.f32) for analysis
        if (const QString keep = qEnvironmentVariable("AUDIO_MIXER_KEEP"); !keep.isEmpty()) {
            QDir().mkpath(keep);
            for (const auto& [suffix, data] : {std::pair{"mlt", &compat}, std::pair{"genau", &exact}}) {
                QFile f(QString("%1/%2-%3.f32").arg(keep, name, suffix));
                if (f.open(QIODevice::WriteOnly)) f.write(reinterpret_cast<const char*>(data->data()), qint64(data->size() * sizeof(float)));
            }
        }
        // AUDIO_MIXER_DUMP=<case>:<seconds>: samples around a place (reference, MLT mode, exact)
        if (const QString dump = qEnvironmentVariable("AUDIO_MIXER_DUMP"); dump.section(':', 0, 0) == name) {
            const qsizetype at = qsizetype(dump.section(':', 1).toDouble() * kRate);
            for (qsizetype i = std::max<qsizetype>(0, at - 8); i < at + 8 && size_t(i) * 2 + 1 < ref.size(); ++i)
                std::printf("       %7lld  ref %+.7f %+.7f  mlt %+.7f %+.7f  genau %+.7f %+.7f\n", qlonglong(i), ref[size_t(i) * 2],
                            ref[size_t(i) * 2 + 1], compat[size_t(i) * 2], compat[size_t(i) * 2 + 1], exact[size_t(i) * 2],
                            exact[size_t(i) * 2 + 1]);
        }
        if (qEnvironmentVariableIsSet("AUDIO_MIXER_ALIGN")) {
            std::printf("     MLT-Modus gegen Referenz:\n");
            alignReport(compat, ref);
            std::printf("     genau gegen Referenz:\n");
            alignReport(exact, ref);
        }
        CHECK(lenOk);
        CHECK(rc.ok);
        CHECK(re.ok);
    }
    return Check::result();
}
