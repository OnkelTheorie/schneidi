// SPDX-License-Identifier: AGPL-3.0-only
// Music beds (core/MusicBed, edit op `music_bed`): per mood a bed of 30 s - the exact sample count, the end at silence,
// true peak at most -1 dBTP (BS.1770 meter), loudness about -16 LUFS, no DC, no clicks (sample jumps), deterministic
// (the same options give the same samples, another seed others), the known grid on the onsets the beat analysis hears
// (core/Beats on the bed), the final hit on a bar one. The op: lengths at 25/30/60 fps exact to a frame (clip and file),
// length:'timeline' over a speech clip with ducking (volume keyframes) on a new track "Musik", the grid in the beats
// cache (no analysis), a second call reuses the file, duck:false, gain, errors, `effects kind:'music'`. Speed: a
// 3-minute bed (printed; checked loosely, the number that counts is measured idle). Distinct moods: per mood a few
// measured features (tempo, spectral centroid, onsets per second, low/high energy, the key heard in the chroma), and
// every two moods differ clearly in at least two of them.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/AppPaths.h"
#include "core/Beats.h"
#include "core/Fft.h"
#include "core/Keyframes.h"
#include "core/Loudness.h"
#include "core/MusicBed.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TruePeak.h"

#include "check.h"
#include "synth_sounds.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw, bool quiet = false)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (!quiet) std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"failed", e.code}, {"message", e.message}};
    }
}

QJsonObject edit(const QString& project, const QString& ops, bool quiet = false)
{
    return run("edit", {{"project", project}, {"ops", ops}}, quiet);
}

QJsonObject bedNote(const QJsonObject& r)
{
    for (const QJsonValue& v : r["notes"].toArray())
        if (v.toObject().contains("music_bed")) return v.toObject()["music_bed"].toObject();
    return {};
}

