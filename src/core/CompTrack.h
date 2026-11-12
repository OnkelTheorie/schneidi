// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Result of the tracker node of the compositing graph (0.19): per clip frame where the tracked thing is. The analysis
// (engine/picture/CompTrack) makes it, Editor::trackCompNode stores it as keyframes of the node's own parameters
// "n<id>.x/.y/.scale/.angle" on the effect "comp", other nodes follow them (numOf(id, "x")).
//
// Follow rule (one for every follower: mask, transform, …): the tracker has a reference point "rx"/"ry" = where the
// tracked thing is at the analysis' start frame (point: a; two points: the middle of a and b; face: the face centre
// found there). A follower is drawn relative to it: its own positions move by (x - rx, y - ry) and scale/turn by
// scale/angle around (x, y). Not analyzed yet (no x/y values or keyframes): x/y = rx/ry, scale 1, angle 0 = nothing
// moves; no rx/ry written yet: what the mode implies from ax/ay/bx/by (the face mode: ax/ay, its centre is unknown).
//
// Losing the point (0.22): the analysis also writes "n<id>.seen" as step keyframes (1 = found, 0 = lost; one key on
// each side of a change, so the curve is exact on every frame). The node's value "lost" says what followers do in a
// gap: bridge (the keyframes join the frames around it, as before 0.22), hold (stay at the last frame where it was
// seen) or fade (hold and fade out over kFadeSeconds, fade in again once found). Gaps up to kBridgeFrames are always
// bridged (a shake, a short blur). Followers read the visibility as numOf(id, "seen") (0..1) and multiply their
// opacity by it. A tracker without seen keys (older projects, not analyzed) has no gaps: everything as before.

#include <QPointF>
#include <QString>
#include <QVector>

#include <cmath>
#include <functional>
#include <utility>

struct Clip;

