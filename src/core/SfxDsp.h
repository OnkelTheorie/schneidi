// SPDX-License-Identifier: AGPL-3.0-only AND MIT
#pragma once
// Signal helpers of the ported earshot sounds (core/SfxWhoosh, core/SfxForge): noise, Butterworth and state variable
// filters, smoothing, coloured noise, shelves, a small room, the edit. Header-only, internal to those two files.
//
// Ported from earshot (https://github.com/Leonxlnx/earshot): earshot/forge/recipes/whoosh.py (`render`, `envelope`,
// `_edit`, `shelf`, helpers), earshot/forge/dsp.py (`svf`) and the parameter sets of earshot/forge/bank/forge_v2.json
// (swipe/*, pro_whoosh_big/*). Changes: C++, mono, own deterministic noise, a simpler noise-tail room instead of
// fx.reverb's FDN, tape saturation without oversampling and emphasis. The ported code is under the MIT licence:
//
// MIT License
//
// Copyright (c) 2026 Leon Lin
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <vector>

namespace SfxDsp {

constexpr double SR = 48000;
constexpr double kPi = std::numbers::pi;
using Vec = std::vector<double>;

// Deterministic noise: splitmix64, Gaussian by Box-Muller
class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull) {}
    double unit()
    {
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return double((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);
    }
    double normal()
    {
        if (m_has) {
            m_has = false;
            return m_spare;
        }
        const double u = std::max(1e-300, unit()), v = unit();
        const double r = std::sqrt(-2 * std::log(u));
        m_spare = r * std::sin(2 * kPi * v);
        m_has = true;
        return r * std::cos(2 * kPi * v);
    }
    Vec normals(size_t n)
    {
        Vec x(n);
        for (double& v : x) v = normal();
        return x;
    }

private:
    std::uint64_t m_state;
    bool m_has = false;
    double m_spare = 0;
};

inline size_t nOf(double s) { return size_t(std::max(1L, std::lround(s * SR))); }

inline double stdOf(const Vec& x)
{
    double m = 0, q = 0;
    for (double v : x) m += v;
    m /= double(x.size());
    for (double v : x) q += (v - m) * (v - m);
    return std::sqrt(q / double(x.size()));
}

inline double rmsOf(const Vec& x)
{
    double q = 0;
    for (double v : x) q += v * v;
    return std::sqrt(q / double(std::max<size_t>(1, x.size())));
}

// Linear convolution by FFT, full length a + b - 1
inline Vec convolve(const Vec& a, const Vec& b)
{
    const size_t n = a.size() + b.size() - 1, m = Fft::size(n);
    std::vector<double> ar(m, 0), ai(m, 0), br(m, 0), bi(m, 0);
    std::copy(a.begin(), a.end(), ar.begin());
    std::copy(b.begin(), b.end(), br.begin());
    Fft::transform(ar, ai, false);
    Fft::transform(br, bi, false);
    for (size_t i = 0; i < m; ++i) {
        const double re = ar[i] * br[i] - ai[i] * bi[i], im = ar[i] * bi[i] + ai[i] * br[i];
        ar[i] = re;
        ai[i] = im;
    }
    Fft::transform(ar, ai, true);
    Vec out(n);
    for (size_t i = 0; i < n; ++i) out[i] = ar[i] / double(m);
    return out;
}

// Biquad (direct form I)
struct Biquad {
    double b0, b1, b2, a1, a2;
};

inline void filter(Vec& x, const Biquad& q)
{
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (double& v : x) {
        const double y = q.b0 * v + q.b1 * x1 + q.b2 * x2 - q.a1 * y1 - q.a2 * y2;
        x2 = x1;
        x1 = v;
        y2 = y1;
        y1 = y;
        v = y;
    }
}

inline Biquad passBiquad(bool high, double f, double q)
{
    const double w0 = 2 * kPi * std::clamp(f, 10.0, SR / 2 * 0.95) / SR, c = std::cos(w0), al = std::sin(w0) / (2 * q);
    const double a0 = 1 + al;
    if (high) return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
    return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
}