std::unique_ptr<Project> load(const QString& path)
{
    ProjectData d;
    ProjectFile::load(path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

const Clip* clipById(const Project& p, int id, int* track = nullptr)
{
    for (int t = 0; t < p.timeline().audio.size(); ++t)
        for (const Clip& c : p.timeline().audio[t].clips)
            if (c.id == id) {
                if (track) *track = t;
                return &c;
            }
    return nullptr;
}

double truePeak(const std::vector<float>& s)
{
    TruePeakMeter m;
    m.addInterleaved(s.data(), int(s.size() / 2), 2, MusicBed::kRate);
    return m.maxDb();
}

double lufs(const std::vector<float>& s)
{
    LoudnessMeter m;
    m.addInterleaved(s.data(), int(s.size() / 2), 2, MusicBed::kRate);
    return m.integrated();
}


// ---------- Features of a bed (the distinctness check) ----------

struct Features {
    double bpm = 0;       // the bed's tempo
    double centroid = 0;  // spectral centroid, Hz (magnitude-weighted over the whole bed)
    double onsets = 0;    // onsets per second (peaks of the spectral flux)
    double lowHighDb = 0; // energy below 250 Hz over energy above 2.5 kHz, dB
    int keyRoot = 0;      // the key that fits the chroma best (Krumhansl-Kessler profiles)
    bool keyMinor = false;
};

Features featuresOf(const MusicBed::Result& r)
{
    Features out;
    out.bpm = r.bpm;
    const long long frames = (long long)r.samples.size() / 2;
    std::vector<double> mono(static_cast<size_t>(frames));
    for (long long f = 0; f < frames; ++f) mono[size_t(f)] = 0.5 * (r.samples[size_t(2 * f)] + r.samples[size_t(2 * f + 1)]);
    const double rate = MusicBed::kRate;
    // Spectrum: centroid (magnitude-weighted), bands, chroma from 100 Hz to 2.5 kHz (16384-point frames: bins of 3 Hz,
    // finer than a semitone there; half overlap, Hann)
    {
        const int n = 16384;
        std::vector<double> win(n);
        for (int i = 0; i < n; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n);
        double sumP = 0, sumFP = 0, low = 0, high = 0;
        std::array<double, 12> chroma{};
        std::vector<double> re(n), im(n);
        for (long long at = 0; at + n <= frames; at += n / 2) {
            for (int i = 0; i < n; ++i) re[size_t(i)] = mono[size_t(at + i)] * win[size_t(i)], im[size_t(i)] = 0;
            Fft::transform(re, im, false);
            for (int k = 1; k < n / 2; ++k) {
                const double f = k * rate / n, pw = re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)];
                const double mag = std::sqrt(pw);
                sumP += mag, sumFP += f * mag;
                if (f < 250) low += pw;
                if (f > 2500) high += pw;
                if (f > 100 && f < 2500) {
                    const int pc = ((int(std::lround(12 * std::log2(f / 440.0) + 69)) % 12) + 12) % 12;
                    chroma[size_t(pc)] += mag;
                }
            }
        }
        out.centroid = sumFP / std::max(1e-12, sumP);
        out.lowHighDb = 10 * std::log10((low + 1e-12) / (high + 1e-12));
        static const double major[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
        static const double minor[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
        const auto corr = [&](const double* prof, int root) {
            double mx = 0, my = 0;
            for (int i = 0; i < 12; ++i) mx += chroma[size_t((i + root) % 12)], my += prof[i];
            mx /= 12, my /= 12;
            double sxy = 0, sxx = 0, syy = 0;
            for (int i = 0; i < 12; ++i) {
                const double x = chroma[size_t((i + root) % 12)] - mx, y = prof[i] - my;
                sxy += x * y, sxx += x * x, syy += y * y;
            }
            return sxy / std::sqrt(sxx * syy + 1e-12);
        };
        double best = -2;
        for (int root = 0; root < 12; ++root)
            for (const bool m : {false, true}) {
                const double c = corr(m ? minor : major, root);
                if (c > best) best = c, out.keyRoot = root, out.keyMinor = m;
            }
    }
    // Onsets: rises of the log spectrum (1024-point frames, hop 512), peaks above the local mean
    {
        const int n = 1024, hop = 512;
        std::vector<double> win(n);
        for (int i = 0; i < n; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n);
        std::vector<double> re(n), im(n), prev(n / 2, 0.0), flux;
        for (long long at = 0; at + n <= frames; at += hop) {
            for (int i = 0; i < n; ++i) re[size_t(i)] = mono[size_t(at + i)] * win[size_t(i)], im[size_t(i)] = 0;
            Fft::transform(re, im, false);
            double sum = 0;
            for (int k = 1; k < n / 2; ++k) {
                const double m = std::log1p(1000 * std::sqrt(re[size_t(k)] * re[size_t(k)] + im[size_t(k)] * im[size_t(k)]));
                sum += std::max(0.0, m - prev[size_t(k)]);
                prev[size_t(k)] = m;
            }
            flux.push_back(sum);
        }
        int count = 0;
        const int w = 8; // about 85 ms each side
        for (int i = w; i + w < int(flux.size()); ++i) {
            double mean = 0, peak = 0;
            for (int j = i - w; j <= i + w; ++j) mean += flux[size_t(j)], peak = std::max(peak, flux[size_t(j)]);
            mean /= 2 * w + 1;
            if (flux[size_t(i)] == peak && flux[size_t(i)] > 1.4 * mean + 2) ++count;
        }
        out.onsets = count / (double(frames) / rate);
    }
    return out;
}

// How many features of two beds differ clearly
int differences(const Features& a, const Features& b, QStringList* which = nullptr)
{
    int n = 0;
    const auto ratio = [](double x, double y) { return std::max(x, y) / std::max(1e-9, std::min(x, y)); };
    const auto add = [&](bool differ, const char* name) {
        if (!differ) return;
        ++n;
        if (which) *which << name;
    };
    add(ratio(a.bpm, b.bpm) > 1.05, "tempo");
    add(ratio(a.centroid, b.centroid) > 1.2, "centroid");
    add(ratio(a.onsets, b.onsets) > 1.25, "onsets");
    add(std::abs(a.lowHighDb - b.lowHighDb) > 3, "low/high");
    add(a.keyRoot != b.keyRoot || a.keyMinor != b.keyMinor, "key");
    return n;
}

// The core checks of one bed (its features for the distinctness check into *features)
void checkBed(MusicBed::Mood mood, Features* features)
{
    const QString name = MusicBed::moodId(mood);
    QElapsedTimer timer;
    timer.start();
    const MusicBed::Result r = MusicBed::make({mood, 30});
    const double ms = timer.nsecsElapsed() / 1e6;
    const long long frames = (long long)r.samples.size() / 2;
    CHECK_EQ(frames, 30LL * MusicBed::kRate);
    *features = featuresOf(r);
    std::printf("       %-6s %3.0f BPM %-9s chords %-38s %5.0f ms, %.1f LUFS, %.1f dBTP\n", qPrintable(name), r.bpm,
                qPrintable(MusicBed::keyName(r.key)), qPrintable(r.chords.join(' ')), ms, r.lufs, r.truePeakDb);
    CHECK_EQ(r.samples.size() % 2, size_t(0));
    // Levels
    const double tp = truePeak(r.samples), l = lufs(r.samples);
    if (!CHECK(tp <= MusicBed::kCeilingDbtp + 0.05)) std::printf("       %s: true peak %.2f dBTP\n", qPrintable(name), tp);
    if (!CHECK(l > -17.5 && l < -15.0)) std::printf("       %s: %.2f LUFS\n", qPrintable(name), l);
    CHECK(std::abs(r.lufs - l) < 0.2);
    CHECK(std::abs(r.truePeakDb - tp) < 0.2);
    // No DC, ends at silence, starts at silence
    double sumL = 0, sumR = 0;
    for (long long f = 0; f < frames; ++f) sumL += r.samples[size_t(2 * f)], sumR += r.samples[size_t(2 * f + 1)];
    if (!CHECK(std::abs(sumL / frames) < 1e-3 && std::abs(sumR / frames) < 1e-3))
        std::printf("       %s: DC %.5f / %.5f\n", qPrintable(name), sumL / frames, sumR / frames);
    CHECK_EQ(r.samples[r.samples.size() - 1], 0.f);
    CHECK_EQ(r.samples[r.samples.size() - 2], 0.f);
    CHECK(std::abs(r.samples[0]) < 1e-3 && std::abs(r.samples[1]) < 1e-3);
    // No clicks: no jump from one sample to the next stands out of the sound around it (a step at a note boundary
    // would be far above the 5 ms around it)
    {
        double worst = 0;
        long long at = 0;
        const int w = 240;
        for (int c = 0; c < 2; ++c) {
            std::vector<double> d(size_t(frames), 0.0);
            for (long long f = 2; f < frames; ++f) // second difference: steps and kinks, not smooth waves
                d[size_t(f)] = std::abs(double(r.samples[size_t(2 * f + c)]) - 2 * r.samples[size_t(2 * (f - 1) + c)]
                                        + r.samples[size_t(2 * (f - 2) + c)]);
            double run = 0;
            for (long long f = 0; f < std::min<long long>(frames, 2 * w + 1); ++f) run += d[size_t(f)];
            for (long long f = w; f + w < frames; ++f) {
                const double mean = run / (2 * w + 1);
                const double ratio = d[size_t(f)] / (mean + 1e-4);
                if (ratio > worst) worst = ratio, at = f;
                run += d[size_t(f + w + 1 < frames ? f + w + 1 : f)] - d[size_t(f - w)];
            }
        }
        if (!CHECK(worst < 30)) std::printf("       %s: a click at %.4f s (%.1f x the local level)\n", qPrintable(name), double(at) / MusicBed::kRate, worst);
    }
    // Deterministic, and another seed sounds different
    const MusicBed::Result again = MusicBed::make({mood, 30});
    CHECK(again.samples.size() == r.samples.size()
          && std::memcmp(again.samples.data(), r.samples.data(), r.samples.size() * sizeof(float)) == 0);
    MusicBed::Options other{mood, 30};
    other.seed = 2;
    const MusicBed::Result two = MusicBed::make(other);
    CHECK(two.samples.size() == r.samples.size()
          && std::memcmp(two.samples.data(), r.samples.data(), r.samples.size() * sizeof(float)) != 0);
    // The grid: bar ones at k % 4 == 0 from 0, and the beat analysis hears the same tempo (or an octave of it) and
    // beats on it
    const Beats::Result& g = r.beats;
    CHECK(g.found && g.phase == 0 && g.downbeat == 0);
    CHECK(std::abs(g.period - 60.0 / MusicBed::defaultBpm(mood)) < 1e-9);
    CHECK_EQ(g.beats, int(std::floor((30 - 1e-6) / g.period)));
    CHECK_EQ(int(g.barEnergyDb.size()), int(std::floor(30 / (4 * g.period) + 1e-9)));
    {
        Beats::Features features(MusicBed::kRate);
        std::vector<float> mono(static_cast<size_t>(frames));
        for (long long f = 0; f < frames; ++f) mono[size_t(f)] = 0.5f * (r.samples[size_t(2 * f)] + r.samples[size_t(2 * f + 1)]);
        features.add(mono.data(), int(mono.size()));
        const Beats::Result heard = Beats::analyze(features.take());
        const double ratio = heard.bpm / g.bpm;
        const bool octave = std::abs(ratio - 1) < 0.01 || std::abs(ratio - 2) < 0.02 || std::abs(ratio - 0.5) < 0.005;
        // Where the heard beats lie on our grid: within 20 ms of a grid beat (or of a half beat when it hears double)
        const double unit = std::abs(ratio - 2) < 0.02 ? g.period / 2 : g.period;
        double off = std::fmod(heard.phase, unit);
        off = std::min(off, unit - off);
        if (!CHECK(heard.found && octave && off < 0.02))
            std::printf("       %s: heard %.2f BPM (grid %.2f, confidence %.2f), phase %.3f s off the grid by %.3f s\n",
                        qPrintable(name), heard.bpm, g.bpm, heard.confidence, heard.phase, off);
    }
    // The final hit on a bar one, the ending section up to the end
    CHECK(r.endBar > 0 && !r.sections.empty() && r.sections.back().kind == MusicBed::Section::Ending
          && r.sections.back().fromBar == r.endBar);
    // describe() gives the same plan without the sound
    const MusicBed::Result d = MusicBed::describe({mood, 30});
    CHECK(d.samples.empty() && d.chords == r.chords && d.endBar == r.endBar && d.beats.period == g.period);
}

} // namespace

int main(int argc, char* argv[])
{
    Check::isolateSettings();
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    AppPaths::setProgramNames();

    // ---------- Core ----------
    std::vector<Features> features;
    for (const MusicBed::Mood m : MusicBed::kMoods) {
        features.emplace_back();
        checkBed(m, &features.back());
    }
    // Distinct: every two moods differ clearly in at least two features
    {
        static const char* const keys[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
        std::printf("       mood     tempo  centroid  onsets/s  low/high  key\n");
        const int count = int(features.size());
        for (int i = 0; i < count; ++i) {
            const Features& f = features[size_t(i)];
            std::printf("       %-7s %6.0f %7.0f Hz %8.2f %7.1f dB  %s%s\n", qPrintable(MusicBed::moodId(MusicBed::kMoods[i])), f.bpm,
                        f.centroid, f.onsets, f.lowHighDb, keys[f.keyRoot], f.keyMinor ? "m" : "");
        }
        for (int i = 0; i < count; ++i)
            for (int j = i + 1; j < count; ++j) {
                QStringList which;
                const int d = differences(features[size_t(i)], features[size_t(j)], &which);
                if (!CHECK(d >= 2))
                    std::printf("       %s and %s differ only in %s\n", qPrintable(MusicBed::moodId(MusicBed::kMoods[i])),
                                qPrintable(MusicBed::moodId(MusicBed::kMoods[j])), qPrintable(which.join(", ")));
            }
    }
    // Keys, moods, names
    {
        const auto k = [](const char* s) { return MusicBed::keyFrom(QString::fromUtf8(s)); };
        CHECK(k("A minor") == (MusicBed::Key{9, true}));
        CHECK(k("F#m") == (MusicBed::Key{6, true}));
        CHECK(k("Bb") == (MusicBed::Key{10, false}));
        CHECK(k("Eb major") == (MusicBed::Key{3, false}));
        CHECK(k("c-moll") == (MusicBed::Key{0, true}));
        CHECK(k("D-Dur") == (MusicBed::Key{2, false}));
        CHECK(!k("H minor") && !k("") && !k("A lydian"));
        CHECK_EQ(MusicBed::keyName({10, true}), QString("Bb minor"));
        CHECK(MusicBed::moodFrom("treibend") == MusicBed::Mood::Drive && MusicBed::moodFrom("Ruhig") == MusicBed::Mood::Calm);
        CHECK(MusicBed::moodFrom("Lo-Fi") == MusicBed::Mood::Lofi && MusicBed::moodFrom("synthwave") == MusicBed::Mood::Retro
              && MusicBed::moodFrom("cinematic") == MusicBed::Mood::Epic && MusicBed::moodFrom("dark") == MusicBed::Mood::Tense);
        CHECK(!MusicBed::moodFrom("sad"));
        // Every id reads back; ids are distinct and the labels are there
        CHECK_EQ(MusicBed::moodIds().size(), int(std::size(MusicBed::kMoods)));
        for (const MusicBed::Mood m : MusicBed::kMoods)
            CHECK(MusicBed::moodFrom(MusicBed::moodId(m)) == m && std::strlen(MusicBed::moodLabel(m)) > 0
                  && !MusicBed::description(m).isEmpty());
        // Harmonic rhythm: calm two bars a chord, happy two chords a bar
        CHECK(MusicBed::describe({MusicBed::Mood::Calm, 30}).chordBeats == 8 && MusicBed::describe({MusicBed::Mood::Happy, 30}).chordBeats == 2);
        // Short beds: no final hit, faded; still the exact length and silence at the end (every mood)
        for (const MusicBed::Mood m : MusicBed::kMoods) {
            const MusicBed::Result s = MusicBed::make({m, 3.3});
            CHECK_EQ((long long)s.samples.size(), 2 * MusicBed::frameCount(3.3));
            CHECK(s.endBar < 0 && s.samples.back() == 0.f);
            if (!CHECK(truePeak(s.samples) <= MusicBed::kCeilingDbtp + 0.05)) std::printf("       %s short: true peak\n", qPrintable(MusicBed::moodId(m)));
        }
        // A given tempo and key
        MusicBed::Options o{MusicBed::Mood::Drive, 20};
        o.bpm = 95;
        o.key = MusicBed::Key{2, false};
        const MusicBed::Result g = MusicBed::describe(o);
        CHECK(g.bpm == 95 && g.key == (MusicBed::Key{2, false}) && g.chords.value(0) == "D");
        CHECK(MusicBed::fileName(o).startsWith("music-bed-drive-95bpm-d-major-s1-960000smp-v"));
        // The exact length is in the name: lengths less than a millisecond apart are different files
        CHECK(MusicBed::fileName({MusicBed::Mood::Calm, 10.0}) != MusicBed::fileName({MusicBed::Mood::Calm, 10.0004}));
        // A long bed has breaks (with a drop after each)
        const MusicBed::Result longBed = MusicBed::make({MusicBed::Mood::Hype, 90});
        int breaks = 0;
        for (const MusicBed::Section& sec : longBed.sections) breaks += sec.kind == MusicBed::Section::Break;
        CHECK(breaks >= 1 && !longBed.beats.drops.empty());
    }
    // Threads: 1, 3, 8 threads and every thread start failing (all on this thread) give the same samples
    {
        const MusicBed::Options o{MusicBed::Mood::Hype, 41.3};
        MusicBed::setThreadsForTests(1);
        const MusicBed::Result one = MusicBed::make(o);
        bool same = true;
        for (const auto& [count, failing] : {std::pair{3, false}, std::pair{8, false}, std::pair{8, true}}) {
            MusicBed::setThreadsForTests(count, failing);
            const MusicBed::Result many = MusicBed::make(o);
            const bool equal = many.samples.size() == one.samples.size()
                               && std::memcmp(many.samples.data(), one.samples.data(), one.samples.size() * sizeof(float)) == 0
                               && many.lufs == one.lufs && many.beats.barEnergyDb == one.beats.barEnergyDb;
            if (!CHECK(equal)) std::printf("       %d threads%s differ from one\n", count, failing ? " (starts failing)" : "");
            same = same && equal;
        }
        MusicBed::setThreadsForTests(0);
    }
    // The WAV: written atomically in blocks (more than one block of 65536 frames here), 24-bit stereo, the samples
    // as they are; writing again replaces the file and leaves nothing else in the folder
    {
        const MusicBed::Result r = MusicBed::make({MusicBed::Mood::Calm, 3.0});
        QDir().mkpath(tmp.path() + "/wav");
        const QString path = tmp.path() + "/wav/bed.wav";
        CHECK(MusicBed::writeWav(path, r) && MusicBed::writeWav(path, r));
        CHECK_EQ(QDir(tmp.path() + "/wav").entryList(QDir::Files), QStringList{"bed.wav"});
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray b = f.readAll();
        const long long frames = (long long)r.samples.size() / 2;
        CHECK_EQ((long long)b.size(), 44 + frames * 6);
        CHECK(b.startsWith("RIFF") && b.mid(36, 4) == "data");
        bool same = b.size() == 44 + frames * 6;
        for (long long i = 0; same && i < 2 * frames; i += 997) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(b.constData()) + 44 + 3 * i;
            std::int32_t q = p[0] | (p[1] << 8) | (p[2] << 16);
            if (q & 0x800000) q -= 0x1000000;
            same = std::abs(q / 8388607.0 - r.samples[size_t(i)]) < 2.0 / 8388607;
        }
        CHECK(same);
    }
    // Speed: 3 minutes (well under a second on an idle PC; loose here, tests run in parallel)
    {
        QElapsedTimer t;
        t.start();
        const MusicBed::Result r = MusicBed::make({MusicBed::Mood::Hype, 180});
        const double ms = t.nsecsElapsed() / 1e6;
        std::printf("       3-minute hype bed: %.0f ms\n", ms);
        CHECK(r.samples.size() == size_t(2 * 180 * MusicBed::kRate) && ms < 5000);
    }

    // ---------- The op ----------
    for (const double fps : {25.0, 30.0, 60.0, 24000 / 1001.0, 30000 / 1001.0, 60000 / 1001.0}) {
        const QString project = tmp.path() + QString("/fps%1.schneidi").arg(int(fps * 1000));
        run("new", {{"project", project}, {"fps", std::round(fps * 1000) / 1000}});
        // 17.3 s: not a whole number of frames at every rate before rounding; the clip and the file are that long
        const QJsonObject r = edit(project, R"([{"op":"music_bed","mood":"drive","length":"17.3s"}])");
        const QJsonObject n = bedNote(r);
        const int want = int(std::lround(17.3 * fps));
        const auto p = load(project);
        const Clip* c = clipById(*p, n["clip"].toInt());
        if (!CHECK(c && c->length() == want && c->start == 0))
            std::printf("       %.3f fps: %s\n", fps, QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        // The file as the project reads it (frames at its rate)
        const MediaInfo* m = p->mediaInfo(n["file"].toString());
        if (!CHECK(m && std::abs(m->length - want) <= 1)) std::printf("       %.3f fps: file %d frames\n", fps, m ? m->length : -1);
        CHECK(n["made"].toBool());
        // Nothing else on the timeline: not ducked
        CHECK(c && !Keys::animated(*c, AnimParam::Volume));
    }

    // Every mood through the op: 7.3 s exact to a frame at 29.97 fps
    {
        const QString pj = tmp.path() + "/moods.schneidi";
        run("new", {{"project", pj}, {"fps", 29.97}});
        for (const QString& mood : MusicBed::moodIds()) {
            const QJsonObject n = bedNote(edit(pj, QString(R"([{"op":"music_bed","mood":"%1","length":"7.3s","track":"A1"}])").arg(mood)));
            const auto p = load(pj);
            const Clip* c = clipById(*p, n["clip"].toInt());
            const int want = int(std::lround(7.3 * 30000 / 1001.0));
            if (!CHECK(c && c->length() == want && n["mood"].toString() == mood)) std::printf("       %s via the op\n", qPrintable(mood));
        }
    }
    // The longest bed at 23.976 fps: 600 s are 14386 frames (600.008 s), taken - a frame more is refused
    {
        const QString pj = tmp.path() + "/long.schneidi";
        run("new", {{"project", pj}, {"fps", 23.976}});
        const QJsonObject n = bedNote(edit(pj, R"([{"op":"music_bed","mood":"calm","length":"600s"}])"));
        const auto p = load(pj);
        const Clip* c = clipById(*p, n["clip"].toInt());
        CHECK(c && c->length() == 14386);
        CHECK_EQ(edit(pj, R"([{"op":"music_bed","mood":"calm","length":14387}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    }
    // length:'timeline' over a speech clip: a new track "Musik", ducked under the speech
    const QString speech = tmp.path() + "/speech.wav";
    {
        Synth::Builder b(48000, 20.0, 3);
        Synth::speech(b, 0.5, 6.0);
        Synth::speech(b, 12.0, 6.0);
        CHECK(Synth::writeWav(speech, b.s, 48000));
    }
    const QString project = tmp.path() + "/talk.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    edit(project, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(speech));
    const auto before = load(project);
    const int audioTracks = before->timeline().audio.size();
    QJsonObject r = edit(project, R"([{"op":"music_bed","mood":"calm"}])");
    QJsonObject n = bedNote(r);
    {
        const auto p = load(project);
        int track = -1;
        const Clip* c = clipById(*p, n["clip"].toInt(), &track);
        if (!CHECK(c && c->length() == 20 * 25 && c->start == 0))
            std::printf("       %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        CHECK_EQ(track, audioTracks); // a new track after the others
        CHECK_EQ(p->timeline().audio.size(), audioTracks + 1);
        CHECK_EQ(p->timeline().audio.value(track).name, T("Musik"));
        if (!CHECK(c && Keys::animated(*c, AnimParam::Volume))) std::printf("       ducking: %s\n", qPrintable(n["ducking"].toString()));
        CHECK(n["ducking"].toString().startsWith("ducked"));
        // The file next to the project, its grid in the cache: snap_to_beat and fit_music do not analyse
        const QString file = n["file"].toString();
        CHECK_EQ(QFileInfo(file).absolutePath(), QFileInfo(tmp.path() + "/talk-music").absoluteFilePath());
        const std::optional<Beats::Result> cached = Cli::detail::cachedSongBeats(file, 0);
        CHECK(cached && std::abs(cached->bpm - MusicBed::defaultBpm(MusicBed::Mood::Calm)) < 1e-9 && cached->phase == 0 && !cached->barEnergyDb.empty());
        const QJsonObject beats = run("beats", {{"file", file}});
        CHECK(beats["bpm"].toDouble() == MusicBed::defaultBpm(MusicBed::Mood::Calm) && beats["confidence"].toDouble() == 1); // the stored grid, not analysed
        CHECK(n["sections"].toArray().size() >= 2 && n["final_hit_s"].toDouble() > 0);
        CHECK(n["lufs"].toDouble() < -15 && n["lufs"].toDouble() > -17.5);
    }
    // The same bed again (onto another track, not ducked): the file is reused, a volume given
    r = edit(project, R"([{"op":"music_bed","mood":"calm","length":"timeline","duck":false,"gain":-6,"track":"A9"}])");
    n = bedNote(r);
    {
        CHECK(!n["made"].toBool(true)); // reused
        const auto p = load(project);
        const Clip* c = clipById(*p, n["clip"].toInt());
        CHECK(c && c->volumeDb == -6 && !Keys::animated(*c, AnimParam::Volume));
    }
    // Ducking only under voice and recorded sound: not under the effect sounds (track "SFX"), a track named "Music",
    // another bed or an audio-only song with analysed beats
    {
        const QString pj = tmp.path() + "/duck.schneidi";
        run("new", {{"project", pj}, {"fps", 25}});
        const QString song = tmp.path() + "/song.wav";
        Synth::Builder b(48000, 12.0, 7);
        Synth::music(b, 0, 12);
        CHECK(Synth::writeWav(song, b.s, 48000));
        Cli::detail::storeSongBeats(QFileInfo(song).absoluteFilePath(), 0, MusicBed::describe({MusicBed::Mood::Drive, 12}).beats);
        edit(pj, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"},{"op":"place","media":"%1","at":0,"track":"A2"},)"
                         R"({"op":"track","track":"A2","name":"SFX"},{"op":"place","media":"%1","at":0,"track":"A3"},)"
                         R"({"op":"track","track":"A3","name":"Music"},{"op":"place","media":"%2","at":0,"track":"A4"}])")
                     .arg(speech, song));
        edit(pj, R"([{"op":"music_bed","mood":"hype","length":"8s","track":"A5","duck":false}])");
        const QJsonObject d = bedNote(edit(pj, R"([{"op":"music_bed","mood":"drive","length":"8s","track":"A6"}])"));
        if (!CHECK(d["ducking"].toString().contains(" of A1:"))) std::printf("       %s\n", qPrintable(d["ducking"].toString()));
        // Only effect sounds besides it: not ducked
        const QString only = tmp.path() + "/sfxonly.schneidi";
        run("new", {{"project", only}, {"fps", 25}});
        edit(only, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"},{"op":"track","track":"A1","name":"SFX"}])").arg(speech));
        const QJsonObject o = bedNote(edit(only, R"([{"op":"music_bed","mood":"calm","length":"8s"}])"));
        CHECK(o["ducking"].toString().startsWith("not ducked"));
        const auto p = load(only);
        const Clip* c = clipById(*p, o["clip"].toInt());
        CHECK(c && !Keys::animated(*c, AnimParam::Volume));
    }
    // Without a track: a second bed replaces the first on its track; a start template's empty "Musik" track is taken
    {
        const QString pj = tmp.path() + "/again.schneidi";
        run("new", {{"project", pj}, {"fps", 25}});
        edit(pj, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(speech));
        const QJsonObject a = bedNote(edit(pj, R"([{"op":"music_bed","mood":"calm"}])"));
        const int tracks = load(pj)->timeline().audio.size();
        const QJsonObject b = bedNote(edit(pj, R"([{"op":"music_bed","mood":"hype"}])"));
        const auto p = load(pj);
        CHECK_EQ(p->timeline().audio.size(), tracks);
        CHECK_EQ(a["track"].toString(), b["track"].toString());
        int track = -1;
        CHECK(clipById(*p, b["clip"].toInt(), &track) && !clipById(*p, a["clip"].toInt()));
        CHECK(track >= 0 && p->timeline().audio[track].clips.size() == 1);

        const QString pod = tmp.path() + "/pod.schneidi";
        run("new", {{"project", pod}, {"fps", 25}, {"template", "podcast"}});
        edit(pod, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(speech));
        const int podTracks = load(pod)->timeline().audio.size();
        const QJsonObject c = bedNote(edit(pod, R"([{"op":"music_bed","mood":"calm"}])"));
        const auto q = load(pod);
        CHECK_EQ(q->timeline().audio.size(), podTracks);
        int musik = -1;
        for (int i = 0; i < q->timeline().audio.size(); ++i)
            if (q->timeline().audio[i].name == T("Musik")) musik = i;
        CHECK(musik >= 0 && c["track"].toString() == QString("A%1").arg(musik + 1));
    }
    // Live: schneidi-cli makes the bed before the edit goes to the app - after the ops before it (here a clip that
    // makes the timeline 40 s long), and not at all when one of those fails (the length would be unknown)
    {
        const QString pj = tmp.path() + "/live.schneidi";
        run("new", {{"project", pj}, {"fps", 25}});
        edit(pj, QString(R"([{"op":"place","media":"%1","at":0,"track":"A1"}])").arg(speech));
        const QString dir = tmp.path() + "/live-music";
        const auto files = [&] { return QDir(dir).entryList(QDir::Files); };
        Cli::Context ctx;
        const QJsonDocument ops = QJsonDocument::fromJson(
            QString(R"([{"op":"place","media":"%1","at":"20s","track":"A1"},{"op":"music_bed","mood":"calm"}])").arg(speech).toUtf8());
        Cli::detail::prepareMusicBed(QJsonObject{{"project", pj}, {"ops", ops.array()}}, ctx);
        const QStringList made = files();
        if (!CHECK(made == QStringList{MusicBed::fileName({MusicBed::Mood::Calm, 40})})) std::printf("       %s\n", qPrintable(made.join(", ")));
        CHECK_EQ(load(pj)->timeline().audio.value(0).clips.size(), 1); // the project itself is left alone
        const QJsonDocument bad = QJsonDocument::fromJson(R"([{"op":"no_such_op"},{"op":"music_bed","mood":"hype"}])");
        Cli::detail::prepareMusicBed(QJsonObject{{"project", pj}, {"ops", bad.array()}}, ctx);
        CHECK_EQ(files(), made);
        // First op: made right away
        const QJsonDocument first = QJsonDocument::fromJson(R"([{"op":"music_bed","mood":"drive","length":"6s"}])");
        Cli::detail::prepareMusicBed(QJsonObject{{"project", pj}, {"ops", first.array()}}, ctx);
        CHECK(files().contains(MusicBed::fileName({MusicBed::Mood::Drive, 6})));
    }
    // snap_to_beat takes the bed's grid: a cut 0.2 s off a beat moves onto it
    {
        const QString pj = tmp.path() + "/snap.schneidi";
        run("new", {{"project", pj}, {"fps", 25}});
        const QString clip = tmp.path() + "/clip.wav";
        Synth::Builder b(48000, 10.0, 5);
        Synth::speech(b, 0.2, 9.5);
        CHECK(Synth::writeWav(clip, b.s, 48000));
        // A titles as video clips: two solid titles, the cut at 2.3 s (120 BPM: beats every 0.5 s)
        edit(pj, R"([{"op":"title","text":"A","at":0,"duration":"2.3s","track":"V1"},{"op":"title","text":"B","at":"2.3s","duration":"3s","track":"V1"}])");
        const QJsonObject bed = edit(pj, R"([{"op":"music_bed","mood":"hype","bpm":120,"length":"8s"}])");
        const QJsonObject s = edit(pj, R"([{"op":"snap_to_beat","grid":"beat"}])");
        const auto p = load(pj);
        bool onBeat = false;
        for (const Clip& c : p->timeline().video.value(0).clips)
            if (c.start > 0) onBeat = c.start == 50 || c.start == 62 || c.start == 63; // 2.0 s or 2.5 s
        if (!CHECK(onBeat && bedNote(bed)["bpm"].toDouble() == 120))
            std::printf("       %s\n", QJsonDocument(s).toJson(QJsonDocument::Compact).constData());
    }
    // Errors
    CHECK_EQ(edit(project, R"([{"op":"music_bed"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"sad","length":"5s"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"calm","length":"5s","key":"X"}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(edit(project, R"([{"op":"music_bed","mood":"calm","length":"5s","bpm":300}])", true)["failed"].toString(), QString("BAD_ARGUMENT"));
    {
        const QString empty = tmp.path() + "/empty.schneidi";
        run("new", {{"project", empty}});
        CHECK_EQ(edit(empty, R"([{"op":"music_bed","mood":"calm"}])", true)["failed"].toString(), QString("EMPTY"));
    }
    // effects kind:'music'
    {
        const QJsonObject e = run("effects", {{"kind", "music"}});
        const QJsonArray moods = e["moods"].toArray();
        CHECK_EQ(moods.size(), int(std::size(MusicBed::kMoods)));
        CHECK_EQ(moods.at(0).toObject()["mood"].toString(), QString("calm"));
        CHECK_EQ(moods.at(2).toObject()["chords"].toArray().size(), 4);
    }
    return Check::result();
}
