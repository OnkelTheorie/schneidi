// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/AudioEvents.h"

#include "core/Fft.h"
#include "core/PathKey.h"
#include "engine/Extensions.h"
#include "engine/Inference.h"
#include "engine/audio/Reader.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <memory>
#include <optional>

namespace AudioEvents {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kVersion = 1; // bump when a rule changes (old cache entries are ignored)

// ---------- Frame features (10 ms hop at 16 kHz) ----------

constexpr int kHop = 160;      // 10 ms
constexpr int kWindow = 512;   // 32 ms analysis window, centred on the 10 ms block
constexpr int kFft = 1024;     // zero-padded: the autocorrelation for the pitch must not wrap around
constexpr int kBins = kFft / 2 + 1;
constexpr double kFrameS = double(kHop) / kRate;
constexpr int kLoBin = 6;      // ~94 Hz
constexpr int kHiBin = 448;    // 7 kHz
constexpr int kMinLag = 16;    // 1000 Hz
constexpr int kMaxLag = 200;   // 80 Hz
constexpr int kLongTrack = 25; // a spectral peak held this many frames (250 ms) counts as a held note
constexpr float kSilentDb = -45.f; // 10 ms RMS below this = silent (dBFS)

struct Frame {
    float db = -120;     // RMS of the 10 ms block, dBFS
    float flux = 0;      // mean rise of the spectrum (dB per bin) against the frame before
    float flat = 0;      // spectral flatness 94 Hz-7 kHz (1 = white noise, 0 = one tone)
    float centroid = 0;  // Hz
    float voicing = 0;   // normalized autocorrelation at the pitch (0..1)
    float pitch = 0;     // Hz (0 = none)
    float held = 0;      // share of the frame's power in spectral peaks held >= 250 ms (held notes)
};

class Extractor {
public:
    Extractor()
    {
        m_window.resize(kWindow);
        for (int i = 0; i < kWindow; ++i) m_window[size_t(i)] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kWindow);
        // Autocorrelation of the window itself: divides the bias out of the signal's autocorrelation
        m_windowAc.assign(kMaxLag + 2, 0.0);
        for (int l = 0; l <= kMaxLag + 1; ++l) {
            double s = 0;
            for (int i = 0; i + l < kWindow; ++i) s += m_window[size_t(i)] * m_window[size_t(i + l)];
            m_windowAc[size_t(l)] = s;
        }
        m_re.resize(kFft);
        m_im.resize(kFft);
        m_power.assign(kBins, 0.0);
        m_prevDb.assign(kBins, -100.0);
        m_run.assign(kBins, 0);
        m_newRun.assign(kBins, 0);
        // The first window reaches 176 samples before the first block: silence there
        m_buf.assign(size_t(kWindow / 2 - kHop / 2), 0.f);
    }

    void add(const float* x, int n)
    {
        m_buf.insert(m_buf.end(), x, x + n);
        m_total += n;
        process(false);
    }
    void finish()
    {
        // Silence after the end so the last blocks get their whole window
        m_buf.insert(m_buf.end(), size_t(kWindow), 0.f);
        process(true);
    }
    std::vector<Frame> frames;

private:
    void process(bool final)
    {
        // Block t covers samples [t*kHop, t*kHop + kHop) of the input; its window starts at m_buf index
        // (t - m_first) * kHop (m_buf has the lead-in in front)
        size_t at = 0;
        while (at + kWindow <= m_buf.size()) {
            if (final && int64_t(frames.size()) * kHop >= m_total) break;
            frame(m_buf.data() + at);
            at += kHop;
        }
        m_buf.erase(m_buf.begin(), m_buf.begin() + ptrdiff_t(at));
    }

    void frame(const float* w)
    {
        Frame f;
        // Loudness of the 10 ms block in the middle of the window
        const float* block = w + (kWindow / 2 - kHop / 2);
        double sum = 0;
        for (int i = 0; i < kHop; ++i) sum += double(block[i]) * block[i];
        const double rms = std::sqrt(sum / kHop);
        f.db = float(rms > 1e-6 ? 20 * std::log10(rms) : -120.0);
        // Spectrum
        double energy = 0;
        for (int i = 0; i < kWindow; ++i) {
            m_re[size_t(i)] = w[i] * m_window[size_t(i)];
            energy += m_re[size_t(i)] * m_re[size_t(i)];
        }
        std::fill(m_re.begin() + kWindow, m_re.end(), 0.0);
        std::fill(m_im.begin(), m_im.end(), 0.0);
        if (energy < 1e-12) { // digital silence: nothing to measure
            std::fill(m_prevDb.begin(), m_prevDb.end(), -100.0);
            std::fill(m_run.begin(), m_run.end(), 0);
            frames.push_back(f);
            return;
        }
        Fft::transform(m_re, m_im, false);
        for (int k = 0; k < kBins; ++k)
            m_power[size_t(k)] = m_re[size_t(k)] * m_re[size_t(k)] + m_im[size_t(k)] * m_im[size_t(k)];
        double total = 0, logSum = 0, weighted = 0, flux = 0, frameMax = 1e-20;
        std::vector<double>& db = m_db;
        db.resize(kBins);
        for (int k = 0; k < kBins; ++k) db[size_t(k)] = 10 * std::log10(m_power[size_t(k)] + 1e-12);
        for (int k = kLoBin; k <= kHiBin; ++k) {
            const double p = m_power[size_t(k)];
            total += p;
            logSum += std::log(p + 1e-12);
            weighted += p * k * double(kRate) / kFft;
            flux += std::max(0.0, std::max(db[size_t(k)], -100.0) - m_prevDb[size_t(k)]);
            frameMax = std::max(frameMax, p);
        }
        const int n = kHiBin - kLoBin + 1;
        f.flat = float(total > 0 ? std::exp(logSum / n) / (total / n) : 0);
        f.centroid = float(total > 0 ? weighted / total : 0);
        f.flux = float(flux / n);
        for (int k = 0; k < kBins; ++k) m_prevDb[size_t(k)] = std::max(db[size_t(k)], -100.0);

        // Held spectral peaks: a peak (above its neighbours and 6 dB above the local mean, at most 40 dB under the
        // frame's strongest) continues a track when the frame before had one within one bin
        const double maxDb = 10 * std::log10(frameMax);
        double heldPower = 0;
        std::fill(m_newRun.begin(), m_newRun.end(), 0);
        for (int k = kLoBin; k <= kHiBin; ++k) {
            const double p = m_power[size_t(k)];
            if (!(p >= m_power[size_t(k - 1)] && p > m_power[size_t(k + 1)] && p >= m_power[size_t(k - 2)]
                  && p >= m_power[size_t(k + 2)]))
                continue;
            if (db[size_t(k)] < maxDb - 40) continue;
            double local = 0;
            for (int j = k - 12; j <= k + 12; ++j) local += db[size_t(std::clamp(j, 0, kBins - 1))];
            if (db[size_t(k)] < local / 25 + 6) continue;
            const int run = std::max({m_run[size_t(k - 1)], m_run[size_t(k)], m_run[size_t(k + 1)]}) + 1;
            m_newRun[size_t(k)] = run;
            if (run >= kLongTrack)
                for (int j = k - 2; j <= k + 2; ++j) heldPower += m_power[size_t(j)];
        }
        m_run.swap(m_newRun);
        f.held = float(total > 0 ? std::min(1.0, heldPower / total) : 0);

        // Pitch: the autocorrelation is the inverse transform of the power spectrum
        for (int k = 0; k < kFft; ++k) {
            const int b = k < kBins ? k : kFft - k;
            m_re[size_t(k)] = m_power[size_t(b)];
            m_im[size_t(k)] = 0;
        }
        Fft::transform(m_re, m_im, true);
        const double r0 = m_re[0];
        if (r0 > 0) {
            std::vector<double>& r = m_ac;
            r.assign(kMaxLag + 2, 0.0);
            for (int l = kMinLag - 1; l <= kMaxLag + 1; ++l)
                r[size_t(l)] = (m_re[size_t(l)] / r0) / (m_windowAc[size_t(l)] / m_windowAc[0]);
            double best = 0;
            for (int l = kMinLag; l <= kMaxLag; ++l) best = std::max(best, r[size_t(l)]);
            // The shortest period close to the best (a sub-octave correlates as well)
            for (int l = kMinLag; l <= kMaxLag; ++l) {
                if (r[size_t(l)] >= 0.85 * best && r[size_t(l)] >= r[size_t(l - 1)] && r[size_t(l)] >= r[size_t(l + 1)]) {
                    const double a = r[size_t(l - 1)], b = r[size_t(l)], c = r[size_t(l + 1)];
                    const double den = a - 2 * b + c;
                    const double shift = std::abs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
                    f.voicing = float(std::clamp(b, 0.0, 1.0));
                    f.pitch = float(kRate / (l + shift));
                    break;
                }
            }
        }
        frames.push_back(f);
    }

