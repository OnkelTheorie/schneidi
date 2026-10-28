// SPDX-License-Identifier: AGPL-3.0-only
#include "core/AudioSync.h"

#include "core/Fft.h"

#include <algorithm>
#include <cmath>

namespace AudioSync {

// ---- Envelope ----

Envelope::Envelope(int sampleRate) : m_rate(std::max(1, sampleRate))
{
    m_next = (m_rate + kEnvelopeRate - 1) / kEnvelopeRate;
    // Octave-wide bands over the range where game sounds, voices and announcements have their attacks; bands
    // above the Nyquist frequency of the material are left out (an 8 kHz recording has no 6 kHz band)
    for (double f : {150.0, 400.0, 1000.0, 2500.0, 6000.0}) {
        if (f >= 0.45 * m_rate) break;
        const double w = 2 * 3.14159265358979323846 * f / m_rate, q = 1.0;
        const double alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        Band b;
        b.b0 = alpha / a0;
        b.b1 = 0;
        b.b2 = -alpha / a0;
        b.a1 = -2 * std::cos(w) / a0;
        b.a2 = (1 - alpha) / a0;
        m_bands.push_back(b);
    }
}

void Envelope::add(const float* mono, int samples)
{
    for (int i = 0; i < samples; ++i) {
        if (m_pos >= m_next) { // value k covers the samples n with floor(n * 200 / rate) == k
            finishValue();
            ++m_value;
            m_next = ((m_value + 1) * m_rate + kEnvelopeRate - 1) / kEnvelopeRate;
        }
        const double x = mono[i];
        for (Band& b : m_bands) {
            const double y = b.b0 * x + b.b1 * b.x1 + b.b2 * b.x2 - b.a1 * b.y1 - b.a2 * b.y2;
            b.x2 = b.x1;
            b.x1 = x;
            b.y2 = b.y1;
            b.y1 = y;
            b.energy += y * y;
        }
        ++m_count;
        ++m_pos;
    }
}

void Envelope::finishValue()
{
    if (m_count == 0) return;
    // Per band: level in log (-70 dB floor), the rise from the previous value is the onset (falling = 0).
    // Log = the same event gives the same onset however loud it is heard; bands = a dull or masked copy still
    // rises in the bands it reaches. (Rising against the maximum of the last values lost quiet shared sounds that
    // fall into the decay of a louder own sound.)
    double onset = 0;
    for (Band& b : m_bands) {
        const double v = std::log10(1e-7 + b.energy / m_count);
        if (!m_first) onset += std::max(0.0, v - b.prev);
        b.prev = v;
        b.energy = 0;
    }
    m_out.push_back(float(onset));
    m_first = false;
    m_count = 0;
}

std::vector<float> Envelope::take()
{
    finishValue();
    std::vector<float> out = std::move(m_out);
    *this = Envelope(m_rate);
    return out;
}

std::vector<float> envelope(const std::vector<float>& mono, int sampleRate)
{
    Envelope e(sampleRate);
    e.add(mono.data(), int(mono.size()));
    return e.take();
}

// ---- Matching ----

namespace {

// Mean removed (constant level must not correlate), as double; norm = 0 for silence/empty
struct Prepared {
    std::vector<double> v;
    double norm = 0;
};

Prepared prepare(const std::vector<float>& e)
{
    Prepared p;
    if (e.empty()) return p;
    double mean = 0;
    for (float x : e) mean += x;
    mean /= double(e.size());
    p.v.resize(e.size());
    double energy = 0;
    for (size_t i = 0; i < e.size(); ++i) {
        p.v[i] = e[i] - mean;
        energy += p.v[i] * p.v[i];
    }
    p.norm = std::sqrt(energy);
    return p;
}

// Normalized cross-correlation c[L + nb - 1] = sum_i a[i + L] * b[i] / (|a| |b|) for all lags L in
// [-(nb - 1), na - 1]. Both real signals go into one complex FFT (a + i b) to halve time and memory.
std::vector<double> crossCorrelate(const Prepared& a, const Prepared& b)
{
    const size_t na = a.v.size(), nb = b.v.size();
    const size_t n = Fft::size(na + nb);
    std::vector<double> re(n, 0.0), im(n, 0.0);
    std::copy(a.v.begin(), a.v.end(), re.begin());
    std::copy(b.v.begin(), b.v.end(), im.begin());
    Fft::transform(re, im, false);
    // A = (Z[k] + conj Z[-k]) / 2, B = (Z[k] - conj Z[-k]) / 2i, P = A conj(B); P[-k] = conj P[k] (real result)
    for (size_t k = 0; k <= n / 2; ++k) {
        const size_t m = (n - k) % n;
        const double zr = re[k], zi = im[k], wr = re[m], wi = -im[m]; // w = conj Z[-k]
        const double sr = zr + wr, si = zi + wi;                     // Z + w
        const double dr = zr - wr, di = -(zi - wi);                  // conj(Z - w)
        // P = i * (Z + w) * conj(Z - w) / 4
        const double pr = sr * dr - si * di, pi = sr * di + si * dr;
        re[k] = -pi / 4;
        im[k] = pr / 4;
        re[m] = re[k];
        im[m] = -im[k];
    }
    Fft::transform(re, im, true);
    const double scale = 1.0 / (double(n) * a.norm * b.norm);
    std::vector<double> c(na + nb - 1);
    for (long long L = -(long long)(nb - 1); L < (long long)na; ++L)
        c[size_t(L + (long long)nb - 1)] = re[size_t(L >= 0 ? L : (long long)n + L)] * scale;
    return c;
}

// Correlation -> score per lag: the sum over the overlap divided by the spread it would have between unrelated
// recordings (overlap length x energy per value of both sides in it). With the global normalization a short
// overlap inside long recordings scored low against random peaks of long overlaps; dividing by the energy alone
// (Pearson) made tiny edge overlaps score high. Lags with less than ~10 s overlap are not considered.
void toZScores(std::vector<double>& c, const Prepared& a, const Prepared& b)
{
    const long long na = (long long)a.v.size(), nb = (long long)b.v.size();
    std::vector<double> pa(size_t(na) + 1, 0.0), pb(size_t(nb) + 1, 0.0);
    for (long long i = 0; i < na; ++i) pa[size_t(i + 1)] = pa[size_t(i)] + a.v[size_t(i)] * a.v[size_t(i)];
    for (long long i = 0; i < nb; ++i) pb[size_t(i + 1)] = pb[size_t(i)] + b.v[size_t(i)] * b.v[size_t(i)];
    // Quiet stretches: at least a quarter of the average energy (one loud onset in silence is no proof)
    const double floorA = 0.25 * pa.back() / double(na), floorB = 0.25 * pb.back() / double(nb);
    const long long minOverlap = std::max(1LL, std::min<long long>(10 * kEnvelopeRate, std::min(na, nb) / 2));
    const double scale = a.norm * b.norm; // c is normalized by the whole energy
    // Overlaps shorter than half the shorter recording count as that long: coincidences in short edge overlaps of
    // sparse onsets are far more frequent than the spread says (tests: z per overlap alone found the edge)
    const double ovRef = 0.5 * double(std::min(na, nb));
    for (long long L = -(nb - 1); L < na; ++L) {
        double& v = c[size_t(L + nb - 1)];
        const long long i0 = std::max(0LL, -L), i1 = std::min(nb, na - L), ov = i1 - i0;
        if (ov < minOverlap) {
            v = 0;
            continue;
        }
        const double va = std::max(floorA, (pa[size_t(i1 + L)] - pa[size_t(i0 + L)]) / double(ov));
        const double vb = std::max(floorB, (pb[size_t(i1)] - pb[size_t(i0)]) / double(ov));
        v = v * scale / std::sqrt(va * vb * std::max(double(ov), ovRef));
    }
}

// Sub-value position of the peak at index i (parabola through the neighbors)
double refine(const std::vector<double>& c, size_t i)
{
    if (i == 0 || i + 1 >= c.size()) return double(i);
    const double y0 = c[i - 1], y1 = c[i], y2 = c[i + 1];
    const double d = y0 - 2 * y1 + y2;
    if (d >= 0) return double(i);
    return double(i) + std::clamp(0.5 * (y0 - y2) / d, -0.5, 0.5);
}

struct Peak {
    long long index = 0;
    double confidence = 0;
    int width = 1;
};

// Best peak of the correlation smoothed over `width` values (a drifting clock smears the peak over several lags) and
// its confidence against the best peak outside its neighborhood (periodic sounds give several equal peaks)
Peak bestPeak(const std::vector<double>& c, long long lo, long long hi, int width)
{
    Peak p;
    p.width = width;
    if (hi < lo) return p;
    const int h = width / 2;
    std::vector<double> prefix(c.size() + 1, 0.0);
    for (size_t i = 0; i < c.size(); ++i) prefix[i + 1] = prefix[i] + c[i];
    auto smooth = [&](long long i) {
        const long long a = std::max<long long>(0, i - h), b = std::min<long long>((long long)c.size() - 1, i + h);
        return (prefix[size_t(b + 1)] - prefix[size_t(a)]) / double(b - a + 1);
    };
    double best = -1e300;
    for (long long i = lo; i <= hi; ++i)
        if (const double v = smooth(i); v > best) {
            best = v;
            p.index = i;
        }
    const long long exclude = std::max(40, 2 * width);
    double second = 0;
    for (long long i = lo; i <= hi; ++i)
        if (std::llabs(i - p.index) > exclude) second = std::max(second, smooth(i));
    p.confidence = best > 0 ? std::clamp(1.0 - second / best, 0.0, 1.0) : 0.0;
    return p;
}

struct WindowLag {
    double center = 0; // index in b
    double lag = 0;
    double weight = 0;
};

// Lag of the strongest peak within [center - radius, center + radius] for the b values [from, to)
bool windowLag(const Prepared& a, const Prepared& b, long long from, long long to, long long center, long long radius,
               WindowLag* out)
{
    const long long na = (long long)a.v.size();
    std::vector<double> g(size_t(2 * radius + 1), 0.0);
    for (long long k = -radius; k <= radius; ++k) {
        const long long L = center + k;
        const long long i0 = std::max(from, -L), i1 = std::min(to, na - L);
        double s = 0;
        for (long long i = i0; i < i1; ++i) s += a.v[size_t(i + L)] * b.v[size_t(i)];
        g[size_t(k + radius)] = s;
    }
    const size_t best = size_t(std::max_element(g.begin(), g.end()) - g.begin());
    double mean = 0;
    for (double v : g) mean += v;
    mean /= double(g.size());
    double var = 0;
    for (double v : g) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / double(g.size()));
    if (sd <= 0) return false;
    out->center = 0.5 * double(from + to);
    out->lag = double(center - radius) + refine(g, best);
    out->weight = (g[best] - mean) / sd;
    return true;
}

} // namespace

