// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Picking a region by one click (0.22, "text behind a wall"): from the clicked point the region of similar color grows
// over the picture (Lab distance to the color around the click, stopped at sharp steps between neighbours), its holes
// are filled (a poster on the wall belongs to it) and its outline is simplified (Douglas-Peucker) to a polygon of at
// most maxPoints corners. The polygon becomes a normal mask "shape: polygon" of the compositing graph; with "follow"
// it moves with a tracker (a four-corner tracker: in perspective, CompNodesMask). No AI: an own color/edge rule that is
// fast and works offline; a model may come later as another way to the same polygon.
//
// pick() is pure on a picture (tests, CLI, app). pickClip() reads the clip's own picture (Tracker::clipPictures, the
// picture the graph works on, so 0..1 positions are the same as the tracker's and the masks'). applyPick() puts a
// polygon into a mask node of a clip's graph (replace its shape, or add/subtract a new mask chained behind it) as
// one undo step, drawn back into the tracker's reference frame when the mask follows a tracker that already moved.

#include <QImage>
#include <QPointF>
#include <QString>
#include <QVector>

class Editor;
struct Clip;
struct ProjectFormat;

namespace RegionPick {

inline constexpr int kMaxPoints = 24;          // corners of the polygon (a mask takes up to 32)
inline constexpr double kDefaultTolerance = 0.35;

struct Options {
    double tolerance = kDefaultTolerance; // 0..1: how different a color may be and still belong (0 = nearly equal)
    int maxPoints = kMaxPoints;
    int maxEdge = 960; // the picture is analysed at most this big (longer side), the polygon is 0..1 anyway
};

struct Result {
    QVector<QPointF> polygon; // 0..1 of the picture, clockwise on screen (y down), first corner not repeated
    double area = 0;          // share of the picture inside the region (before simplifying)
    bool touchesEdge = false; // the region reaches the picture's border
    QString warning;          // English, for the CLI and the app's status (too big, too small); empty = fine
    QString error;            // English: nothing picked (point outside, empty picture)
    bool ok() const { return error.isEmpty() && polygon.size() >= 3; }
};

// The region around point (0..1 of img) as a polygon
Result pick(const QImage& img, QPointF point, const Options& o = {});

// The picture of clip c at clip frame `frame` (its own picture without bars, Tracker::clipPictures) and pick() on it.
// A clip without its own picture (title, compound, multicam): error
Result pickClip(const ProjectFormat& format, const Clip& c, int frame, QPointF point, const Options& o = {});

// Where on the polygon's outline a point tracker finds structure: the corner (or point between corners) with the
// strongest texture in a square of `side` (0..1 of the picture width) around it - a flat wall's middle has none.
// The polygon's first corner when the picture is empty
QPointF trackablePoint(const QImage& img, const QVector<QPointF>& polygon, double side);

enum class Combine { Replace, Add, Subtract };
bool combineFromId(const QString& id, Combine* out); // "replace", "add", "subtract"

// Puts `polygon` (0..1 of the clip's picture, as seen at timeline frame `at`) into the clip's graph, one undo step:
// Replace: mask node `node` becomes that polygon (shape, points, p<i>x/y; its follow stays). Add / Subtract: a new mask
// node with the polygon goes behind the end of the mask chain starting at `node` (the chain end feeds its "mask"
// input, combine add/subtract; what read the chain end reads the new node) and follows the same tracker. When the
// mask follows a tracker, the corners are drawn back from frame `at` to the tracker's reference (the follow rule or,
// for a four-corner tracker, its homography) so the mask lands where it was picked.
// picture (the clip's picture at `at`, may be null): Replace on a mask following a fresh point tracker (not analyzed,
// its point never set) also puts the tracker's point on structure at the surface's edge (trackablePoint;
// *trackerPoint = where, else left alone). Returns the node that holds the polygon, 0 + *err (English) on failure
int applyPick(Editor& editor, const ProjectFormat& format, int clipId, int node, Combine mode,
              const QVector<QPointF>& polygon, int at, const QImage* picture = nullptr, QPointF* trackerPoint = nullptr,
              QString* err = nullptr);

// The clip's own picture at clip frame `frame` (Tracker::clipPictures, longer side at most maxEdge); null for a clip
// without its own picture (title, compound, multicam)
QImage clipPicture(const ProjectFormat& format, const Clip& c, int frame, int maxEdge = 960);

// The last mask of the chain that starts at mask node `node` (masks reading it through their "mask" input)
int chainEnd(const Clip& c, int node);

} // namespace RegionPick
