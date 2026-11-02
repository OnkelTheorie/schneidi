// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/VoiceClean.h"

#include <rnnoise.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <numeric>
#include <vector>

namespace VoiceClean {

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = 480; // 10 ms, given by RNNoise
constexpr int kCh = 2;

float dbToLin(float db) { return std::pow(10.f, db / 20.f); }
// Smoothing coefficient of a one-pole filter with time constant ms
float coef(float ms) { return std::exp(-1.f / (ms * 0.001f * kRate)); }

// mici's "Empfohlen" (recommended) preset
constexpr float kHighPassHz = 100.f;
constexpr float kThumpSensitivity = 0.5f;
constexpr float kVadThreshold = 0.5f;
constexpr int kVadHoldFrames = 30; // 300 ms
constexpr float kVadFloorDb = -45.f;
constexpr float kClickSensitivity = 0.5f;
constexpr float kGateMarginDb = 25.f;
constexpr float kPresenceDb = 2.f;
constexpr float kCompThresholdDb = -24.f, kCompRatio = 3.f, kCompMakeupDb = 6.f;
constexpr float kDeessThresholdDb = -28.f;
constexpr float kLimitCeilingDb = -1.f;

// Bump damper: level in 1 ms blocks against the level 30-80 ms before; the sound runs kThumpLook behind so the gain
// is down before the bump
constexpr int kThumpLook = 192; // 4 ms
constexpr int kThumpBlocks = 80;
// RNNoise (v0.2) returns a frame two calls later
constexpr int kRnnDelay = 2 * kFrame;
// Look-ahead of the voice gate and the click damper: the decision knows the next 40 ms
constexpr int kLookFrames = 2;
constexpr int kVadLead = 2 + kLookFrames;
constexpr int kVadHist = 16;
constexpr int kClickLook = 96; // 2 ms: lowered before the click
constexpr int kLookDelay = kLookFrames * kFrame + kClickLook;
constexpr int kRing = 2048;
constexpr int kLimLook = 64;
// Delay of the chain at 48 kHz (every stage delays always, so it does not depend on the parameters)
constexpr int kChainDelay = kThumpLook + kRnnDelay + kLookDelay + (kLimLook - 1);

const float kGateAttack = coef(1.f);
const float kGateRelease = coef(80.f);
const float kGateEnvRelease = coef(50.f);
const int kGateHoldSamples = kRate * 150 / 1000;
// The level gate learns only speech at most this much below the voice so far (voices in the background do not pull
// the threshold down); quieter speech the gate lets through lowers the learned level by 1 dB/s
constexpr float kGateVoiceWindowDb = 10.f;
constexpr float kGateVoiceSinkDb = 0.01f;
constexpr float kGateVoiceRise = 0.05f;
constexpr float kGateVoiceStartDb = -40.f;
const float kVadAttack = coef(3.f);
const float kVadRelease = coef(60.f);
const float kClickFastRelease = coef(1.f);
const float kClickSlow = coef(30.f);
const float kClickAttack = coef(0.3f);
const float kClickRelease = coef(15.f);
const float kClickFloor = dbToLin(-30.f);
const float kClickMinLevel = dbToLin(-60.f);
const int kClickHoldSamples = kClickLook + kRate * 30 / 1000;
const float kCompAttack = coef(5.f);
const float kCompRelease = coef(120.f);
constexpr float kCompKneeDb = 6.f;
const float kDeessAttack = coef(1.f);
const float kDeessRelease = coef(60.f);
constexpr float kDeessFreq = 5000.f;
constexpr float kDeessMaxDb = 12.f;
const float kLimAttack = coef(0.3f);
const float kLimRelease = coef(60.f);
const float kThumpAttack = coef(0.7f);
const float kThumpRelease = coef(25.f);
const float kThumpEnvRelease = coef(20.f);
const float kThumpFloor = dbToLin(-40.f);      // level left during a bump
const float kThumpMinLevel = dbToLin(-40.f);   // quieter jumps do not matter
const float kThumpVoiceMargin = dbToLin(6.f);  // a bump is this much louder than the voice so far
const float kThumpVoiceDecay = dbToLin(-0.01f); // per ms: 10 dB/s
const float kThumpRingLevel = dbToLin(-50.f);  // quieter rumble is left to the high pass
const int kThumpMinSamples = kRate * 120 / 1000;
const float kThumpBassRatio = dbToLin(10.f);   // bass this much above the highs = the arm still rings
const int kThumpQuietSamples = kRate * 25 / 1000;
const int kThumpMaxSamples = kRate * 3;

// Second-order section (transposed direct form II), RBJ cookbook coefficients at 48 kHz
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void pass(bool high, float freq, float q)
    {
        const float w0 = 2.f * std::numbers::pi_v<float> * freq / kRate;
        const float cw = std::cos(w0), alpha = std::sin(w0) / (2.f * q), a0 = 1.f + alpha;
        b0 = (high ? 1.f + cw : 1.f - cw) / 2.f / a0;
        b1 = (high ? -(1.f + cw) : 1.f - cw) / a0;
        b2 = b0;
        a1 = -2.f * cw / a0;
        a2 = (1.f - alpha) / a0;
    }
    void highpass1(float freq)
    {
        const float k = std::tan(std::numbers::pi_v<float> * freq / kRate);
        b0 = 1.f / (1.f + k);
        b1 = -b0;
        b2 = a2 = 0;
        a1 = (k - 1.f) / (1.f + k);
    }
    void peak(float freq, float q, float gainDb)
    {
        const float A = std::pow(10.f, gainDb / 40.f);
        const float w0 = 2.f * std::numbers::pi_v<float> * freq / kRate;
        const float cw = std::cos(w0), alpha = std::sin(w0) / (2.f * q), a0 = 1.f + alpha / A;
        b0 = (1.f + alpha * A) / a0;
        b1 = -2.f * cw / a0;
        b2 = (1.f - alpha * A) / a0;
        a1 = b1;
        a2 = (1.f - alpha / A) / a0;
    }
    void clear() { z1 = z2 = 0; }
    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2 + 1e-20f; // against denormals
        z2 = b2 * x - a2 * y;
        return y;
    }
};