// Butterworth low/high pass of order 1, 2 or 4 (cascaded biquads with the Butterworth Qs, as scipy's sos)
inline Vec butter(Vec x, bool high, double f, int order)
{
    if (order == 1) {
        const double k = std::tan(kPi * std::clamp(f, 10.0, SR / 2 * 0.95) / SR);
        const double b0 = high ? 1 / (1 + k) : k / (1 + k), b1 = high ? -b0 : b0, a1 = (k - 1) / (k + 1);
        double x1 = 0, y1 = 0;
        for (double& v : x) {
            const double y = b0 * v + b1 * x1 - a1 * y1;
            x1 = v;
            y1 = y;
            v = y;
        }
        return x;
    }
    if (order == 2) {
        filter(x, passBiquad(high, f, std::numbers::sqrt2 / 2));
        return x;
    }
    filter(x, passBiquad(high, f, 0.541196));
    filter(x, passBiquad(high, f, 1.306563));
    return x;
}

// dsp.svf: the time-varying state variable filter (Cytomic TPT); mode 0 low, 2 high, 3 band normalised to 1
inline Vec svf(const Vec& x, const Vec& fc, double q, int mode, double lo = 20, double hi = SR * 0.45)
{
    Vec y(x.size());
    double ic1 = 0, ic2 = 0;
    const double k = 1.0 / std::max(q, 0.05);
    for (size_t i = 0; i < x.size(); ++i) {
        const double g = std::tan(kPi * std::clamp(fc[i], std::max(20.0, lo), std::min(SR * 0.45, hi)) / SR);
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x[i] - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        y[i] = mode == 0 ? v2 : mode == 3 ? k * v1 : x[i] - k * v1 - v2;
    }
    return y;
}


// Peak of a 10 ms RMS envelope (a layer's level for the mix)
inline double lvl(const Vec& x)
{
    const size_t h = nOf(0.01), k = x.size() / h;
    if (k == 0) return rmsOf(x) + 1e-12;
    double best = 0;
    for (size_t b = 0; b < k; ++b) {
        double q = 0;
        for (size_t i = b * h; i < (b + 1) * h; ++i) q += x[i] * x[i];
        best = std::max(best, std::sqrt(q / double(h)));
    }
    return best + 1e-12;
}

// Unit-std random control signal band-limited to fc
inline Vec lfnoise(size_t n, double fc, Rng& r)
{
    Vec w = butter(r.normals(n), false, fc, 2);
    const double s = stdOf(w) + 1e-12;
    for (double& v : w) v /= s;
    return w;
}

// Moving Hann smoothing (zero phase, edges held)
inline Vec smooth(const Vec& x, double sec)
{
    const size_t k = std::max<size_t>(3, size_t(sec * SR) | 1), h = k / 2;
    Vec w(k);
    double sum = 0;
    for (size_t i = 0; i < k; ++i) sum += (w[i] = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(k - 1)));
    for (double& v : w) v /= sum;
    Vec padded(x.size() + 2 * h);
    for (size_t i = 0; i < padded.size(); ++i) padded[i] = x[std::clamp<long>(long(i) - long(h), 0, long(x.size()) - 1)];
    const Vec c = convolve(padded, w);
    return Vec(c.begin() + long(k) - 1, c.begin() + long(k) - 1 + long(x.size()));
}

// Noise with a 1/f^power amplitude spectrum (pink 0.5, brown 1), unit std
inline Vec colored(size_t n, Rng& r, double power)
{
    const size_t m = Fft::size(n + 256);
    std::vector<double> re = r.normals(m), im(m, 0);
    Fft::transform(re, im, false);
    for (size_t i = 0; i < m; ++i) {
        const size_t bin = std::min(i, m - i);
        const double f = std::max(15.0, double(bin) * SR / double(m));
        const double g = std::pow(f, -power);
        re[i] *= g;
        im[i] *= g;
    }
    Fft::transform(re, im, true);
    Vec out(re.begin() + 128, re.begin() + 128 + long(n));
    const double s = stdOf(out) + 1e-12;
    for (double& v : out) v /= s;
    return out;
}

