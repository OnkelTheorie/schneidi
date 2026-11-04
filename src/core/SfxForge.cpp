// SPDX-License-Identifier: AGPL-3.0-only AND MIT
// Ported from earshot (https://github.com/Leonxlnx/earshot): earshot/forge/recipes/darkriser.py, shutter.py,
// knock.py, subdrop.py and _tonal.py (burst, thump, modes, strike, finish), earshot/forge/dsp.py (room_ir, room, fade, trim_tail)
// and the parameter sets riser/*, shutter/*, knock/* of earshot/forge/bank/forge04.json and pro_sub_drop/* of
// forge_v2.json. Changes: C++, mono, own
// deterministic noise, _tonal.finish's small room as a noise tail instead of fx.reverb. The ported code is under the
// MIT licence:
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

#include "core/SfxForge.h"

#include "core/SfxDsp.h"

namespace SfxForge {

namespace {

using namespace SfxDsp;

Result done(Vec y, double anchor)
{
    Result out;
    out.anchor = anchor;
    double peak = 1e-12;
    for (double v : y) peak = std::max(peak, std::abs(v));
    out.samples.resize(y.size());
    for (size_t i = 0; i < y.size(); ++i) out.samples[i] = float(y[i] / peak);
    return out;
}

// ---------- darkriser ----------

struct RiserP {
    double length, start_db, curve, tail_tau, body_f0, body_f1, body_q, sub_f, sub_db, sub_drive, crackle_db,
        crackle_rate, crackle_hp, gate_at, gate_f0, gate_f1, gate_depth, duty, width, room;
};

const RiserP kRisers[] = {
    // riser/dark (forge04 bank): the music films' short dark riser
    {0.74, 41.86, 1.378, 0.02767, 148.4, 502.3, 1.136, 47.85, -10.93, 0.7114, -50.0, 134.9, 3039.0, 0.7039, 7.817,
     19.91, 0.8088, 0.3958, 0.2022, 0.2256},
    // riser/snatch: a steep late build under a light fast stutter
    {0.45, 24.0, 3.108, 0.02393, 279.2, 373.3, 2.0, 37.65, -13.57, 1.628, -22.68, 20.44, 4463.0, 0.337, 7.732, 45.0,
     0.2926, 0.3132, 0.0431, 0.2851},
    // riser/build: the effects-led film's designed build (driven sub, high crackle, an early stutter); its crackle
    // 14 dB lower than the bank's -6 dB (it read as crackling to the user)
    {1.2, 44.04, 0.8445, 0.07787, 94.27, 150.0, 2.0, 33.58, 3.405, 1.116, -20.0, 112.1, 9000.0, 0.2, 6.657, 12.49, 0.75,
     0.5742, 0.0, 0.1399},
};

// ---------- riser (the designed 'build' render) ----------

struct BuildP {
    double length, curve, rise_db, noise_db, nf0, nf1, nbw, pink, tone_db, fifth_db, oct_db;
    int wave; // dsp.osc: 0 sine, 1 saw, 3 tri
    double tf0, toct, tbright, detune, width, gate_depth, gate_on, gate_p0, gate_p1, gate_duty, gate_sharp, gate_pow,
        tone_gate, rswell_db, rswell_rt, verb, rt60, sat, hp, end_ms;
};

const BuildP kBuilds[] = {
    // riser/tension (forge_v2 bank): pitch leads, a detuned tri chord 600 Hz -> 2.4 kHz under a tremolo speeding up
    // 5 -> 33 Hz, an opening noise band and a little reverse swell support it
    {2.8, 2.4, 30.0, -2.0, 1200.0, 11000.0, 3.0, 0.6, 0.0, -6.0, -14.0, 3, 600.0, 2.0, 4.0, 22.0, 0.5, 0.5, 0.1, 0.2,
     0.03, 0.5, 0.15, 1.5, 1.0, -10.0, 2.0, 0.25, 2.0, 1.0, 260.0, 4.0},
    // riser/swell: the reverse-cymbal build, a reversed computed reverb tail of a bright burst, a bright band and a
    // thin high sine chord, no gate
    {2.2, 2.8, 38.0, -14.0, 2000.0, 16000.0, 1.4, 0.3, -22.0, -6.0, -30.0, 0, 1500.0, 1.6, 4.0, 8.0, 0.5, 0.0, 0.5,
     0.12, 0.03, 0.5, 0.2, 2.0, 0.5, 0.0, 2.6, 0.2, 1.6, 1.0, 500.0, 4.0},
    // riser/air: a noise band opening up under a light accelerating tremolo, a faint tri chord rising 2 octaves
    {2.4, 2.6, 34.0, 0.0, 900.0, 13000.0, 2.2, 0.5, -12.0, -6.0, -26.0, 3, 700.0, 2.0, 4.0, 14.0, 0.6, 0.3, 0.15, 0.25,
     0.034, 0.55, 0.3, 1.6, 0.6, -6.0, 2.2, 0.3, 2.0, 1.0, 300.0, 5.0},
};
constexpr double kFinalOpen = 0.045; // s: the gate is open over the last 45 ms, so the riser peaks on the cut
constexpr double kUnison[5] = {-1.0, -0.45, 0.0, 0.45, 1.0}; // voice offsets (x detune cents) and pans (x width)

// riser.unison, mono (the mid of its pan): 5 voices detuned kUnison x detune x detScale cents
Vec unison(const Vec& f, int wave, const Vec& detune, double detScale, double width, const double* phases)
{
    const size_t n = f.size();
    Vec out(n, 0.0), fk(n);
    for (int k = 0; k < 5; ++k) {
        for (size_t i = 0; i < n; ++i) fk[i] = f[i] * std::exp2(kUnison[k] * detune[i] * detScale / 1200.0);
        const Vec v = osc(fk, wave, phases[k]);
        const double th = (std::clamp(kUnison[k] * width, -1.0, 1.0) + 1) * kPi / 4;
        const double g = (std::cos(th) + std::sin(th)) / 2 * std::numbers::sqrt2 / 5;
        for (size_t i = 0; i < n; ++i) out[i] += g * v[i];
    }
    return out;
}

// riser.wet_of, mono: the reverb return alone of x (a unit-energy noise tail in place of fx.reverb_ir's FDN), cut to
// nOut samples
Vec wetOf(const Vec& x, Rng& r, double rt60, size_t nOut, double damp, double predelay, double lowcut)
{
    Vec ir = noiseIr(r, rt60, damp, predelay, lowcut, 1.44 * rt60); // fx.reverb_ir's tail: 1.2 x rt60 x bass 1.2
    if (ir.size() > nOut) ir.resize(nOut);
    Vec y = convolve(x, ir);
    y.resize(nOut, 0.0);
    return y;
}

Result build(const BuildP& p0, std::uint64_t salt, double length)
{
    BuildP p = p0;
    if (length > 0) p.length = length;
    Rng r(salt);
    const double L = p.length;
    const size_t n = nOf(L);
    Vec w(n), lv(n), per(n), ph(n), gateN(n), gateT(n);
    for (size_t i = 0; i < n; ++i) {
        const double u = double(i) / SR / L;
        w[i] = std::pow(u, p.curve); // accelerating progress
        lv[i] = std::pow(10.0, -p.rise_db * (1 - w[i]) / 20);
        per[i] = p.gate_p0 * std::pow(p.gate_p1 / p.gate_p0, std::pow(u, p.gate_pow));
    }
    // The accelerating gate, laid out from the end (riser.phase_from_end: the last pulse open on the final sample)
    double cycles = 0;
    for (size_t i = n; i-- > 0;) {
        cycles += 1.0 / per[i] / SR;
        ph[i] = 0.5 * p.gate_duty - cycles;
    }
    for (size_t i = 0; i < n; ++i) { // riser.gate_shape: a smooth tremolo .. a hard gate with 1.2 ms edges
        const double t = double(i) / SR, u = t / L, fr = ph[i] - std::floor(ph[i]);
        const double trem = 0.5 + 0.5 * std::cos(2 * kPi * (fr - 0.5 * p.gate_duty));
        const double on = std::clamp(fr * per[i] / 0.0012, 0.0, 1.0);
        const double off = std::clamp((p.gate_duty - fr) * per[i] / 0.0018, 0.0, 1.0);
        const double g = (1 - p.gate_sharp) * trem + p.gate_sharp * std::min(on, off);
        const double o = std::clamp((u - p.gate_on) / 0.2, 0.0, 1.0);
        double fin = std::clamp((L - t) / kFinalOpen, 0.0, 1.0);
        fin = fin * fin * (3 - 2 * fin);
        const double depth = p.gate_depth * o * o * (3 - 2 * o) * fin;
        gateN[i] = 1 - depth * (1 - g);
        gateT[i] = 1 - p.tone_gate * depth * (1 - g);
    }
    // Noise: an unresonant band moving up (24 dB/oct low pass nf0 -> nf1, a high pass nbw octaves under it)
    Vec nz = r.normals(n);
    {
        const Vec pk = colored(n, r, 0.5);
        Vec fc(n), fl(n);
        for (size_t i = 0; i < n; ++i) {
            nz[i] = (1 - p.pink) * nz[i] + p.pink * pk[i] * 1.6;
            const double f = p.nf0 * std::pow(p.nf1 / p.nf0, w[i]);
            fc[i] = std::min(f, 18000.0);
            fl[i] = std::max(f / std::exp2(p.nbw), 20.0);
        }
        nz = svf(svf(nz, fc, 0.54, 0), fc, 1.31, 0);
        nz = butter(svf(svf(nz, fl, 0.54, 2), fl, 1.31, 2), true, p.hp, 4);
    }
    // Tonal: a unison chord (root + fifth + a late octave) on a concave glide
    Vec tone, octv;
    {
        Vec f(n), det(n), f2(n);
        for (size_t i = 0; i < n; ++i) {
            const double u = double(i) / SR / L;
            f[i] = p.tf0 * std::exp2(p.toct * std::pow(u, std::max(1.0, 0.8 * p.curve)));
            f2[i] = 2 * f[i];
            det[i] = p.detune * (0.6 + 0.8 * w[i]);
        }
        double ph0[15];
        for (double& v : ph0) v = r.unit();
        tone = unison(f, p.wave, det, 1.0, 0.5 * p.width, ph0);
        if (p.fifth_db > -47) {
            Vec f5(n);
            for (size_t i = 0; i < n; ++i) f5[i] = 1.5 * f[i];
            const Vec v = unison(f5, p.wave, det, 1.0, 0.7 * p.width, ph0 + 5);
            for (size_t i = 0; i < n; ++i) tone[i] += std::pow(10.0, p.fifth_db / 20) * v[i];
        }
        octv = unison(f2, p.wave, det, 0.7, 0.6 * p.width, ph0 + 10);
        for (size_t i = 0; i < n; ++i) {
            const double k = std::clamp((double(i) / SR / L - 0.4) / 0.6, 0.0, 1.0);
            octv[i] *= k * k;
        }
        if (p.wave == 1) {
            Vec cut(n), cut2(n);
            for (size_t i = 0; i < n; ++i) cut[i] = std::min(f[i] * p.tbright, 18000.0), cut2[i] = std::min(2 * cut[i], 18000.0);
            tone = svf(tone, cut, 0.7, 0);
            octv = svf(octv, cut2, 0.7, 0);
        }
        tone = butter(std::move(tone), true, p.hp * 0.8, 2);
        octv = butter(std::move(octv), true, p.hp, 2);
    }
    // (The shepard layer is off in these sets, shep_db -48: not ported)
    // Reverse swell: the reversed reverb tail of a bright burst, concave by nature, ending on the last sample
    // (its decay at most 1.2 x the length, unlike the original: a short riser keeps its rise)
    Vec sw;
    if (p.rswell_db > -47) {
        const size_t kb = nOf(0.03);
        Vec hb = butter(r.normals(kb), true, 1500.0, 2);
        for (size_t i = 0; i < kb; ++i) hb[i] *= std::exp(-double(i) / SR / 0.008);
        const double rt = std::min(p.rswell_rt, 1.2 * L);
        const Vec wet = wetOf(hb, r, rt, nOf(rt + 0.1), 0.3, 0.0, 400.0);
        const size_t k = std::min(n, wet.size());
        sw.assign(n, 0.0);
        for (size_t j = 0; j < k; ++j) sw[n - k + j] = wet[k - 1 - j];
        sw = butter(std::move(sw), true, p.hp, 2);
    }
    // The mix: each layer at its level re its loudest 10 ms in the last quarter
    const auto at = [](double db, const Vec& x) { return std::pow(10.0, db / 20) / lvl(x, 0.25); };
    const double gn = at(p.noise_db, nz), gt = at(p.tone_db, tone), go = at(p.oct_db, octv);
    Vec send(n);
    for (size_t i = 0; i < n; ++i) send[i] = (gn * nz[i] * gateN[i] + (gt * tone[i] + go * octv[i]) * gateT[i]) * lv[i];
    if (!sw.empty()) { // the swell carries its own (reversed-decay) curve
        const double gs = at(p.rswell_db, sw);
        for (size_t i = 0; i < n; ++i) send[i] += gs * sw[i] * (0.25 + 0.75 * gateN[i]);
    }
    Vec y = send;
    if (p.verb > 0.005) { // the space grows with the build, cut at the end (never after the hit)
        const Vec wet = wetOf(send, r, p.rt60, n, 0.5, 0.02, 200.0);
        const double g = p.verb / lvl(wet, 0.25) * lvl(send, 0.25);
        for (size_t i = 0; i < n; ++i) y[i] += g * wet[i];
    }
    if (p.sat > 0.1) { // fx.saturate tanh, level 'rms' (without its oversampling)
        const double g = std::pow(10.0, p.sat / 20), before = rmsOf(y);
        for (double& v : y) v = std::tanh(g * v);
        const double k = before / (rmsOf(y) + 1e-12);
        for (double& v : y) v *= k;
    }
    y = butter(butter(std::move(y), true, 120.0, 2), false, 18000.0, 4);
    fade(y, 0.004, p.end_ms / 1000.0);
    return done(std::move(y), double(n) / SR);
}

// ---------- shutter ----------

struct ShutterP {
    double gap, a_db, a_hz, a_ms, a_modes, whirr, whirr_hz, b_hz, b_decay, b_noise, b_noise_hz, b_contact, body, body_f,
        body_ms, rattle, rattle_hz, hp, lp, room, rt, width;
};

const ShutterP kShutters[] = {
    // shutter/snap (forge04 bank): a dry snap
    {0.05821, -22.88, 3300.0, 2.964, 0.1957, -15.59, 5000.0, 2037.0, 0.004, -2.783, 2530.0, 0.00031, -10.33, 156.8,
     5.495, -48.36, 2581.0, 173.8, 9437.0, 0.01662, 0.0526, 0.234},
    // shutter/mirror: a mirror box
    {0.06462, -18.51, 8113.0, 0.5578, 0.1115, -6.0, 688.7, 1511.0, 0.00622, 0.0, 1468.0, 0.003, -33.33, 300.0, 22.0,
     -26.59, 7000.0, 128.8, 6009.0, 0.3, 0.374, 0.09233},
    // shutter/dslr: a DSLR
    {0.0391, -23.5, 5537.0, 0.5005, 0.3966, -9.218, 1085.0, 1433.0, 0.006, -1.243, 1355.0, 0.00221, -20.61, 238.2,
     19.41, -26.17, 5575.0, 92.91, 5091.0, 0.2557, 0.2552, 0.1069},
};
const double kBModes[5][3] = {{1.0, 1.0, 1.0}, {1.62, 0.7, 0.7}, {2.31, 0.5, 0.5}, {3.17, 0.35, 0.35}, {4.4, 0.2, 0.25}};
const double kAModes[3][2] = {{1.0, 1.0}, {1.71, 0.6}, {2.63, 0.35}};

// ---------- knock ----------

struct KnockP {
    double f1, decay, slope, tilt, cloud, cloud_hi, cloud_dec, jitter, bend, bend_ms, contact, grit, noise, noise_hz,
        noise_ms, flam, flam_ms, bounce, bounce_ms, body, body_f, body_ms, hp, lp, room, rt, width, jit;
};

const KnockP kKnocks[] = {
    // knock/snap (forge04 bank, material 'block'): a bright snap
    {1055.0, 0.00446, 0.9923, 0.4321, -53.72, 10.0, 0.4418, 0.01216, 4.0, 1.017, 0.004, 1.0, -21.06, 1316.0, 11.51,
     -15.46, 38.32, -11.12, 21.61, -51.53, 180.6, 23.32, 94.95, 5813.0, 0.03, 0.08, 0.2403, 0.2051},
    // knock/wood: the films' wooden knock
    {400.0, 0.00898, 1.4, 0.4048, -9.5, 6.712, 0.8552, 0.01331, 2.919, 3.488, 0.00076, 0.6466, 0.01577, 300.0, 0.3,
     -42.5, 12.92, -19.75, 44.74, -11.36, 102.0, 4.521, 107.2, 11210.0, 0.1648, 0.1519, 0.2037, 0.1328},
};
// The slit block's modes: ratio, amplitude, decay multiplier
const double kBlock[5][3] = {{1.0, 1.0, 1.0}, {1.48, 0.55, 0.6}, {2.15, 0.45, 0.45}, {3.4, 0.25, 0.3}, {4.6, 0.12, 0.2}};

Vec peak1(Vec y)
{
    double peak = 1e-12;
    for (double v : y) peak = std::max(peak, std::abs(v));
    for (double& v : y) v /= peak;
    return y;
}

Vec knockHit(const KnockP& p, Rng& r, size_t n)
{
    Vec fr, am, ta;
    for (const auto& m : kBlock) {
        const double f = p.f1 * m[0];
        if (f >= 0.42 * SR) continue;
        fr.push_back(f);
        am.push_back(m[1] * std::pow(m[0], p.tilt));
        ta.push_back(std::max(0.0008, p.decay * m[2] * std::pow(m[0], -p.slope)));
    }
    Vec y = modes(n, r, fr, am, ta, p.jitter, p.bend, p.bend_ms / 1000);
    if (p.cloud > -59) { // the grain: many weak modes over the main ones
        const double hi = std::min(p.f1 * p.cloud_hi, 0.4 * SR);
        Vec fc(28), tc(28), ac(28);
        for (int i = 0; i < 28; ++i) {
            const double lo = std::log(1.1 * p.f1), top = std::log(std::max(hi, 1.2 * p.f1));
            fc[size_t(i)] = std::exp(lo + (top - lo) * r.unit());
        }
        for (int i = 0; i < 28; ++i) {
            tc[size_t(i)] = std::max(0.0008, p.decay * p.cloud_dec * std::pow(fc[size_t(i)] / p.f1, -p.slope));
            ac[size_t(i)] = (0.3 + 0.7 * r.unit()) * std::pow(fc[size_t(i)] / p.f1, p.tilt - 0.5);
        }
        const Vec c = peak1(modes(n, r, fc, ac, tc, 0, p.bend, p.bend_ms / 1000));
        y = peak1(std::move(y));
        for (size_t i = 0; i < n; ++i) y[i] += std::pow(10.0, p.cloud / 20) * c[i];
    }
    y = peak1(strike(y, r, p.contact, p.grit));
    if (p.noise > -59) {
        const Vec b = burst(n, r, p.noise_hz / 2, p.noise_hz * 2, p.noise_ms / 1000, 0.0002);
        for (size_t i = 0; i < n; ++i) y[i] += std::pow(10.0, p.noise / 20) * b[i];
    }
    return y;
}

// ---------- subdrop ----------

struct DropP {
    double f0, f1, glide, glide_curve, bloom, hold, decay_db, length, harm, harm_lp, click, click_f, click_ms, thump,
        grit, width, room;
};

const DropP kDrops[] = {
    // pro_sub_drop/soft (forge_v2 bank; its bright and quick members scored 0.43-0.48 as real and are left out)
    {62.0, 44.0, 0.75, 1.3, 0.43, 0.08, 34.0, 1.4818, 0.12125, 160.033, 0.14804, 1440.02, 31.0003, 0.0, 0.05, 0.03, 0.05},
};

} // namespace

double riserLength(Riser m)
{
    return m >= Riser::Tension ? kBuilds[int(m) - int(Riser::Tension)].length : kRisers[int(m)].length;
}

Result riser(Riser member, unsigned seed, double length)
{
    if (member >= Riser::Tension)
        return build(kBuilds[int(member) - int(Riser::Tension)], std::uint64_t(seed) * 8 + std::uint64_t(member) + 101,
                     length);
    RiserP p = kRisers[int(member)];
    if (length > 0) p.length = length;
    Rng r(std::uint64_t(seed) * 8 + std::uint64_t(member) + 101);
    const double T = p.length;
    const size_t n = nOf(T + p.tail_tau * 60.0 / 8.686 + 0.02);
    // The build in dB, the accelerating stutter gate (open into the peak and after it)
    const double t0 = p.gate_at * T;
    Vec a(n), g(n), fc(n);
    double ph = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        const double u = std::clamp(t / T, 0.0, 1.0);
        const double L = t > T ? -8.686 * (t - T) / p.tail_tau : -p.start_db * std::pow(1.0 - u, p.curve);
        a[i] = std::pow(10.0, std::max(L, -120.0) / 20);
        const double v = std::clamp((t - t0) / std::max(T - t0, 1e-3), 0.0, 1.0);
        if (t >= t0) ph += p.gate_f0 * std::pow(p.gate_f1 / p.gate_f0, v) / SR;
        const double frac = std::fmod(ph + 0.5 * p.duty, 1.0);
        const double on = frac < p.duty ? 0.5 - 0.5 * std::cos(2 * kPi * std::min(frac / p.duty, 1.0)) : 0.0;
        double gv = t < t0 ? 1.0 : 1.0 - p.gate_depth * (1.0 - on);
        const double k = std::clamp((t - (T - 0.03)) / 0.03, 0.0, 1.0);
        gv = gv * (1 - k) + k;
        g[i] = t > T ? 1.0 : gv;
        fc[i] = p.body_f0 * std::pow(p.body_f1 / p.body_f0, std::pow(u, 1.5));
    }
    // Body: pink noise through a low-Q band pass that opens with the build
    // (high-passed at 90 Hz, unlike the original: noise below it buffets like wind on a microphone; the sub is a tone)
    Vec body = butter(svf(colored(n, r, 0.5), fc, p.body_q, 3), true, 90.0, 2);
    const double br = rmsOf(body) + 1e-12;
    // Sub: one pitch, a little drive
    const double sph = r.unit() * 2 * kPi, gd = 1.0 + 2.0 * p.sub_drive;
    // Crackle: sparse 3-8 ms Hann grains, denser towards the peak
    Vec crk(n, 0.0);
    for (double tg = 0;;) {
        const double rate = p.crackle_rate * (0.15 + 0.85 * std::pow(std::min(1.0, tg / T), 1.5));
        tg += -std::log(std::max(1e-12, 1 - r.unit())) / rate;
        if (tg >= T) break;
        const size_t k = nOf(0.003 + 0.005 * r.unit()), i0 = nOf(tg);
        const double level = std::pow(10.0, 12 * std::log10(0.05 + 0.95 * r.unit()) / 20);
        for (size_t j = 0; j < k && i0 + j < n; ++j)
            crk[i0 + j] += r.normal() * (0.5 - 0.5 * std::cos(2 * kPi * double(j) / double(k - 1))) * level;
    }
    crk = butter(crk, true, p.crackle_hp, 2);
    const double cr = rmsOf(crk) + 1e-12;
    Vec y(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        const double sub = std::tanh(gd * std::sin(2 * kPi * p.sub_f * t + sph)) / std::tanh(gd) / 0.7;
        y[i] = (body[i] / br + std::pow(10.0, p.sub_db / 20) * sub + std::pow(10.0, p.crackle_db / 20) * crk[i] / cr)
               * a[i] * g[i];
    }
    if (p.room > 0.01) {
        y = smallRoom(y, r, 0.35, 0.4, 0.6, p.room);
        y.resize(n);
    }
    y = butter(butter(y, true, 25.0, 2), false, 18000.0, 4);
    fade(y, 0.002, std::min(0.02, 0.5 * p.tail_tau * 60.0 / 8.686));
    return done(std::move(y), T);
}

