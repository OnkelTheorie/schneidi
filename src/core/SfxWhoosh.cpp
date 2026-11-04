// SPDX-License-Identifier: AGPL-3.0-only AND MIT
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

#include "core/SfxWhoosh.h"

#include "core/SfxDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <numeric>

namespace SfxWhoosh {

namespace {

using namespace SfxDsp;

struct P {
    double rise, rise_k, hold, fall, fall_k, start_db, track, f_peak, up_oct, down_oct, dop_st, dop_w, q_air, lp_ratio,
        flut_depth, flut_f, rustle, rustle_f, low_db, low_f, low_smooth, sub_db, sub_f, sub_glide, sub_drive, hiss_db,
        hiss_hp, hiss_open, hiss_pow, tone_db, tone_q, tone_ratio, tone_pure, grain_db, grain_rate, grain_f, room_wet,
        rt60, pan, width, drive_db, end_db, end_fade, low_lag, hiss_rustle, hiss_lo, eq_lo_db, eq_lo_f, eq_hi_db,
        eq_hi_f, hiss_pre, hiss_tail, hiss_mfloor, room_damp, tail_sweep, tail_tau, hiss_q;
};

struct Entry {
    const char* id;
    double length;
    P p;
};

// The delivered forge_v2 sets (hiss_lo / hiss_pre / hiss_tail 0 = not set: the original's None)
const Entry kEntries[] = {
    // pro_whoosh_big/windy (forge_v2 bank, 0.770 s)
    {"pro_whoosh_big/windy", 0.7704, {0.318412, 1.16116, 0.0286618, 0.148732, 0.822635, -32.5953, 0.834482, 236.449, 0.86451, 1.44171, 1.80926, 0.0856222, 0.715548, 2.65915, 0.263469, 14.406, 0.51473, 42.7275, 1.12486, 85.4012, 0.0220234, -4.3023, 46.8641, 0.617006, 0.982259, -26.988, 1197.25, 1.6457, 1.17587, -41.4741, 24.1913, 1.69659, 0.039324, -48.6146, 206.589, 3194.96, 0.112697, 0.493544, 0.0133901, 0.035285, 2.02144, -64.1667, 0.0508512, 0, 0.4658, 1395.68, 0, 150, 0, 4000, 0, 0, 0, 0.55, 0, 0.25, 0.6}},
    // pro_whoosh_big/long (forge_v2 bank, 1.120 s)
    {"pro_whoosh_big/long", 1.1199, {0.430001, 1.19016, 0.04, 0.238972, 0.868, -34.5457, 0.713178, 81.9929, 0.919157, 1.59806, 2.02966, 0.0622694, 0.741849, 5.93119, 0.57865, 6.70241, 0.0152868, 99.1283, -1.4095, 90.0196, 0.0396753, 1.65445, 43.453, 0.71888, 0.573051, -28.6911, 910.036, 1.44458, 1.13106, -38.12, 18.4199, 1.64692, 0.0149752, -49.2732, 344.425, 3089.63, 0.091569, 0.667392, 0.00320563, 0.0202811, 2.87195, -64.0242, 0.0598376, 0, 0, 0, 0, 150, 0, 4000, 0, 0, 0, 0.55, 0, 0.25, 0.6}},
    // swipe/glide (forge_v2 bank, 0.736 s)
    {"swipe/glide", 0.7355, {0.158505, 0.95739, 0.0942032, 0.149754, 0.894542, -32.9049, 0.660052, 446.03, 1.66717, 1.66945, 2.43109, 0.131976, 0.966193, 4.50418, 0.281157, 12.6073, 0.20083, 114.074, -4.08676, 118.298, 0.0639045, -49.7688, 58.1633, 0.370752, 0.628255, -17.3546, 2938.12, 1.27786, 1.12796, -49.7011, 18.8225, 1.67588, 0.0103624, -31.7321, 1678.29, 4992.48, 0.178184, 0.465014, 0.156947, 0.427425, 1.16444, -67.1974, 0.0610742, 0.0195544, 0.358176, 1546.92, -1.8561, 110.793, -2.97798, 8801.41, 0.605138, 0.737029, 0.575974, 0.302557, 0, 0.25, 0.6}},
    // swipe/flick (forge_v2 bank, 0.568 s)
    {"swipe/flick", 0.5681, {0.0898011, 0.812439, 0.0316805, 0.125865, 0.699947, -32.6794, 0.611726, 520.219, 1.12419, 1.87939, 1.13025, 0.038001, 0.835107, 3.68532, 0.545318, 38.2368, 0.474075, 104.035, -7.17825, 115.501, 0.0417412, -49.2216, 58.893, 0.296899, 0.652909, -15.3752, 3177.75, 1.24321, 1.03087, -49.386, 22.1752, 1.62304, 0.00503035, -18.7666, 910.762, 5784.76, 0.130736, 0.297155, 0.306499, 0.416721, 1.38731, -68.4212, 0.0561292, 0.0279892, 0.679051, 1734.67, -1.65138, 122.086, -2.98695, 8770.15, 0.488659, 0.63406, 0.507572, 0.279076, 0, 0.25, 0.6}},
    // swipe/cloth (forge_v2 bank, 0.676 s)
    {"swipe/cloth", 0.6757, {0.142288, 1.08525, 0.031473, 0.17472, 0.694622, -33.1966, 0.732837, 349.066, 1.20302, 2.01539, 0.807282, 0.0633964, 0.77491, 4.14785, 0.295695, 13.1795, 0.502833, 115.712, -6.48337, 114.299, 0.0471785, -47.6216, 59.5223, 0.328977, 0.464969, -19.3893, 2974.82, 1.08527, 1.29535, -49.4863, 20.2417, 1.71892, 0.0263186, -16.9728, 1522.99, 5917.14, 0.14209, 0.297798, 0.148107, 0.386195, 1.13833, -65.4809, 0.0413295, 0.0217135, 0.717675, 1970.37, -3.26108, 116.538, -3.05141, 8321.14, 0.579212, 0.725819, 0.543915, 0.339572, 0, 0.25, 0.6}},
    // swipe/paper (forge_v2 bank, 0.770 s)
    {"swipe/paper", 0.7697, {0.120936, 1.00854, 0.0316961, 0.130245, 0.640291, -33.9417, 0.709705, 512.312, 1.1996, 2.00628, 0.989023, 0.0513031, 1.10186, 4.0605, 0.296174, 14.1131, 0.500154, 150.954, -6.10535, 152.096, 0.0486629, -49.881, 59.8274, 0.311442, 0.48869, -18.9809, 2988.74, 1.20015, 1.00564, -50, 20.6909, 1.70575, 0.000249963, -16.0269, 2454.42, 5939.24, 0.118613, 0.345118, 0.150807, 0.301807, 1.55974, -65.9919, 0.0493894, 0.0200392, 0.500805, 2030.05, -2.10752, 121.966, -3.89252, 8981.92, 0.399263, 0.515228, 0.586367, 0.294384, 0, 0.25, 0.6}},
};

const Entry& entry(Member m) { return kEntries[int(m)]; }

} // namespace

double naturalLength(Member m) { return entry(m).length; }

Result render(Member member, unsigned seed, double stretch)
{
    P p = entry(member).p;
    stretch = std::clamp(stretch, 0.25, 4.0);
    p.rise *= stretch;
    p.hold *= stretch;
    p.fall *= stretch;
    Rng r(std::uint64_t(seed) * 8 + std::uint64_t(member) + 1);

    // envelope(): the level curve every layer shares, its peak at tp
    const double tp = p.rise * std::pow(std::max(-p.start_db, 1.0) / 30.0, 1.0 / p.rise_k);
    const double tEnd = tp + p.hold + p.fall * std::pow(95.0 / 30.0, 1.0 / p.fall_k)
                        + 0.9 * p.rt60 * std::min(1.0, 4 * p.room_wet + 0.2) + 0.05;
    const size_t n = nOf(std::min(tp + std::max(tEnd - tp, 0.1), 4.5));
    Vec a(n), dt(n);
    for (size_t i = 0; i < n; ++i) {
        dt[i] = double(i) / SR - tp;
        const double x = dt[i] < 0 ? -dt[i] / p.rise : std::max(dt[i] - p.hold, 0.0) / p.fall;
        const double L = -30.0 * std::pow(std::max(x, 0.0), dt[i] < 0 ? p.rise_k : p.fall_k);
        a[i] = std::pow(10.0, std::max(L, -140.0) / 20);
    }
    a = smooth(a, std::clamp(0.3 * std::min(p.rise, p.fall + p.hold), 0.004, 0.1));
    const double amax = *std::max_element(a.begin(), a.end()) + 1e-12;
    for (double& v : a) v /= amax;

    Vec m(n), dop(n), fAir(n), gt(n, 0.0);
    const double lf = std::log2(p.f_peak);
    for (size_t i = 0; i < n; ++i) {
        m[i] = std::pow(a[i], p.track);
        dop[i] = std::pow(2.0, -(p.dop_st / 12.0) * 0.5 * std::tanh(dt[i] / p.dop_w));
        const double span = dt[i] < 0 ? p.up_oct : p.down_oct;
        fAir[i] = std::pow(2.0, lf - span * (1.0 - m[i])) * dop[i];
        if (p.tail_sweep > 0) {
            gt[i] = 1.0 - std::exp(-std::max(dt[i], 0.0) / p.tail_tau);
            fAir[i] *= std::pow(2.0, -0.5 * p.tail_sweep * gt[i]);
        }
    }
    Vec flut = lfnoise(n, p.flut_f, r);
    for (double& v : flut) v = std::exp(p.flut_depth * v - 0.5 * p.flut_depth * p.flut_depth);

    // Air body: three bands moving together, a cliff above
    Vec air(n, 0.0);
    {
        const Vec src = colored(n, r, 0.5);
        const double bands[3][2] = {{1.0, 1.0}, {1.6, 0.55}, {2.5, 0.3}};
        for (const auto& [ratio, g] : bands) {
            Vec f(n);
            for (size_t i = 0; i < n; ++i) f[i] = std::clamp(fAir[i] * ratio, 30.0, 20000.0);
            const Vec b = svf(ratio == 1.0 ? src : colored(n, r, 0.5), f, p.q_air, 3);
            for (size_t i = 0; i < n; ++i) air[i] += g * b[i];
        }
        Vec f(n);
        for (size_t i = 0; i < n; ++i) f[i] = std::clamp(fAir[i] * p.lp_ratio, 60.0, 20000.0);
        air = svf(air, f, 0.7, 0);
        if (p.rustle > 0) {
            Vec rus = lfnoise(n, p.rustle_f, r);
            for (double& v : rus) v = std::exp(p.rustle * 1.2 * v);
            const double rr = rmsOf(rus);
            for (size_t i = 0; i < n; ++i) air[i] *= rus[i] / rr;
        }
        for (size_t i = 0; i < n; ++i) air[i] *= a[i] * flut[i];
    }
    std::vector<std::pair<double, Vec>> layers{{0.0, air}};

    // Low weight
    if (p.low_db > -39.5) {
        Vec al = smooth(a, p.low_smooth);
        const double mx = *std::max_element(al.begin(), al.end()) + 1e-12;
        for (double& v : al) v /= mx;
        if (p.low_lag > 0) {
            const size_t lag = nOf(p.low_lag);
            Vec shifted(n, 0.0);
            for (size_t i = lag; i < n; ++i) shifted[i] = al[i - lag];
            al = shifted;
        }
        Vec f(n);
        for (size_t i = 0; i < n; ++i) f[i] = std::clamp(p.low_f * (0.6 + 0.4 * m[i]), 25.0, 2000.0);
        Vec low = butter(svf(colored(n, r, 1.0), f, 0.75, 0), true, std::max(28.0, p.low_f / 2.5), 2);
        for (size_t i = 0; i < n; ++i) low[i] *= al[i];
        layers.emplace_back(p.low_db, low);
    }
    // Sub swell
    if (p.sub_db > -49.5) {
        Vec al = smooth(a, std::max(p.low_smooth, 0.02));
        const double mx = *std::max_element(al.begin(), al.end()) + 1e-12;
        double ph = r.unit() * 2 * kPi;
        Vec sub(n);
        for (size_t i = 0; i < n; ++i) {
            const double s = 0.5 + 0.5 * std::tanh(dt[i] / (0.5 * p.fall + 0.02));
            ph += 2 * kPi * p.sub_f * std::pow(2.0, -p.sub_glide * s) / SR;
            double v = std::sin(ph);
            if (p.sub_drive > 0) {
                const double g = 1 + 2 * p.sub_drive;
                v = std::tanh(g * v + 0.15 * p.sub_drive) - std::tanh(0.15 * p.sub_drive);
            }
            sub[i] = v;
        }
        sub = butter(sub, false, 600, 2);
        for (size_t i = 0; i < n; ++i) sub[i] *= al[i] / mx;
        layers.emplace_back(p.sub_db, sub);
    }
    // Hiss top
    if (p.hiss_db > -44.5) {
        const bool fab = p.hiss_rustle > 0;
        const double hLo = fab && p.hiss_lo > 0 ? p.hiss_lo : p.hiss_hp;
        const double mf = p.hiss_mfloor;
        Vec hLp(n), hHp(n);
        for (size_t i = 0; i < n; ++i) {
            const double mh = mf <= 0 ? m[i] : mf + (1.0 - mf) * m[i];
            hLp[i] = std::clamp(p.hiss_hp * std::pow(2.0, p.hiss_open * mh) * 1.5 * dop[i], 500.0, 20000.0);
            if (p.tail_sweep > 0) {
                const double sh = std::pow(2.0, -p.tail_sweep * gt[i]);
                hLp[i] = std::clamp(hLp[i] * sh, 300.0, 20000.0);
                hHp[i] = std::clamp(hLo * sh, 150.0, 16000.0);
            }
        }
        Vec h = p.tail_sweep > 0 ? svf(svf(r.normals(n), hHp, 0.54, 2), hHp, 1.31, 2) : butter(r.normals(n), true, hLo, 4);
        h = svf(h, hLp, p.hiss_q, 0);
        if (fab) {
            Vec ru = lfnoise(n, 1.8 * p.rustle_f, r);
            for (double& v : ru) v = std::exp(p.hiss_rustle * 1.4 * v);
            const double rr = rmsOf(ru);
            for (size_t i = 0; i < n; ++i) h[i] *= ru[i] / rr;
        }
        const double pre = p.hiss_pre > 0 ? p.hiss_pre : p.hiss_pow, post = p.hiss_tail > 0 ? p.hiss_tail : p.hiss_pow;
        const bool split = p.hiss_pre > 0 || p.hiss_tail > 0;
        for (size_t i = 0; i < n; ++i) {
            double he = std::pow(a[i], p.hiss_pow);
            if (split) {
                const double w = 0.5 + 0.5 * std::tanh(dt[i] / 0.005);
                he = (1 - w) * std::pow(a[i], pre) + w * std::pow(a[i], post);
            }
            h[i] *= he * flut[i];
        }
        layers.emplace_back(p.hiss_db, h);
    }
    // Tonal / whistle
    if (p.tone_db > -49.5) {
        Vec ft(n);
        for (size_t i = 0; i < n; ++i) ft[i] = std::clamp(fAir[i] * p.tone_ratio, 40.0, 16000.0);
        Vec nb = svf(r.normals(n), ft, p.tone_q, 3);
        const double s = stdOf(nb) + 1e-12;
        double ph = 0;
        Vec tone(n);
        for (size_t i = 0; i < n; ++i) {
            ph += 2 * kPi * ft[i] / SR;
            tone[i] = ((1 - p.tone_pure) * nb[i] / s + p.tone_pure * std::sin(ph) * std::numbers::sqrt2) * std::pow(a[i], 1.5);
        }
        layers.emplace_back(p.tone_db, tone);
    }
    // Texture grains: soft 3 ms bumps of band noise
    if (p.grain_db > -49.5) {
        Vec ev(n), f(n);
        for (size_t i = 0; i < n; ++i) {
            const bool hit = r.unit() < p.grain_rate * a[i] / SR;
            ev[i] = hit ? std::exp(0.5 * r.normal()) : 0.0;
            f[i] = std::clamp(p.grain_f * dop[i], 200.0, 18000.0);
        }
        const Vec env = smooth(ev, 0.003);
        Vec gr = svf(r.normals(n), f, 1.2, 3);
        for (size_t i = 0; i < n; ++i) gr[i] *= env[i] * a[i];
        layers.emplace_back(p.grain_db, gr);
    }

    Vec x(n, 0.0);
    for (const auto& [db, y] : layers) {
        const double g = std::pow(10.0, db / 20) / lvl(y);
        for (size_t i = 0; i < n; ++i) x[i] += g * y[i];
    }
    // Space (mono: the original's stereo pan and side signals cancel in the mono sum)
    Vec y = x;
    if (p.room_wet > 0.005) y = room(x, r, p.rt60, p.room_damp, p.room_wet, std::max(0.2, 1.1 * p.rt60));
    if (p.drive_db > 0.05) { // light tape saturation, the input level kept
        double peak = 1e-12;
        for (double v : y) peak = std::max(peak, std::abs(v));
        const double before = rmsOf(y), g = std::pow(10.0, p.drive_db / 20), b = 0.12;
        for (double& v : y) v = std::tanh(g * v / peak + b) - std::tanh(b);
        y = butter(y, true, 8, 1);
        const double after = rmsOf(y) + 1e-12;
        for (double& v : y) v *= before / after;
    }
    if (std::abs(p.eq_lo_db) > 0.05) y = shelf(y, p.eq_lo_f, p.eq_lo_db, false);
    if (std::abs(p.eq_hi_db) > 0.05) y = shelf(y, p.eq_hi_f, p.eq_hi_db, true);
    // A gentle high pass at 90 Hz (the original: 25 Hz): moving air below it reads as wind on a microphone
    y = butter(butter(butter(y, true, 25, 4), true, 90, 2), false, 19000, 4);
    y = edit(y, p.end_db, p.end_fade);

    Result out;
    out.anchor = tp;
    double peak = 1e-12;
    for (double v : y) peak = std::max(peak, std::abs(v));
    out.samples.resize(y.size());
    for (size_t i = 0; i < y.size(); ++i) out.samples[i] = float(y[i] / peak);
    return out;
}

} // namespace SfxWhoosh