// The chain at 48 kHz on stereo frames of kFrame samples
class Chain {
public:
    Chain()
    {
        for (int c = 0; c < kCh; ++c) {
            m_rnn[c] = rnnoise_create(nullptr);
            m_hp[c].pass(true, kHighPassHz, std::numbers::sqrt2_v<float> / 2);
            m_presence[c].peak(4000.f, 1.f, kPresenceDb);
            m_deessSplit[c].highpass1(kDeessFreq);
        }
        m_deessDetect.pass(true, kDeessFreq, std::numbers::sqrt2_v<float> / 2);
        m_thumpLow.pass(false, 200.f, std::numbers::sqrt2_v<float> / 2);
        m_thumpHigh.pass(true, 300.f, std::numbers::sqrt2_v<float> / 2);
        m_clickHp.pass(true, 2000.f, std::numbers::sqrt2_v<float> / 2);
        reset();
    }
    ~Chain()
    {
        for (DenoiseState* s : m_rnn)
            if (s) rnnoise_destroy(s);
    }
    Chain(const Chain&) = delete;
    Chain& operator=(const Chain&) = delete;

    void setParams(const Params& p)
    {
        m_p = p;
        if (m_fresh) m_mix = float(std::clamp(p.amount, 0.0, 1.0)); // a change later glides over a frame
    }