    std::vector<double> m_window, m_windowAc, m_re, m_im, m_power, m_prevDb, m_db, m_ac;
    std::vector<int> m_run, m_newRun;
    std::vector<float> m_buf;
    int64_t m_total = 0;
};

// ---------- Rules ----------

double ramp(double x, double a, double b) { return std::clamp((x - a) / (b - a), 0.0, 1.0); }

double percentile(std::vector<float> v, double q)
{
    if (v.empty()) return -120;
    const size_t i = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(i), v.end());
    return v[i];
}

double timeOf(int frame) { return frame * kFrameS; }

// Features of a one-second block around a tick
struct Block {
    double active = 0;   // share of frames above the silence level
    double lef = 0;      // "low energy frames": share with less than half the block's mean power (speech: pauses)
    double voiced = 0;   // share of active frames with a clear pitch
    double voiceMean = 0; // mean autocorrelation of the active frames
    double pitch = 0;    // median pitch of the voiced frames (Hz)
    double flat = 0, centroid = 0, held = 0, roughness = 0;
    double onsets = 0;   // spectral flux peaks per second
    double meanDb = -120;
};

Block blockAt(const std::vector<Frame>& fr, int from, int to)
{
    Block b;
    from = std::max(0, from);
    to = std::min(int(fr.size()), to);
    const int n = to - from;
    if (n <= 0) return b;
    double meanPower = 0;
    for (int i = from; i < to; ++i) meanPower += std::pow(10.0, fr[size_t(i)].db / 10);
    meanPower /= n;
    b.meanDb = 10 * std::log10(meanPower + 1e-12);
    int active = 0, low = 0, voiced = 0, onsets = 0;
    std::vector<float> pitches;
    double rough = 0;
    for (int i = from; i < to; ++i) {
        const Frame& f = fr[size_t(i)];
        if (std::pow(10.0, f.db / 10) < 0.5 * meanPower) ++low;
        if (f.db < kSilentDb) continue;
        ++active;
        b.voiceMean += f.voicing;
        b.flat += f.flat;
        b.centroid += f.centroid;
        b.held += f.held;
        if (f.voicing > 0.6f && f.pitch >= 70) {
            ++voiced;
            pitches.push_back(f.pitch);
        }
        if (i > from) rough += std::abs(f.db - fr[size_t(i - 1)].db);
        if (i > 0 && i + 1 < int(fr.size()) && f.flux > 3.0f && f.flux >= fr[size_t(i - 1)].flux && f.flux > fr[size_t(i + 1)].flux)
            ++onsets;
    }
    b.active = double(active) / n;
    b.lef = double(low) / n;
    if (active > 0) {
        b.voiced = double(voiced) / active;
        b.voiceMean /= active;
        b.flat /= active;
        b.centroid /= active;
        b.held /= active;
        b.roughness = rough / active;
    }
    b.onsets = onsets / (n * kFrameS);
    b.pitch = pitches.empty() ? 0 : percentile(pitches, 0.5);
    return b;
}

struct Scores {
    double speech = 0, music = 0, applause = 0, cheering = 0;
};

