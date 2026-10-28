// SPDX-License-Identifier: AGPL-3.0-only
// Own audio engine, step 4 (docs/archiv/plan-0.10-ton.md): the analyses take their sound from the own engine
// (Audio::Mixer for clip and timeline levels, Audio::Reader for waveforms and sound sync) instead of MLT.
// Every check runs both paths (SCHNEIDI_AUDIO=mlt|own in the same process) on generated sources:
// - clip peak/loudness ("Normalize audio levels"): own = MLT, clip volume/fades/effects ignored, also with speed
// - frame peaks of a clip (silence detection) and of the mix (silence, ducking): own = MLT frame by frame, own mix
//   = Audio::Mixer itself
// - sound later than the picture (phones, OBS): frame peaks, waveform and sync envelope start exactly at 0.3 s on
//   the own path (MLT places it 65-87 ms early, only reported)
// - sound sync: envelopes like MLT, offsets exact
// - waveform (MediaCache): same values as MLT, sample-exact start of a late stream
// With the sound references of step 0 (private docs folder; skipped without it): loudness and frame peaks of every
// reference project, own against MLT and against the mixer with the project's media lengths, plus the times.

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

void usePath(bool own) { qputenv("SCHNEIDI_AUDIO", own ? "own" : "mlt"); }

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
    struct Case {
        const char* name;
        Clip clip;
        double tolDb;
    };
    Clip slow = clipOf(tone, 1, 0, 0, 2 * kFrames - 1);
    slow.speed = 0.5;
    slow.keepPitch = false;
    Clip rev = clipOf(tone);
    rev.reverse = true;
    Clip part = clipOf(tone, 1, 0, 60, 120); // only the quiet part (0.25)
    const Case cases[] = {{"ganzer Clip", clipOf(tone), 0.01}, {"Ausschnitt", part, 0.01}, {"50 % ohne Tonhöhe", slow, 0.05},
                          {"rückwärts", rev, 0.01}};
    for (const Case& c : cases) {
        usePath(false);
        const auto mltPeak = AudioAnalysis::clipPeakDb(kFmt, c.clip);
        const auto mltLoud = AudioAnalysis::clipLoudness(kFmt, c.clip);
        usePath(true);
        const auto ownPeak = AudioAnalysis::clipPeakDb(kFmt, c.clip);
        const auto ownLoud = AudioAnalysis::clipLoudness(kFmt, c.clip);
        if (!CHECK(mltPeak && ownPeak && mltLoud && ownLoud)) continue;
        std::printf("  %-20s Spitze %.3f / %.3f dBFS, Lautheit %.3f / %.3f LUFS, True Peak %.3f / %.3f (MLT / eigen)\n",
                    c.name, *mltPeak, *ownPeak, mltLoud->integrated, ownLoud->integrated, mltLoud->truePeakDb,
                    ownLoud->truePeakDb);
        CHECK(std::abs(*mltPeak - *ownPeak) < c.tolDb);
        CHECK(std::abs(mltLoud->integrated - ownLoud->integrated) < 0.05);
        CHECK(std::abs(mltLoud->truePeakDb - ownLoud->truePeakDb) < 0.1);
    }
    usePath(true);
    // The source alone: volume, pan, fades, transitions and effects do not count
    Clip loud = clipOf(tone);
    loud.volumeDb = -12;
    loud.pan = 80;
    loud.fadeIn = loud.fadeOut = 40;
    loud.subframe = 50;
    const auto plain = AudioAnalysis::clipPeakDb(kFmt, clipOf(tone));
    const auto styled = AudioAnalysis::clipPeakDb(kFmt, loud);
    CHECK(plain && styled && *plain == *styled);
    CHECK(plain && std::abs(*plain - 20 * std::log10(0.5)) < 0.01);
    // A clip later in the timeline measures the same
    CHECK(styled && AudioAnalysis::clipPeakDb(kFmt, clipOf(tone, 1, 400)) == plain);
    // No sound: title, freeze frame, missing file, missing stream
    Clip title = clipOf({});
    title.kind = ClipKind::Title;
    Clip freeze = clipOf(tone);
    freeze.freeze = true;
    Clip stream = clipOf(tone);
    stream.audioStream = 3;
    for (bool own : {false, true}) {
        usePath(own);
        CHECK(!AudioAnalysis::clipPeakDb(kFmt, title));
        CHECK(!AudioAnalysis::clipPeakDb(kFmt, freeze));
        CHECK(!AudioAnalysis::clipPeakDb(kFmt, clipOf(tone + ".missing")));
    }
    // A stream the file does not have: silent like in the mix (MLT took the first stream instead)
    usePath(true);
    CHECK(!AudioAnalysis::clipPeakDb(kFmt, stream));
    // Cancel
    usePath(true);
    CHECK(!AudioAnalysis::clipLoudness(kFmt, clipOf(tone), [](double f) { return f < 0.3; }));
}

