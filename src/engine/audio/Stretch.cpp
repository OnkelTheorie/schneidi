#include "engine/audio/Stretch.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace Audio {

namespace {

using Cpx = std::complex<float>;

constexpr int kOverlap = 4;     // window / hop (Hann: overlap-add of squared windows is flat at 4)
constexpr float kRise = 2.f;    // power ratio within a hop that counts a bin as rising (3 dB)
constexpr double kOnset = 0.25; // share of rising bins that makes an onset
constexpr int kPrime = 3;       // frames synthesized before the first needed one after a jump
constexpr float kTwoPi = float(2 * std::numbers::pi);

float wrap(float a) { return a - kTwoPi * std::nearbyint(a / kTwoPi); }

// Spectrum of one frame: per channel at the frame position and one hop before it, and the power summed over channels
struct Analysis {
    std::vector<Cpx> x, x2; // [channel * bins + bin]
    std::vector<float> power, power2;
    double rise = 0;        // share of bins that rose by kRise within the hop before
    double pos = 0;         // source position of the frame centre
};

} // namespace

struct Stretch::Impl {
    int ch = 2, n = 2048, hop = 512, bins = 1025;
    Source source;
    Map map;

    AVTXContext *fwd = nullptr, *inv = nullptr;
    av_tx_fn fwdFn = nullptr, invFn = nullptr;
    float* real = nullptr; // n samples (aligned for the FFT)
    Cpx* spec = nullptr;   // bins values
    std::vector<float> win;

    // Source cache [srcStart, srcEnd) per channel
    std::vector<std::vector<float>> src;
    int64_t srcStart = 0, srcEnd = 0;
    std::vector<float*> ptr;

    // prev = the frame synthesized last, cur = the one to synthesize, next = look-ahead (is cur's onset the peak?)
    Analysis prev, cur, next;
    bool havePrev = false;
    std::vector<float> theta, thetaPrev; // phase rotation per bin (synthesis - analysis), the same for all channels
    std::vector<int> peaks;

    // Overlap-add of the output samples [accStart, accStart + acc[c].size())
    std::vector<std::vector<float>> acc;
    int64_t accStart = 0;
    int64_t frame = 0;         // next frame to synthesize (centre frame * hop)
    int64_t nextT = INT64_MIN; // a read there continues
    int transients = 0;