// Each kind as a fuzzy AND (the smallest of its conditions), 0..1
Scores score(const Block& b)
{
    Scores s;
    // Speech: a pitch in the voice range part of the time, pauses between syllables and words (many low-energy
    // frames), no held notes, a smooth loudness. Speech-like noise without a pitch (whispering) is missed.
    s.speech = std::min({ramp(b.active, 0.15, 0.35), ramp(b.voiced, 0.15, 0.3), ramp(b.lef, 0.25, 0.4),
                         1 - ramp(b.held, 0.1, 0.2), ramp(b.pitch, 60, 80), 1 - ramp(b.pitch, 380, 480),
                         1 - ramp(b.flat, 0.3, 0.45), 1 - ramp(b.roughness, 5, 7)});
    // Music: held notes (spectral peaks that stay >= 250 ms) and few pauses. Drums alone, without notes, are missed.
    s.music = std::min({ramp(b.active, 0.6, 0.85), ramp(b.held, 0.1, 0.2), 1 - ramp(b.lef, 0.4, 0.55)});
    // Applause: dense noise bursts - flat, bright spectrum, rough loudness (claps), no pitch, no held notes
    s.applause = std::min({ramp(b.active, 0.5, 0.75), ramp(b.flat, 0.1, 0.18), ramp(b.centroid, 1000, 1600),
                           1 - ramp(b.voiceMean, 0.3, 0.4), ramp(b.roughness, 3.5, 5.5), 1 - ramp(b.lef, 0.55, 0.7),
                           1 - ramp(b.held, 0.05, 0.1)});
    // Cheering: many voices at once - continuous sound with a weak, unclear pitch (voices overlap), some noise,
    // a smooth loudness (no claps), no held notes, no pauses
    s.cheering = std::min({ramp(b.active, 0.6, 0.85), ramp(b.voiceMean, 0.3, 0.38), 1 - ramp(b.voiceMean, 0.65, 0.75),
                           1 - ramp(b.lef, 0.3, 0.45), 1 - ramp(b.held, 0.08, 0.15), ramp(b.flat, 0.05, 0.1),
                           1 - ramp(b.roughness, 3.5, 5.5)});
    return s;
}

struct Range {
    int from, to; // frames
    double conf;
};

// Ticks every 10 frames scored by `get`; on above 0.5 (median of 5 smoothed), gaps up to maxGap frames closed,
// shorter than minLen dropped; edges moved to where the sound starts/ends
QVector<Range> segments(const std::vector<double>& ticks, int tickFrames, int nFrames, int maxGap, int minLen,
                        const std::vector<Frame>& fr)
{
    const int n = int(ticks.size());
    std::vector<double> sm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        std::vector<double> w;
        for (int j = std::max(0, i - 2); j <= std::min(n - 1, i + 2); ++j) w.push_back(ticks[size_t(j)]);
        std::nth_element(w.begin(), w.begin() + ptrdiff_t(w.size() / 2), w.end());
        sm[size_t(i)] = w[w.size() / 2];
    }
    QVector<Range> out;
    for (int i = 0; i < n;) {
        if (sm[size_t(i)] <= 0.5) {
            ++i;
            continue;
        }
        int j = i;
        double sum = 0;
        while (j < n && sm[size_t(j)] > 0.5) sum += sm[size_t(j++)];
        const Range r{i * tickFrames, std::min(nFrames, j * tickFrames), sum / (j - i)};
        if (!out.isEmpty() && r.from - out.last().to <= maxGap) {
            const double a = out.last().to - out.last().from, b = r.to - r.from;
            out.last().conf = (out.last().conf * a + r.conf * b) / std::max(1.0, a + b);
            out.last().to = r.to;
        } else {
            out << r;
        }
        i = j;
    }
    // Edges: a tick's block reaches half a second to each side; move the edge to the first/last sound within reach
    // (gaps up to 100 ms between claps or syllables do not stop it)
    const int reach = 80, gap = 10;
    const auto soundNear = [&](int k, int dir) {
        for (int j = 0; j < gap; ++j) {
            const int i = k + dir * j;
            if (i >= 0 && i < nFrames && fr[size_t(i)].db >= kSilentDb) return true;
        }
        return false;
    };
    for (Range& r : out) {
        int a = r.from;
        while (a > std::max(0, r.from - reach) && soundNear(a - 1, -1)) --a;
        while (a < r.to - 1 && fr[size_t(a)].db < kSilentDb && a < r.from + reach) ++a;
        int b = r.to;
        while (b < std::min(nFrames, r.to + reach) && soundNear(b, 1)) ++b;
        while (b > a + 1 && fr[size_t(b - 1)].db < kSilentDb && b > r.to - reach) --b;
        r.from = a;
        r.to = b;
    }
    // Moved edges can meet: join again
    QVector<Range> joined;
    for (const Range& r : out) {
        if (!joined.isEmpty() && r.from - joined.last().to <= maxGap) {
            const double a = joined.last().to - joined.last().from, b = r.to - r.from;
            joined.last().conf = (joined.last().conf * a + r.conf * b) / std::max(1.0, a + b);
            joined.last().to = std::max(joined.last().to, r.to);
        } else {
            joined << r;
        }
    }
    QVector<Range> kept;
    for (const Range& r : joined)
        if (r.to - r.from >= minLen) kept << r;
    return kept;
}

// Removes `cut` ranges from `from` (speech under laughter: the laughter wins); pieces shorter than minLen go
QVector<Range> subtract(const QVector<Range>& from, const QVector<Range>& cut, int minLen)
{
    QVector<Range> out = from;
    for (const Range& c : cut) {
        QVector<Range> next;
        for (const Range& r : out) {
            if (c.to <= r.from || c.from >= r.to) {
                next << r;
                continue;
            }
            if (c.from - r.from >= minLen) next << Range{r.from, c.from, r.conf};
            if (r.to - c.to >= minLen) next << Range{c.to, r.to, r.conf};
        }
        out = next;
    }
    return out;
}

bool overlaps(const QVector<Range>& rs, int from, int to)
{
    for (const Range& r : rs)
        if (r.from < to && from < r.to) return true;
    return false;
}

// Silence: runs of 10 ms blocks under -45 dBFS for at least half a second
QVector<Range> silences(const std::vector<Frame>& fr)
{
    QVector<Range> out;
    const int n = int(fr.size()), minLen = 50;
    for (int i = 0; i < n;) {
        if (fr[size_t(i)].db >= kSilentDb) {
            ++i;
            continue;
        }
        int j = i;
        double sum = 0;
        while (j < n && fr[size_t(j)].db < kSilentDb) sum += fr[size_t(j++)].db;
        if (j - i >= minLen) {
            // Digital silence = 1, just under the level = 0.6
            const double mean = sum / (j - i);
            out << Range{i, j, 0.6 + 0.4 * ramp(-mean, -kSilentDb, 80)};
        }
        i = j;
    }
    return out;
}