    void reset()
    {
        for (int c = 0; c < kCh; ++c) {
            if (m_rnn[c]) rnnoise_init(m_rnn[c], nullptr);
            m_hp[c].clear();
            m_presence[c].clear();
            m_deessSplit[c].clear();
        }
        m_twin = true;
        m_fresh = true;
        std::memset(m_thumpDelay, 0, sizeof m_thumpDelay);
        m_thumpPos = 0;
        std::fill(std::begin(m_thumpBlocks), std::end(m_thumpBlocks), 1.f); // no false alarm right at the start
        m_thumpBlockPos = m_thumpBlockFill = 0;
        m_thumpBlockPk = 0;
        m_thumpRef = 1.f;
        m_thumpVoice = m_thumpEnv = m_thumpLowEnv = m_thumpHighEnv = 0;
        m_thumpLow.clear();
        m_thumpHigh.clear();
        m_thumpAge = -1;
        m_thumpQuiet = 0;
        m_thumpGain = 1.f;
        std::memset(m_dry, 0, sizeof m_dry);
        m_dryPos = 0;
        m_mix = float(std::clamp(m_p.amount, 0.0, 1.0));
        std::memset(m_look, 0, sizeof m_look);
        m_lookPos = 0;
        std::memset(m_vadHist, 0, sizeof m_vadHist);
        m_vadT = 0;
        m_vadPrev = 0;
        m_sinceSpeech = 1 << 20;
        m_vadOpen = false;
        m_vadGain = 1.f;
        m_clickHp.clear();
        m_clickFast = m_clickSlow = 0;
        m_clickHold = 0;
        m_clickGain = 1.f;
        m_gateEnv = 0;
        m_gateHold = 0;
        m_gateOpen = true;
        m_gateGain = 1.f;
        m_gateNoiseDb = -70.f;
        m_gateVoiceDb = kGateVoiceStartDb;
        m_compEnv = 0;
        m_deessDetect.clear();
        m_deessEnv = 0;
        std::memset(m_limDelay, 0, sizeof m_limDelay);
        std::fill(std::begin(m_limReq), std::end(m_limReq), 1.f);
        m_limPos = 0;
        m_limGain = 1.f;
    }

    // In place, two channels of kFrame samples
    void processFrame(float* const* b)
    {
        m_fresh = false;
        thump(b);
        if (m_p.shape)
            for (int c = 0; c < kCh; ++c)
                for (int i = 0; i < kFrame; ++i) b[c][i] = m_hp[c].process(b[c][i]);
        else
            for (Biquad& f : m_hp) f.clear();
        const float vad = suppress(b);
        gate(b, vad);
        if (m_p.shape) shape(b);
        limit(b);
    }

private:
    // A bump against the microphone or its arm jumps 30 dB and more within ~1 ms, speech even at P and T at most
    // ~25 dB; it also has to be clearly louder than the voice so far. Detection on both channels together.
    void thump(float* const* b)
    {
        const bool on = m_p.clicks;
        const float jump = dbToLin(36.f - 16.f * kThumpSensitivity);
        for (int i = 0; i < kFrame; ++i) {
            const float a = std::max(std::fabs(b[0][i]), std::fabs(b[1][i]));
            const float mid = 0.5f * (b[0][i] + b[1][i]);
            m_thumpEnv = std::max(a, m_thumpEnv * kThumpEnvRelease);
            m_thumpLowEnv = std::max(std::fabs(m_thumpLow.process(mid)), m_thumpLowEnv * kThumpEnvRelease);
            m_thumpHighEnv = std::max(std::fabs(m_thumpHigh.process(mid)), m_thumpHighEnv * kThumpEnvRelease);
            m_thumpBlockPk = std::max(m_thumpBlockPk, a);
            if (++m_thumpBlockFill == kRate / 1000) {
                m_thumpBlocks[m_thumpBlockPos] = m_thumpBlockPk;
                m_thumpBlockPos = (m_thumpBlockPos + 1) % kThumpBlocks;
                m_thumpBlockPk = 0;
                m_thumpBlockFill = 0;
                // Reference: loudest block 30-80 ms ago (the oldest is now at m_thumpBlockPos)
                float ref = 0;
                for (int k = 0; k < kThumpBlocks - 30; ++k)
                    ref = std::max(ref, m_thumpBlocks[(m_thumpBlockPos + k) % kThumpBlocks]);
                m_thumpRef = ref;
                // Voice level so far, 30 ms behind like the reference; bumps do not count
                const float blk = m_thumpBlocks[(m_thumpBlockPos + kThumpBlocks - 31) % kThumpBlocks];
                m_thumpVoice = m_thumpAge < 0 ? std::max(blk, m_thumpVoice * kThumpVoiceDecay)
                                              : m_thumpVoice * kThumpVoiceDecay;
            }
            if (on && a > kThumpMinLevel && a > m_thumpRef * jump && a > m_thumpVoice * kThumpVoiceMargin) {
                m_thumpAge = 0;
                m_thumpQuiet = 0;
            }
            if (m_thumpAge >= 0) {
                ++m_thumpAge;
                // Over when the rumble has faded or the highs are back (speech)
                const bool ringing = m_thumpLowEnv > kThumpRingLevel && m_thumpLowEnv > m_thumpHighEnv * kThumpBassRatio;
                m_thumpQuiet = ringing ? 0 : m_thumpQuiet + 1;
                if (!on || m_thumpAge > kThumpMaxSamples
                    || (m_thumpAge > kThumpMinSamples && m_thumpQuiet > kThumpQuietSamples))
                    m_thumpAge = -1;
            }
            const float target = m_thumpAge >= 0 ? kThumpFloor : 1.f;
            m_thumpGain = target + (m_thumpGain - target) * (target < m_thumpGain ? kThumpAttack : kThumpRelease);
            for (int c = 0; c < kCh; ++c) {
                const float y = m_thumpDelay[c][m_thumpPos];
                m_thumpDelay[c][m_thumpPos] = b[c][i];
                b[c][i] = y * m_thumpGain;
            }
            m_thumpPos = (m_thumpPos + 1) % kThumpLook;
        }
    }

