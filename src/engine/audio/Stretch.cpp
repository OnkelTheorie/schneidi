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
constexpr float kRise = 2.f;    // power ratio that counts a bin as rising within a frame's hop (3 dB)
constexpr float kDetRise = 4.f; // the same for the onset detector, against the loudest of the 3 windows before (6 dB)
constexpr double kOnset = 0.25; // share of rising bins that makes an onset
constexpr int kDetSpan = 4;     // an onset is the largest rise within this many grid points on each side
constexpr int kPrime = 3;       // frames synthesized before the first needed one after a jump
constexpr float kTwoPi = float(2 * std::numbers::pi);
constexpr int64_t kNoCut = INT64_MAX;

float wrap(float a) { return a - kTwoPi * std::nearbyint(a / kTwoPi); }

// Spectrum of one frame: per channel at the frame position and one hop before it, and the power summed over channels
struct Analysis {
    std::vector<Cpx> x, x2; // [channel * bins + bin]
    std::vector<float> power, power2;
    int64_t centre = 0;     // source sample the window is centred on
    int64_t cut = kNoCut;   // the window ends before this source sample (an onset ahead of the centre)
    bool onset = false;     // the first frame with an onset at or behind its centre: phases start over
    double onsetAt = 0;     // there: the onset relative to the centre (<= 0)
    double speed = 1;       // source samples per output sample around the frame
};

struct Onset {
    int64_t at;
    float strength;
};

class Fft {
public:
    explicit Fft(int n) : m_n(n)
    {
        const float one = 1.f;
        av_tx_init(&m_fwd, &m_fwdFn, AV_TX_FLOAT_RDFT, 0, n, &one, 0);
        av_tx_init(&m_inv, &m_invFn, AV_TX_FLOAT_RDFT, 1, n, &one, 0);
        m_real = static_cast<float*>(av_malloc(sizeof(float) * size_t(n + 2)));
        m_spec = static_cast<Cpx*>(av_malloc(sizeof(Cpx) * size_t(n / 2 + 2)));
    }
    ~Fft()
    {
        av_tx_uninit(&m_fwd);
        av_tx_uninit(&m_inv);
        av_free(m_real);
        av_free(m_spec);
    }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;

    float* real() { return m_real; }
    Cpx* spec() { return m_spec; }
    void forward() { m_fwdFn(m_fwd, m_spec, m_real, sizeof(float)); }
    void inverse() { m_invFn(m_inv, m_real, m_spec, sizeof(Cpx)); }

private:
    int m_n;
    AVTXContext *m_fwd = nullptr, *m_inv = nullptr;
    av_tx_fn m_fwdFn = nullptr, m_invFn = nullptr;
    float* m_real = nullptr;
    Cpx* m_spec = nullptr;
};

std::vector<float> hann(int n)
{
    std::vector<float> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / n));
    return w;
}

// Share of the bins with sound (70 dB below the loudest, or ~-100 dBFS) whose power rose by `ratio` from `before`
double rise(const std::vector<float>& now, const std::vector<float>& before, int n, float ratio)
{
    float top = 0;
    for (float v : now) top = std::max(top, v);
    const float floor = std::max(top * 1e-7f, 1e-10f * float(n) * float(n));
    int active = 0, rising = 0;
    const int bins = int(now.size());
    for (int b = 2; b < bins; ++b) {
        const float p = now[size_t(b)], p2 = before[size_t(b)];
        if (p <= floor && p2 <= floor) continue;
        ++active;
        if (p > ratio * p2) ++rising;
    }
    return double(rising) / std::max(active, bins / 4);
}

} // namespace

struct Stretch::Impl {
    int ch = 2, n = 2048, hop = 512, bins = 1025;
    Source source;
    Map map;

    std::unique_ptr<Fft> fft;
    std::vector<float> win;

    // Source cache [srcStart, srcEnd) per channel
    std::vector<std::vector<float>> src;
    int64_t srcStart = 0, srcEnd = 0;
    std::vector<float*> ptr;