// Shots, bangs, explosions, hits: the sound jumps >= 12 dB within 50 ms, is loudest within 80 ms of that (a real shot
// builds up over a few blocks: transients, the room) and does not get louder in the 170 ms after (a consonant or a
// breath is followed by a louder vowel), stands 10 dB above the median of the second before, has no clear pitch
// while it is loud, is not all bass (a kick drum) and dies away (10 dB down within 0.6 s) (a kick drum, a plucked note or a
// syllable has one). Dense rises (claps, rain, a rattle) are no shots. Ends where it fell 20 dB (at most 1.5 s).
QVector<Range> shots(const std::vector<Frame>& fr)
{
    QVector<Range> out;
    const int n = int(fr.size());
    const bool dump = qEnvironmentVariableIsSet("AUDIO_EVENTS_DUMP");
    const auto riseAt = [&](int j) {
        return fr[size_t(j)].db - std::min({fr[size_t(j - 1)].db, fr[size_t(j - 2)].db, fr[size_t(j - 3)].db});
    };
    int last = -1000;
    for (int t = 3; t < n; ++t) {
        // A rise starts here (>= 4 dB in a block) and reaches >= 12 dB over the level before within 50 ms
        if (riseAt(t) < 4 || t - last < 10) continue;
        const float base = std::min({fr[size_t(t - 1)].db, fr[size_t(t - 2)].db, fr[size_t(t - 3)].db});
        float quick = -120;
        for (int j = t; j < std::min(n, t + 5); ++j) quick = std::max(quick, fr[size_t(j)].db);
        if (quick - base < 12) continue;
        int p = t;
        for (int j = t; j < std::min(n, t + 8); ++j)
            if (fr[size_t(j)].db > fr[size_t(p)].db) p = j;
        const float peak = fr[size_t(p)].db;
        float after = -120;
        for (int j = p + 1; j < std::min(n, t + 25); ++j) after = std::max(after, fr[size_t(j)].db);
        // Pitch while loud: the mean autocorrelation of the 50 ms from the peak
        float voicing = 0;
        for (int j = p; j < p + 5; ++j) voicing += fr[size_t(std::min(n - 1, j))].voicing / 5;
        int rises = 0;
        for (int j = std::max(3, t - 50); j < std::min(n, t + 50); ++j) rises += riseAt(j) >= 12 && fr[size_t(j)].db >= -40;
        // Against the usual level of the second before (its median: in a firefight the shots before are louder
        // than that, the gaps between them are not)
        std::vector<float> before;
        for (int j = std::max(0, t - 100); j < t - 3; ++j) before.push_back(fr[size_t(j)].db);
        const double ref = before.empty() ? -120 : percentile(before, 0.5);
        const double over = peak - std::max(ref, -90.0);
        if (dump)
            std::fprintf(stderr, "shot? %.2f peak %.1f after %+.1f over %.1f voicing %.2f rises %d centroid %.0f\n",
                         timeOf(t), peak, after - peak, over, voicing, rises, fr[size_t(p)].centroid);
        // It dies away: 10 dB down within 0.6 s (a note or music going on does not)
        float low = peak;
        for (int j = p; j < std::min(n, p + 60); ++j) low = std::min(low, fr[size_t(j)].db);
        // A kick drum is all bass (centroid well under 200 Hz); a shot, a hit or a bang has more in it
        if (peak < -40 || after > peak + 3 || voicing > 0.6f || rises > 12 || over < 10 || low > peak - 10
            || fr[size_t(p)].centroid < 200)
            continue;
        int end = p;
        while (end < std::min(n, p + 150) && fr[size_t(end)].db > peak - 20) ++end;
        out << Range{t, std::max(end, t + 1), 0.5 + 0.5 * ramp(over, 10, 30) * (1 - ramp(voicing, 0.3, 0.6))};
        last = t;
    }
    // A steady row of them (4 or more, 0.2-0.8 s apart, evenly) is a rhythm: footsteps, a drum, a hammer - not shots
    // (automatic fire is faster than that)
    QVector<Range> kept;
    for (int i = 0; i < out.size();) {
        int j = i;
        while (j + 1 < out.size()) {
            const int gap = out[j + 1].from - out[j].from;
            const int first = out[i + 1 < out.size() ? i + 1 : i].from - out[i].from;
            if (gap < 20 || gap > 80 || std::abs(gap - first) > 0.15 * first) break;
            ++j;
        }
        if (j - i + 1 < 4)
            for (int k = i; k <= j; ++k) kept << out[k];
        i = j + 1;
    }
    return kept;
}

