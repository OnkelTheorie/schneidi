// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Analysis of the tracker node (compositing graph, 0.19): follows one point, two points (position, size, rotation),
// four corners of a flat surface (0.22: perspective; Tracker::QuadTracker) or a face over a frame range and returns where it is per frame. Runs once on request (op/app later), never per frame
// during playback: the result becomes keyframes of the node's own parameters (Editor::trackCompNode), the node itself
// only passes its picture through.
//
// trackFrames is pure on pictures (a frame callback): tests, CLI and app can feed it what they like. trackNode reads a
// tracker node of a clip's "comp" graph and feeds it the clip's own pictures (Tracker::clipFrames).

#include "core/CompTrack.h"
#include "core/ProjectFormat.h"
#include "core/Types.h"
#include "engine/FaceDetect.h"

#include <QImage>
#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>
#include <vector>

namespace Picture::Comp {

enum class TrackMode { Point, Two, Face, Quad };
// "point", "two", "face", "quad" (the node's `mode` values); false = unknown
bool trackModeFromId(const QString& id, TrackMode* mode);

struct TrackStart {
    TrackMode mode = TrackMode::Point;
    QPointF a{0.5, 0.5};  // point (or first point, or where the face is) at the start frame, 0..1 of the picture
    QPointF b{0.6, 0.5};  // second point (mode Two)
    QPointF corners[4] = {{0.3, 0.3}, {0.7, 0.3}, {0.7, 0.7}, {0.3, 0.7}}; // mode Quad: clockwise from the top left
    double size = 0.05;   // side of the square looked for around each point, relative to the picture width
    int maxLost = ::Comp::kDefaultMaxLost; // frames in a row without a match before it gives up (a longer hiding)
};

struct TrackRun {
    QVector<::Comp::TrackPoint> points; // the start frame first, then every frame where it was found, in order
    QVector<int> lostFrames;            // frames where it was not found (no point there: keyframes bridge them)
    int lostAt = -1;                    // first frame lost (-1 = never)
    int foundAt = -1;                   // first frame found again after lostAt (-1 = not again)
    bool gaveUp = false;                // lost for maxLost frames in a row: stopped there
    bool canceled = false;
    int outliers = 0;                   // two points: frames that used only one of them; quad: frames without all four
    QVector<int> movedIn;               // quad: corners (0..3) without structure, tracked a bit further inside
    QString hint;                       // translated, for the user (moved corners), empty = none
    QString error;                      // nothing tracked (translated)
};

// Faces of a picture (FaceDetect boxes as fractions); nullopt + *error = the detector failed
using FaceFinder = std::function<std::optional<std::vector<FaceDetect::Face>>(const QImage& img, QString* error)>;
// The installed YuNet model (extension "faces"); nullptr + *error (translated, names the extension) without it
FaceFinder installedFaces(QString* error);

// Tracks from frame `from` to `to` (backwards when to < from), frame(t) gives the picture at t (any size, all the same).
// faces: for mode Face (empty = installedFaces()). progress(done, total) after every frame; cancel stops after it
TrackRun trackFrames(const std::function<QImage(int frame)>& frame, const TrackStart& start, int from, int to,
                     const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                     const FaceFinder& faces = {});

// Start of a tracker node from its values (keys without the "n<id>." prefix, missing = defaults)
TrackStart startOf(const QVariantMap& values);

// A tracker node's value as its followers read it (numOf, the follow rule of core/CompTrack.h): like numValue, but
// rx/ry not set = the point the mode implies (::Comp::impliedReference), x/y not set (not analyzed yet) = rx/ry, so a
// follower does not move. scale/angle missing = 1/0 (their defaults). The engine's numOf does the same with keyframes
double trackerValue(const QVariantMap& values, const QString& key);

// What followers of tracker `node` need at clip frame t beyond its values there (core/CompTrack.h FollowTime): its
// lost mode (values, missing = fade), gaps (the clip's seen keys; none without a clip) and values at other frames
// (keyframes of the clip, else `values`)
::Comp::FollowTime followTime(const Clip* clip, int node, const QVariantMap& values, double t, double fps);

// Where followers of tracker `node` of clip c (its "comp" graph) are carried at clip frame t, by the follow rule of
// core/CompTrack.h: from the reference point (rx, ry) to (x, y), scaled and turned (clockwise degrees) around it;
// points 0..1 of the clip's picture. seen = the follower's visibility (lost mode). For followers outside the graph
// (a shape title on a track above, 0.22). false = the clip has no such tracker
struct FollowAt {
    QPointF from, to;
    double scale = 1, angle = 0, seen = 1;
};
bool followAt(const Clip& c, int node, double t, double fps, FollowAt* out);

// Tracker node `node` of clip c's "comp" graph over clip frames from..to; the node's start values at `from`
// (keyframes resolved). Pictures: the clip alone without its effects, its own picture without the bars of another
// aspect ratio (Tracker::clipPictures), so positions are 0..1 of the picture the graph works on
TrackRun trackNode(const ProjectFormat& format, const Clip& c, int node, int from, int to,
                   const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                   const FaceFinder& faces = {});

} // namespace Picture::Comp
