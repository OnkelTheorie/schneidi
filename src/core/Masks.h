// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Mask shapes (0.19, compositing node "mask") as pure geometry without pixels: rectangle, ellipse, polygon, bezier
// curve, linear and radial gradient; soft edge, invert, rotation, following a tracker, combining with another mask.
// The rasterization is in engine/MaskRaster. Rectangle and ellipse are the windows of the Color page
// (core/GradeWindows): toWindow() gives the same shape as a window, so both rasterize identically.
//
// Two forms: Spec = the node's values (positions 0..1 of the picture, x right, y down; sizes relative to the picture
// WIDTH; angles in degrees clockwise), Geometry = the shape in output pixels (pixel centers at x + 0.5).

#include "core/GradeWindows.h"

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

namespace Masks {

inline constexpr int kMaxPoints = 32; // polygon / bezier corners

enum class Shape { Rect, Ellipse, Polygon, Bezier, Linear, Radial };
// How a shape goes together with the mask at the node's `mask` input: add = union (larger value), subtract = the
// input without the shape (min(in, 1 - shape)), intersect = only where both are (smaller value)
enum class Combine { Add, Subtract, Intersect };

const char* shapeId(Shape s); // "rect", "ellipse", "polygon", "bezier", "linear", "radial"
bool shapeFromId(const QString& id, Shape* s);
const char* combineId(Combine c); // "add", "subtract", "intersect"
bool combineFromId(const QString& id, Combine* c);

struct Spec {
    Shape shape = Shape::Rect;
    // Rect / ellipse: center and full size (width and height both relative to the picture width)
    QPointF center{0.5, 0.5};
    double width = 0.4, height = 0.4;
    // Polygon / bezier: the corners in order (closed); linear: from points[0] (inside, 1) to points[1] (0);
    // radial: center points[0] (1) out to the distance of points[1] (0)
    QVector<QPointF> points;
    // Bezier: handles as offsets from their point (0..1 of the picture per axis); missing = (0, 0), a corner
    QVector<QPointF> handlesIn, handlesOut;
    double rotation = 0; // degrees clockwise; rect/ellipse around their center, the others around their points' middle
    double soft = 0;     // soft edge outside the shape in output pixels (at least one pixel: an anti-aliased edge)
    bool invert = false;
};

// Moves a shape along with a tracked point: what was drawn at `from` goes to `to`, scaled and turned (clockwise
// degrees) around it. Pixels of the output picture
struct Follow {
    QPointF from, to;
    double scale = 1, angle = 0;
};

struct Geometry {
    Shape shape = Shape::Rect;
    bool empty = true; // nothing to draw (zero size, fewer than three corners, gradient of zero length)
    // Rect / ellipse
    QPointF center;
    double halfW = 0, halfH = 0;
    double rotation = 0; // clockwise degrees
    // Polygon (bezier flattened into it), closed
    QVector<QPointF> path;
    // Gradients
    QPointF a, b;
    double soft = 1; // >= 1 pixel
    bool invert = false;
};

// The shape in pixels of a picture of `size` (bezier curves flattened to within a quarter pixel)
Geometry toPixels(const Spec& s, QSize size);
Geometry followed(Geometry g, const Follow& f);
// Pixels where the shape's value can be > 0 without invert (whole-picture bounds for a linear gradient); not
// clipped to the picture. Empty for an empty shape
QRectF bounds(const Geometry& g);
// Rect / ellipse as a window of the Color page on a picture of `size` (same pixels in GradeWindows::Mask). Invert
// is left out: the caller inverts the plane
GradeWindows::Geometry toWindow(const Geometry& g, QSize size);

// Points of the closed bezier curve through pts (handles as absolute positions), each segment split so the polyline
// stays within `tolerance` of the curve
QVector<QPointF> flattenBezier(const QVector<QPointF>& pts, const QVector<QPointF>& in, const QVector<QPointF>& out,
                               double tolerance);

} // namespace Masks