// RBJ shelf (slope s 0.8): above (high) or below f0 moves gain_db
inline Vec shelf(Vec x, double f0, double gainDb, bool high)
{
    const double s = 0.8, A = std::pow(10.0, gainDb / 40), w0 = 2 * kPi * std::min(f0, SR * 0.45) / SR;
    const double c = std::cos(w0), sn = std::sin(w0), al = sn / 2 * std::sqrt((A + 1 / A) * (1 / s - 1) + 2);
    const double q = 2 * std::sqrt(A) * al;
    double b0, b1, b2, a0, a1, a2;
    if (high) {
        b0 = A * ((A + 1) + (A - 1) * c + q), b1 = -2 * A * ((A - 1) + (A + 1) * c), b2 = A * ((A + 1) + (A - 1) * c - q);
        a0 = (A + 1) - (A - 1) * c + q, a1 = 2 * ((A - 1) - (A + 1) * c), a2 = (A + 1) - (A - 1) * c - q;
    } else {
        b0 = A * ((A + 1) - (A - 1) * c + q), b1 = 2 * A * ((A - 1) - (A + 1) * c), b2 = A * ((A + 1) - (A - 1) * c - q);
        a0 = (A + 1) + (A - 1) * c + q, a1 = -2 * ((A - 1) + (A + 1) * c), a2 = (A + 1) + (A - 1) * c - q;
    }
    filter(x, {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0});
    return x;
}

// The room: a short smooth noise tail (a real room's late field is Gaussian noise; highs die faster with damp),
// unit energy, high-passed at lowcut, after a predelay; dry + wet * it
inline Vec room(const Vec& x, Rng& r, double rt60, double damp, double wet, double tailS)
{
    const size_t n = nOf(tailS), pd = nOf(0.006);
    const double rtHi = rt60 * std::max(0.05, 1 - 0.85 * damp);
    const Vec lo = butter(r.normals(n), false, 4000, 2), hiN = butter(r.normals(n), true, 4000, 2);
    Vec ir(pd + n, 0);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        const double onset = std::min(1.0, t / 0.004);
        ir[pd + i] = onset * (lo[i] * std::pow(10.0, -3 * t / rt60) + hiN[i] * std::pow(10.0, -3 * t / rtHi));
    }
    ir = butter(ir, true, 120, 2);
    const double e = std::sqrt(std::inner_product(ir.begin(), ir.end(), ir.begin(), 0.0)) + 1e-12;
    for (double& v : ir) v /= e;
    Vec y = convolve(x, ir);
    for (double& v : y) v *= wet;
    for (size_t i = 0; i < x.size(); ++i) y[i] += x[i];
    return y;
}

// The edit: end where the 5 ms envelope has fallen end_db under its peak, a raised-cosine fade of end_fade, a 1.5 ms
// start ramp
inline Vec edit(Vec y, double endDb, double endFade)
{
    const size_t h = nOf(0.005), k = y.size() / h;
    Vec e(k);
    for (size_t b = 0; b < k; ++b) {
        double q = 0;
        for (size_t i = b * h; i < (b + 1) * h; ++i) q += y[i] * y[i];
        e[b] = 20 * std::log10(std::sqrt(q / double(h)) + 1e-12);
    }
    Vec es(k);
    for (size_t b = 0; b < k; ++b) { // np.convolve(e, ones(4) / 4, 'same')
        double s = 0;
        for (long j = long(b) - 2; j <= long(b) + 1; ++j)
            if (j >= 0 && j < long(k)) s += e[size_t(j)];
        es[b] = s / 4;
    }
    const size_t i = size_t(std::max_element(es.begin(), es.end()) - es.begin());
    size_t below = k - i;
    for (size_t j = i; j < k; ++j)
        if (es[j] < es[i] + endDb) {
            below = j - i;
            break;
        }
    const size_t end = std::min(y.size(), (i + below) * h + nOf(endFade * 0.5));
    y.resize(end);
    const size_t nf = std::min(nOf(endFade), y.size() - 1);
    for (size_t j = 0; j < nf; ++j) y[y.size() - nf + j] *= 0.5 + 0.5 * std::cos(kPi * double(j) / double(nf - 1));
    const size_t ni = nOf(0.0015);
    for (size_t j = 0; j < ni && j < y.size(); ++j) {
        const double s = std::sin(0.5 * kPi * double(j) / double(ni - 1));
        y[j] *= s * s;
    }
    return y;
}