    Impl(int channels, int rate, Source s, Map m, const Options& o)
        : ch(std::max(1, channels)), source(std::move(s)), map(std::move(m))
    {
        n = o.window;
        if (n <= 0) { // ~43 ms: 2048 at 44.1/48 kHz, 4096 at 96 kHz
            n = 256;
            while (n < rate * 0.04) n *= 2;
        }
        hop = n / kOverlap;
        bins = n / 2 + 1;
        const float one = 1.f;
        av_tx_init(&fwd, &fwdFn, AV_TX_FLOAT_RDFT, 0, n, &one, 0);
        av_tx_init(&inv, &invFn, AV_TX_FLOAT_RDFT, 1, n, &one, 0);
        real = static_cast<float*>(av_malloc(sizeof(float) * size_t(n + 2)));
        spec = static_cast<Cpx*>(av_malloc(sizeof(Cpx) * size_t(bins + 1)));
        win.resize(size_t(n));
        for (int i = 0; i < n; ++i) win[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n));
        src.assign(size_t(ch), {});
        ptr.resize(size_t(ch));
        acc.assign(size_t(ch), {});
        for (Analysis* a : {&prev, &cur, &next}) {
            a->x.resize(size_t(ch * bins));
            a->x2.resize(size_t(ch * bins));
            a->power.resize(size_t(bins));
            a->power2.resize(size_t(bins));
        }
        theta.assign(size_t(bins), 0.f);
        thetaPrev.assign(size_t(bins), 0.f);
    }

    ~Impl()
    {
        av_tx_uninit(&fwd);
        av_tx_uninit(&inv);
        av_free(real);
        av_free(spec);
    }

    // Makes the cache hold [a, b). Moving on reads only the new part (and a window ahead, fewer small reads).
    void fetch(int64_t a, int64_t b)
    {
        if (a >= srcStart && b <= srcEnd) return;
        int64_t from = a, to = b;
        if (a >= srcStart && a <= srcEnd && srcEnd > srcStart) {
            from = srcEnd;
            to = std::max(b, srcEnd + n);
            for (auto& s : src) s.erase(s.begin(), s.begin() + (a - srcStart));
            srcStart = a;
        } else {
            for (auto& s : src) s.clear();
            srcStart = a;
        }
        const size_t at = size_t(from - srcStart);
        for (int c = 0; c < ch; ++c) {
            src[size_t(c)].resize(size_t(to - srcStart));
            ptr[size_t(c)] = src[size_t(c)].data() + at;
        }
        source(from, int(to - from), ptr.data());
        srcEnd = to;
    }

    // Spectrum of channel c, window centred on source sample `centre` (zero phase: the centre is time 0)
    void spectrum(int c, int64_t centre, Cpx* out)
    {
        const float* s = src[size_t(c)].data() + (centre - n / 2 - srcStart);
        const int half = n / 2;
        for (int i = 0; i < n; ++i) real[(i + half) & (n - 1)] = s[i] * win[size_t(i)];
        fwdFn(fwd, spec, real, sizeof(float));
        std::copy(spec, spec + bins, out);
    }

    void analyze(int64_t k, Analysis& a)
    {
        a.pos = map(double(k * hop));
        const int64_t q = std::llround(a.pos);
        fetch(q - n / 2 - hop, q + n / 2);
        for (int c = 0; c < ch; ++c) {
            spectrum(c, q, a.x.data() + size_t(c * bins));
            spectrum(c, q - hop, a.x2.data() + size_t(c * bins));
        }
        float top = 0;
        for (int b = 0; b < bins; ++b) {
            float p = 0, p2 = 0;
            for (int c = 0; c < ch; ++c) {
                p += std::norm(a.x[size_t(c * bins + b)]);
                p2 += std::norm(a.x2[size_t(c * bins + b)]);
            }
            a.power[size_t(b)] = p;
            a.power2[size_t(b)] = p2;
            top = std::max(top, p);
        }
        // Onset measure: rising bins among those with sound (70 dB below the loudest, or ~-100 dBFS)
        const float floor = std::max(top * 1e-7f, 1e-10f * float(n) * float(n));
        int active = 0, rising = 0;
        for (int b = 2; b < bins; ++b) {
            const float p = a.power[size_t(b)], p2 = a.power2[size_t(b)];
            if (p <= floor && p2 <= floor) continue;
            ++active;
            if (p > kRise * p2) ++rising;
        }
        a.rise = double(rising) / std::max(active, bins / 4);
    }

    // Peaks: larger than two neighbours on each side and above the noise floor of the frame
    void findPeaks(const std::vector<float>& p)
    {
        peaks.clear();
        float top = 0;
        for (float v : p) top = std::max(top, v);
        const float floor = std::max(top * 1e-9f, 1e-14f * float(n) * float(n));
        for (int b = 2; b + 2 < bins; ++b) {
            const float v = p[size_t(b)];
            if (v > floor && v > p[size_t(b - 1)] && v >= p[size_t(b + 1)] && v > p[size_t(b - 2)] && v >= p[size_t(b + 2)])
                peaks.push_back(b);
        }
    }

    // Where the onset of cur lies in its window (samples from the centre): the slope of the phase over the rising bins
    // (a click at d turns bin k by -2 pi k d / n). The frame plays the source unstretched around its centre, so an
    // onset at d would sound at d instead of d / speed; the returned shift moves it there. Only when faster: slower,
    // the strongest onset is found close to the centre (frames lie close together in the source).
    float onsetShift() const
    {
        const double speed = (next.pos - (havePrev ? prev.pos : cur.pos)) / (havePrev ? 2.0 * hop : double(hop));
        if (speed <= 1.0) return 0.f;
        Cpx sum(0, 0);
        for (int c = 0; c < ch; ++c) {
            const Cpx* x = cur.x.data() + size_t(c * bins);
            for (int b = 1; b + 1 < bins; ++b)
                if (cur.power[size_t(b)] > kRise * cur.power2[size_t(b)] && cur.power[size_t(b + 1)] > kRise * cur.power2[size_t(b + 1)])
                    sum += x[b + 1] * std::conj(x[b]);
        }
        if (std::abs(sum) <= 0) return 0.f;
        const double d = -std::arg(sum) * n / (2 * std::numbers::pi);
        return float(std::clamp(d / speed - d, -double(hop), double(hop)));
    }

    // Frame `frame` from cur into the overlap-add
    void synthesize()
    {
        const bool onset = havePrev && cur.rise >= kOnset && cur.rise >= prev.rise && cur.rise > next.rise;
        if (onset) ++transients;
        findPeaks(cur.power);
        if (peaks.empty()) std::fill(theta.begin(), theta.end(), 0.f);
        const float shift = onset ? onsetShift() : 0.f;
        for (size_t i = 0; i < peaks.size(); ++i) {
            const int p = peaks[i];
            // Region of the peak: up to the lowest bin between it and its neighbours
            int lo = 0, hi = bins;
            if (i > 0) lo = int(std::min_element(cur.power.begin() + peaks[i - 1], cur.power.begin() + p) - cur.power.begin()) + 1;
            if (i + 1 < peaks.size())
                hi = int(std::min_element(cur.power.begin() + p, cur.power.begin() + peaks[i + 1]) - cur.power.begin()) + 1;
            float th = 0;
            if (onset && cur.power[size_t(p)] > kRise * cur.power2[size_t(p)]) {
                // Reset: the analysis phases, moved by the shift that puts the onset where the map wants it
                for (int b = lo; b < hi; ++b) theta[size_t(b)] = wrap(-kTwoPi * float(b) / float(n) * shift);
                continue;
            }
            if (havePrev) {
                int c0 = 0; // the channel loudest at the peak carries the phase
                for (int c = 1; c < ch; ++c)
                    if (std::norm(cur.x[size_t(c * bins + p)]) > std::norm(cur.x[size_t(c0 * bins + p)])) c0 = c;
                const size_t at = size_t(c0 * bins + p);
                const float phase = std::arg(cur.x[at]);
                const float centre = kTwoPi * float(p) / float(n);
                const float omega = centre + wrap(phase - std::arg(cur.x2[at]) - centre * float(hop)) / float(hop);
                th = wrap(thetaPrev[size_t(p)] + omega * float(hop) - (phase - std::arg(prev.x[at])));
            }
            std::fill(theta.begin() + lo, theta.begin() + hi, th);
        }
        theta[0] = theta[size_t(bins - 1)] = 0; // real bins stay real

        const int64_t start = frame * hop - n / 2;
        const size_t off = size_t(start - accStart);
        const float gain = 2.f / (3.f * float(n)); // inverse FFT unscaled, sum of squared Hann at 4x overlap = 1.5
        const int half = n / 2;
        for (int c = 0; c < ch; ++c) {
            const Cpx* x = cur.x.data() + size_t(c * bins);
            float lastTh = 1e9f;
            Cpx rot(1, 0);
            for (int b = 0; b < bins; ++b) {
                if (theta[size_t(b)] != lastTh) {
                    lastTh = theta[size_t(b)];
                    rot = Cpx(std::cos(lastTh), std::sin(lastTh));
                }
                spec[b] = x[b] * rot;
            }
            invFn(inv, real, spec, sizeof(Cpx));
            std::vector<float>& a = acc[size_t(c)];
            if (a.size() < off + size_t(n)) a.resize(off + size_t(n), 0.f);
            float* dst = a.data() + off;
            for (int i = 0; i < n; ++i) dst[i] += real[(i + half) & (n - 1)] * win[size_t(i)] * gain;
        }
        theta.swap(thetaPrev);
        std::swap(prev, cur);
        std::swap(cur, next);
        havePrev = true;
        ++frame;
        analyze(frame + 1, next);
    }

    void restart(int64_t t)
    {
        // First frame that reaches t: frame * hop + n / 2 > t
        const int64_t first = int64_t(std::floor(double(t - n / 2) / hop)) + 1;
        frame = first - kPrime;
        havePrev = false;
        std::fill(thetaPrev.begin(), thetaPrev.end(), 0.f);
        for (auto& a : acc) a.clear();
        accStart = frame * hop - n / 2;
        analyze(frame, cur);
        analyze(frame + 1, next);
    }

    void read(int64_t t, int count, float* const* out)
    {
        if (t != nextT) restart(t);
        nextT = t + count;
        while (frame * hop - n / 2 < t + count) synthesize();
        for (int c = 0; c < ch; ++c) {
            std::vector<float>& a = acc[size_t(c)];
            const size_t from = size_t(t - accStart);
            if (a.size() < from + size_t(count)) a.resize(from + size_t(count), 0.f);
            std::copy(a.begin() + from, a.begin() + from + count, out[c]);
            a.erase(a.begin(), a.begin() + from + count);
        }
        accStart = t + count;
    }
};

Stretch::Stretch(int channels, int sampleRate, Source source, Map map, const Options& options)
    : d(std::make_unique<Impl>(channels, sampleRate, std::move(source), std::move(map), options))
{
}

Stretch::~Stretch() = default;

int Stretch::channels() const { return d->ch; }
int Stretch::window() const { return d->n; }
int Stretch::hop() const { return d->hop; }
int Stretch::transients() const { return d->transients; }

void Stretch::read(int64_t t, int count, float* const* out)
{
    if (count > 0) d->read(t, count, out);
}

} // namespace Audio
