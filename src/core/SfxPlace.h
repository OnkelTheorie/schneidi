// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Where effect sounds go (op 'sfx'): pure placement logic, no project. A sound lands with its anchor on the moment
// (a whoosh's peak on the cut, a riser's end on the drop, a hit's attack on hit_s). Sparing on purpose: never two
// sounds closer than minGap, at most maxPerMinute in any minute, an impact at most once per video. When the rules
// leave out moments, the ones with the higher weight stay (equal weights: the earlier ones).

#include <QString>
#include <QVector>

#include <optional>

namespace SfxPlace {

struct Moment {
    double at = 0;     // seconds on the timeline
    double weight = 1; // which moments stay first when the rules leave some out
};

struct Rules {
    double minGap = 1.5;   // seconds between the moments of two sounds, at least
    int maxPerMinute = 4;  // sounds in any 60 s, at most (0 = no cap)
    bool impact = false;   // the sound is an impact: at most one per video
    double minGapLimit = 0.5; // minGap below this is raised to it
};

// A sound that is on the timeline already (the SFX track): its moment counts for the spacing and the cap
struct Existing {
    double at = 0;
    bool impact = false;
};

struct Placed {
    double moment = 0; // seconds
    double start = 0;  // seconds: moment - anchor
};

struct Skipped {
    double moment = 0;
    QString why; // "too_close", "per_minute", "impact_once", "before_start", "after_end"
};

struct Result {
    QVector<Placed> placed;   // sorted by time
    QVector<Skipped> skipped; // sorted by time
};

// anchor: of the sound (seconds from its start); end = the timeline's end (seconds; the moment must lie before it)
Result place(QVector<Moment> moments, double anchor, double end, const QVector<Existing>& existing, Rules rules);

// Level of a sound by name: 'mild' (default) -10 dB, 'normal' -4 dB on top of the sound's own peak level
double levelDb(const QString& level);
// The cap per minute that goes with a level: mild 3, normal 6
int perMinute(const QString& level);
// Clip volume that puts a sound (its own peak soundPeakDb) `under` dB below the game sound's peak (`short` sfx:'mild':
// hits 14 dB under it); no game sound (nullopt) = level 'mild'. Within -40..0 dB
double underPeakDb(std::optional<double> gamePeakDb, double soundPeakDb, double under = 14.0);

} // namespace SfxPlace