void checkFramePeaks(const QString& tone, const QString& late)
{
    std::printf("Spitze je Bild (Stille, Ducking):\n");
    usePath(false);
    const auto mlt = AudioAnalysis::clipFramePeaks(kFmt, clipOf(tone));
    usePath(true);
    const auto own = AudioAnalysis::clipFramePeaks(kFmt, clipOf(tone));
    if (!CHECK(mlt && own)) return;
    std::printf("  Clip: %zu / %zu Bilder, größte Abweichung %.5f\n", mlt->size(), own->size(), maxDiff(*mlt, *own));
    CHECK_EQ(own->size(), size_t(kFrames));
    CHECK_EQ(mlt->size(), own->size());
    CHECK(maxDiff(*mlt, *own) < 1e-4);
    CHECK((*own)[55] == 0.f && (*own)[70] == 0.f); // 2-3 s silent

    // Sound stream 0.3 s after the picture: frame 7 (0.28-0.32 s) is the first with sound
    usePath(false);
    const auto mltLate = AudioAnalysis::clipFramePeaks(kFmt, clipOf(late));
    usePath(true);
    const auto ownLate = AudioAnalysis::clipFramePeaks(kFmt, clipOf(late));
    if (!CHECK(mltLate && ownLate)) return;
    std::printf("  Ton 0,3 s später: erstes Bild mit Ton MLT %d, eigen %d (richtig: 7)\n", firstSound(*mltLate),
                firstSound(*ownLate));
    CHECK_EQ(firstSound(*ownLate), 7);

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
    usePath(false);
    const auto mltMix = AudioAnalysis::timelineFramePeaks(kFmt, tl, 0, end);
    const auto mltLoud = AudioAnalysis::timelineLoudness(kFmt, tl, 0, end);
    usePath(true);
    const auto ownMix = AudioAnalysis::timelineFramePeaks(kFmt, tl, 0, end);
    const auto ownLoud = AudioAnalysis::timelineLoudness(kFmt, tl, 0, end);
    if (!CHECK(mltMix && ownMix && mltLoud && ownLoud)) return;
    // Clip edges differ on purpose (the mixer's gain curves and declick, docs/dev/ton.md); MLT's limiter delays the
    // sound by its latency (5 ms), so the frames where the source falls silent (50 on A1, 125 on A2) still sound
    // there. The rest is the same mix.
    double inner = 0;
    for (size_t i = 0; i < ownMix->size(); ++i)
        if (i != 0 && i != 50 && i != 74 && i != 75 && i != 125 && i != 149 && i != 150 && i != 174)
            inner = std::max(inner, std::abs(double((*mltMix)[i]) - double((*ownMix)[i])));
    std::printf("  Mix: %zu / %zu Bilder, Abweichung ohne Clipkanten %.5f, Lautheit %.3f / %.3f LUFS, True Peak %.2f / %.2f\n",
                mltMix->size(), ownMix->size(), inner, mltLoud->integrated, ownLoud->integrated, mltLoud->truePeakDb,
                ownLoud->truePeakDb);
    CHECK_EQ(ownMix->size(), size_t(end));
    CHECK(inner < 2e-3);
    std::printf("  A1 verstummt in Bild 50: MLT %.3f, eigen %.3f\n", (*mltMix)[50], (*ownMix)[50]);
    CHECK((*ownMix)[50] == 0.f);
    CHECK(std::abs(mltLoud->integrated - ownLoud->integrated) < 0.05);
    CHECK(std::abs(mltLoud->truePeakDb - ownLoud->truePeakDb) < 0.2);
    CHECK(ownLoud->truePeakDb < -2.9); // the limiter holds

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
    CHECK(part && part->size() == 50 && maxDiff(*part, *ownMix) >= 0 && std::equal(part->begin(), part->end(), ownMix->begin() + 40));
    // MixMeter: several measurements of one timeline, fader changes in between
    AudioAnalysis::MixMeter meter(kFmt);
    const auto m1 = meter.measure(tl, 0, end);
    tl.audio[0].volumeDb = -10;
    const auto m2 = meter.measure(tl, 0, end);
    tl.audio[0].volumeDb = -4;
    const auto m3 = meter.measure(tl, 0, end);
    CHECK(m1 && m2 && m3 && m1->integrated == ownLoud->integrated && m3->integrated == m1->integrated
          && m2->integrated < m1->integrated - 0.2);

    // Ducking ranges: where A2 sounds (its source is silent 2-3 s = frames 125-149); MLT one frame longer (limiter)
    Ducking::Params dp;
    usePath(false);
    const auto mltRanges = AudioAnalysis::soundRanges(kFmt, tl, {1}, dp);
    usePath(true);
    const auto ownRanges = AudioAnalysis::soundRanges(kFmt, tl, {1}, dp);
    if (CHECK(mltRanges && ownRanges)) {
        auto text = [](const Ducking::Ranges& rs) {
            QStringList t;
            for (const auto& r : rs) t << QString("%1-%2").arg(r.first).arg(r.second);
            return t.join(", ");
        };
        std::printf("  Ducking-Bereiche: MLT %s, eigen %s\n", qPrintable(text(*mltRanges)), qPrintable(text(*ownRanges)));
        CHECK(*ownRanges == (Ducking::Ranges{{75, 125}, {150, 175}}));
    }
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
    usePath(false);
    const auto mltEnv = SoundSync::fileEnvelope(noise, 0);
    const auto mltLate = SoundSync::align({{padded, 0}, {noiseLate, 0}});
    const auto mltPad = SoundSync::align({{noise, 0}, {padded, 0}});
    usePath(true);
    const auto ownEnv = SoundSync::fileEnvelope(noise, 0);
    const auto ownLate = SoundSync::align({{padded, 0}, {noiseLate, 0}});
    const auto ownPad = SoundSync::align({{noise, 0}, {padded, 0}});
    if (!CHECK(mltEnv && ownEnv)) return;
    // The truth: the envelope of the file decoded by ffmpeg (the source is mono PCM)
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", noise, "-map", "0:a:0", "-f", "f32le", "-"});
    p.waitForFinished(60000);
    const QByteArray raw = p.readAllStandardOutput();
    const auto* pcm = reinterpret_cast<const float*>(raw.constData());
    const std::vector<float> truth = AudioSync::envelope(std::vector<float>(pcm, pcm + raw.size() / 4), 48000);
    std::printf("  Hüllkurve: %zu / %zu Werte (Datei %zu), Korrelation mit der Datei MLT %.5f, eigen %.7f\n",
                mltEnv->size(), ownEnv->size(), truth.size(), correlation(truth, *mltEnv), correlation(truth, *ownEnv));
    CHECK_EQ(ownEnv->size(), truth.size());
    CHECK(correlation(truth, *ownEnv) > 0.99999);
    if (!CHECK(mltPad.size() == 2 && ownPad.size() == 2 && mltLate.size() == 2 && ownLate.size() == 2)) return;
    std::printf("  WAV 0,3 s verzögert: MLT %+.4f s, eigen %+.4f s\n", mltPad[1].offsetS, ownPad[1].offsetS);
    CHECK(ownPad[1].found && std::abs(std::abs(ownPad[1].offsetS) - 0.3) < 0.006);
    // The late stream and the padded WAV hold the sound at the same place (0.3 s after position 0)
    std::printf("  Ton 0,3 s später als Bild gegen WAV mit 0,3 s Stille: MLT %+.4f s, eigen %+.4f s (richtig: 0)\n",
                mltLate[1].offsetS, ownLate[1].offsetS);
    CHECK(ownLate[1].found && std::abs(ownLate[1].offsetS) < 0.006);
    // Cancel, missing stream
    CHECK(!SoundSync::fileEnvelope(noise, 0, [](double f) { return f < 0.5; }));
    CHECK(!SoundSync::fileEnvelope(noise, 2));
}