namespace Comp {

inline constexpr const char* kTrackerType = "tracker";

struct TrackPoint {
    int frame = 0;   // clip frame
    double x = 0.5;  // position 0..1 of the picture (point / middle of the two points / face center)
    double y = 0.5;
    double scale = 1; // size against the start frame (two points: distance; face: width)
    double angle = 0; // degrees clockwise against the start frame (two points only)
};

// The parameters the result is written into, in TrackPoint order
inline const char* const kTrackKeys[4] = {"x", "y", "scale", "angle"};
// The reference point (plain values, no keyframes): written by Editor::trackCompNode from the start frame's point
inline const char* const kRefKeys[2] = {"rx", "ry"};

// Seen keyframes (1 = found, 0 = lost), the choice what followers do in a gap and the frames in a row it may be lost
// before the analysis gives up
inline const char* const kSeenKey = "seen";
inline const char* const kLostKey = "lost";
inline const char* const kMaxLostKey = "max_lost";
inline constexpr int kBridgeFrames = 5;     // gaps up to this many frames are bridged in every mode
inline constexpr double kFadeSeconds = 0.2; // fade out/in of mode fade
inline constexpr int kDefaultMaxLost = 50;

enum class LostMode { Bridge, Hold, Fade };
// "bridge", "hold", "fade"; false = unknown ("auto", missing, a newer version's value)
bool lostModeFromId(const QString& id, LostMode* mode);
const char* lostModeId(LostMode mode);
// The mode a tracker's value "lost" stands for: its own choice, else ("auto", missing - older projects, recipes) hold
// for a face (a blur over it must never fade away and show the face) and fade for points
LostMode lostModeOf(const QString& id, const QString& trackMode);

// What an analysis did not find (Editor::trackCompNode): the lost frames, whether it gave up (lost maxLost frames in
// a row: the rest of the asked range counts as lost) and where the asked range ended (clip frame, -1 = unknown).
// A run that ends lost without giving up (the last frames blurred) is no gap: followers hold the last place
struct TrackLoss {
    QVector<int> frames;
    bool gaveUp = false;
    int to = -1;
};

// A run of frames where the tracked thing was not seen (clip frames, both inclusive). openStart: lost before the first
// key (a backwards analysis that gave up) - from is 0; openEnd: never found again (gave up, or the analysis ended
// lost) - to is the clip's last frame
struct TrackGap {
    int from = 0;
    int to = 0;
    bool openStart = false;
    bool openEnd = false;
    int whole = 0; // its length before it was cut to the clip (trackGaps of a trimmed clip), 0 = length()
    int length() const { return to - from + 1; }
    // Short and found again on both sides: followers bridge it in every mode
    bool bridged() const { return !openStart && !openEnd && (whole > 0 ? whole : length()) <= kBridgeFrames; }
    bool operator==(const TrackGap&) const = default;
};

// The gaps of seen keys (clip frame, value), sorted by frame; last = the clip's last frame. A gap starts at a key
// below 0.5 after one at or above it (or at the first key) and ends before the next key at or above 0.5
QVector<TrackGap> trackGaps(const QVector<std::pair<int, double>>& seen, int last);
// The gaps of tracker `node` of clip c (its keyframes "n<id>.seen" of the effect "comp"); none without them. Cut to
// the clip's frames 0..length-1 (keys lie in source frames: a trimmed clip keeps those outside), gaps outside dropped -
// what badge, Inspector, info and check show. Rendering holds to frames outside: it reads the keys as they are
QVector<TrackGap> trackGaps(const Clip& c, int node);
// The seen keys of tracker `node` of clip c (clip frames)
QVector<std::pair<int, double>> seenKeys(const Clip& c, int node);

// What a follower needs beyond the value at its own frame: the time, the tracker's gaps and mode, its values at
// other frames (to hold them). Missing (nullptr) = no time: seen = 1, nothing held
struct FollowTime {
    double t = 0;   // clip frame
    double fps = 25;
    LostMode lost = LostMode::Fade;
    QVector<TrackGap> gaps;
    std::function<double(const QString& key, double t)> valueAt; // the tracker's value of key at another clip frame
};

// The gap t lies in that followers do not bridge (nullptr = none: seen, or a short gap)
const TrackGap* gapAt(const QVector<TrackGap>& gaps, double t);
// Visibility of a follower at time.t (0..1): 1 in bridge and hold mode; in fade mode it fades out over kFadeSeconds
// from the first lost frame and in again from the frame found (a box filter over the gaps not bridged)
double followVisibility(const FollowTime& time);

// The tracker id a follower's "follow" value names (one rounding for every kind and check): 0 = follows nothing
inline int followId(double v)
{
    return std::isfinite(v) && v >= 0.5 && v < 2e9 ? int(std::lround(v)) : 0;
}

// The reference point a tracker's mode implies before an analysis wrote rx/ry ("two": middle of a and b, else a)
inline QPointF impliedReference(const QString& mode, QPointF a, QPointF b)
{
    return mode == QLatin1String("two") ? (a + b) / 2 : a;
}

// A tracker's value as followers read it, by the rule above. has(key) = the node has a value or keyframes for key,
// value(key) = its value at the frame (missing = the kind's default), mode = the node's mode id
// With time (0.22): "seen" = followVisibility; in hold and fade mode x/y/scale/angle stay at the last frame seen
// while the tracked thing is lost (the first one seen again after a gap at the clip's start)
inline double followValue(const QString& key, const QString& mode, const std::function<bool(const QString&)>& has,
                          const std::function<double(const QString&)>& value, const FollowTime* time = nullptr)
{
    if (key == QLatin1String(kSeenKey)) return time ? followVisibility(*time) : 1.0;
    if (time && time->lost != LostMode::Bridge && time->valueAt) {
        if (const TrackGap* g = gapAt(time->gaps, time->t)) {
            const double at = g->openStart ? g->to + 1 : g->from - 1;
            if (!(g->openStart && g->openEnd)) // lost everywhere: nothing to hold to
                return followValue(key, mode, has, [&](const QString& q) { return time->valueAt(q, at); });
        }
    }
    // One point follows a position only: size and turn stay as they are (keys an earlier two-point run left behind
    // would otherwise scale the follower up to 15 times - text flew out of the picture)
    if (mode == QLatin1String("point")) {
        if (key == QLatin1String("scale")) return 1.0;
        if (key == QLatin1String("angle")) return 0.0;
    }
    const bool isX = key == QLatin1String("x"), isY = key == QLatin1String("y");
    const bool isRx = key == QLatin1String("rx"), isRy = key == QLatin1String("ry");
    if (!(isX || isY || isRx || isRy) || has(key)) return value(key);
    if (isX || isY) return followValue(QLatin1String(isX ? "rx" : "ry"), mode, has, value);
    const QPointF r = impliedReference(mode, {value(QStringLiteral("ax")), value(QStringLiteral("ay"))},
                                       {value(QStringLiteral("bx")), value(QStringLiteral("by"))});
    return isRx ? r.x() : r.y();
}

} // namespace Comp