// Laughter: a row of at least 5 short bursts ("ha-ha-ha") at a steady pace of 3-7.5 per second (much steadier than
// syllables), each a sharp rise and fall (>= 8 dB down between them), voiced or breathy. Real laughter that is
// quiet, irregular or mixed with talk is often missed; a steady "ha" sequence in speech or a fast rhythm in a
// noisy recording can be taken for it.
QVector<Range> laughter(const std::vector<Frame>& fr, const QVector<Range>& music)
{
    const int n = int(fr.size());
    // Smoothed loudness (30 ms)
    std::vector<float> e(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double s = 0;
        int c = 0;
        for (int j = std::max(0, i - 1); j <= std::min(n - 1, i + 1); ++j, ++c) s += fr[size_t(j)].db;
        e[size_t(i)] = float(s / c);
    }
    struct Burst {
        int peak, from, to;
        float db;
        bool voiced;
    };
    std::vector<Burst> bursts;
    for (int i = 6; i + 6 < n; ++i) {
        bool isMax = e[size_t(i)] > kSilentDb + 10;
        for (int j = i - 6; j <= i + 6 && isMax; ++j)
            if (j != i && (e[size_t(j)] > e[size_t(i)] || (j < i && e[size_t(j)] == e[size_t(i)]))) isMax = false;
        if (!isMax) continue;
        int a = i, b = i;
        while (a > 0 && e[size_t(a - 1)] > e[size_t(i)] - 6) --a;
        while (b + 1 < n && e[size_t(b + 1)] > e[size_t(i)] - 6) ++b;
        // Voiced (a pitch) or breathy (noisy) around the peak
        int sounding = 0;
        for (int j = std::max(0, i - 5); j <= std::min(n - 1, i + 5); ++j)
            if (fr[size_t(j)].voicing > 0.45f || fr[size_t(j)].flat > 0.2f) ++sounding;
        if (sounding < 4) continue;
        bursts.push_back({i, a, b + 1, e[size_t(i)], fr[size_t(i)].voicing > 0.5f || fr[size_t(i + 1)].voicing > 0.5f});
    }
    QVector<Range> out;
    const auto dipBetween = [&](const Burst& x, const Burst& y) {
        float low = 0;
        for (int j = x.peak; j <= y.peak; ++j) low = std::min(low, e[size_t(j)]);
        return std::min(x.db, y.db) - low;
    };
    for (size_t i = 0; i < bursts.size();) {
        size_t j = i;
        while (j + 1 < bursts.size()) {
            const int gap = bursts[j + 1].peak - bursts[j].peak;
            if (gap < 13 || gap > 33 || dipBetween(bursts[j], bursts[j + 1]) < 8
                || std::abs(bursts[j + 1].db - bursts[i].db) > 10)
                break;
            ++j;
        }
        const size_t count = j - i + 1;
        if (count >= 5) {
            std::vector<double> gaps, lens;
            for (size_t k = i; k < j; ++k) gaps.push_back(bursts[k + 1].peak - bursts[k].peak);
            for (size_t k = i; k <= j; ++k) lens.push_back(bursts[k].to - bursts[k].from);
            const auto cv = [](const std::vector<double>& v) {
                double m = 0, s = 0;
                for (double x : v) m += x;
                m /= double(v.size());
                for (double x : v) s += (x - m) * (x - m);
                return std::sqrt(s / double(v.size())) / std::max(1e-9, m);
            };
            const double gapCv = cv(gaps), lenCv = cv(lens);
            double longest = 0;
            size_t voiced = 0;
            for (double l : lens) longest = std::max(longest, l);
            for (size_t k = i; k <= j; ++k) voiced += bursts[k].voiced;
            // Bursts of the same length at a steady pace, most of them voiced; not inside music (a fast rhythm)
            if (gapCv < 0.12 && lenCv < 0.25 && longest <= 25 && 2 * voiced >= count
                && !overlaps(music, bursts[i].from, bursts[j].to))
                out << Range{bursts[i].from, bursts[j].to,
                             std::min(0.9, 0.5 + 0.4 * (1 - gapCv / 0.12) * ramp(double(count), 4, 7))};
        }
        i = j + 1;
    }
    return out;
}

// Screams: a voiced, high (median pitch >= 350 Hz) sound held 0.3-4 s, at least 10 dB louder than what is around it
// (the median of the 3 s before and after). A loud high note of a singer or a whistle is the same to this rule
// outside music.
QVector<Range> screams(const std::vector<Frame>& fr, const QVector<Range>& music)
{
    const int n = int(fr.size());
    QVector<Range> out;
    for (int i = 0; i < n;) {
        const auto voicedHigh = [&](int k) {
            return fr[size_t(k)].voicing > 0.6f && fr[size_t(k)].pitch >= 300 && fr[size_t(k)].db >= kSilentDb;
        };
        if (!voicedHigh(i)) {
            ++i;
            continue;
        }
        // A run of such frames, gaps up to 50 ms bridged
        int j = i, lastGood = i;
        while (j < n && j - lastGood <= 5) {
            if (voicedHigh(j)) lastGood = j;
            ++j;
        }
        const int from = i, to = lastGood + 1;
        i = j;
        const int len = to - from;
        if (len < 30 || len > 400) continue;
        std::vector<float> pitches, inside, around;
        for (int k = from; k < to; ++k) {
            inside.push_back(fr[size_t(k)].db);
            if (voicedHigh(k)) pitches.push_back(fr[size_t(k)].pitch);
        }
        if (double(pitches.size()) < 0.7 * len || percentile(pitches, 0.5) < 350) continue;
        for (int k = std::max(0, from - 300); k < from - 10; ++k) around.push_back(fr[size_t(k)].db);
        for (int k = to + 10; k < std::min(n, to + 300); ++k) around.push_back(fr[size_t(k)].db);
        const double over = percentile(inside, 0.5) - (around.empty() ? -120.0 : std::max(-90.0, percentile(around, 0.5)));
        if (over < 10) continue;
        if (overlaps(music, from, to)) continue;
        out << Range{from, to, 0.5 + 0.3 * ramp(over, 10, 25)};
    }
    return out;
}

QVector<Event> rules(const std::vector<Frame>& fr)
{
    QVector<Event> out;
    const int n = int(fr.size());
    if (n == 0) return out;
    constexpr int kTick = 10, kHalf = 50; // a tick every 100 ms, its block +-0.5 s
    const int ticks = (n + kTick - 1) / kTick;
    const size_t nt = static_cast<size_t>(ticks);
    std::vector<double> speech(nt), music(nt), applause(nt), cheering(nt);
    const bool dump = qEnvironmentVariableIsSet("AUDIO_EVENTS_DUMP");
    for (int i = 0; i < ticks; ++i) {
        const int c = i * kTick + kTick / 2;
        const Block b = blockAt(fr, c - kHalf, c + kHalf);
        const Scores s = score(b);
        speech[size_t(i)] = s.speech;
        music[size_t(i)] = s.music;
        applause[size_t(i)] = s.applause;
        cheering[size_t(i)] = s.cheering;
        if (dump)
            std::fprintf(stderr, "%6.2f act %.2f lef %.2f voi %.2f vm %.2f f0 %4.0f flat %.3f cen %4.0f held %.2f ons %4.1f "
                  "rough %4.1f | sp %.2f mu %.2f ap %.2f ch %.2f\n",
                  timeOf(c), b.active, b.lef, b.voiced, b.voiceMean, b.pitch, b.flat, b.centroid, b.held, b.onsets,
                  b.roughness, s.speech, s.music, s.applause, s.cheering);
    }
    const QVector<Range> mu = segments(music, kTick, n, 60, 200, fr);
    const QVector<Range> laughs = laughter(fr, mu);
    const QVector<Range> screamRanges = screams(fr, mu);
    QVector<Range> cutFromSpeech = laughs;
    cutFromSpeech += screamRanges;
    // The first moments of music after a pause look like speech to the block rules (pauses in the block, the
    // notes not held long enough yet): a short speech piece right before or at the start of music goes
    const QVector<Range> ap = segments(applause, kTick, n, 50, 100, fr);
    const QVector<Range> ch = subtract(segments(cheering, kTick, n, 50, 100, fr), screamRanges, 100);
    // The same for the fading end of a crowd (fewer voices, pauses)
    QVector<Range> other = mu;
    other += ap;
    other += ch;
    QVector<Range> sp;
    for (const Range& r : subtract(segments(speech, kTick, n, 80, 60, fr), cutFromSpeech, 60))
        if (r.to - r.from >= 150 || !overlaps(other, r.from - 30, r.to + 30)) sp << r;
    // Shots inside applause/cheering are its claps and shouts starting, inside laughter its breaths
    QVector<Range> crowd = ap;
    crowd += ch;
    crowd += laughs;
    QVector<Range> sh;
    for (const Range& r : shots(fr)) {
        bool inCrowd = false;
        for (const Range& c : crowd) inCrowd |= r.from >= c.from - 10 && r.from < c.to;
        if (!inCrowd) sh << r;
    }
    const auto add = [&out](const char* kind, const QVector<Range>& rs) {
        for (const Range& r : rs) out << Event{kind, timeOf(r.from), timeOf(r.to), std::clamp(r.conf, 0.0, 1.0)};
    };
    add("speech", sp);
    add("music", mu);
    add("silence", silences(fr));
    add("laughter", laughs);
    add("applause", ap);
    add("cheering", ch);
    add("shot", sh);
    add("scream", screamRanges);
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        return a.from != b.from ? a.from < b.from : a.kind < b.kind;
    });
    return out;
}

