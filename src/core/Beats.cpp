#include "core/Beats.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace Beats {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kValueTime = 0.5; // a value's time: its middle (an attack lands in the value it falls into)

double valueTime(int i) { return (i + kValueTime) / kRate; }
double dB(double meanSquare) { return 10 * std::log10(meanSquare + 1e-12); }

// RBJ cookbook biquads, normalized
void design(double* c, double f, double q, int rate, bool high)
{
    const double w = 2 * kPi * f / rate, cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    const double b0 = high ? (1 + cw) / 2 : (1 - cw) / 2;
    c[0] = b0 / a0;
    c[1] = (high ? -(1 + cw) : 1 - cw) / a0;
    c[2] = b0 / a0;
    c[3] = -2 * cw / a0;
    c[4] = (1 - alpha) / a0;
}

// An onset with its time (seconds), only where the onsets rise above their local mean
struct Hit {
    double t, w;
};

// Onsets above their local mean (+-0.4 s): the attacks stand out of a dense texture, a steady hiss does not count
std::vector<double> attacks(const std::vector<float>& onset)
{
    const int n = int(onset.size()), h = int(0.4 * kRate);
    std::vector<double> prefix(size_t(n) + 1, 0.0), x(size_t(n), 0.0);
    for (int i = 0; i < n; ++i) prefix[size_t(i) + 1] = prefix[size_t(i)] + onset[size_t(i)];
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - h), b = std::min(n, i + h + 1);
        x[size_t(i)] = std::max(0.0, onset[size_t(i)] - (prefix[size_t(b)] - prefix[size_t(a)]) / (b - a));
    }
    return x;
}

// r[L] = mean of x[i] * x[i + L], L = 0..maxLag
std::vector<double> autocorrelation(const std::vector<double>& x, int maxLag)
{
    const int n = int(x.size());
    std::vector<double> r(size_t(maxLag) + 1, 0.0);
    for (int L = 0; L <= maxLag && L < n; ++L) {
        double s = 0;
        for (int i = 0; i + L < n; ++i) s += x[size_t(i)] * x[size_t(i + L)];
        r[size_t(L)] = s / (n - L);
    }
    return r;
}

double at(const std::vector<double>& r, double lag)
{
    if (lag < 0 || lag >= double(r.size() - 1)) return 0;
    const int i = int(lag);
    const double f = lag - i;
    return r[size_t(i)] * (1 - f) + r[size_t(i) + 1] * f;
}

// Sub-bin position of a peak at i in a circular (or plain) histogram: parabola through the neighbours
double refine(const std::vector<double>& h, int i, bool circular)
{
    const int n = int(h.size());
    if (!circular && (i <= 0 || i >= n - 1)) return i;
    const double y0 = h[size_t((i - 1 + n) % n)], y1 = h[size_t(i)], y2 = h[size_t((i + 1) % n)];
    const double d = y0 - 2 * y1 + y2;
    if (d >= 0) return i;
    return i + std::clamp(0.5 * (y0 - y2) / d, -0.5, 0.5);
}

// The hits folded onto period p: histogram of their phase in bins of about `bin` seconds, smoothed circularly with a
// triangle of half width `smooth` seconds. Bin j is centred on phase (j + 0.5) * p / size.
std::vector<double> fold(const std::vector<Hit>& hits, double p, double bin, double smooth)
{
    const int n = std::max(8, int(std::lround(p / bin)));
    std::vector<double> h(size_t(n), 0.0);
    for (const Hit& x : hits) {
        const double ph = std::fmod(x.t, p) / p;
        h[size_t(std::min(n - 1, int(ph * n)))] += x.w;
    }
    const int k = std::max(0, int(std::lround(smooth / (p / n))));
    if (k == 0) return h;
    std::vector<double> out(size_t(n), 0.0);
    const double norm = double(k + 1) * (k + 1);
    for (int j = 0; j < n; ++j)
        for (int d = -k; d <= k; ++d) out[size_t(j)] += h[size_t((j + d + n) % n)] * (k + 1 - std::abs(d)) / norm;
    return out;
}

