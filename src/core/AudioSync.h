#pragma once
// Audio sync for multicam (docs/archiv/plan-0.8.md): finds the offset between two recordings of the same event by their
// sound. Each recording becomes an onset envelope (how suddenly the level rises in five bands, log-compressed,
// 200 values/s), so only shared events count (round start, spike, announcements) and not how loud or dull each one
// hears them. The envelopes are cross-correlated over the whole length (FFT, scored per lag against the spread of
// unrelated recordings), the peak is refined to sub-values (parabola, windows along the overlap for the drift) and
// checked against the next best peak (confidence): silence, nothing shared or only periodic beeps are "not found"
// instead of a guess. Pure computation without MLT; decoding files is engine/SoundSync.

#include <QVector>

#include <vector>

namespace AudioSync {

constexpr int kEnvelopeRate = 200; // envelope values per second (5 ms each)

// Onset envelope of a mono signal, fed in blocks of any size (the result does not depend on the block size)
class Envelope {
public:
    explicit Envelope(int sampleRate);
    void add(const float* mono, int samples);
    // The envelope so far (a started last value included); starts over afterwards
    std::vector<float> take();

private:
    void finishValue();
    // Band pass (biquad) with the energy of the current value and the level of the previous one
    struct Band {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double energy = 0, prev = 0;
    };
    int m_rate;
    long long m_pos = 0;  // samples so far
    long long m_next = 0; // first sample of the next value
    long long m_value = 0;
    std::vector<Band> m_bands;
    int m_count = 0;
    bool m_first = true;
    std::vector<float> m_out;
};
std::vector<float> envelope(const std::vector<float>& mono, int sampleRate);

struct Options {
    double maxOffsetS = 0;       // largest offset searched (seconds), 0 = any
    double minConfidence = 0.3;  // below: not found (synthetic tests: found >= 0.39, unrelated <= 0.21)
};

struct Match {
    bool found = false;
    double offsetS = 0;    // the other recording starts this many seconds after the reference (negative = before)
    double confidence = 0; // 0..1: 1 - next best peak / best peak
    double driftMs = 0;    // clock drift: offset at the end of the overlap minus at its start (only reported)
    int via = -1;          // alignAll: index of the recording it was matched against (-1 = reference/none)
};

// Offset of `other` relative to `reference` (envelopes from Envelope)
Match match(const std::vector<float>& reference, const std::vector<float>& other, const Options& options = {});

// Several recordings: offsets relative to the first one. A recording that does not overlap the first is matched
// against the others already found (chain, Match::via); [0] is the reference itself (found, offset 0).
QVector<Match> alignAll(const QVector<std::vector<float>>& envelopes, const Options& options = {});

} // namespace AudioSync