// ---------- YAMNet ----------

constexpr int kYamnetHop = 7680;      // 0.48 s
constexpr int kYamnetPatch = 15360;   // 0.96 s
constexpr int kYamnetWindow = 15600;  // 0.975 s: what the fixed-size exports take

// YAMNet display names (AudioSet ontology) per kind
const QHash<QString, QString>& classKinds()
{
    static const QHash<QString, QString> map = [] {
        QHash<QString, QString> m;
        const auto put = [&m](const char* kind, std::initializer_list<const char*> names) {
            for (const char* n : names) m.insert(QString(n).toLower(), kind);
        };
        put("speech", {"Speech", "Child speech, kid speaking", "Conversation", "Narration, monologue", "Babbling"});
        put("music", {"Music", "Musical instrument", "Singing", "Song"});
        put("silence", {"Silence"});
        put("laughter", {"Laughter", "Baby laughter", "Giggle", "Snicker", "Belly laugh", "Chuckle, chortle"});
        put("applause", {"Applause", "Clapping"});
        put("cheering", {"Cheering", "Crowd", "Children shouting"});
        put("shot", {"Gunshot, gunfire", "Machine gun", "Fusillade", "Artillery fire", "Cap gun", "Explosion",
                     "Burst, pop", "Boom", "Fireworks", "Firecracker", "Bang", "Smash, crash", "Slam"});
        put("scream", {"Screaming", "Shout", "Yell", "Bellow", "Battle cry", "Whoop"});
        return m;
    }();
    return map;
}

// Score a kind needs (YAMNet's scores are per class, 0..1; speech and music are high whenever present)
double threshold(const QString& kind)
{
    if (kind == "speech" || kind == "silence") return 0.5;
    if (kind == "music") return 0.4;
    return 0.3;
}

// One CSV line: fields, quotes respected ("Child speech, kid speaking")
QStringList csvFields(const QString& line)
{
    QStringList out;
    QString cur;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                cur += '"';
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (c == ',' && !quoted) {
            out << cur;
            cur.clear();
        } else {
            cur += c;
        }
    }
    out << cur;
    return out;
}

// Class index -> kind index (-1 = unmapped); empty with *error if the list cannot be read
std::vector<int> readLabels(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = "cannot read the YAMNet class list " + path;
        return {};
    }
    std::vector<int> out;
    QTextStream in(&f);
    bool first = true;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QStringList fields = csvFields(line);
        if (first) { // "index,mid,display_name"
            first = false;
            if (fields.value(0).trimmed().toLower() == "index") continue;
        }
        const QString name = (fields.size() >= 3 ? fields[2] : fields.last()).trimmed().toLower();
        out.push_back(int(kinds().indexOf(classKinds().value(name))));
    }
    if (out.empty() && error) *error = "the YAMNet class list is empty: " + path;
    return out;
}

} // namespace

QVector<Event> heuristics(const std::vector<float>& mono16k, const std::function<bool(double)>& progress)
{
    Extractor ex;
    const size_t chunk = size_t(kRate) * 10;
    for (size_t at = 0; at < mono16k.size(); at += chunk) {
        if (progress && !progress(double(at) / double(std::max<size_t>(1, mono16k.size())))) return {};
        ex.add(mono16k.data() + at, int(std::min(chunk, mono16k.size() - at)));
    }
    ex.finish();
    return rules(ex.frames);
}

struct Listener::Data {
    Extractor ex;
};

Listener::Listener() : d(std::make_unique<Data>()) {}
Listener::~Listener() = default;
void Listener::add(const float* mono16k, int n)
{
    if (n > 0) d->ex.add(mono16k, n);
}
QVector<Event> Listener::finish()
{
    d->ex.finish();
    return rules(d->ex.frames);
}