    // RNNoise per channel (once while both channels carry the same sound), mixed with the equally delayed dry sound;
    // returns the voice activity (0..1)
    float suppress(float* const* b)
    {
        const bool same = m_twin && std::memcmp(b[0], b[1], kFrame * sizeof(float)) == 0;
        if (m_twin && !same) { // the second channel's state is the first one's so far
            std::memcpy(static_cast<void*>(m_rnn[1]), m_rnn[0], size_t(rnnoise_get_size()));
            m_twin = false;
        }
        float vad = 0;
        float wet[kCh][kFrame];
        for (int c = 0; c < (same ? 1 : kCh); ++c) {
            for (int i = 0; i < kFrame; ++i) wet[c][i] = b[c][i] * 32768.f;
            vad = std::max(vad, rnnoise_process_frame(m_rnn[c], wet[c], wet[c]));
        }
        if (same) std::memcpy(wet[1], wet[0], sizeof wet[0]);
        const float target = float(std::clamp(m_p.amount, 0.0, 1.0));
        const float step = (target - m_mix) / kFrame;
        for (int c = 0; c < kCh; ++c) {
            float* dry = m_dry[c] + m_dryPos * kFrame; // the frame of two calls ago
            float mix = m_mix;
            for (int i = 0; i < kFrame; ++i) {
                const float d = dry[i];
                dry[i] = b[c][i];
                mix += step;
                b[c][i] = mix * wet[c][i] / 32768.f + (1.f - mix) * d;
            }
        }
        m_mix = target;
        m_dryPos ^= 1;
        return vad;
    }