// ---- for the forge recipes (core/SfxForge) ----

// Band pass lo..hi (2nd-order high pass then low pass: scipy's butter 'band' of order 2 in shape)
inline Vec bandpass(Vec x, double lo, double hi)
{
    return butter(butter(std::move(x), true, std::max(lo, 20.0), 2), false, std::min(hi, 0.45 * SR), 2);
}

// dsp.room_ir: a few early reflections and a decaying noise tail whose highs die faster
inline Vec roomIr(Rng& r, double rt60, double size, double damp)
{
    const double seconds = std::min(2.0, rt60 * 1.2 + 0.05);
    const size_t n = nOf(seconds);
    Vec tail(n);
    for (size_t i = 0; i < n; ++i) tail[i] = r.normal() * std::exp(-6.91 * (double(i) / SR) / std::max(rt60, 0.02));
    const Vec lo = butter(tail, false, 1500 + (1 - damp) * 9000, 1);
    Vec ir(n);
    for (size_t i = 0; i < n; ++i) {
        const double mix = std::pow(double(i) / SR / seconds, 0.7);
        ir[i] = ((1 - mix * damp) * tail[i] + mix * damp * lo[i]) * 0.3;
    }
    ir[0] += 1.0;
    for (int k = 0; k < 6; ++k) {
        const size_t d = size_t((0.002 + r.unit() * (0.002 + 0.02 * size)) * SR);
        const double g = 0.15 + 0.3 * r.unit();
        if (d < n) ir[d] += r.unit() < 0.5 ? -g : g;
    }
    return ir;
}

// dsp.room: x in a small room, wet share of the (energy-normalised) reflections; longer by the tail
inline Vec smallRoom(const Vec& x, Rng& r, double rt60, double size, double damp, double wet)
{
    Vec ir = roomIr(r, rt60, size, damp);
    ir[0] = 0.0;
    const double e = std::sqrt(std::inner_product(ir.begin(), ir.end(), ir.begin(), 0.0)) + 1e-9;
    Vec y = convolve(x, ir);
    for (double& v : y) v *= wet / e;
    for (size_t i = 0; i < x.size(); ++i) y[i] += x[i];
    return y;
}

// dsp.fade: raised-cosine fade in / out
inline void fade(Vec& x, double fin, double fout)
{
    const size_t a = std::min(x.size(), nOf(fin)), b = std::min(x.size(), nOf(fout));
    if (a > 1)
        for (size_t i = 0; i < a; ++i) x[i] *= 0.5 - 0.5 * std::cos(kPi * double(i) / double(a - 1));
    if (b > 1)
        for (size_t i = 0; i < b; ++i) x[x.size() - b + i] *= 0.5 + 0.5 * std::cos(kPi * double(i) / double(b - 1));
}

// dsp.trim_tail: cut what stays floor_db under the peak, keeping `keep` s after the last loud sample
inline void trimTail(Vec& x, double floorDb, double keep)
{
    double peak = 0;
    for (double v : x) peak = std::max(peak, std::abs(v));
    const double limit = peak * std::pow(10.0, floorDb / 20);
    size_t last = x.size();
    for (size_t i = x.size(); i-- > 0;)
        if (std::abs(x[i]) >= limit) {
            last = i;
            break;
        }
    x.resize(std::min(x.size(), last + nOf(keep)));
}

inline Vec times(size_t n)
{
    Vec t(n);
    for (size_t i = 0; i < n; ++i) t[i] = double(i) / SR;
    return t;
}