QVector<Event> yamnetWith(const QString& model, const QString& labels, const std::vector<float>& mono16k,
                          QString* error, const std::function<bool(double)>& progress)
{
    const std::vector<int> classes = readLabels(labels, error);
    if (classes.empty()) return {};
    auto session = Inference::Session::open(model, error);
    if (!session) return {};
    if (session->inputs().size() != 1 || !session->inputs()[0].isFloat) {
        if (error) *error = "the YAMNet model must take one float input (the waveform)";
        return {};
    }
    int scores = -1;
    for (size_t i = 0; i < session->outputs().size() && scores < 0; ++i) {
        const Inference::Port& p = session->outputs()[i];
        if (p.isFloat && !p.shape.empty() && p.shape.back() == int64_t(classes.size())) scores = int(i);
    }
    if (scores < 0) {
        if (error) *error = QString("the model has no output with %1 class scores").arg(classes.size());
        return {};
    }
    // The input: a waveform of any length ([-1]) or fixed windows ([15600] / [1, 15600])
    const std::vector<int64_t>& shape = session->inputs()[0].shape;
    const int64_t fixed = shape.empty() ? -1 : shape.back();
    const bool batch = shape.size() == 2;
    const int nKinds = int(kinds().size());
    std::vector<std::vector<float>> rows; // per 0.48 s step: score per kind
    const auto addRow = [&](const float* s, int count) {
        std::vector<float> row(size_t(nKinds), 0.f);
        for (int c = 0; c < count && c < int(classes.size()); ++c)
            if (classes[size_t(c)] >= 0) row[size_t(classes[size_t(c)])] = std::max(row[size_t(classes[size_t(c)])], s[c]);
        rows.push_back(row);
    };
    const int64_t total = int64_t(mono16k.size());
    if (fixed > 0) {
        for (int64_t at = 0; at < std::max<int64_t>(1, total - kYamnetPatch + kYamnetHop); at += kYamnetHop) {
            if (progress && !progress(double(at) / double(std::max<int64_t>(1, total)))) {
                if (error) *error = "canceled";
                return {};
            }
            Inference::Tensor in;
            in.shape = batch ? std::vector<int64_t>{1, fixed} : std::vector<int64_t>{fixed};
            in.data.assign(size_t(fixed), 0.f);
            const int64_t n = std::min<int64_t>(fixed, total - at);
            std::copy(mono16k.begin() + at, mono16k.begin() + at + n, in.data.begin());
            const auto out = session->run({in}, error);
            if (!out) return {};
            const Inference::Tensor& t = (*out)[size_t(scores)];
            if (t.data.size() < classes.size()) {
                if (error) *error = "unexpected output size";
                return {};
            }
            addRow(t.data.data(), int(classes.size()));
        }
    } else {
        // Whole minutes at a time; row i of a chunk = the patch at chunk start + i * 0.48 s
        constexpr int64_t kStep = int64_t(kYamnetHop) * 125; // 60 s
        for (int64_t at = 0; at < total; at += kStep) {
            if (progress && !progress(double(at) / double(std::max<int64_t>(1, total)))) {
                if (error) *error = "canceled";
                return {};
            }
            const int64_t n = std::min<int64_t>(kStep + kYamnetPatch - kYamnetHop, total - at);
            Inference::Tensor in;
            in.data.assign(mono16k.begin() + at, mono16k.begin() + at + n);
            if (int64_t(in.data.size()) < kYamnetWindow) in.data.resize(size_t(kYamnetWindow), 0.f);
            in.shape = batch ? std::vector<int64_t>{1, int64_t(in.data.size())} : std::vector<int64_t>{int64_t(in.data.size())};
            const auto out = session->run({in}, error);
            if (!out) return {};
            const Inference::Tensor& t = (*out)[size_t(scores)];
            const int width = int(classes.size());
            const int count = int(t.data.size() / size_t(width));
            const int keep = std::min<int>(count, int((std::min<int64_t>(kStep, total - at) + kYamnetHop - 1) / kYamnetHop));
            for (int r = 0; r < keep; ++r) addRow(t.data.data() + size_t(r) * size_t(width), width);
        }
    }
    // Rows above a kind's threshold, neighbours joined, conf = the best score
    QVector<Event> out;
    const double end = double(total) / kRate;
    for (int k = 0; k < nKinds; ++k) {
        const QString kind = kinds()[k];
        const double th = threshold(kind);
        for (int r = 0; r < int(rows.size());) {
            if (rows[size_t(r)][size_t(k)] < th) {
                ++r;
                continue;
            }
            int s = r;
            float best = 0;
            while (r < int(rows.size()) && rows[size_t(r)][size_t(k)] >= th) best = std::max(best, rows[size_t(r++)][size_t(k)]);
            const double from = s * double(kYamnetHop) / kRate;
            const double to = std::min(end, (r - 1) * double(kYamnetHop) / kRate + double(kYamnetPatch) / kRate);
            out << Event{kind, from, std::max(from, to), std::clamp(double(best), 0.0, 1.0), "yamnet"};
        }
    }
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        return a.from != b.from ? a.from < b.from : a.kind < b.kind;
    });
    if (error) error->clear();
    return out;
}

bool yamnetAvailable(QString* why)
{
    if (Extensions::audioEventsModel().isEmpty() || Extensions::audioEventsLabels().isEmpty()) {
        if (why) *why = "YAMNet is not installed (extension \"audio-events\")";
        return false;
    }
    return Inference::available(why);
}

QVector<Event> yamnet(const std::vector<float>& mono16k, QString* error, const std::function<bool(double)>& progress)
{
    if (!yamnetAvailable(error)) return {};
    return yamnetWith(Extensions::audioEventsModel(), Extensions::audioEventsLabels(), mono16k, error, progress);
}

QJsonArray toJson(const QVector<Event>& events)
{
    QJsonArray out;
    for (const Event& e : events)
        out << QJsonObject{{"kind", e.kind}, {"from_s", std::round(e.from * 1000) / 1000},
                           {"to_s", std::round(e.to * 1000) / 1000}, {"conf", std::round(e.conf * 100) / 100},
                           {"by", e.by}};
    return out;
}

// ---------- Files and the cache ----------

namespace {

std::atomic<int> g_measured{0};
QMutex g_mutex;
QHash<QString, Result>& memory()
{
    static QHash<QString, Result> cache;
    return cache;
}

QString cacheFile(const QString& key)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/audio-events/" + key + ".json";
}

std::optional<Result> loadDisk(const QString& key)
{
    QFile f(cacheFile(key));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o["version"].toInt() != kVersion) return std::nullopt;
    Result r;
    r.by = o["by"].toString();
    r.duration = o["duration"].toDouble();
    for (const QJsonValue& v : o["events"].toArray()) {
        const QJsonObject e = v.toObject();
        r.events << Event{e["kind"].toString(), e["from_s"].toDouble(), e["to_s"].toDouble(), e["conf"].toDouble(),
                          e["by"].toString("heuristic")};
    }
    return r;
}

void saveDisk(const QString& key, const Result& r)
{
    const QJsonObject o{{"version", kVersion}, {"by", r.by}, {"duration", r.duration}, {"events", toJson(r.events)}};
    const QString file = cacheFile(key);
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0) out.commit();
}

