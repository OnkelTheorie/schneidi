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
// microphone does not count faint room noise as speech). A hissing microphone whose floor is above -43 dBFS and that
// is quiet in less than 2 % of its frames that way gets floor + 4 dB instead (above the cap). A frame is quiet on a
// track when its peak is below that.

#include "core/TranscriptCut.h"

#include <QString>
#include <QVector>

#include <vector>

namespace PauseCut {

inline constexpr double kFloorPercentile = 0.10;
inline constexpr double kAboveFloorDb = 8.0;
inline constexpr double kMaxThresholdDb = -35.0;
inline constexpr double kMinThresholdDb = -60.0;

inline constexpr double kNoisyFloorDb = -43.0;
inline constexpr double kNoisyQuietShare = 0.02;
inline constexpr double kNoisyAboveFloorDb = 4.0;

// The noise floor (dBFS): 10th percentile of the frames with sound; none = -200
double noiseFloorDb(const std::vector<float>& peaks);
// Share of frames below the threshold (0..1; empty = 1)
double quietShare(const std::vector<float>& peaks, double thresholdDb);

struct Threshold {
    double db = kMaxThresholdDb;
    double floorDb = -200;
    double quietShare = 1; // with `db`
    bool raised = false;   // a hissing microphone: floor + 4 dB above the cap
};
// The automatic threshold of a track from its per-frame peaks (linear); no sound at all = kMaxThresholdDb
Threshold autoThreshold(const std::vector<float>& peaks);
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
// present (optional, the same length): frames where at least one chosen track has an enabled clip; only those can be
// a pause (a picture between two parts of an interview - B-roll, a title - is never cut). A hole is different: a run
// without a clip shorter than minPause between two parts (two recordings of one call placed with a sliver between
// them) is missing sound, not a pause - it is always cut, with the quiet touching it. That quiet keeps `pad` on its
// side only when there is at least `pad` of it; less (the soundless tail of a recording) goes with the hole, so the
// sound on both sides meets instead of dropping out for a moment.
QVector<Range> pauses(const QVector<std::vector<bool>>& quiet, const QVector<int>& tracks, int minPause, int pad,
                      const std::vector<bool>* present = nullptr);

// Filler words (ScriptAlign::isFiller with the word's language) of `words` on one of `tracks`, but only while no other
// chosen track speaks over it (else the other speaker's words would go). Another track speaks when its loudest frame
// there is above its threshold and not more than kBleedDb under the filler's own loudest frame (the filler heard on
// the other microphone - bleed - does not count). The cut's edges snap to the nearest quiet frame of the filler's own
// track within kSnapFrames of the word's start and spoken end (no cut into a sound); none there = the filler stays.
// peaks[i] / thresholdsDb[i] belong to tracks[i]; an empty `peaks` = not measured (every filler counts, unsnapped).
inline constexpr double kBleedDb = 12.0;
inline constexpr int kSnapFrames = 3;
QVector<Range> fillers(const QVector<TranscriptCut::Word>& words, const QVector<std::vector<float>>& peaks,
                       const QVector<double>& thresholdsDb, const QVector<int>& tracks);

// Pauses and fillers together, by time, as their union: overlapping or touching ranges become one (a pause when a
// pause is part of it; the tracks of both), so nothing is counted twice
QVector<Range> combined(const QVector<Range>& pauses, const QVector<Range>& fillers);

QString kindId(Kind k); // "pause", "filler"

} // namespace PauseCut