    // Look-ahead delay with the click damper, the level gate (threshold learned below the voice) and the voice gate
    void gate(float* const* b, float vad)
    {
        const bool gateOn = m_p.gate, clickOn = m_p.clicks;
        // The frame going out now is the one of kVadLead calls ago: the decision knows the next 40 ms
        m_vadHist[m_vadT % kVadHist] = vad;
        // Opens on two frames above the threshold or one clearly above (single noises do not count); stays open
        // down to 60 % of the threshold
        const float thr = kVadThreshold;
        const bool speech = (vad >= thr && m_vadPrev >= thr) || vad >= thr + (1.f - thr) * 0.5f
                            || (m_vadOpen && vad >= thr * 0.6f);
        m_vadPrev = vad;
        m_sinceSpeech = speech ? 0 : std::min(m_sinceSpeech + 1, 1 << 20);
        m_vadOpen = m_sinceSpeech <= kVadHoldFrames + kVadLead;
        float vadNear = 0.f, vadOut = 0.f; // around the frame going out (and the coming ones)
        for (int k = 0; k <= kVadLead + 1; ++k) {
            const float v = m_vadHist[(m_vadT + kVadHist - k) % kVadHist];
            vadNear = std::max(vadNear, v);
            if (k == kVadLead || k == kVadLead - 1) vadOut = std::max(vadOut, v);
        }
        ++m_vadT;

        const float vadTarget = !gateOn || m_vadOpen ? 1.f : dbToLin(kVadFloorDb);
        const float clickRatio = dbToLin(20.f - 12.f * kClickSensitivity);
        const bool clickArmed = clickOn && vadNear < 0.5f;
        // Level gate: just below the voice, but always above the noise floor
        const float gateThrDb = std::clamp(std::max(m_gateVoiceDb - kGateMarginDb, m_gateNoiseDb + 6.f), -80.f, -15.f);
        const float openLin = dbToLin(gateThrDb), closeLin = openLin * 0.63f; // 4 dB hysteresis

        float framePk = 0.f;
        constexpr int mask = kRing - 1;
        for (int i = 0; i < kFrame; ++i) {
            float y[kCh];
            for (int c = 0; c < kCh; ++c) {
                m_look[c][m_lookPos] = b[c][i];
                y[c] = m_look[c][(m_lookPos - kLookDelay) & mask];
            }
            if (clickOn) {
                const int ahead = (m_lookPos - kLookDelay + kClickLook) & mask;
                const float a = std::fabs(m_clickHp.process(0.5f * (m_look[0][ahead] + m_look[1][ahead])));
                m_clickFast = std::max(a, m_clickFast * kClickFastRelease);
                m_clickSlow = a + (m_clickSlow - a) * kClickSlow + 1e-20f;
                if (clickArmed && m_clickFast > kClickMinLevel && m_clickFast > m_clickSlow * clickRatio)
                    m_clickHold = kClickHoldSamples;
                const float target = m_clickHold > 0 ? kClickFloor : 1.f;
                if (m_clickHold > 0) --m_clickHold;
                m_clickGain = target + (m_clickGain - target) * (target < m_clickGain ? kClickAttack : kClickRelease);
                for (float& v : y) v *= m_clickGain;
            } else {
                m_clickHp.clear();
                m_clickFast = m_clickSlow = 0;
                m_clickHold = 0;
                m_clickGain = 1.f;
            }
            m_lookPos = (m_lookPos + 1) & mask;

            if (gateOn) {
                const float a = std::max(std::fabs(y[0]), std::fabs(y[1]));
                m_gateEnv = a > m_gateEnv ? a : m_gateEnv * kGateEnvRelease;
                framePk = std::max(framePk, m_gateEnv);
                if (m_gateEnv > openLin) {
                    m_gateOpen = true;
                    m_gateHold = kGateHoldSamples;
                } else if (m_gateEnv < closeLin) {
                    if (m_gateHold > 0) --m_gateHold;
                    else m_gateOpen = false;
                }
                const float target = m_gateOpen ? 1.f : 0.f;
                m_gateGain = target + (m_gateGain - target) * (target > m_gateGain ? kGateAttack : kGateRelease);
            }
            m_vadGain = vadTarget + (m_vadGain - vadTarget) * (vadTarget > m_vadGain ? kVadAttack : kVadRelease);
            const float g = (gateOn ? m_gateGain : 1.f) * m_vadGain;
            for (int c = 0; c < kCh; ++c) b[c][i] = y[c] * g;
        }
        if (!gateOn) {
            m_vadGain = 1.f;
            m_gateOpen = true;
            m_gateGain = 1.f;
            return;
        }
        // Noise floor: falls fast, rises slowly (3 dB/s) - minimum tracking. Voice: only on sure speech; near the
        // voice so far it rises fast and falls slowly, quieter speech above the threshold lowers it by 1 dB/s
        const float lvl = 20.f * std::log10(framePk + 1e-9f);
        m_gateNoiseDb = lvl < m_gateNoiseDb ? m_gateNoiseDb + (lvl - m_gateNoiseDb) * 0.3f : m_gateNoiseDb + 0.03f;
        m_gateNoiseDb = std::max(m_gateNoiseDb, -100.f);
        if (vadOut >= 0.8f && lvl > m_gateNoiseDb + 10.f) {
            if (lvl > m_gateVoiceDb - kGateVoiceWindowDb)
                m_gateVoiceDb += (lvl - m_gateVoiceDb) * (lvl > m_gateVoiceDb ? kGateVoiceRise : 0.01f);
            else if (lvl > gateThrDb)
                m_gateVoiceDb -= kGateVoiceSinkDb;
        }
    }