// _tonal.burst: a short contact noise band-passed lo..hi, a `rise` s rise and an exponential decay; peak 1
inline Vec burst(size_t n, Rng& r, double lo, double hi, double tau, double rise = 0.0003)
{
    const size_t k = std::min(n, nOf(std::max(6 * tau + rise, 0.002)));
    Vec w = bandpass(r.normals(k), lo, hi);
    double peak = 1e-12;
    for (size_t i = 0; i < k; ++i) {
        const double t = double(i) / SR;
        w[i] *= (1 - std::exp(-t / std::max(rise, 1e-5))) * std::exp(-t / std::max(tau, 1e-5));
        peak = std::max(peak, std::abs(w[i]));
    }
    Vec y(n, 0.0);
    for (size_t i = 0; i < k; ++i) y[i] = w[i] / peak;
    return y;
}

// _tonal.thump: a damped sine at f with a soft onset; peak ~1
inline Vec thump(size_t n, double f, double tau, double rise = 0.002)
{
    Vec y(n);
    double peak = 1e-12;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        y[i] = std::sin(2 * kPi * f * t) * (1 - std::exp(-t / std::max(rise, 1e-4))) * std::exp(-t / std::max(tau, 1e-4));
        peak = std::max(peak, std::abs(y[i]));
    }
    for (double& v : y) v /= peak;
    return y;
}

// _tonal.modes: struck modes (damped sines), `bend` = the pitch starts this fraction high and settles
inline Vec modes(size_t n, Rng& r, const Vec& freqs, const Vec& amps, const Vec& taus, double jitter = 0, double bend = 0,
                 double bendTau = 0.004)
{
    Vec y(n, 0.0);
    for (size_t m = 0; m < freqs.size(); ++m) {
        if (amps[m] <= 0 || freqs[m] >= 0.45 * SR) continue;
        const double f = freqs[m] * (1 + jitter * r.normal());
        double ph = r.unit() * 2 * kPi;
        for (size_t i = 0; i < n; ++i) {
            const double t = double(i) / SR;
            ph += 2 * kPi * f * (1 + bend * std::exp(-t / std::max(bendTau, 1e-4))) / SR;
            y[i] += amps[m] * std::exp(-t / std::max(taus[m], 1e-5)) * std::sin(ph);
        }
    }
    return y;
}

// _tonal.strike: the modes driven by a contact (a raised-cosine force pulse with a little grit)
inline Vec strike(const Vec& y, Rng& r, double contactS, double bright)
{
    const size_t n = std::max<size_t>(2, nOf(contactS));
    Vec pulse(n);
    for (size_t i = 0; i < n; ++i) {
        const double p = 0.5 - 0.5 * std::cos(2 * kPi * double(i) / double(n - 1));
        pulse[i] = p + r.normal() * p * bright;
    }
    Vec out = convolve(pulse, y);
    out.resize(y.size());
    return out;
}

// _tonal.finish (mono): band limits, a small room, the tail trimmed, a clean start and a convex end fade
inline Vec finish(Vec y, Rng& r, double hp, double lp, double roomWet, double rt60, double endDb = -60, double fadeS = 0.006)
{
    const size_t kd = std::min({y.size(), nOf(0.02), size_t(0.15 * double(y.size()))});
    for (size_t i = 0; i < kd; ++i) {
        const double c = std::cos(0.5 * kPi * double(i) / double(std::max<size_t>(1, kd - 1)));
        y[y.size() - kd + i] *= c * c;
    }
    y = butter(std::move(y), true, hp, 4);
    if (lp < 17500) y = butter(std::move(y), false, lp, 2);
    if (roomWet > 0.005) y = room(y, r, rt60, 0.6, roomWet, std::max(0.1, 1.2 * rt60));
    trimTail(y, endDb, 0.01);
    const size_t k = std::min(y.size(), nOf(fadeS));
    for (size_t i = 0; i < k; ++i) {
        const double c = std::cos(0.5 * kPi * double(i) / double(std::max<size_t>(1, k - 1)));
        y[y.size() - k + i] *= c * c;
    }
    const size_t a = std::min(y.size(), nOf(0.0003));
    for (size_t i = 0; i < a; ++i) y[i] *= 0.5 - 0.5 * std::cos(kPi * double(i) / double(std::max<size_t>(1, a - 1)));
    return y;
}

} // namespace SfxDsp