// Shot times from the heuristics inside YAMNet's coarse shot windows (YAMNet decides, the heuristic says when)
QVector<Event> combine(const QVector<Event>& model, const QVector<Event>& heur)
{
    QVector<Event> out;
    QVector<Event> heurShots;
    for (const Event& e : heur) {
        if (e.kind == "silence") out << e; // exact, from the level
        if (e.kind == "shot") heurShots << e;
    }
    QVector<bool> used(heurShots.size(), false);
    for (const Event& e : model) {
        if (e.kind == "silence") continue;
        if (e.kind != "shot") {
            out << e;
            continue;
        }
        bool any = false;
        for (int i = 0; i < heurShots.size(); ++i) {
            const Event& h = heurShots[i];
            if (h.from >= e.from && h.from < e.to) {
                out << Event{"shot", h.from, h.to, std::max(e.conf, h.conf), "yamnet"};
                used[i] = any = true;
            }
        }
        if (!any) out << e;
    }
    for (int i = 0; i < heurShots.size(); ++i)
        if (!used[i]) out << heurShots[i];
    std::sort(out.begin(), out.end(), [](const Event& a, const Event& b) {
        return a.from != b.from ? a.from < b.from : a.kind < b.kind;
    });
    return out;
}

Result filtered(Result r, const Options& o)
{
    if (!o.only.isEmpty() || o.minConf > 0) {
        QVector<Event> keep;
        for (const Event& e : r.events)
            if ((o.only.isEmpty() || o.only.contains(e.kind)) && e.conf >= o.minConf) keep << e;
        r.events = keep;
    }
    return r;
}

} // namespace

QString wayKey(const QString& model)
{
    if (model.isEmpty()) return "heuristic";
    const QFileInfo m(model);
    return QString("yamnet|%1|%2|%3").arg(m.absoluteFilePath()).arg(m.size()).arg(m.lastModified().toMSecsSinceEpoch());
}

QString way(Engine engine)
{
    if (engine == Engine::Heuristic) return wayKey({});
    // Once per process and model file (the index asks on every cache lookup, on the GUI thread too): whether the
    // runtime loads is asked only when the model file is new or changed
    const QString model = Extensions::audioEventsModel();
    if (model.isEmpty()) return wayKey({});
    const QString stat = wayKey(model);
    static QMutex mutex;
    static QString lastStat, lastWay;
    {
        QMutexLocker lock(&mutex);
        if (stat == lastStat) return lastWay;
    }
    const QString w = yamnetAvailable() ? stat : wayKey({});
    QMutexLocker lock(&mutex);
    lastStat = stat;
    lastWay = w;
    return w;
}

Result analyzeFile(const QString& path, int stream, const Options& options)
{
    Result result;
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        result.error = "no such file: " + path;
        return result;
    }
    QString why;
    const bool model = options.engine != Engine::Heuristic && yamnetAvailable(&why);
    if (options.engine == Engine::Yamnet && !model) {
        result.error = why;
        return result;
    }
    // The key names everything the result depends on: the file, the stream, the way and the model's file
    const QString way = model ? wayKey(Extensions::audioEventsModel()) : wayKey({});
    const QString id = QString("%1|%2|%3|%4|%5|%6")
                           .arg(PathKey::of(path))
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(stream)
                           .arg(way)
                           .arg(kVersion);
    const QString key = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
    if (options.useCache) {
        {
            QMutexLocker lock(&g_mutex);
            if (const auto it = memory().constFind(key); it != memory().constEnd()) {
                Result r = *it;
                r.cached = true;
                return filtered(r, options);
            }
        }
        if (auto disk = loadDisk(key)) {
            QMutexLocker lock(&g_mutex);
            memory().insert(key, *disk);
            disk->cached = true;
            return filtered(*disk, options);
        }
    }
    // Measure: the sound at 16 kHz mono, in pieces (the features are small; YAMNet needs the samples)
    QString openError;
    const auto reader = Audio::Reader::open(fi.absoluteFilePath(), stream, Audio::Reader::Options{kRate, 1}, &openError);
    if (!reader) {
        result.error = openError.isEmpty() ? "no audio stream " + QString::number(stream) : openError;
        return result;
    }
    const int64_t end = reader->info().end;
    if (end <= 0) {
        result.error = "the audio stream is empty";
        return result;
    }
    ++g_measured;
    Extractor ex;
    std::vector<float> all;
    if (model) all.reserve(size_t(std::min<int64_t>(end, int64_t(kRate) * 60 * 30)));
    constexpr int kChunk = kRate; // 1 s
    std::vector<float> buf(kChunk);
    float* out[1] = {buf.data()};
    const double share = model ? 0.4 : 1.0; // decoding vs YAMNet
    for (int64_t pos = 0; pos < end; pos += kChunk) {
        if (options.progress && (pos / kChunk) % 10 == 0 && !options.progress(share * double(pos) / double(end))) {
            result.error = "canceled";
            return result;
        }
        const int n = int(std::min<int64_t>(kChunk, end - pos));
        if (!reader->read(pos, n, out)) result.incomplete = true;
        ex.add(buf.data(), n);
        if (model) all.insert(all.end(), buf.begin(), buf.begin() + n);
    }
    ex.finish();
    // Reading the file failed (a network share gone, the reader lost the file): not the file's sound. A file that
    // is damaged for good (cut off, broken packets) reads the same every time and is cached as it is.
    if (reader->stats().readErrors > 0) result.incomplete = true;
    result.duration = double(end) / kRate;
    const QVector<Event> heur = rules(ex.frames);
    if (model) {
        QString error;
        bool canceled = false;
        const QVector<Event> y = yamnet(all, &error, [&](double p) {
            if (!options.progress || options.progress(share + (1 - share) * p)) return true;
            canceled = true;
            return false;
        });
        if (canceled) {
            result.error = "canceled";
            return result;
        }
        if (!error.isEmpty()) {
            if (options.engine == Engine::Yamnet) {
                result.error = error;
                return result;
            }
            result.events = heur; // Auto: the model failed on this file - the heuristics still answer
            result.by = "heuristic";
        } else {
            result.events = combine(y, heur);
            result.by = "yamnet";
        }
    } else {
        result.events = heur;
        result.by = "heuristic";
    }
    if (options.progress) options.progress(1.0);
    if (options.useCache && (result.by == "yamnet") == model && !result.incomplete) {
        saveDisk(key, result);
        QMutexLocker lock(&g_mutex);
        memory().insert(key, result);
    }
    return filtered(result, options);
}

QVector<Event> analyze(const QString& path, int stream, const Options& options)
{
    return analyzeFile(path, stream, options).events;
}

void clearMemoryCache()
{
    QMutexLocker lock(&g_mutex);
    memory().clear();
}

int measuredCount() { return g_measured.load(); }

} // namespace AudioEvents