    // Presence, compressor (stereo-linked, soft knee), de-esser (lowers only the highs)
    void shape(float* const* b)
    {
        const float slope = 1.f / kCompRatio - 1.f;
        for (int i = 0; i < kFrame; ++i) {
            for (int c = 0; c < kCh; ++c) b[c][i] = m_presence[c].process(b[c][i]);
            const float a = std::max(std::fabs(b[0][i]), std::fabs(b[1][i]));
            m_compEnv = a + (m_compEnv - a) * (a > m_compEnv ? kCompAttack : kCompRelease) + 1e-20f;
            const float over = 20.f * std::log10(m_compEnv + 1e-9f) - kCompThresholdDb;
            float gr = 0;
            if (2.f * std::fabs(over) <= kCompKneeDb)
                gr = slope * (over + kCompKneeDb / 2) * (over + kCompKneeDb / 2) / (2 * kCompKneeDb);
            else if (2.f * over > kCompKneeDb)
                gr = slope * over;
            const float g = dbToLin(gr + kCompMakeupDb);
            for (int c = 0; c < kCh; ++c) b[c][i] *= g;

            const float d = std::fabs(m_deessDetect.process(0.5f * (b[0][i] + b[1][i])));
            m_deessEnv = d + (m_deessEnv - d) * (d > m_deessEnv ? kDeessAttack : kDeessRelease) + 1e-20f;
            const float dOver = 20.f * std::log10(m_deessEnv + 1e-9f) - kDeessThresholdDb;
            const float dGr = dOver > 0 ? std::min(dOver * 0.75f, kDeessMaxDb) : 0.f; // 4 : 1
            const float keep = 1.f - dbToLin(-dGr);
            for (int c = 0; c < kCh; ++c) {
                const float h = m_deessSplit[c].process(b[c][i]); // x - first-order high pass = low pass: never boosts
                b[c][i] -= h * keep;
            }
        }
    }

    // Look-ahead limiter (hard ceiling when shaping); the delay runs always (same latency)
    void limit(float* const* b)
    {
        const bool on = m_p.shape;
        const float ceil = dbToLin(kLimitCeilingDb);
        for (int i = 0; i < kFrame; ++i) {
            const float a = std::max(std::fabs(b[0][i]), std::fabs(b[1][i]));
            for (int c = 0; c < kCh; ++c) m_limDelay[c][m_limPos] = b[c][i];
            m_limReq[m_limPos] = on && a > ceil ? ceil / a : 1.f;
            m_limPos = (m_limPos + 1) % kLimLook;
            float need = 1.f;
            for (float r : m_limReq) need = std::min(need, r);
            m_limGain = need + (m_limGain - need) * (need < m_limGain ? kLimAttack : kLimRelease);
            for (int c = 0; c < kCh; ++c) {
                const float y = m_limDelay[c][m_limPos] * m_limGain; // kLimLook - 1 samples old
                b[c][i] = on ? std::clamp(y, -ceil, ceil) : y;
            }
        }
    }

    Params m_p;
    DenoiseState* m_rnn[kCh] = {};
    bool m_fresh = true; // nothing processed since reset()
    bool m_twin = true; // both channels have carried the same sound so far (RNNoise ran only on the first)

    float m_thumpDelay[kCh][kThumpLook] = {};
    int m_thumpPos = 0;
    float m_thumpBlocks[kThumpBlocks] = {};
    int m_thumpBlockPos = 0, m_thumpBlockFill = 0;
    float m_thumpBlockPk = 0, m_thumpRef = 1.f, m_thumpVoice = 0, m_thumpEnv = 0;
    Biquad m_thumpLow, m_thumpHigh;
    float m_thumpLowEnv = 0, m_thumpHighEnv = 0;
    int m_thumpAge = -1, m_thumpQuiet = 0;
    float m_thumpGain = 1.f;