Result click(Click member, unsigned seed)
{
    ShutterP p = kShutters[int(member)];
    Rng r(std::uint64_t(seed) * 8 + std::uint64_t(member) + 201);
    const double gap = p.gap * std::exp(0.03 * r.normal()); // each take: the mechanism a little off its last run
    p.b_hz *= std::exp(0.03 * r.normal());
    p.a_hz *= std::exp(0.03 * r.normal());
    const double tail = 7.0 * std::max(p.b_decay, p.body > -59 ? p.body_ms / 1000 : 0.0);
    const size_t n = nOf(std::min(1.0, gap + tail + 0.03));
    Vec y(n, 0.0);
    // Stage A: the release (a bright latch)
    const size_t ka = std::min(n, nOf(0.03));
    Vec a = burst(ka, r, p.a_hz / 1.8, std::min(p.a_hz * 1.8, 18000.0), p.a_ms / 1000, 0.0002);
    if (p.a_modes > 0.005) {
        Vec f, am, ta;
        for (const auto& m : kAModes) f.push_back(p.a_hz * m[0]), am.push_back(m[1]), ta.push_back(0.004 / m[0]);
        const Vec mm = peak1(modes(ka, r, f, am, ta, 0.01));
        for (size_t i = 0; i < ka; ++i) a[i] += p.a_modes * mm[i];
    }
    a = peak1(std::move(a));
    // Stage B: the landing (the box's modes, struck)
    const size_t kb = n - std::min(n, nOf(gap));
    Vec f, am, ta;
    for (const auto& m : kBModes)
        f.push_back(p.b_hz * m[0]), am.push_back(m[1]), ta.push_back(std::max(0.0008, p.b_decay * m[2]));
    Vec b = peak1(strike(modes(kb, r, f, am, ta, 0.01), r, p.b_contact, 0.6));
    if (p.b_noise > -39) {
        const Vec bn = burst(kb, r, p.b_noise_hz / 2.0, p.b_noise_hz * 2.0, 0.001, 0.0002);
        for (size_t i = 0; i < kb; ++i) b[i] += std::pow(10.0, p.b_noise / 20) * bn[i];
    }
    if (p.body > -59) {
        const Vec th = thump(kb, p.body_f, p.body_ms / 1000, 0.0008);
        for (size_t i = 0; i < kb; ++i) b[i] += std::pow(10.0, p.body / 20) * th[i];
    }
    if (p.rattle > -59) {
        const Vec rr = burst(kb, r, p.rattle_hz / 1.6, p.rattle_hz * 1.6, 0.006, 0.002);
        const size_t s = std::min(kb, nOf(0.004)); // 4 ms after the slap
        for (size_t i = s; i < kb; ++i) b[i] += std::pow(10.0, p.rattle / 20) * rr[i - s];
    }
    for (size_t i = 0; i < ka; ++i) y[i] += std::pow(10.0, p.a_db / 20) * a[i];
    for (size_t i = 0; i < kb; ++i) y[n - kb + i] += b[i];
    if (p.whirr > -59) { // the curtain travelling between the stages
        const size_t kw = std::min(n, nOf(gap));
        Vec w = bandpass(r.normals(kw), p.whirr_hz / 1.7, p.whirr_hz * 1.7);
        for (size_t i = 0; i < kw; ++i) w[i] *= std::pow(std::sin(kPi * double(i) / double(std::max<size_t>(1, kw - 1))), 0.7);
        w = peak1(std::move(w));
        for (size_t i = 0; i < kw; ++i) y[i] += std::pow(10.0, p.whirr / 20) * w[i];
    }
    return done(finish(std::move(y), r, p.hp, p.lp, p.room, p.rt), double(n - kb) / SR);
}

