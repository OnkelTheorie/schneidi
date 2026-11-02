// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Voice cleaning (effect "voiceclean", "Stimme säubern"): one-click chain for voice-overs, TTS and voice chat in game
// clips. Ported from the author's microphone filter mici: bump and click dampers, neural noise suppression (RNNoise,
// third_party/rnnoise), voice gate (RNNoise's voice activity plus a level gate that learns the voice), and a "shape"
// stage (high pass, presence, compressor, de-esser, limiter) with mici's "Empfohlen" (recommended) values - except the
// compressor's attack (1 ms instead of 5, so already compressed voice keeps its peaks). The closed gate lowers by
// 30 dB instead of muting, and a room tone residual of the original stays under everything: pauses never go silent.
//
// Works at any sample rate: the chain runs at 48 kHz in frames of 480 samples (RNNoise's), other rates are resampled
// there and back (windowed sinc, polyphase). Stereo is processed linked: noise suppression per channel (once when both
// channels carry the same sound), every gain decision (gate, dampers, compressor, limiter) on both channels together,
// so the stereo image stays. Fixed latency (latency(), the same for every parameter), compensated by the mixer like
// that of engine/Denoise: preview and export read the source that far ahead.

#include <memory>

namespace VoiceClean {

constexpr const char* EffectId = "voiceclean";

struct Params {
    double amount = 1.0; // 0..1: share of the noise-suppressed sound (0 = suppression off)
    bool gate = true;    // lower everything between words (voice activity + level gate)
    bool clicks = true;  // dampen bumps against the microphone and clicks (keys, mouse) away from speech
    bool shape = true;   // high pass, presence, compressor, de-esser, limiter (-1 dBFS)
    // Room tone (dBFS, -80..-40): a residual of the original in place of part of the cleaned sound, never gated, so
    // pauses keep a natural room sound instead of going silent; its noise lands at this level, but always at least
    // 12 dB below the original's
    double roomDb = -60.0;
};

constexpr double kRoomMinDb = -80.0, kRoomMaxDb = -40.0;

// The processor's latency at that rate in samples (2750 at 48 kHz, ~57 ms)
int latencyFor(int sampleRate);

class Processor {
public:
    explicit Processor(int sampleRate); // two channels
    ~Processor();
    Processor(const Processor&) = delete;
    Processor& operator=(const Processor&) = delete;

    void setParams(const Params& params);
    void reset();
    // In place on data[0] and data[1]; the output is the input `latency()` samples later
    void process(float* const* data, int n);
    int latency() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace VoiceClean