Match match(const std::vector<float>& reference, const std::vector<float>& other, const Options& options)
{
    Match m;
    const Prepared a = prepare(reference), b = prepare(other);
    if (a.norm <= 1e-9 || b.norm <= 1e-9) return m; // silence or nothing
    const long long na = (long long)a.v.size(), nb = (long long)b.v.size();
    std::vector<double> c = crossCorrelate(a, b);
    toZScores(c, a, b);

    // Allowed lags as indices in c
    long long lo = 0, hi = (long long)c.size() - 1;
    if (options.maxOffsetS > 0) {
        const long long maxLag = std::llround(options.maxOffsetS * kEnvelopeRate);
        lo = std::max(lo, nb - 1 - maxLag);
        hi = std::min(hi, nb - 1 + maxLag);
    }
    Peak peak;
    for (int width : {1, 7, 31, 121}) {
        const Peak p = bestPeak(c, lo, hi, width);
        if (p.confidence > peak.confidence) peak = p;
    }
    m.confidence = peak.confidence;
    if (peak.confidence < options.minConfidence || peak.confidence <= 0) return m;
    const long long coarse = peak.index - (nb - 1);

    // Fine position and drift: lag in windows along the overlap, straight line through them (lag at b's start =
    // offset, slope = drift). Short overlaps or too few clear windows: the refined global peak, no drift.
    const long long from = std::max(0LL, -coarse), to = std::min(nb, na - coarse);
    const long long len = to - from;
    const long long radius = std::max(20, peak.width) + 10;
    const long long minWindow = 30 * kEnvelopeRate, maxWindow = 300 * kEnvelopeRate;
    QVector<WindowLag> lags;
    if (len >= 2 * minWindow) {
        const long long w = std::clamp(len / 6, minWindow, maxWindow);
        for (long long s = from; s + w / 2 <= to; s += w) {
            WindowLag l;
            // A window without shared sound peaks anywhere: only lags within the smear of the peak count
            if (windowLag(a, b, s, std::min(to, s + w), coarse, radius, &l) && l.weight >= 3
                && std::abs(l.lag - double(coarse)) <= peak.width / 2 + 3)
                lags << l;
        }
    }
    double offset = 0, slope = 0;
    bool fitted = false;
    for (int pass = 0; pass < 2 && lags.size() >= 2; ++pass) {
        double sw = 0, sx = 0, sy = 0;
        for (const WindowLag& l : lags) {
            sw += l.weight;
            sx += l.weight * l.center;
            sy += l.weight * l.lag;
        }
        const double mx = sx / sw, my = sy / sw;
        double sxx = 0, sxy = 0;
        for (const WindowLag& l : lags) {
            sxx += l.weight * (l.center - mx) * (l.center - mx);
            sxy += l.weight * (l.center - mx) * (l.lag - my);
        }
        const double span = lags.last().center - lags.first().center;
        slope = sxx > 0 && span >= len / 3.0 ? sxy / sxx : 0.0;
        offset = my - slope * mx;
        fitted = true;
        // Windows far off the line had a wrong peak (no shared sound in them): fit again without them
        QVector<WindowLag> keep;
        for (const WindowLag& l : lags)
            if (std::abs(l.lag - (offset + slope * l.center)) <= 3.0) keep << l;
        if (keep.size() == lags.size()) break;
        lags = keep;
    }
    if (!fitted) {
        WindowLag l;
        if (windowLag(a, b, from, to, coarse, radius, &l)) offset = l.lag;
        else offset = double(coarse);
        slope = 0;
    }
    m.found = true;
    m.offsetS = offset / kEnvelopeRate;
    m.driftMs = slope * double(len) / kEnvelopeRate * 1000.0;
    return m;
}

QVector<Match> alignAll(const QVector<std::vector<float>>& envelopes, const Options& options)
{
    QVector<Match> out(envelopes.size());
    if (envelopes.isEmpty()) return out;
    out[0].found = true;
    out[0].confidence = 1;
    QVector<int> found{0}; // in the order they were found: the chain tries the reference first
    for (int next = 0; next < found.size(); ++next) {
        const int via = found[next];
        for (int i = 1; i < envelopes.size(); ++i) {
            if (out[i].found) continue;
            Match m = match(envelopes[via], envelopes[i], options);
            if (!m.found) continue;
            m.offsetS += out[via].offsetS;
            m.via = via;
            out[i] = m;
            found << i;
        }
    }
    return out;
}

} // namespace AudioSync
