// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/audio/Retimer.h"

#include "core/Retime.h"
#include "engine/audio/Reader.h"
#include "engine/audio/Stretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Audio {

namespace {

// Windowed sinc: half width in zero crossings, Kaiser window; table of the prototype kernel with linear
// interpolation between its points. Stop band ~ -100 dB.
constexpr int kZeros = 32;
constexpr int kTableRes = 512; // points per zero crossing
constexpr double kBeta = 10.0;
constexpr double kCutoff = 0.97; // of the source's Nyquist frequency (headroom for the transition band)

double besselI0(double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

const std::vector<float>& kernel()
{
    static const std::vector<float> table = [] {
        std::vector<float> t(size_t(kZeros) * kTableRes + 2, 0.f);
        const double norm = besselI0(kBeta);
        for (size_t i = 0; i <= size_t(kZeros) * kTableRes; ++i) {
            const double x = double(i) / kTableRes;
            const double r = x / kZeros;
            const double w = besselI0(kBeta * std::sqrt(std::max(0.0, 1 - r * r))) / norm;
            const double s = x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            t[i] = float(s * w);
        }
        return t;
    }();
    return table;
}

} // namespace

struct Retimer::Impl {
    std::unique_ptr<Reader> reader;
    Clip clip;
    RetimeMap map;
    double spf = 1920;        // output samples per frame
    int64_t fileSamples = 0;  // reverse: playback index k = file sample fileSamples - 1 - k
    int ch = 2;
    bool reverse = false;
    bool pitch = false;       // own stretcher (Stretch)
    bool plainReverse = false; // reverse at 100 %: copy

    // Playback-direction source (y[k]): window read from the reader
    std::vector<std::vector<float>> window;
    int64_t windowStart = 0, windowEnd = 0;

    std::unique_ptr<Stretch> stretch;

    Impl(std::unique_ptr<Reader> r, const Clip& c, int fileFrames, double samplesPerFrame)
        : reader(std::move(r)), clip(c), map(c, fileFrames), spf(samplesPerFrame)
    {
        ch = reader->channels();
        reverse = c.reverse;
        fileSamples = std::llround(fileFrames * spf);
        const bool constant = !c.hasRamp();
        pitch = c.keepPitch && !(constant && c.speed == 1.0);
        plainReverse = reverse && constant && c.speed == 1.0;
        if (pitch)
            stretch = std::make_unique<Stretch>(
                ch, reader->rate(), [this](int64_t from, int n, float* const* out) { readSource(from, n, out); },
                [this](double mu) { return position(mu); });
    }

    // Position in playback-direction source samples of material sample mu, and the speed there
    double position(double mu) const { return map.sourceAt(mu / spf) * spf; }
    double speed(double mu) const { return std::max(1e-3, map.speedAt(mu / spf)); }

    // y[from, to) into the window (per channel)
    void load(int64_t from, int64_t to)
    {
        const int n = int(to - from);
        window.assign(size_t(ch), std::vector<float>(size_t(std::max(0, n)), 0.f));
        windowStart = from;
        windowEnd = to;
        if (n <= 0) return;
        std::vector<float*> out(static_cast<size_t>(ch));
        for (int c = 0; c < ch; ++c) out[size_t(c)] = window[size_t(c)].data();
        readSource(from, n, out.data());
    }

    // y[from, from + n) into out
    void readSource(int64_t from, int n, float* const* out)
    {
        if (!reverse) {
            reader->read(from, n, out);
            return;
        }
        // y[k] = x[fileSamples - 1 - k]: read x[fileSamples - from - n, fileSamples - from) and turn it around
        reader->read(fileSamples - from - n, n, out);
        for (int c = 0; c < ch; ++c) std::reverse(out[c], out[c] + n);
    }

    void readCopy(int64_t mu, int count, float* const* out)
    {
        const int64_t from = std::llround(position(double(mu)));
        load(from, from + count);
        for (int c = 0; c < ch; ++c) std::copy(window[size_t(c)].begin(), window[size_t(c)].end(), out[c]);
    }

    void readInterpolated(int64_t mu, int count, float* const* out)
    {
        const std::vector<float>& k = kernel();
        std::vector<double> pos(static_cast<size_t>(count)), cut(static_cast<size_t>(count));
        double lo = 1e300, hi = -1e300, minCut = 1;
        for (int i = 0; i < count; ++i) {
            const double m = double(mu + i);
            pos[size_t(i)] = position(m);
            cut[size_t(i)] = std::min(1.0, 1.0 / speed(m)) * kCutoff;
            lo = std::min(lo, pos[size_t(i)]);
            hi = std::max(hi, pos[size_t(i)]);
            minCut = std::min(minCut, cut[size_t(i)]);
        }
        const int64_t reach = int64_t(std::ceil(kZeros / minCut)) + 1;
        load(int64_t(std::floor(lo)) - reach, int64_t(std::ceil(hi)) + reach + 1);
        for (int i = 0; i < count; ++i) {
            const double p = pos[size_t(i)], c = cut[size_t(i)];
            const double half = kZeros / c;
            const int64_t a = std::max(windowStart, int64_t(std::ceil(p - half)));
            const int64_t b = std::min(windowEnd - 1, int64_t(std::floor(p + half)));
            double acc[8] = {};
            for (int64_t j = a; j <= b; ++j) {
                const double x = std::abs(p - double(j)) * c * kTableRes;
                const size_t xi = size_t(x);
                if (xi >= size_t(kZeros) * kTableRes) continue;
                const double f = x - double(xi);
                const double h = c * (k[xi] + (k[xi + 1] - k[xi]) * f);
                for (int cc = 0; cc < ch && cc < 8; ++cc) acc[cc] += h * window[size_t(cc)][size_t(j - windowStart)];
            }
            for (int cc = 0; cc < ch; ++cc) out[cc][i] = float(cc < 8 ? acc[cc] : 0.0);
        }
    }
};

Retimer::Retimer(std::unique_ptr<Reader> reader, const Clip& clip, int fileFrames, double samplesPerFrame)
    : d(std::make_unique<Impl>(std::move(reader), clip, fileFrames, samplesPerFrame))
{
}

Retimer::~Retimer() = default;

int Retimer::channels() const { return d->ch; }

void Retimer::read(int64_t mu, int count, float* const* out)
{
    if (count <= 0) return;
    if (d->plainReverse) d->readCopy(mu, count, out);
    else if (d->pitch) d->stretch->read(mu, count, out);
    else d->readInterpolated(mu, count, out);
}

} // namespace Audio