    // Onsets in the source, on a fixed grid (so they are the same however the source is reached): a short window
    // (n / 4) every grid step (hop / 4), compared with the loudest of the windows 2-4 steps before (from half a short
    // window on: the comparison peaks with the onset at the centre; the maximum keeps noise and the single pulses of
    // a voice from counting). An onset is the largest share of rising bins within kDetSpan steps, above kOnset.
    int grid = 128, det = 512;
    std::unique_ptr<Fft> detFft;
    std::vector<float> detWin;
    int64_t detNext = 0, detFrom = 0; // next grid index to measure, first one measured
    bool detEmpty = true;
    std::vector<std::vector<float>> detPower; // ring of the last 5 grid points' power
    std::vector<float> detBefore;
    std::vector<double> detRise;              // ring of the last 2 * kDetSpan + 1 rises
    int detValid = 0;                         // grid points measured since the start
    std::vector<Onset> onsets;                // sorted, from detFrom on

    // prev = the frame synthesized last, cur = the one to synthesize, next = look-ahead
    Analysis prev, cur, next;
    bool havePrev = false;
    std::vector<float> theta, thetaPrev; // phase rotation per bin (synthesis - analysis), the same for all channels
    std::vector<int> peaks;
    std::vector<float> amp; // gain per bin (reset regions of an onset frame)

    // Overlap-add of the output samples [accStart, accStart + accWin.size()): per channel the frames, and the sum of
    // analysis x synthesis window (flat 1.5 at 4x overlap; less where a window was cut before an onset)
    std::vector<std::vector<float>> acc;
    std::vector<float> accWin;
    int64_t accStart = 0;
    int64_t frame = 0;         // next frame to synthesize (centre frame * hop)
    int64_t nextT = INT64_MIN; // a read there continues
    int transients = 0;