Result knock(Knock member, unsigned seed)
{
    KnockP p = kKnocks[int(member)];
    Rng r(std::uint64_t(seed) * 8 + std::uint64_t(member) + 301);
    p.f1 *= std::exp(0.02 * p.jit * r.normal()); // every knock lands a little differently
    p.decay *= std::exp(0.1 * p.jit * r.normal());
    p.contact *= std::exp(0.15 * p.jit * r.normal());
    const bool flam = p.flam > -59, bounce = p.bounce > -59;
    const double lead = flam ? p.flam_ms / 1000 : 0.0;
    double ring = 0;
    for (const auto& m : kBlock) ring = std::max(ring, p.decay * m[2] * std::pow(m[0], -p.slope));
    if (p.cloud > -59)
        ring = std::max(ring, p.decay * p.cloud_dec * std::max(std::pow(1.1, -p.slope), std::pow(p.cloud_hi, -p.slope)));
    const double dur = lead + (bounce ? p.bounce_ms / 1000 : 0.0)
                       + 7.0 * std::max(ring, p.body > -59 ? p.body_ms / 1000 : 0.0) + 0.02;
    const size_t n = nOf(std::min(1.0, dur));
    Vec y(n, 0.0);
    const size_t k = flam ? std::min(n - 1, nOf(lead)) : 0;
    const Vec main = knockHit(p, r, n - k);
    for (size_t i = 0; i < main.size(); ++i) y[k + i] += main[i];
    if (flam) { // a softer first contact
        KnockP q = p;
        q.contact *= 1.5;
        const Vec f = knockHit(q, r, n);
        for (size_t i = 0; i < n; ++i) y[i] += std::pow(10.0, p.flam / 20) * f[i];
    }
    if (bounce) { // a softer contact after it
        const size_t kb = k + nOf(p.bounce_ms / 1000);
        if (kb + 8 < n) {
            KnockP q = p;
            q.contact *= 1.3;
            const Vec b = knockHit(q, r, n - kb);
            for (size_t i = 0; i < b.size(); ++i) y[kb + i] += std::pow(10.0, p.bounce / 20) * b[i];
        }
    }
    if (p.body > -59) { // the block / table under it
        const Vec tb = thump(n - k, p.body_f * (1 + 0.03 * r.normal()), p.body_ms / 1000, 0.001);
        for (size_t i = 0; i < tb.size(); ++i) y[k + i] += std::pow(10.0, p.body / 20) * tb[i];
    }
    return done(finish(std::move(y), r, p.hp, p.lp, p.room, p.rt), double(k) / SR);
}

