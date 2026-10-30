// SPDX-License-Identifier: AGPL-3.0-only
// Analyses on the own audio engine (Audio::Mixer for clip and timeline levels, Audio::Reader for waveforms and sound
// sync), checked against values known from the generated sources:
// - clip peak/loudness ("Normalize audio levels"): the source's level (0.5 sine = -6.02 dBFS), clip volume/fades/
//   effects ignored, also with speed and reversed; loudness against frozen values of the generated tone
// - frame peaks of a clip (silence detection) and of the mix (silence, ducking): the source's levels frame by frame,
//   the mix = Audio::Mixer itself (the export's sound)
// - sound later than the picture (phones, OBS): frame peaks, waveform and sync envelope start exactly at 0.3 s
// - sound sync: the envelope = that of the file decoded by ffmpeg, offsets exact
// - waveform (MediaCache): levels of the source, sample-exact start of a late stream
// With the sound references (private docs folder; skipped without it): the analysis of every reference project hears
// what the export mixer writes (frame peaks equal).

#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/MediaCache.h"
#include "engine/SoundSync.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef SCHNEIDI_AUDIO_REFERENCE
#define SCHNEIDI_AUDIO_REFERENCE ""
#endif

namespace {

const ProjectFormat kFmt{320, 180, {25, 1}};
constexpr int kFrames = 150; // 6 s at 25 fps
// Frozen loudness of the generated tone (see checkClipLevels); regenerate: run the test, take the printed values
constexpr double kToneLufs = -9.404, kPartLufs = -12.412, kSlowLufs = -9.599, kMixLufs = -8.706;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Every frame in [from, to) with a peak within tol of want
bool levels(const std::vector<float>& peaks, int from, int to, double want, double tol)
{
    for (int i = from; i < to; ++i)
        if (i >= int(peaks.size()) || !near(peaks[size_t(i)], want, tol)) {
            std::printf("       frame %d: %.5f, want %.5f\n", i, i < int(peaks.size()) ? peaks[size_t(i)] : -1.f, want);
            return false;
        }
    return true;
}

Clip clipOf(const QString& path, int id = 1, int start = 0, int in = 0, int out = kFrames - 1)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX)
{
    double m = 0;
    for (size_t i = from; i < std::min({a.size(), b.size(), to}); ++i) m = std::max(m, std::abs(double(a[i]) - double(b[i])));
    return m;
}

// First frame with sound (peak above -60 dBFS), -1 = none
int firstSound(const std::vector<float>& peaks)
{
    for (size_t i = 0; i < peaks.size(); ++i)
        if (peaks[i] > 0.001f) return int(i);
    return -1;
}

void checkClipLevels(const QString& tone)
{
    std::printf("Pegel eines Clips (Normalisieren):\n");
    // Frozen reference values of the generated tone (0.5 sine 440 Hz 0-2 s, 0.25 sine 1 kHz 3-6 s, stereo PCM):
    // loudness measured once with the meter checked against the standard (test_loudness); the peak is the amplitude
    struct Case {
        const char* name;
        Clip clip;
        double peakDb, lufs, tol;
    };
    const double half = 20 * std::log10(0.5), quarter = 20 * std::log10(0.25);
    Clip slow = clipOf(tone, 1, 0, 0, 2 * kFrames - 1);
    slow.speed = 0.5;
    slow.keepPitch = false;
    Clip rev = clipOf(tone);
    rev.reverse = true;
    Clip part = clipOf(tone, 1, 0, 60, 120); // only the quiet part (0.25) after a bit of silence
    const Case cases[] = {{"ganzer Clip", clipOf(tone), half, kToneLufs, 0.05},
                          {"Ausschnitt", part, quarter, kPartLufs, 0.05},
                          {"50 % ohne Tonhöhe", slow, half, kSlowLufs, 0.1},
                          {"rückwärts", rev, half, kToneLufs, 0.05}};
    for (const Case& c : cases) {
        const auto peak = AudioAnalysis::clipPeakDb(kFmt, c.clip);
        const auto loud = AudioAnalysis::clipLoudness(kFmt, c.clip);
        if (!CHECK(peak && loud)) continue;
        std::printf("  %-20s Spitze %.3f dBFS, Lautheit %.3f LUFS, True Peak %.3f dBTP\n", c.name, *peak, loud->integrated,
                    loud->truePeakDb);
        CHECK(near(*peak, c.peakDb, 0.01));
        CHECK(near(loud->integrated, c.lufs, c.tol));
        // a low sine: the true peak is the sample peak (within the meter's tolerance)
        CHECK(loud->truePeakDb >= *peak - 0.01 && loud->truePeakDb < *peak + 0.1);
    }
    // The source alone: volume, pan, fades, transitions and effects do not count
    Clip loud = clipOf(tone);
    loud.volumeDb = -12;
    loud.pan = 80;
    loud.fadeIn = loud.fadeOut = 40;
    loud.subframe = 50;
    const auto plain = AudioAnalysis::clipPeakDb(kFmt, clipOf(tone));
    const auto styled = AudioAnalysis::clipPeakDb(kFmt, loud);
    CHECK(plain && styled && *plain == *styled);
    // A clip later in the timeline measures the same
    CHECK(styled && AudioAnalysis::clipPeakDb(kFmt, clipOf(tone, 1, 400)) == plain);
    // No sound: title, freeze frame, missing file, a stream the file does not have (silent like in the mix)
    Clip title = clipOf({});
    title.kind = ClipKind::Title;
    Clip freeze = clipOf(tone);
    freeze.freeze = true;
    Clip stream = clipOf(tone);
    stream.audioStream = 3;
    CHECK(!AudioAnalysis::clipPeakDb(kFmt, title));
    CHECK(!AudioAnalysis::clipPeakDb(kFmt, freeze));
    CHECK(!AudioAnalysis::clipPeakDb(kFmt, clipOf(tone + ".missing")));
    CHECK(!AudioAnalysis::clipPeakDb(kFmt, stream));
    // Cancel
    CHECK(!AudioAnalysis::clipLoudness(kFmt, clipOf(tone), [](double f) { return f < 0.3; }));
}

