// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Auto-Reframe, the pure part: footage of one shape in a timeline of another (a 16:9 recording in a 9:16 short) is
// zoomed to fill the frame and moved with keyframes so its subject stays in the visible part. The engine measures
// where the subject is (engine/ReframeAnalysis); here that measurement becomes a calm camera path (dead zone, eased
// moves without overshoot, hard jumps only at scene cuts) and the path becomes the clip's transform keyframes.
//
// Coordinates: the centre of the visible window in the clip's picture, 0..1 of its width/height (0.5 = middle).

#include "core/Types.h"

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace Reframe {

enum class Mode {
    Center, // fill the frame, the middle of the picture stays (static)
    Auto,   // fill the frame, follow the subject (keyframes)
    Fit,    // the whole picture fitted in, a blurred copy fills the rest
};
const char* modeId(Mode m); // "center", "auto", "fit" (CLI, settings)
bool modeFromId(const QString& id, Mode* m);

// Where the subject is at one clip frame (measured), how sure the measurement is (0..1) and whether a scene cut lies
// between the previous target and this one
struct Target {
    int frame = 0; // clip frame (0 = first frame of the clip)
    double x = 0.5, y = 0.5;
    double weight = 1;
    bool cut = false;
};

// A point of the camera path: the window centre from this clip frame on (eased towards the next point)
struct Key {
    int frame = 0;
    double x = 0.5, y = 0.5;
    bool operator==(const Key&) const = default;
};

struct PathParams {
    double smoothness = 0.5; // 0 = follows closely, 1 = calm (larger dead zone, slower moves)
    double fps = 25;
    // Visible window as a fraction of the picture (1 = that side is not cropped): 9:16 out of 16:9 = 0.316 x 1
    double windowW = 1, windowH = 1;
};

// Calm camera path through the targets (sorted by frame). Between two keys the value eases in and out (no
// overshoot: every section runs monotonically from one key to the next); at a scene cut the path jumps (a key on the
// frame before the cut and one on the cut). Always at least one key (a still camera); keys on a side that is not
// cropped stay at 0.5.
QVector<Key> path(const QVector<Target>& targets, const PathParams& p);

// Geometry of a picture with aspect `mediaAspect` (width / height as displayed) zoomed so that its `content` (the
// part between black bars, fractions of the picture; engine/BarDetect) fills `frame`
struct Fill {
    double zoom = 1;   // ClipTransform zoom (1 = fitted in)
    QSizeF picture;    // the whole zoomed picture in frame pixels
    QRectF content{0, 0, 1, 1};
    double windowW = 1, windowH = 1; // the visible part as a fraction of the content
};
Fill fill(double mediaAspect, QSizeF frame, const QRectF& content = QRectF(0, 0, 1, 1));
// ClipTransform position (pixels, y up) that puts window centre (x, y; fractions of the content) into the middle of
// the frame, kept inside the content
QPointF position(const Fill& f, QSizeF frame, double x, double y);

// The clip's transform for the mode: zoom, position (keyframes for Auto, from `keys` in clip frames), fill. Other
// transform values (rotation, opacity) and other keyframes stay; the crop too, unless `content` is less than the whole
// picture: then the bars around it are cropped (cropBars) and Fit fits the content. mediaAspect <= 0 = unknown
// (treated as the frame's shape: nothing to crop).
void apply(Clip& c, Mode mode, const QVector<Key>& keys, double mediaAspect, QSizeF frame,
           const QRectF& content = QRectF(0, 0, 1, 1));

// Crops what is outside `content` (fractions of the picture of aspect mediaAspect as fitted into `frame`): static
// crop values, crop keyframes removed. A whole-picture content leaves the crop alone. False = nothing to do.
bool cropBars(Clip& c, const QRectF& content, double mediaAspect, QSizeF frame);

// After the clip's picture changed its shape from oldAspect to newAspect (Clip::pictureAspect): a transform as apply
// left it (zoomed to fill the frame, or Fit with the content fitted in) follows the new shape with the same window
// centres. The crop must already be in the new shape's pixels (its share of the picture is the content). Other
// transforms stay. False = not one apply made.
bool reshape(Clip& c, double oldAspect, double newAspect, QSizeF frame);

// Can the clip be reframed (a video or still clip; not titles, audio, compound or multicam clips, not a kept region)?
bool canReframe(const Clip& c);

// Is the clip an overlay that reframing the whole timeline leaves alone: on a track above V1 (`track` > 0), not a
// kept region, and its own transform shows it smaller than the frame (zoom below 0.95, or a crop that leaves less
// than 70 % of the picture), e.g. a webcam picture in a corner? Zooming it to fill the frame would cover the picture
// below. mediaAspect <= 0 = unknown (the frame's shape).
bool isOverlay(const Clip& c, int track, double mediaAspect, QSizeF frame);

// A part of the picture kept readable above the reframed picture: the kill feed of a game, which a 9:16 crop of 16:9
// footage cuts away. Shown by a second copy of the clip on a track above (picture in picture), cropped to the region
// and placed near the top of the frame.
// What a game's kill feed shows at one moment (measured by engine/KillFeedLines): how many lines, how far left the
// widest one reaches and where the last one ends, fractions of the whole recording (as Keep::region)
struct FeedSample {
    double frame = 0; // clip frame (0 = the clip's first)
    double file = -1; // its time in the file, seconds (-1 = unknown: short moments count in clip frames only)
    int lines = 0;
    double left = 1, bottom = 0; // only with lines
    bool operator==(const FeedSample&) const = default;
};
// The kill feed over a clip. Not measured = the copy shows the whole region all the time.
struct Feed {
    bool measured = false;
    QVector<FeedSample> samples; // sorted by frame, a few per second
    double fps = 25;             // the timeline's frame rate (fades and moves are times)
    double lineHeight = 0;       // one line, part of the recording's height (0 = unknown: no limit on the text size)
    bool operator==(const Feed&) const = default;
};
// A stretch of the clip in which the kept kill feed stays the same: hidden (no lines) or showing `lines` lines
// from `left` to the region's right edge and from its top to `bottom` (fractions of the recording)
struct FeedState {
    int from = 0, to = 0; // clip frames [from, to)
    int lines = 0;
    double left = 1, bottom = 0;
    bool operator==(const FeedState&) const = default;
};
// The stretches of a measured feed over a clip of `length` frames: a line missing for a moment (a flash, a sample
// that missed it) does not count, nor one seen for a moment only (a moment in the clip or in the file: a slowed clip
// does not show a one-sample blip longer; such a moment between two counts takes the larger: a new line that reads
// as none while it slides in shows at once); the widest line of a stretch sets its left edge.
// Empty when the feed is not measured.
QVector<FeedState> feedStates(const Feed& f, int length);
// How the copy of a kill feed shows: Follow = its lines (measured, with lines), Whole = the whole region all the time
// (not measured; or measured over kFeedFallback seconds of footage or more without a single line: the game's HUD
// probably looks other than the analysis knows - another scale or layout), None = no copy (measured, no line over a
// shorter clip)
enum class FeedUse { Follow, Whole, None };
inline constexpr double kFeedFallback = 20;
FeedUse feedUse(const Feed& f);
// Is the feed measured without a line (feedUse None or a Whole fallback)?
bool feedLineless(const Feed& f);

struct Keep {
    QString id;    // what it keeps: "killfeed" (a game's, core/GameHud.h) or "custom"
    QRectF region; // fractions of the whole picture
    // The kill feed's lines over the clip (killfeed only): the copy shows only them, hidden while there are none
    Feed feed;
    bool operator==(const Keep&) const = default;
};
// Which regions a reframe keeps: Auto = the kill feed of a known game (core/GameHud.h, from the file's folder or name,
// or `game`) when the reframe cuts the sides away; KillFeed = that kill feed in any case; Custom = `region`
struct KeepChoice {
    enum Kind { Auto, KillFeed, None, Custom } kind = Auto;
    QString game;  // id of the game whose kill feed (empty = each file's own)
    QRectF region; // Custom: fractions of the whole picture
};
// Where a kept region goes: centred, `top` of the frame height below its top edge (under the app buttons of the
// platforms, ExportCheck safeTop), `width` of the frame width wide but at most `maxHeight` of the frame height tall
// (`maxLine`: a measured kill feed's line at most this part of the frame height, so one short line is not blown up)
struct KeepPlace {
    double top = 0.1, width = 0.84, maxHeight = 0.15;
    double maxLine = 0.036;
};
// The copy of `base` that shows only `k.region`: the same file, time, speed and effects (a grade looks the same), no
// sound link, no transform keyframes, cropped to the region and placed by `place`. Its id is left to the caller.
// With a measured feed the copy follows its lines (keyframes): faded out while there are none, its crop's left and
// bottom edge (and the zoom and place that keep it centred at the top) eased to the lines there are; the region's
// top and right edge stay.
Clip keepCopy(const Clip& base, const Keep& k, double mediaAspect, QSizeF frame, const KeepPlace& place = {});
// The same for a kept-region copy (its crop already in the new shape's pixels): placed anew for the new shape, unless
// it was moved or zoomed by hand since. A copy that follows its kill feed's lines gets its zoom and place keys anew
// (the text size limit taken from its keys). False = left as it is.
bool reshapeKeep(Clip& c, double oldAspect, double newAspect, QSizeF frame);
// Is `c` a kept-region copy of `base` (same file and the same part of it at the same place)?
bool isKeepOf(const Clip& c, const Clip& base);
// A kept-region copy that goes along with `base`: isKeepOf, or (both at their own speed) one of the same file that
// overlaps it in sync, e.g. after the base was trimmed or bladed since its copy was made
bool isKeepAlong(const Clip& c, const Clip& base);
// `region` (fractions of a game's picture) in fractions of the whole recording, when the game fills only `content`
// of it (black bars around a 4:3 game)
QRectF inContent(const QRectF& region, const QRectF& content);
// The regions a reframe in `mode` to `frame` keeps for clip `c` (Editor::reframeKeeps; also usable in a thread on a
// copy of the clip): content = the file's part between black bars. A kill feed of a game not known goes to *noGame
// when asked for explicitly (KeepChoice::KillFeed).
QVector<Keep> keepsOf(const Clip& c, Mode mode, QSizeF frame, const QRectF& content, const KeepChoice& choice,
                      bool* noGame = nullptr);
// Does a reframe cut the sides of the picture away (a 9:16 crop of 16:9 footage)? Then a kill feed at the edge is gone.
bool cropsSides(Mode mode, double mediaAspect, QSizeF frame, const QRectF& content = QRectF(0, 0, 1, 1));

} // namespace Reframe
