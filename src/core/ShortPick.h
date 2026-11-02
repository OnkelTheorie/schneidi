// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which moments of one or more recordings go into a short (`short` command): the best highlight candidates as long
// as they fit the longest length, each padded to a watchable minimum, then widened evenly until the shortest length
// is reached. Pure function on seconds, no Qt beyond containers -> testable, same on every platform.

#include <QVector>

namespace ShortPick {

struct Moment {
    int file = 0;       // index into the recordings
    double from = 0;    // seconds in that file
    double to = 0;
    double score = 0;   // from `highlights`; higher = better
    bool operator==(const Moment&) const = default;
};

struct Options {
    double minS = 15, maxS = 45; // length of the short
    double minMomentS = 6;       // a candidate shorter than this grows to it (a kill needs its build-up and reaction)
    double joinGapS = 2;         // moments of the same file closer than this become one (no jump cut of 1 s)
    double fallbackS = 30;       // nothing found: this long (within minS..maxS) from the end of every recording
};

// candidates: best first; durations: seconds of each recording. Result: in order of file and time, no overlaps,
// together between minS and maxS where the recordings are long enough (all of them when shorter than minS).
// Without candidates: the end of each recording (a clip tool saves the moment that just happened).
QVector<Moment> pick(const QVector<Moment>& candidates, const QVector<double>& durations, const Options& o = {});
double total(const QVector<Moment>& moments);

} // namespace ShortPick