    Biquad m_hp[kCh];
    float m_dry[kCh][2 * kFrame] = {};
    int m_dryPos = 0;
    float m_mix = 1.f;

    float m_look[kCh][kRing] = {};
    int m_lookPos = 0;
    float m_vadHist[kVadHist] = {};
    unsigned m_vadT = 0;
    float m_vadPrev = 0;
    int m_sinceSpeech = 1 << 20;
    bool m_vadOpen = false;
    float m_vadGain = 1.f;
    Biquad m_clickHp;
    float m_clickFast = 0, m_clickSlow = 0;
    int m_clickHold = 0;
    float m_clickGain = 1.f;
    float m_gateEnv = 0;
    int m_gateHold = 0;
    bool m_gateOpen = true;
    float m_gateGain = 1.f, m_gateNoiseDb = -70.f, m_gateVoiceDb = kGateVoiceStartDb;

    Biquad m_presence[kCh];
    float m_compEnv = 0;
    Biquad m_deessDetect, m_deessSplit[kCh];
    float m_deessEnv = 0;

    float m_limDelay[kCh][kLimLook] = {};
    float m_limReq[kLimLook] = {};
    int m_limPos = 0;
    float m_limGain = 1.f;
};

int64_t floorDiv(int64_t a, int64_t b)
{
    const int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// Cutoff of the resampling a little below the lower Nyquist frequency (relative to the input rate), and the taps on
// each side
double cutoff(int from, int to) { return 0.94 * std::min(1.0, double(to) / from); }
int halfWidth(int from, int to) { return int(std::ceil(12.0 / cutoff(from, to))); }

// Polyphase windowed-sinc interpolation from rate `from` to rate `to`: output j lies at input position j·from/to
struct Resampler {
    int64_t num = 1, den = 1; // position = j·num/den (reduced)
    int half = 0;             // taps on each side
    std::vector<float> taps;  // den phases × 2·half: tap k weighs input index i - half + 1 + k

    void build(int from, int to)
    {
        const int g = std::gcd(from, to);
        num = from / g;
        den = to / g;
        const double cut = cutoff(from, to);
        half = halfWidth(from, to);
        taps.assign(size_t(den) * size_t(2 * half), 0.f);
        for (int64_t p = 0; p < den; ++p) {
            const double frac = double(p) / double(den);
            float* t = taps.data() + p * 2 * half;
            double sum = 0;
            for (int k = 0; k < 2 * half; ++k) {
                const double x = k - half + 1 - frac; // distance from the wanted position in input samples
                const double s = x == 0 ? 1.0 : std::sin(std::numbers::pi * cut * x) / (std::numbers::pi * cut * x);
                const double w = std::abs(x) >= half ? 0.0
                                                     : 0.42 + 0.5 * std::cos(std::numbers::pi * x / half)
                                                           + 0.08 * std::cos(2 * std::numbers::pi * x / half);
                t[k] = float(s * w);
                sum += s * w;
            }
            for (int k = 0; k < 2 * half; ++k) t[k] = float(t[k] / sum); // unity gain at DC
        }
    }
};

// Samples with a global index; old ones are dropped
struct History {
    std::vector<float> data[kCh];
    int64_t base = 0; // global index of data[c][0]
    int64_t end() const { return base + int64_t(data[0].size()); }
    float at(int c, int64_t i) const
    {
        return i < base || i >= end() ? 0.f : data[c][size_t(i - base)];
    }
    void dropBefore(int64_t i)
    {
        const int64_t n = std::min(i - base, int64_t(data[0].size()));
        if (n < 8192) return; // in larger steps
        for (auto& d : data) d.erase(d.begin(), d.begin() + n);
        base += n;
    }
    void clear(int64_t at)
    {
        for (auto& d : data) d.clear();
        base = at;
    }
};

} // namespace

