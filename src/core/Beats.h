// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Beats of a song (schneidi-cli `beats`): tempo, beat grid, bars (4/4), phrases, energy per bar, breaks and drops, so
// an AI can cut to the song in one call. Pure computation on a mono signal fed in blocks; decoding is the caller's
// (engine/AudioAnalysis).
//
// Features, 200 values per second: the onset envelope of the audio sync (core/AudioSync: log energy rises in five
// bands) for the beats, the energy of the whole signal, of the bass (below ~120 Hz) and of the highs (above ~5 kHz)
// for bars, phrases, breaks and drops.
//
// Tempo: autocorrelation of the onsets (above their local mean) summed over four multiples of each period in the
// tempo range, refined over the peaks at many multiples and then by folding the onsets of the whole signal onto the
// period (the sharpest fold wins; a fixed grid, as electronic music and most produced songs have). Octaves: a beat
// twice as fast counts when every second beat of it is about as strong as the others (Tekk at 200 BPM, not 100),
// half as fast when they are much weaker (eighth-note hi-hats over a 90 BPM groove). Phase: the peak of the fold.
// Bars: the beat (of four) where the sound changes most between one bar-long window and the next, plus the bass
// attack (kick) as a weaker cue. Phrases: the same one level up (bars, four or eight of them). Breaks: beats whose
// bass is far below the song's usual bass, or the whole sound far below its usual level. Drops: the beat where a
// break ends and the sound comes back, ranked by how much the bass and the level jump and how long the break was.

#include "core/AudioSync.h"

#include <vector>

namespace Beats {

constexpr int kRate = AudioSync::kEnvelopeRate; // feature values per second (5 ms each)

// What the analysis looks at, kRate values per second (energies: mean square of the samples of a value)
struct Values {
    std::vector<float> onset, energy, bass, high;
};

// Features of a mono signal, fed in blocks of any size (the result does not depend on the block size)
class Features {
public:
    explicit Features(int sampleRate);
    void add(const float* mono, int samples);
    // The values so far (a started last value included); starts over afterwards
    Values take();

private:
    struct Biquad {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double run(double x);
    };
    void finishValue();
    int m_rate;
    AudioSync::Envelope m_onsets;
    Biquad m_bass[2], m_high; // 4th order low pass (two sections), 2nd order high pass
    long long m_pos = 0, m_next = 0, m_value = 0;
    double m_energy = 0, m_bassEnergy = 0, m_highEnergy = 0;
    int m_count = 0;
    Values m_out;
};

struct Options {
    double minBpm = 70, maxBpm = 220; // the beat is looked for in this range (octaves of it are folded into it)
};

// A stretch without bass or much quieter than the song: beats [from, to) of the grid
struct Break {
    int from = 0, to = 0;
    double bassDb = 0;   // bass during the break relative to the song's usual bass (negative)
    double energyDb = 0; // the whole sound relative to its usual level
    bool atStart = false, atEnd = false; // the intro / the end of the song
};

// The sound coming back after a break: at beat `beat` (the break's end)
struct Drop {
    int beat = 0;
    double score = 0;        // for the order, best first
    double bassJumpDb = 0;   // bass two bars after the drop minus two bars before it
    double energyJumpDb = 0; // the same for the whole sound
    double breakS = 0;       // length of the break before it
    int breakIndex = 0;      // in Result::breaks
};

struct Result {
    bool found = false;     // false: too short or no onsets at all
    double bpm = 0;
    double confidence = 0;  // 0..1: how much of the onsets sit on one steady grid
    double period = 0;      // seconds per beat
    double phase = 0;       // first beat with sound (seconds from the start of the signal)
    int beats = 0;          // whole beats of the grid up to the last one with sound: beat k at phase + k * period
    int downbeat = 0;       // beat k is a bar one when k % 4 == downbeat (first bar at beat `downbeat`)
    double downbeatConfidence = 0; // 0..1
    int phraseBars = 4;     // bars per phrase (4 or 8)
    int phraseOffset = 0;   // bar b starts a phrase when b % phraseBars == phraseOffset (bar 0 = at `downbeat`)
    double phraseConfidence = 0;
    // Per whole bar from bar 0: level of the whole sound and of the bass (dB, mean square)
    std::vector<double> barEnergyDb, barBassDb;
    std::vector<Break> breaks; // in time order
    std::vector<Drop> drops;   // best first
};

Result analyze(const Values& values, const Options& options = {});

// Time of beat k (seconds from the start of the signal)
inline double beatTime(const Result& r, double k) { return r.phase + k * r.period; }

} // namespace Beats