void checkFramePeaks(const QString& tone, const QString& late)
{
    std::printf("Spitze je Bild (Stille, Ducking):\n");
    const auto own = AudioAnalysis::clipFramePeaks(kFmt, clipOf(tone));
    if (!CHECK(own)) return;
    CHECK_EQ(own->size(), size_t(kFrames));
    // 0.5 sine for 2 s (frames 0-49), silent to 3 s (50-74), 0.25 sine to the end (24-bit source: exact to 1e-3)
    CHECK(levels(*own, 0, 50, 0.5, 1e-3));
    CHECK(levels(*own, 50, 75, 0.0, 0.0));
    CHECK(levels(*own, 75, kFrames, 0.25, 1e-3));

    // Sound stream 0.3 s after the picture: frame 7 (0.28-0.32 s) is the first with sound
    const auto ownLate = AudioAnalysis::clipFramePeaks(kFmt, clipOf(late));
    if (!CHECK(ownLate)) return;
    std::printf("  Ton 0,3 s später: erstes Bild mit Ton %d (richtig: 7)\n", firstSound(*ownLate));
    CHECK_EQ(firstSound(*ownLate), 7);
    CHECK(levels(*ownLate, 8, kFrames, 0.5, 2e-3)); // 16-bit source

    // The mix: two tracks, faders, pan, master fader and limiter
    Timeline tl;
    tl.audio.resize(2);
    tl.audio[0].clips << clipOf(tone, 1, 0);
    tl.audio[0].volumeDb = -4;
    Clip second = clipOf(tone, 2, 75, 0, 99);
    second.volumeDb = 3;
    second.pan = -40;
    tl.audio[1].clips << second;
    tl.masterVolumeDb = 2;
    tl.masterLimiter = true;
    tl.masterLimiterDb = -3;
    const int end = TimelineOps::endFrame(tl);
    const auto ownMix = AudioAnalysis::timelineFramePeaks(kFmt, tl, 0, end);
    const auto ownLoud = AudioAnalysis::timelineLoudness(kFmt, tl, 0, end);
    if (!CHECK(ownMix && ownLoud)) return;
    std::printf("  Mix: %zu Bilder, Lautheit %.3f LUFS, True Peak %.2f dBTP\n", ownMix->size(), ownLoud->integrated,
                ownLoud->truePeakDb);
    CHECK_EQ(ownMix->size(), size_t(end));
    // A1 alone: 0.5 at -4 dB track and +2 dB master = 0.5 * 10^(-2/20), below the limiter's ceiling (0.708)
    CHECK(levels(*ownMix, 2, 48, 0.5 * std::pow(10.0, -2.0 / 20), 2e-3));
    CHECK(levels(*ownMix, 50, 75, 0.0, 0.0)); // A1 silent from frame 50, A2 starts at 75
    // Both: 0.25 * 0.631 + 0.5 * 1.413 (A2 +3 dB) = 0.86, times 1.26 master = 1.09 -> held by the limiter at -3 dB
    const double ceiling = std::pow(10.0, -3.0 / 20);
    double loudest = 0;
    for (int i = 80; i < 120; ++i) loudest = std::max(loudest, double((*ownMix)[size_t(i)]));
    std::printf("  beide Spuren: lauteste Spitze %.4f (Decke %.4f)\n", loudest, ceiling);
    CHECK(loudest <= ceiling + 1e-4 && loudest > ceiling - 0.05);
    CHECK(ownLoud->truePeakDb < -2.9); // the limiter holds
    CHECK(near(ownLoud->integrated, kMixLufs, 0.05));

    // = the mixer itself (the export's sound)
    Audio::Mixer mixer(tl, kFmt.rate);
    std::vector<float> direct;
    std::vector<float> l, r;
    for (int f = 0; f < end; ++f) {
        const int64_t a = mixer.frameStart(f);
        const int n = int(mixer.frameStart(f + 1) - a);
        l.resize(size_t(n));
        r.resize(size_t(n));
        float* out[2] = {l.data(), r.data()};
        mixer.render(a, n, out);
        float p = 0;
        for (int i = 0; i < n; ++i) p = std::max({p, std::abs(l[size_t(i)]), std::abs(r[size_t(i)])});
        direct.push_back(p);
    }
    CHECK(*ownMix == direct);
    // A part of it: the same frames
    const auto part = AudioAnalysis::timelineFramePeaks(kFmt, tl, 40, 90);
    CHECK(part && part->size() == 50 && std::equal(part->begin(), part->end(), ownMix->begin() + 40));
    // MixMeter: several measurements of one timeline, fader changes in between
    AudioAnalysis::MixMeter meter(kFmt);
    const auto m1 = meter.measure(tl, 0, end);
    tl.audio[0].volumeDb = -10;
    const auto m2 = meter.measure(tl, 0, end);
    tl.audio[0].volumeDb = -4;
    const auto m3 = meter.measure(tl, 0, end);
    CHECK(m1 && m2 && m3 && m1->integrated == ownLoud->integrated && m3->integrated == m1->integrated
          && m2->integrated < m1->integrated - 0.2);

    // Ducking ranges: where A2 sounds (its source is silent 2-3 s = frames 125-149)
    Ducking::Params dp;
    const auto ownRanges = AudioAnalysis::soundRanges(kFmt, tl, {1}, dp);
    if (CHECK(ownRanges)) CHECK(*ownRanges == (Ducking::Ranges{{75, 125}, {150, 175}}));
}