double peakOf(const std::vector<double>& h) { return *std::max_element(h.begin(), h.end()); }

double median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0;
    const size_t i = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + i, v.end());
    return v[i];
}

// How strong the onsets half a period after the strongest ones are, relative to them (above the histogram's
// median): about 1 = every second beat of a beat twice as fast is as strong, about 0 = only the strongest count
double halfRatio(const std::vector<Hit>& hits, double p)
{
    const std::vector<double> h = fold(hits, p, 0.0025, 0.008);
    const int n = int(h.size());
    const int top = int(std::max_element(h.begin(), h.end()) - h.begin());
    const double base = median(h), peak = h[size_t(top)] - base;
    if (peak <= 0) return 0;
    double half = 0;
    for (int d = -3; d <= 3; ++d) half = std::max(half, h[size_t((top + n / 2 + d + n) % n)]);
    return std::max(0.0, half - base) / peak;
}

// Period (seconds) from the autocorrelation: the best of four multiples summed, then refined over the peaks at all
// multiples up to the end of r
double coarsePeriod(const std::vector<double>& r, const Options& o)
{
    const double lo = 60.0 * kRate / o.maxBpm, hi = 60.0 * kRate / o.minBpm;
    double best = -1, lag = 0;
    for (double L = lo; L <= hi; L += 0.05) {
        double s = 0;
        for (int k = 1; k <= 4; ++k) s += at(r, k * L);
        if (s > best) {
            best = s;
            lag = L;
        }
    }
    // Peaks near k * lag: least squares through the origin (a peak at a far multiple pins the period finely)
    double sxy = 0, sxx = 0;
    for (int k = 1; (k + 0.5) * lag < double(r.size()) - 2; ++k) {
        const int c = int(std::lround(k * lag)), w = 3;
        int top = c;
        for (int i = std::max(1, c - w); i <= std::min(int(r.size()) - 2, c + w); ++i)
            if (r[size_t(i)] > r[size_t(top)]) top = i;
        if (top == c - w || top == c + w) continue; // no peak there, only a slope
        const double pos = refine(r, top, false);
        sxy += k * pos;
        sxx += double(k) * k;
    }
    return (sxx > 0 ? sxy / sxx : lag) / kRate;
}

// The period near p whose fold over the whole signal is sharpest (a fixed grid: a period off by 0.01 % drifts by a
// beat over a few minutes, so this pins it down)
double finePeriod(const std::vector<Hit>& hits, double p, double duration)
{
    // Steps small enough that the drift over the signal changes by less than ~3 ms per step
    const double step = std::clamp(0.003 / std::max(1.0, duration), 2e-6, 1e-4);
    double best = -1, out = p;
    for (double s = -0.004; s <= 0.004; s += step) {
        const double q = p * (1 + s);
        const double v = peakOf(fold(hits, q, 0.0025, 0.008));
        if (v > best) {
            best = v;
            out = q;
        }
    }
    return out;
}

// Changes of the sound between the `w` units before u and the `w` units from u on, summed over the features (a
// pattern that repeats every w units does not count), only where they peak
std::vector<double> novelty(const std::vector<const std::vector<double>*>& features, int w)
{
    const int n = features.empty() ? 0 : int(features[0]->size());
    std::vector<double> nov(size_t(n), 0.0);
    for (const std::vector<double>* f : features) {
        std::vector<double> prefix(size_t(n) + 1, 0.0);
        for (int i = 0; i < n; ++i) prefix[size_t(i) + 1] = prefix[size_t(i)] + (*f)[size_t(i)];
        for (int u = w; u + w <= n; ++u) {
            const double after = prefix[size_t(u + w)] - prefix[size_t(u)];
            const double before = prefix[size_t(u)] - prefix[size_t(u - w)];
            nov[size_t(u)] += std::abs(after - before) / w;
        }
    }
    std::vector<double> peaks(size_t(n), 0.0);
    for (int u = 1; u + 1 < n; ++u)
        if (nov[size_t(u)] > 0 && nov[size_t(u)] >= nov[size_t(u - 1)] && nov[size_t(u)] > nov[size_t(u + 1)])
            peaks[size_t(u)] = nov[size_t(u)];
    return peaks;
}

