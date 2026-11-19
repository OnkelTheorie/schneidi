// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Cutting the pauses of a conversation (edit op cut_pauses, the recipe "talk"): the time where EVERY chosen track is
// quiet, measured per track against its own threshold, and filler words (äh, ähm, euh …) from the transcripts while
// the other tracks are quiet. Pure functions on per-frame peaks and timeline words; measuring and cutting are done by
// the caller (cli/EditOpsPauses.cpp).
//
// Per track: the sample peak of every timeline frame of that track alone (others muted). Its threshold comes from its
// own noise floor: the 10th percentile of the frames that have sound (digital silence of gaps left out) + 8 dB, at
// most -35 dBFS (a noisy microphone does not count its hiss as speech) and at least -60 dBFS (a gated, very clean
// microphone does not count faint room noise as speech). A frame is quiet on a track when its peak is below that.

#include "core/TranscriptCut.h"

#include <QString>
#include <QVector>

#include <vector>

namespace PauseCut {

inline constexpr double kFloorPercentile = 0.10;
inline constexpr double kAboveFloorDb = 8.0;
inline constexpr double kMaxThresholdDb = -35.0;
inline constexpr double kMinThresholdDb = -60.0;

// The automatic threshold (dBFS) of a track from its per-frame peaks (linear); no sound at all = kMaxThresholdDb
double autoThresholdDb(const std::vector<float>& peaks);
// Per frame: below the threshold (frames past the end of `peaks` up to `length` count as quiet)
std::vector<bool> quietFrames(const std::vector<float>& peaks, double thresholdDb, int length);

enum class Kind { Pause, Filler };

struct Range {
    int from = 0, to = 0; // timeline frames [from, to) to cut
    Kind kind = Kind::Pause;
    QVector<int> tracks;  // audio track indices: a pause = every chosen track, a filler = its own track
    QString text;         // filler: the word as written
    bool operator==(const Range&) const = default;
};

// Runs of at least `minPause` frames where every one of `quiet` is quiet (all the same length), each cut so that
// `pad` frames of quiet stay at both ends (at the timeline's start and end too). `tracks` = the track index of each
// entry of `quiet` (for Range::tracks).
QVector<Range> pauses(const QVector<std::vector<bool>>& quiet, const QVector<int>& tracks, int minPause, int pad);

// Filler words (ScriptAlign::isFiller) of `words` on one of `tracks`, cut from the word's start to its spoken end, but
// only while no other chosen track speaks over it (else the other speaker's words would go). Another track speaks when
// its loudest frame there is above its threshold and not more than kBleedDb under the filler's own loudest frame (the
// filler heard on the other microphone - bleed - does not count). peaks[i] / thresholdsDb[i] belong to tracks[i]; an
// empty `peaks` = not measured (every filler counts).
inline constexpr double kBleedDb = 12.0;
QVector<Range> fillers(const QVector<TranscriptCut::Word>& words, const QVector<std::vector<float>>& peaks,
                       const QVector<double>& thresholdsDb, const QVector<int>& tracks);

// Pauses and fillers together, by time; a filler inside a pause (or overlapping one) is left out
QVector<Range> combined(const QVector<Range>& pauses, const QVector<Range>& fillers);

QString kindId(Kind k); // "pause", "filler"

} // namespace PauseCut