double correlation(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i) {
        ab += double(a[i]) * b[i];
        aa += double(a[i]) * a[i];
        bb += double(b[i]) * b[i];
    }
    return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}

void checkSync(const QString& noise, const QString& noiseLate, const QString& padded)
{
    std::printf("Ton-Sync:\n");
    const auto ownEnv = SoundSync::fileEnvelope(noise, 0);
    const auto ownLate = SoundSync::align({{padded, 0}, {noiseLate, 0}});
    const auto ownPad = SoundSync::align({{noise, 0}, {padded, 0}});
    if (!CHECK(ownEnv)) return;
    // The truth: the envelope of the file decoded by ffmpeg (the source is mono PCM)
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", noise, "-map", "0:a:0", "-f", "f32le", "-"});
    p.waitForFinished(60000);
    const QByteArray raw = p.readAllStandardOutput();
    const auto* pcm = reinterpret_cast<const float*>(raw.constData());
    const std::vector<float> truth = AudioSync::envelope(std::vector<float>(pcm, pcm + raw.size() / 4), 48000);
    std::printf("  Hüllkurve: %zu Werte (Datei %zu), Korrelation mit der Datei %.7f\n", ownEnv->size(), truth.size(),
                correlation(truth, *ownEnv));
    CHECK_EQ(ownEnv->size(), truth.size());
    CHECK(correlation(truth, *ownEnv) > 0.99999);
    if (!CHECK(ownPad.size() == 2 && ownLate.size() == 2)) return;
    std::printf("  WAV 0,3 s verzögert: %+.4f s\n", ownPad[1].offsetS);
    CHECK(ownPad[1].found && std::abs(std::abs(ownPad[1].offsetS) - 0.3) < 0.006);
    // The late stream and the padded WAV hold the sound at the same place (0.3 s after position 0)
    std::printf("  Ton 0,3 s später als Bild gegen WAV mit 0,3 s Stille: %+.4f s (richtig: 0)\n", ownLate[1].offsetS);
    CHECK(ownLate[1].found && std::abs(ownLate[1].offsetS) < 0.006);
    // Cancel, missing stream
    CHECK(!SoundSync::fileEnvelope(noise, 0, [](double f) { return f < 0.5; }));
    CHECK(!SoundSync::fileEnvelope(noise, 2));
}