// Levels in dB with a floor (silence must not count as a huge change)
std::vector<double> floored(std::vector<double> v, double range)
{
    if (v.empty()) return v;
    const double top = *std::max_element(v.begin(), v.end());
    for (double& x : v) x = std::max(x, top - range);
    return v;
}

// Sum of `v` per residue of the index modulo m
std::vector<double> perResidue(const std::vector<double>& v, int m)
{
    std::vector<double> s(size_t(m), 0.0);
    for (size_t i = 0; i < v.size(); ++i) s[i % size_t(m)] += v[i];
    return s;
}

// Best residue and how clearly it wins over the second best (0..1)
std::pair<int, double> bestResidue(const std::vector<double>& s)
{
    const int best = int(std::max_element(s.begin(), s.end()) - s.begin());
    double second = 0;
    for (int i = 0; i < int(s.size()); ++i)
        if (i != best) second = std::max(second, s[size_t(i)]);
    return {best, s[size_t(best)] > 0 ? std::clamp(1 - second / s[size_t(best)], 0.0, 1.0) : 0.0};
}

} // namespace

// ---- Features ----

double Features::Biquad::run(double x)
{
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

Features::Features(int sampleRate) : m_rate(std::max(1, sampleRate)), m_onsets(m_rate)
{
    m_next = (m_rate + kRate - 1) / kRate;
    // 4th order Butterworth low pass at 120 Hz (kick and bass), 2nd order high pass at 5 kHz (hi-hats, cymbals)
    const double q[2] = {0.5412, 1.3066};
    for (int i = 0; i < 2; ++i) {
        double c[5];
        design(c, std::min(120.0, 0.4 * m_rate), q[i], m_rate, false);
        m_bass[i].b0 = c[0], m_bass[i].b1 = c[1], m_bass[i].b2 = c[2], m_bass[i].a1 = c[3], m_bass[i].a2 = c[4];
    }
    double c[5];
    design(c, std::min(5000.0, 0.4 * m_rate), 0.7071, m_rate, true);
    m_high.b0 = c[0], m_high.b1 = c[1], m_high.b2 = c[2], m_high.a1 = c[3], m_high.a2 = c[4];
}

void Features::add(const float* mono, int samples)
{
    m_onsets.add(mono, samples);
    for (int i = 0; i < samples; ++i) {
        if (m_pos >= m_next) { // value k covers the samples n with floor(n * kRate / rate) == k, as the envelope
            finishValue();
            ++m_value;
            m_next = ((m_value + 1) * m_rate + kRate - 1) / kRate;
        }
        const double x = mono[i];
        const double bass = m_bass[1].run(m_bass[0].run(x)), high = m_high.run(x);
        m_energy += x * x;
        m_bassEnergy += bass * bass;
        m_highEnergy += high * high;
        ++m_count;
        ++m_pos;
    }
}

void Features::finishValue()
{
    if (m_count == 0) return;
    m_out.energy.push_back(float(m_energy / m_count));
    m_out.bass.push_back(float(m_bassEnergy / m_count));
    m_out.high.push_back(float(m_highEnergy / m_count));
    m_energy = m_bassEnergy = m_highEnergy = 0;
    m_count = 0;
}

Values Features::take()
{
    finishValue();
    Values out = std::move(m_out);
    out.onset = m_onsets.take();
    const size_t n = std::min({out.onset.size(), out.energy.size(), out.bass.size(), out.high.size()});
    out.onset.resize(n);
    out.energy.resize(n);
    out.bass.resize(n);
    out.high.resize(n);
    *this = Features(m_rate);
    return out;
}

// ---- Analysis ----

Result analyze(const Values& v, const Options& o)
{
    Result r;
    const int n = int(std::min({v.onset.size(), v.energy.size(), v.bass.size(), v.high.size()}));
    const double duration = double(n) / kRate;
    if (duration < 4 * 60.0 / o.minBpm) return r; // not even four beats of the slowest tempo

    // Tempo: log onsets (every attack counts, however loud) for the period; rises of the linear energy (the loud
    // attacks: kick, snare) for the octave and the phase
    const std::vector<double> x = attacks(v.onset);
    std::vector<Hit> hits, loud;
    for (int i = 0; i < n; ++i) {
        if (x[size_t(i)] > 0) hits.push_back({valueTime(i), x[size_t(i)]});
        if (i >= 3) {
            const size_t j = size_t(i);
            const double before = (double(v.energy[j - 1]) + v.energy[j - 2] + v.energy[j - 3]) / 3;
            if (const double rise = v.energy[j] - before; rise > 0) loud.push_back({valueTime(i), rise});
        }
    }
    if (hits.size() < 8 || loud.size() < 8) return r;
    const int maxLag = std::min(n - 1, std::max(int(std::ceil(4 * 60.0 * kRate / o.minBpm)) + 4, 4 * kRate));
    const std::vector<double> ac = autocorrelation(x, maxLag);
    double p = finePeriod(hits, coarsePeriod(ac, o), duration);

    // Octaves: twice as fast when every second beat of it is about as loud as the others (Tekk: a kick on every
    // beat), half as fast when those are much quieter (an off-beat bass or hi-hat, however sharp its attack)
    bool faster = false;
    while (60.0 / (p / 2) <= o.maxBpm * 1.001 && halfRatio(loud, p) >= 0.5) {
        p /= 2;
        faster = true;
    }
    if (!faster && 60.0 / (2 * p) >= o.minBpm * 0.999 && halfRatio(loud, 2 * p) < 0.3) p *= 2;
    r.period = p;
    r.bpm = 60.0 / p;

    // Phase: the peak of both folds together (an off-beat with a sharp attack does not win over a louder beat), and
    // how sharp the grid is
    {
        std::vector<double> h = fold(hits, p, 0.001, 0.006);
        const std::vector<double> hl = fold(loud, p, 0.001, 0.006);
        const double mh = peakOf(h), ml = peakOf(hl);
        for (size_t j = 0; j < h.size() && j < hl.size(); ++j) h[j] = h[j] / mh + (ml > 0 ? hl[j] / ml : 0);
        const int top = int(std::max_element(h.begin(), h.end()) - h.begin());
        const double pos = refine(h, top, true);
        r.phase = std::fmod((pos + 0.5) * p / double(h.size()) + p, p);
        const std::vector<double> c = fold(hits, p, 0.0025, 0.008);
        const double peak = peakOf(c), mean = std::accumulate(c.begin(), c.end(), 0.0) / double(c.size());
        r.confidence = peak + mean > 0 ? std::clamp((peak - mean) / (peak + mean), 0.0, 1.0) : 0.0;
    }
    r.beats = std::max(0, int(std::floor((duration - r.phase) / p + 1e-9)));
    r.found = true;
    if (r.beats < 8) return r;

    // Per beat: level of the whole sound, the bass and the highs (dB), and the bass attack at the beat (kick)
    std::vector<double> pe(size_t(n) + 1, 0.0), pb(size_t(n) + 1, 0.0), ph(size_t(n) + 1, 0.0);
    for (int i = 0; i < n; ++i) {
        pe[size_t(i) + 1] = pe[size_t(i)] + v.energy[size_t(i)];
        pb[size_t(i) + 1] = pb[size_t(i)] + v.bass[size_t(i)];
        ph[size_t(i) + 1] = ph[size_t(i)] + v.high[size_t(i)];
    }
    const auto index = [n](double t) { return std::clamp(int(std::lround(t * kRate - kValueTime)), 0, n); };
    const auto mean = [](const std::vector<double>& prefix, int a, int b) {
        return b > a ? (prefix[size_t(b)] - prefix[size_t(a)]) / (b - a) : 0.0;
    };
    std::vector<double> energy, bass, high, kick, energyLin, bassLin;
    const auto perBeat = [&] {
        const size_t nb = size_t(r.beats);
        for (std::vector<double>* f : {&energy, &bass, &high, &kick, &energyLin, &bassLin}) f->assign(nb, 0.0);
        for (int k = 0; k < r.beats; ++k) {
            const int a = index(beatTime(r, k)), b = index(beatTime(r, k + 1));
            energyLin[size_t(k)] = mean(pe, a, b);
            bassLin[size_t(k)] = mean(pb, a, b);
            energy[size_t(k)] = dB(energyLin[size_t(k)]);
            bass[size_t(k)] = dB(bassLin[size_t(k)]);
            high[size_t(k)] = dB(mean(ph, a, b));
            const int w = int(0.05 * kRate);
            kick[size_t(k)] = std::max(0.0, dB(mean(pb, a, std::min(n, a + w))) - dB(mean(pb, std::max(0, a - w), a)));
        }
    };
    perBeat();
    // The grid where there is sound: silent beats before and after the song are left out (the first beat is where
    // the song starts, an empty end is no break)
    {
        const double top = *std::max_element(energy.begin(), energy.end());
        int first = 0, last = r.beats;
        while (first < last && energy[size_t(first)] < top - 60) ++first;
        while (last > first && energy[size_t(last) - 1] < top - 60) --last;
        if (first > 0 || last < r.beats) {
            r.phase += first * p;
            r.beats = last - first;
            if (r.beats < 8) return r;
            perBeat();
        }
    }
    const int beats = r.beats;
    const size_t nb = size_t(beats);

    // Bars: the beat (of four) where the sound changes most from one bar to the next, the kick as a weaker cue
    const std::vector<double> fe = floored(energy, 50), fb = floored(bass, 50), fh = floored(high, 50);
    {
        const std::vector<double> nov = novelty({&fe, &fb, &fh}, 4);
        std::vector<double> s = perResidue(nov, 4), a = perResidue(kick, 4);
        const double ns = std::accumulate(s.begin(), s.end(), 0.0), na = std::accumulate(a.begin(), a.end(), 0.0);
        for (int j = 0; j < 4; ++j)
            s[size_t(j)] = (ns > 0 ? s[size_t(j)] / ns : 0.25) + 0.25 * (na > 0 ? a[size_t(j)] / na : 0.25);
        const auto [best, clear] = bestResidue(s);
        r.downbeat = best;
        r.downbeatConfidence = clear;
    }

    // Per whole bar from the first bar one
    const int bars = (beats - r.downbeat) / 4;
    std::vector<double> barHigh;
    for (int b = 0; b < bars; ++b) {
        const int a = index(beatTime(r, r.downbeat + 4 * b)), e = index(beatTime(r, r.downbeat + 4 * b + 4));
        r.barEnergyDb.push_back(dB(mean(pe, a, e)));
        r.barBassDb.push_back(dB(mean(pb, a, e)));
        barHigh.push_back(dB(mean(ph, a, e)));
    }

    // Phrases: the same one level up, four bars (eight when every second 4-bar boundary hardly changes anything)
    if (bars >= 8) {
        const std::vector<double> be = floored(r.barEnergyDb, 50), bb = floored(r.barBassDb, 50);
        const std::vector<double> bh = floored(barHigh, 50);
        // Changes from one bar to the next and between two-bar halves (each counts the same)
        std::vector<double> nov(be.size(), 0.0);
        for (int w : {1, 2}) {
            const std::vector<double> part = novelty({&be, &bb, &bh}, w);
            const double sum = std::accumulate(part.begin(), part.end(), 0.0);
            for (size_t i = 0; i < nov.size() && sum > 0; ++i) nov[i] += part[i] / sum;
        }
        const auto [q, clear] = bestResidue(perResidue(nov, 4));
        r.phraseOffset = q;
        r.phraseConfidence = clear;
        const std::vector<double> s8 = perResidue(nov, 8);
        const double a = s8[size_t(q)], b = s8[size_t(q + 4)];
        if (bars >= 16 && std::max(a, b) > 0 && std::min(a, b) < 0.35 * std::max(a, b)) {
            r.phraseBars = 8;
            r.phraseOffset = a >= b ? q : q + 4;
        }
    }

    // Breaks: beats whose bass is far below the song's usual bass (the louder quarter), or the whole sound far
    // below its usual level; runs of at least a bar, gaps of up to two beats closed
    const double usualBass = percentile(bass, 0.75), usualEnergy = percentile(energy, 0.75);
    std::vector<char> quiet(nb);
    for (int k = 0; k < beats; ++k)
        quiet[size_t(k)] = bass[size_t(k)] < usualBass - 12 || energy[size_t(k)] < usualEnergy - 10;
    std::vector<std::pair<int, int>> runs;
    for (int k = 0; k < beats;) {
        if (!quiet[size_t(k)]) {
            ++k;
            continue;
        }
        int e = k;
        while (e < beats && quiet[size_t(e)]) ++e;
        if (!runs.empty() && k - runs.back().second <= 2) runs.back().second = e;
        else runs.push_back({k, e});
        k = e;
    }
    const auto levelDb = [](const std::vector<double>& lin, int a, int b) {
        a = std::max(0, a);
        b = std::min(int(lin.size()), b);
        if (b <= a) return -120.0;
        return dB(std::accumulate(lin.begin() + a, lin.begin() + b, 0.0) / (b - a));
    };
    for (const auto& [a, b] : runs) {
        if (b - a < 4) continue;
        Break br;
        br.from = a;
        br.to = b;
        br.bassDb = levelDb(bassLin, a, b) - usualBass;
        br.energyDb = levelDb(energyLin, a, b) - usualEnergy;
        br.atStart = a == 0;
        br.atEnd = b >= beats;
        r.breaks.push_back(br);
    }

    // Drops: where a break ends and the sound comes back (at least a bar of it), by the jump of bass and level and the
    // length of the break; a return that stays far below the usual bass and the end of an intro count less
    for (int i = 0; i < int(r.breaks.size()); ++i) {
        const Break& br = r.breaks[size_t(i)];
        if (br.atEnd || beats - br.to < 4) continue;
        Drop d;
        d.beat = br.to;
        d.breakIndex = i;
        d.breakS = (br.to - br.from) * p;
        d.bassJumpDb = levelDb(bassLin, d.beat, d.beat + 8) - levelDb(bassLin, std::max(br.from, d.beat - 8), d.beat);
        d.energyJumpDb =
            levelDb(energyLin, d.beat, d.beat + 8) - levelDb(energyLin, std::max(br.from, d.beat - 8), d.beat);
        const double back = levelDb(bassLin, d.beat, d.beat + 8) - usualBass;
        d.score = std::max(0.0, d.bassJumpDb) / 12 + std::max(0.0, d.energyJumpDb) / 6 + 0.5 * std::log2(1 + d.breakS / 2);
        if (back < -20) d.score *= 0.5;
        if (br.atStart) d.score *= 0.8;
        r.drops.push_back(d);
    }
    std::stable_sort(r.drops.begin(), r.drops.end(), [](const Drop& a, const Drop& b) { return a.score > b.score; });
    return r;
}

} // namespace Beats