// Waveform of a file from a fresh cache (the disk cache of the test paths is cleared first)
std::shared_ptr<const Waveform> waveform(const QString& path, bool own, qint64* ms)
{
    usePath(own);
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
    qint64 mltMs = 0, ownMs = 0;
    const auto mlt = waveform(tone, false, &mltMs);
    const auto own = waveform(tone, true, &ownMs);
    if (!CHECK(mlt && own && !mlt->levels.isEmpty() && !own->levels.isEmpty())) return;
    const QVector<quint8>& a = mlt->levels[0];
    const QVector<quint8>& b = own->levels[0];
    int worst = 0, off = 0;
    for (int i = 0; i < std::min(a.size(), b.size()); ++i) {
        const int d = std::abs(int(a[i]) - int(b[i]));
        worst = std::max(worst, d);
        off += d > 1;
    }
    std::printf("  %d / %d Bilder, größte Abweichung %d von 255, %d Werte > 1 daneben (MLT %lld ms, eigen %lld ms)\n",
                mlt->frames, own->frames, worst, off, mltMs, ownMs);
    CHECK_EQ(own->frames, kFrames);
    CHECK_EQ(mlt->frames, own->frames);
    CHECK(worst <= 1);
    CHECK(b[0] >= 127 && b[0] <= 128); // 0.5 -> 16384 >> 7 (s16 like the MLT path)

    // Late stream: the sound begins exactly with bucket 4 of frame 7 (sample 14400 = 13440 + 4 * 240)
    const auto mltLate = waveform(late, false, nullptr);
    const auto ownLate = waveform(late, true, nullptr);
    if (!CHECK(mltLate && ownLate)) return;
    auto first = [](const Waveform& w) {
        for (int i = 0; i < w.levels[0].size(); ++i)
            if (w.levels[0][i] > 0) return i;
        return -1;
    };
    const int k = Waveform::kBucketsPerFrame;
    std::printf("  Ton 0,3 s später: erster Wert MLT Bild %.3f, eigen Bild %.3f (richtig: 7,500)\n",
                double(first(*mltLate)) / k, double(first(*ownLate)) / k);
    CHECK_EQ(first(*ownLate), 7 * k + 4);
}