// Waveform of a file from a fresh cache (the disk cache of the test paths is cleared first)
std::shared_ptr<const Waveform> waveform(const QString& path, qint64* ms)
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/peaks").removeRecursively();
    MediaCache cache;
    cache.setFormat(kFmt);
    QElapsedTimer t;
    t.start();
    std::shared_ptr<const Waveform> w = cache.waveform(path);
    while ((!w || !w->complete) && t.elapsed() < 60000) {
        QEventLoop loop;
        QObject::connect(&cache, &MediaCache::updated, &loop, &QEventLoop::quit);
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec();
        w = cache.waveform(path);
    }
    if (ms) *ms = t.elapsed();
    return w && w->complete ? w : nullptr;
}

void checkWaveform(const QString& tone, const QString& late)
{
    std::printf("Wellenform:\n");
    qint64 ms = 0;
    const auto own = waveform(tone, &ms);
    if (!CHECK(own && !own->levels.isEmpty())) return;
    const QVector<quint8>& b = own->levels[0];
    std::printf("  %d Bilder (%lld ms)\n", own->frames, ms);
    CHECK_EQ(own->frames, kFrames);
    const int k = Waveform::kBucketsPerFrame;
    CHECK_EQ(b.size(), kFrames * k);
    // 0.5 -> 16384 >> 7 (s16), silence 0, 0.25 -> 64 (every bucket spans several periods of the sine)
    auto range = [&](int from, int to, int lo, int hi) {
        for (int i = from * k; i < to * k && i < b.size(); ++i)
            if (b[i] < lo || b[i] > hi) {
                std::printf("       bucket %d: %d, want %d-%d\n", i, b[i], lo, hi);
                return false;
            }
        return true;
    };
    CHECK(range(0, 50, 127, 128));
    CHECK(range(50, 75, 0, 0));
    CHECK(range(76, kFrames, 63, 64));

    // Late stream: the sound begins exactly with bucket 4 of frame 7 (sample 14400 = 13440 + 4 * 240)
    const auto ownLate = waveform(late, nullptr);
    if (!CHECK(ownLate)) return;
    auto first = [](const Waveform& w) {
        for (int i = 0; i < w.levels[0].size(); ++i)
            if (w.levels[0][i] > 0) return i;
        return -1;
    };
    std::printf("  Ton 0,3 s später: erster Wert Bild %.3f (richtig: 7,500)\n", double(first(*ownLate)) / k);
    CHECK_EQ(first(*ownLate), 7 * k + 4);
}