    Impl(int channels, int rate, Source s, Map m, const Options& o)
        : ch(std::max(1, channels)), source(std::move(s)), map(std::move(m))
    {
        n = o.window;
        if (n <= 0) { // ~85 ms: 4096 at 44.1/48 kHz (bench_stretch: clearer bass and chords than 2048, speech alike)
            n = 256;
            while (n < rate * 0.07) n *= 2;
        }
        hop = n / kOverlap;
        bins = n / 2 + 1;
        fft = std::make_unique<Fft>(n);
        win = hann(n);
        det = 128; // ~11 ms whatever the frame window: 512 at 44.1/48 kHz
        while (det < rate * 0.01) det *= 2;
        grid = det / 4;
        detFft = std::make_unique<Fft>(det);
        detWin = hann(det);
        detPower.assign(5, std::vector<float>(size_t(det / 2 + 1), 0.f));
        detBefore.resize(size_t(det / 2 + 1));
        detRise.assign(2 * kDetSpan + 1, 0.0);
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
        amp.assign(size_t(bins), 1.f);
        thetaPrev.assign(size_t(bins), 0.f);
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

    // ---- Onsets ----

    // Power (summed over channels) of the short window centred on grid point g
    void detPowerAt(int64_t g, std::vector<float>& out)
    {
        const int64_t centre = g * grid;
        std::fill(out.begin(), out.end(), 0.f);
        float* real = detFft->real();
        const Cpx* spec = detFft->spec();
        for (int c = 0; c < ch; ++c) {
            const float* s = src[size_t(c)].data() + (centre - det / 2 - srcStart);
            for (int i = 0; i < det; ++i) real[i] = s[i] * detWin[size_t(i)];
            detFft->forward();
            for (size_t b = 0; b < out.size(); ++b) out[b] += std::norm(spec[b]);
        }
    }

    static size_t slot(int64_t g, int ring) { return size_t(((g % ring) + ring) % ring); }

    void detRestart(int64_t g)
    {
        detEmpty = false;
        detNext = detFrom = g;
        detValid = 0;
        onsets.clear();
    }

    // Measures the grid up to source sample `to` (the source must be in the cache from detNext's window on); onsets
    // are known up to kDetSpan steps before it
    void detect(int64_t to)
    {
        constexpr int span = 2 * kDetSpan + 1;
        for (; detNext * grid <= to; ++detNext) {
            std::vector<float>& p = detPower[slot(detNext, 5)];
            detPowerAt(detNext, p);
            ++detValid;
            double r = 0;
            if (detValid >= 5) {
                const std::vector<float>&a = detPower[slot(detNext - 2, 5)], &b = detPower[slot(detNext - 3, 5)],
                                         &c = detPower[slot(detNext - 4, 5)];
                for (size_t i = 0; i < detBefore.size(); ++i) detBefore[i] = std::max({a[i], b[i], c[i]});
                r = rise(p, detBefore, det, kDetRise);
            }
            detRise[slot(detNext, span)] = r;
            if (detValid < 4 + span) continue;
            // The point kDetSpan steps back: the largest within the span (ties: the first)
            const int64_t g = detNext - kDetSpan;
            const double v = detRise[slot(g, span)];
            if (v < kOnset) continue;
            bool top = true;
            for (int64_t j = g - kDetSpan; j <= detNext && top; ++j)
                if (j != g) top = j < g ? detRise[slot(j, span)] < v : detRise[slot(j, span)] <= v;
            if (top) onsets.push_back({locate(g * grid), float(v)});
        }
    }

    // The grid point is up to a short window off (the share of rising bins saturates with a loud onset anywhere in
    // the window's first half): the onset is where the level rises most against the 128 samples before, in steps of
    // 16 samples around it
    int64_t locate(int64_t at)
    {
        constexpr int step = 16, before = 8;
        const int64_t from = at - det / 2 - before * step, to = at + det / 2 + det / 4;
        const int blocks = int((to - from) / step);
        std::vector<double> e(size_t(blocks), 0.0);
        for (int c = 0; c < ch; ++c) {
            const float* s = src[size_t(c)].data() + (from - srcStart);
            for (int j = 0; j < blocks; ++j)
                for (int i = 0; i < step; ++i) e[size_t(j)] += double(s[j * step + i]) * s[j * step + i];
        }
        const double top = *std::max_element(e.begin(), e.end());
        double best = 0;
        int bestJ = -1;
        for (int j = before; j < blocks; ++j) {
            double past = 0;
            for (int i = j - before; i < j; ++i) past += e[size_t(i)];
            const double r = e[size_t(j)] / (past / before + 1e-3 * top + 1e-12);
            if (r > best) {
                best = r;
                bestJ = j;
            }
        }
        return bestJ < 0 ? at : from + int64_t(bestJ) * step;
    }

    // ---- Frames ----

    // Spectrum of channel c, window centred on source sample `centre` (zero phase: the centre is time 0), ending
    // with a short fade before `cut`
    void spectrum(int c, int64_t centre, int64_t cut, Cpx* out)
    {
        const float* s = src[size_t(c)].data() + (centre - n / 2 - srcStart);
        const int half = n / 2;
        float* real = fft->real();
        for (int i = 0; i < n; ++i) real[(i + half) & (n - 1)] = s[i] * win[size_t(i)] * cutGain(centre - half + i, cut);
        fft->forward();
        std::copy(fft->spec(), fft->spec() + bins, out);
    }

    // The onset estimate lies within half a grid step of the real one: the cut fades out over a grid step and ends
    // half a step before it
    float cutGain(int64_t x, int64_t cut) const
    {
        const int64_t end = cut - grid / 2, start = end - grid;
        if (x < start) return 1.f;
        if (x >= end) return 0.f;
        return float(0.5 + 0.5 * std::cos(std::numbers::pi * double(x - start) / grid));
    }

    void analyze(int64_t k, Analysis& a)
    {
        const double p = map(double(k * hop)), before = map(double((k - 1) * hop));
        const int64_t q = std::llround(p);
        // Onsets that concern the frame: between the frame before and this one (a reset is due), and ahead of the
        // centre within the window (cut). Far apart (fast), only the stretch within a window before.
        const int64_t from = std::max<int64_t>(int64_t(std::floor(before)), q - n / 2) + 1;
        const int64_t to = q + n / 2 + hop + grid; // a reset frame can sit up to a hop later
        if (detEmpty || from < detFrom * grid || from > detNext * grid + n)
            detRestart(int64_t(std::floor(double(from) / grid)) - (kDetSpan + 6)); // onsets from from / grid - 1 on
        const int64_t detLo = (detNext - kDetSpan) * grid - det / 2 - 8 * 16; // also what locate() reads
        fetch(std::min(detLo, q - n / 2 - hop), to + (kDetSpan + 1) * grid + det / 2 + 1);
        detect(to + (kDetSpan + 1) * grid);
        while (onsets.size() > 64 && onsets.front().at < from - n) onsets.erase(onsets.begin());

        // The strongest onset in (before, p]: this frame starts its phases over there. It is read so that the onset
        // lands where the map puts it: at d / speed from the centre, if it lies d from p in the source.
        a.onset = false;
        a.centre = q;
        const Onset* best = nullptr;
        for (const Onset& o : onsets)
            if (o.at >= from && double(o.at) <= p && (!best || o.strength > best->strength)) best = &o;
        if (best) {
            a.onset = true;
            a.speed = std::max(1e-6, (p - before) / hop);
            a.centre = std::llround(double(best->at) - (double(best->at) - p) / a.speed);
            a.onsetAt = double(best->at - a.centre);
        }
        // The first onset ahead of the centre ends the window before it (no pre-echo of what comes)
        a.cut = kNoCut;
        for (const Onset& o : onsets)
            if (o.at > a.centre && o.at < a.centre + n / 2) a.cut = std::min(a.cut, o.at);
        // One hop before: for a reset frame without the onset (which bins rose), otherwise like the frame
        const int64_t cut2 = a.onset ? std::min(a.cut, best->at) : a.cut;
        for (int c = 0; c < ch; ++c) {
            spectrum(c, a.centre, a.cut, a.x.data() + size_t(c * bins));
            spectrum(c, a.centre - hop, cut2, a.x2.data() + size_t(c * bins));
        }
        for (int b = 0; b < bins; ++b) {
            float pw = 0, pw2 = 0;
            for (int c = 0; c < ch; ++c) {
                pw += std::norm(a.x[size_t(c * bins + b)]);
                pw2 += std::norm(a.x2[size_t(c * bins + b)]);
            }
            a.power[size_t(b)] = pw;
            a.power2[size_t(b)] = pw2;
        }
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

    // Gain for the onset in a reset frame. The frames after it carry the onset on (their phases follow), each with
    // its analysis window where the onset lies in it times its synthesis window there; the overlap-add expects the
    // sum of squared windows. Faster than 1x the onset lies in fewer frames (further apart in the source) and would
    // come out up to 3.5 dB too soft: the reset frame makes up the difference (never quieter: slower it adds up).
    float onsetGain(const Analysis& a) const
    {
        auto w = [this](double x) {
            return std::abs(x) >= n / 2 ? 0.0 : std::pow(std::cos(std::numbers::pi * x / n), 2);
        };
        const double e = a.onsetAt, ha = a.speed * hop;
        double want = 0, tail = 0;
        for (int j = 0; j < kOverlap; ++j) {
            want += w(e - j * hop) * w(e - j * hop);
            if (j > 0) tail += w(e - j * ha) * w(e - j * hop);
        }
        const double self = w(e) * w(e);
        if (self < 0.1) return 1.f;
        return float(std::clamp((want - tail) / self, 1.0, 2.0));
    }

    // Frame `frame` from cur into the overlap-add
    void synthesize()
    {
        const bool onset = havePrev && cur.onset;
        if (onset) ++transients;
        const float boost = onset ? onsetGain(cur) : 1.f;
        findPeaks(cur.power);
        if (peaks.empty()) {
            std::fill(theta.begin(), theta.end(), 0.f);
            std::fill(amp.begin(), amp.end(), 1.f);
        }
        for (size_t i = 0; i < peaks.size(); ++i) {
            const int p = peaks[i];
            // Region of the peak: up to the lowest bin between it and its neighbours
            int lo = 0, hi = bins;
            if (i > 0) lo = int(std::min_element(cur.power.begin() + peaks[i - 1], cur.power.begin() + p) - cur.power.begin()) + 1;
            if (i + 1 < peaks.size())
                hi = int(std::min_element(cur.power.begin() + p, cur.power.begin() + peaks[i + 1]) - cur.power.begin()) + 1;
            float th = 0; // reset: the analysis phases (the regions the onset brought)
            const bool reset = onset && cur.power[size_t(p)] > kRise * cur.power2[size_t(p)];
            std::fill(amp.begin() + lo, amp.begin() + hi, reset ? boost : 1.f);
            if (havePrev && !reset) {
                int c0 = 0; // the channel loudest at the peak carries the phase
                for (int c = 1; c < ch; ++c)
                    if (std::norm(cur.x[size_t(c * bins + p)]) > std::norm(cur.x[size_t(c0 * bins + p)])) c0 = c;
                const size_t at = size_t(c0 * bins + p);
                const float phase = std::arg(cur.x[at]);
                const float centre = kTwoPi * float(p) / float(n);
                const float omega = centre + wrap(phase - std::arg(cur.x2[at]) - centre * float(hop)) / float(hop);
                // (each frame's analysis phase refers to its own centre, which lands on its output centre: the
                // synthesis phase advances by omega * hop whatever the distance of the centres in the source)
                th = wrap(thetaPrev[size_t(p)] + omega * float(hop) - (phase - std::arg(prev.x[at])));
            }
            std::fill(theta.begin() + lo, theta.begin() + hi, th);
        }
        theta[0] = theta[size_t(bins - 1)] = 0; // real bins stay real

        const int64_t start = frame * hop - n / 2;
        const size_t off = size_t(start - accStart);
        const int half = n / 2;
        if (accWin.size() < off + size_t(n)) accWin.resize(off + size_t(n), 0.f);
        for (int i = 0; i < n; ++i) accWin[off + size_t(i)] += win[size_t(i)] * win[size_t(i)] * cutGain(cur.centre - half + i, cur.cut);
        const float gain = 1.f / float(n); // inverse FFT unscaled
        for (int c = 0; c < ch; ++c) {
            const Cpx* x = cur.x.data() + size_t(c * bins);
            Cpx* spec = fft->spec();
            float lastTh = 1e9f;
            Cpx rot(1, 0);
            for (int b = 0; b < bins; ++b) {
                if (theta[size_t(b)] != lastTh) {
                    lastTh = theta[size_t(b)];
                    rot = Cpx(std::cos(lastTh), std::sin(lastTh));
                }
                spec[b] = x[b] * rot * amp[size_t(b)];
            }
            fft->inverse();
            const float* real = fft->real();
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
        accWin.clear();
        accStart = frame * hop - n / 2;
        detEmpty = true;
        analyze(frame, cur);
        analyze(frame + 1, next);
    }

    void read(int64_t t, int count, float* const* out)
    {
        if (t != nextT) restart(t);
        nextT = t + count;
        while (frame * hop - n / 2 < t + count) synthesize();
        const size_t from = size_t(t - accStart);
        if (accWin.size() < from + size_t(count)) accWin.resize(from + size_t(count), 0.f);
        for (int c = 0; c < ch; ++c) {
            std::vector<float>& a = acc[size_t(c)];
            if (a.size() < from + size_t(count)) a.resize(from + size_t(count), 0.f);
            for (int i = 0; i < count; ++i) out[c][i] = a[from + size_t(i)] / std::max(accWin[from + size_t(i)], 0.1f);
            a.erase(a.begin(), a.begin() + from + count);
        }
        accWin.erase(accWin.begin(), accWin.begin() + from + count);
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