int latencyFor(int sampleRate)
{
    const int rate = std::max(1000, sampleRate);
    // 48 kHz: output n is the chain's output n - latency + kChainDelay, ready once the frame holding it is complete
    if (rate == kRate) return kChainDelay + kFrame - 1;
    // Output n reads the chain output up to (n - L)·48000/rate + kChainDelay + down taps; its frame needs the chain
    // input up to kFrame - 1 later, which needs the input up to that·rate/48000 + up taps <= n
    const int64_t chain = kChainDelay + halfWidth(kRate, rate) + kFrame - 1;
    return int((chain * rate + kRate - 1) / kRate) + halfWidth(rate, kRate);
}

struct Processor::Impl {
    int rate;
    bool direct;
    Resampler up, down; // rate -> 48 kHz, 48 kHz -> rate
    int latency = 0;
    Chain chain;
    History in;    // input at `rate`
    History clean; // chain output at 48 kHz
    int64_t upNext = 0;   // next 48 kHz sample to feed the chain
    int64_t outNext = 0;  // next output sample
    float frame[kCh][kFrame];
    int fill = 0;

    explicit Impl(int r) : rate(std::max(1000, r)), direct(rate == kRate)
    {
        if (!direct) {
            up.build(rate, kRate);
            down.build(kRate, rate);
        }
        latency = latencyFor(rate);
        reset();
    }

    void reset()
    {
        chain.reset();
        in.clear(0);
        clean.clear(0);
        upNext = outNext = 0;
        fill = 0;
    }

    void feed(const float* l, const float* r)
    {
        // One chain sample; a full frame goes through the chain
        frame[0][fill] = *l;
        frame[1][fill] = *r;
        if (++fill < kFrame) return;
        fill = 0;
        float* b[kCh] = {frame[0], frame[1]};
        chain.processFrame(b);
        for (int c = 0; c < kCh; ++c) clean.data[c].insert(clean.data[c].end(), frame[c], frame[c] + kFrame);
    }

    void process(float* const* data, int n)
    {
        if (n <= 0) return;
        if (direct) {
            for (int i = 0; i < n; ++i) feed(&data[0][i], &data[1][i]);
            for (int i = 0; i < n; ++i, ++outNext)
                for (int c = 0; c < kCh; ++c) data[c][i] = clean.at(c, outNext - latency + kChainDelay);
            clean.dropBefore(outNext - latency + kChainDelay);
            return;
        }
        for (int c = 0; c < kCh; ++c) in.data[c].insert(in.data[c].end(), data[c], data[c] + n);
        // Up to 48 kHz as far as the input reaches
        for (;; ++upNext) {
            const int64_t pos = upNext * up.num;
            const int64_t i = pos / up.den, p = pos % up.den;
            if (i + up.half >= in.end()) break;
            const float* t = up.taps.data() + p * 2 * up.half;
            float v[kCh] = {0, 0};
            for (int k = 0; k < 2 * up.half; ++k)
                for (int c = 0; c < kCh; ++c) v[c] += t[k] * in.at(c, i - up.half + 1 + k);
            feed(&v[0], &v[1]);
        }
        in.dropBefore(upNext * up.num / up.den - up.half);
        // Back to the rate
        for (int j = 0; j < n; ++j, ++outNext) {
            const int64_t pos = (outNext - latency) * down.num;
            const int64_t q = floorDiv(pos, down.den);
            const int64_t i = q + kChainDelay, p = pos - q * down.den;
            const float* t = down.taps.data() + p * 2 * down.half;
            float v[kCh] = {0, 0};
            for (int k = 0; k < 2 * down.half; ++k)
                for (int c = 0; c < kCh; ++c) v[c] += t[k] * clean.at(c, i - down.half + 1 + k);
            data[0][j] = v[0];
            data[1][j] = v[1];
        }
        clean.dropBefore(floorDiv((outNext - latency) * down.num, down.den) + kChainDelay - down.half);
    }
};

Processor::Processor(int sampleRate) : d(std::make_unique<Impl>(sampleRate)) {}

Processor::~Processor() = default;

void Processor::setParams(const Params& params) { d->chain.setParams(params); }

void Processor::reset() { d->reset(); }

void Processor::process(float* const* data, int n) { d->process(data, n); }

int Processor::latency() const { return d->latency; }

} // namespace VoiceClean
