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
    // How far growing may reach (seconds in the file; < 0 = up to the recording's edge): a kill's lead-in is short
    // (lo = its start: padding goes after it, never before), its end where the player died (hi)
    double lo = -1, hi = -1;
    // hi is where the player died (or the game picture ends): never joined across it to a later moment
    bool died = false;
    bool operator==(const Moment&) const = default;
};

struct Options {
    double minS = 15, maxS = 45; // length of the short
    double minMomentS = 6;       // a candidate shorter than this grows to it (a kill needs its build-up and reaction)
    double joinGapS = 2;         // moments of the same file closer than this become one (no jump cut of 1 s)
    double fallbackS = 30;       // nothing found: this long (within minS..maxS) from the end of every recording
};

// candidates: best first; durations: seconds of each recording. Result: in order of file and time, no overlaps,
// together between minS and maxS where the recordings are long enough (all of them when shorter than minS); moments
// grow only within their lo/hi, so a short whose moments cannot reach minS without dead footage stays shorter.
// Without candidates: the end of each recording (a clip tool saves the moment that just happened).
QVector<Moment> pick(const QVector<Moment>& candidates, const QVector<double>& durations, const Options& o = {});
double total(const QVector<Moment>& moments);

// Up to `count` shorts of the same candidates (the `direct` recipe short's variants): the first is pick(); each next
// one picks among the candidates that share no time with a moment of the ones before (a candidate overlapping a used
// moment is left out) and never grows into a moment of the ones before. A variant needs a candidate of its own: none
// left (or only the fallback) = no more variants.
QVector<QVector<Moment>> variants(const QVector<Moment>& candidates, const QVector<double>& durations, int count,
                                  const Options& o = {});

// Helpers of pick (also for core/ReelPick):
// sorted by file and time, overlapping or close ones (gap below `gap`) of the same file joined (the better score kept)
QVector<Moment> joined(QVector<Moment> list, double gap);
// grows [from, to) by `by` seconds, a bit more after than before (the reaction), within [0, duration) and the moment's
// lo/hi; what one side cannot take goes to the other (what neither can take is left: the moment stays shorter)
void widen(Moment& m, double by, double duration);

} // namespace ShortPick