void checkReferences(const QString& refDir)
{
    std::printf("Referenzprojekte (Lautheit und Spitze je Bild, MLT / eigen):\n");
    QStringList projects;
    for (const QString& f : QDir(refDir + "/projekte").entryList({"*.schneidi"}, QDir::Files, QDir::Name))
        projects << refDir + "/projekte/" + f;
    if (QFileInfo::exists(refDir + "/echt/tiktok.schneidi")) projects << refDir + "/echt/tiktok.schneidi";
    // Where MLT's sound is known to be wrong (docs/archiv/plan-0.10-ton.md, steps 0-2): sound stream later than the picture,
    // tempo with pitch (Rubberband settles differently), MKV dropping samples (tiktok); 5.1: MLT's analyses ask for
    // float and get the downmix without normalization, 7.7 dB louder than MLT's own export (s16, normalized)
    const QStringList mltWrong{"late_start", "speed_pitch", "ramp", "tiktok", "surround"};
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
        usePath(false);
        t.start();
        const auto mltLoud = AudioAnalysis::timelineLoudness(d.format, tl, 0, end);
        const qint64 mltMs = t.elapsed();
        const auto mltPeaks = AudioAnalysis::timelineFramePeaks(d.format, tl, 0, end);
        usePath(true);
        t.restart();
        const auto ownLoud = AudioAnalysis::timelineLoudness(d.format, tl, 0, end);
        const qint64 ownMs = t.elapsed();
        const auto ownPeaks = AudioAnalysis::timelineFramePeaks(d.format, tl, 0, end);
        if (!CHECK(mltLoud && ownLoud && mltPeaks && ownPeaks)) continue;
        // Frames that differ by more than 1 dB: clip edges (the mixer's curves reach 0 on the edge, MLT's a frame
        // later), noise reduction before the curves; together a little loudness (fades, keyframes: < 0.15 LU)
        int frames = 0;
        for (size_t i = 0; i < std::min(mltPeaks->size(), ownPeaks->size()); ++i) {
            const double a = (*mltPeaks)[i], b = (*ownPeaks)[i];
            if (std::max(a, b) > 0.01 && std::abs(20 * std::log10(std::max(a, 1e-6) / std::max(b, 1e-6))) > 1) ++frames;
        }
        const double diff = ownLoud->integrated - mltLoud->integrated;
        std::printf("  %-16s %8.3f / %8.3f LUFS (%+.3f), %d von %d Bildern > 1 dB daneben, %lld / %lld ms%s\n",
                    qPrintable(name), mltLoud->integrated, ownLoud->integrated, diff, frames, end, mltMs, ownMs,
                    mltWrong.contains(name) ? "  (MLT bekannt falsch)" : "");
        if (!mltWrong.contains(name)) CHECK(std::abs(diff) < 0.15);
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
    Check::initMlt();

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