void checkReferences(const QString& refDir)
{
    std::printf("Referenzprojekte (Lautheit, Analyse = Export-Mischer):\n");
    QStringList projects;
    for (const QString& f : QDir(refDir + "/projekte").entryList({"*.schneidi"}, QDir::Files, QDir::Name))
        projects << refDir + "/projekte/" + f;
    if (QFileInfo::exists(refDir + "/echt/tiktok.schneidi")) projects << refDir + "/echt/tiktok.schneidi";
    for (const QString& path : projects) {
        const QString name = QFileInfo(path).completeBaseName();
        ProjectData d;
        QString error;
        if (!CHECK(ProjectFile::load(path, &d, &error))) continue;
        Project project;
        project.load(d);
        const Timeline tl = project.renderTimeline();
        bool missing = false;
        for (const MediaInfo& m : project.media()) missing = missing || !QFileInfo::exists(m.path);
        if (missing) {
            std::printf("  %-16s übersprungen: Quellen fehlen\n", qPrintable(name));
            continue;
        }
        const int end = TimelineOps::endFrame(tl);
        QElapsedTimer t;
        t.start();
        const auto ownLoud = AudioAnalysis::timelineLoudness(d.format, tl, 0, end);
        const qint64 ownMs = t.elapsed();
        const auto ownPeaks = AudioAnalysis::timelineFramePeaks(d.format, tl, 0, end);
        if (!CHECK(ownLoud && ownPeaks)) continue;
        std::printf("  %-16s %8.3f LUFS, %d Bilder, %lld ms\n", qPrintable(name), ownLoud->integrated, end, ownMs);
        // The analysis hears what the export writes: the mixer with the project's media lengths
        QHash<QString, int> lengths;
        for (const MediaInfo& m : project.media()) lengths.insert(m.path, m.length);
        Audio::Mixer::Options o;
        o.fileLength = [&lengths](const QString& p) { return lengths.value(p); };
        Audio::Mixer mixer(tl, d.format.rate, o);
        std::vector<float> l, r;
        bool same = true;
        for (int f = 0; f < end && same; ++f) {
            const int64_t a = mixer.frameStart(f);
            const int n = int(mixer.frameStart(f + 1) - a);
            l.resize(size_t(n));
            r.resize(size_t(n));
            float* out[2] = {l.data(), r.data()};
            mixer.render(a, n, out);
            float p = 0;
            for (int i = 0; i < n; ++i) p = std::max({p, std::abs(l[size_t(i)]), std::abs(r[size_t(i)])});
            same = p == (*ownPeaks)[size_t(f)];
            if (!same) std::printf("       Bild %d: Analyse %.6f, Export-Mischer %.6f\n", f, (*ownPeaks)[size_t(f)], p);
        }
        CHECK(same);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio_analysis");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initEngine();

    // 0.5 sine 0-2 s, silence 2-3 s, 0.25 sine 3-6 s
    const QString tone = Check::makeMedia(
        tmp.filePath("tone.mov"),
        {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=6", "-f", "lavfi", "-i",
         "aevalsrc=if(lt(t\\,2)\\,0.5*sin(2*PI*440*t)\\,if(lt(t\\,3)\\,0\\,0.25*sin(2*PI*1000*t))):s=48000:d=6:c=stereo",
         "-c:v", "mpeg4", "-q:v", "5", "-c:a", "pcm_s24le", "-shortest"});
    // The same sound stream starting 0.3 s after the picture
    const QString late = Check::makeMedia(
        tmp.filePath("late.mov"),
        {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=6", "-itsoffset", "0.3", "-f", "lavfi", "-i",
         "aevalsrc=0.5*sin(2*PI*440*t):s=48000:d=5.7:c=stereo", "-c:v", "mpeg4", "-q:v", "5", "-c:a", "pcm_s16le"});
    // Noise bursts at irregular places (sync), as a video, as a video with the sound 0.3 s late, as a WAV with 0.3 s
    // of silence in front
    const QString bursts = "(random(0)*2-1)*gt(sin(2*PI*1.7*t)+sin(2*PI*0.63*t)\\,0.9)*0.5";
    const QString noise = Check::makeMedia(
        tmp.filePath("noise.mov"), {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=12", "-f", "lavfi", "-i",
                                    QString("aevalsrc=%1:s=48000:d=12").arg(bursts), "-c:v", "mpeg4", "-q:v", "5",
                                    "-c:a", "pcm_s16le"});
    const QString noiseLate = Check::makeMedia(
        tmp.filePath("noise_late.mov"),
        {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=12", "-itsoffset", "0.3", "-f", "lavfi", "-i",
         QString("aevalsrc=%1:s=48000:d=11.7").arg(bursts), "-c:v", "mpeg4", "-q:v", "5", "-c:a", "pcm_s16le"});
    const QString padded = Check::makeMedia(
        tmp.filePath("padded.wav"),
        {"-f", "lavfi", "-i", QString("aevalsrc=%1:s=48000:d=11.7").arg(bursts), "-af", "adelay=300:all=1"});
    if (!CHECK(!tone.isEmpty() && !late.isEmpty() && !noise.isEmpty() && !noiseLate.isEmpty() && !padded.isEmpty()))
        return Check::result();

    checkClipLevels(tone);
    checkFramePeaks(tone, late);
    checkSync(noise, noiseLate, padded);
    checkWaveform(tone, late);

    const QString refDir = qEnvironmentVariable("AUDIO_REFERENCE_DIR", SCHNEIDI_AUDIO_REFERENCE);
    if (refDir.isEmpty() || !QFileInfo::exists(refDir + "/manifest.json")) {
        std::printf("Referenzen übersprungen: keine Ton-Referenzen (docs/testdaten/ton-referenz)\n");
        return Check::result();
    }
    checkReferences(refDir);
    return Check::result();
}