Result drop(Drop member, unsigned seed)
{
    const DropP& p = kDrops[int(member)];
    Rng r(std::uint64_t(seed) * 8 + std::uint64_t(member) + 401);
    const double life = p.bloom + p.hold + 70.0 / p.decay_db; // never past the decay's -70 dB
    const double seconds = std::max(0.3, std::min(p.length, life));
    const size_t n = nOf(seconds);
    Vec f(n), ph(n), env(n);
    double phase = r.unit() * 0.2;
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        const double s = std::pow(std::clamp(t / std::max(p.glide, 1e-3), 0.0, 1.0), p.glide_curve);
        f[i] = std::exp(std::log(p.f0) + (std::log(p.f1) - std::log(p.f0)) * s);
        phase += 2 * kPi * f[i] / SR;
        ph[i] = phase;
        env[i] = t < p.bloom ? 0.5 - 0.5 * std::cos(kPi * t / p.bloom)
                 : t > p.bloom + p.hold ? std::pow(10.0, -p.decay_db * (t - p.bloom - p.hold) / 20)
                                        : 1.0;
    }
    // Harmonics 2-6 of the gliding sub for small speakers, nothing above 300 Hz
    Vec harm(n, 0.0);
    if (p.harm > 0.01) {
        for (size_t i = 0; i < n; ++i) {
            double h = 0;
            for (int k = 2; k <= 6; ++k) {
                const double fk = k * f[i], taper = std::clamp((300.0 - fk) / 80.0, 0.0, 1.0);
                h += (0.6 / k) / (1 + std::pow(fk / p.harm_lp, 4)) * std::sin(k * ph[i] + 0.4 * k) * taper;
            }
            harm[i] = h * env[i];
        }
        harm = peak1(std::move(harm));
        for (double& v : harm) v *= p.harm;
    }
    // (The original's grit, low brown noise under the sub, is left out: the low end stays a clean tone decay)
    // The click on top: a noise burst plus a short ping, and a kick-like thump, in a small dark room
    Vec top(n, 0.0);
    {
        const size_t L = std::min(n, std::max<size_t>(16, nOf(p.click_ms / 1000)));
        Vec w(L);
        for (size_t i = 0; i < L; ++i)
            w[i] = r.normal() * std::pow(1 - double(i) / double(L), 3) * (1 - std::exp(-double(i) / (SR * 0.0002)));
        w = peak1(butter(std::move(w), true, p.click_f * 0.5, 2));
        Vec c(n, 0.0);
        for (size_t i = 0; i < L; ++i) c[i] += w[i];
        for (size_t i = 0; i < std::min(n, nOf(0.02)); ++i) {
            const double t = double(i) / SR;
            c[i] += 0.6 * std::sin(2 * kPi * p.click_f * t) * std::exp(-t / 0.003) * (1 - std::exp(-t / 0.0003));
        }
        // Softer than the original: the click low-passed at 4 kHz and at 60 % (a hard bright click on a boom crackles)
        c = peak1(butter(std::move(c), false, 4000.0, 2));
        for (size_t i = 0; i < n; ++i) top[i] = 0.6 * p.click * c[i];
        if (p.thump > 0) {
            const double off = r.unit() * 0.3;
            double tp = 0;
            Vec th(n);
            for (size_t i = 0; i < n; ++i) {
                const double t = double(i) / SR;
                tp += 2 * kPi * (90 + 60 * std::exp(-t / 0.03)) / SR;
                th[i] = std::sin(tp + off) * std::exp(-t / 0.06) * (1 - std::exp(-t / 0.001));
            }
            fade(th, 0.0, 0.01);
            for (size_t i = 0; i < n; ++i) top[i] += 0.7 * p.thump * th[i];
        }
        if (p.room > 0.01) {
            top = smallRoom(top, r, 0.35, 0.4, 0.7, p.room);
            top.resize(n);
        }
    }
    // An inaudible floor (-80 dB, 300 Hz - 12 kHz) under the envelope
    Vec fl = peak1(bandpass(r.normals(n), 300.0, 12000.0));
    Vec y(n);
    for (size_t i = 0; i < n; ++i)
        y[i] = std::sin(ph[i]) * env[i] + harm[i] + top[i] + 1e-4 * fl[i] * std::max(env[i], 0.01);
    y = butter(std::move(y), true, 22.0, 2);
    fade(y, 0.0015, std::max(0.05, 0.15 * seconds));
    return done(std::move(y), 0.0);
}

} // namespace SfxForge
