// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shape titles (0.22, TitleKind::Shape) and the "clip in a shape" effect: the outline of each ShapeKind as points, its
// place at a clip frame (keyframes, tracker) and the part of it a self-drawing stroke has drawn. Pure data in project
// pixels (y down); the picture engine fills and strokes it (engine/picture/Renderer drawShape), the fill's soft edge is
// the mask raster of the compositing graph (engine/MaskRaster, a Masks::Geometry polygon).

#include "core/Masks.h"
#include "core/Types.h"

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QVector>

namespace Shapes {

// "ellipse", "rect", … / "color", "none", "blur", "pixelate"; false = unknown id (value unchanged)
const char* kindId(ShapeKind k);
bool kindFromId(const QString& id, ShapeKind* k);
const char* fillId(ShapeFill f);
bool fillFromId(const QString& id, ShapeFill* f);

// The outline in the shape's own frame: centre (0, 0), y down, not rotated, inside a w x h box. Closed shapes start at
// their top (12 o'clock) and run clockwise, so a self-drawing stroke starts there; a line runs left to right (open).
// Curves are flattened to within `tolerance` (pixels of the same unit as w and h)
struct Outline {
    QVector<QPointF> points;
    bool closed = true;
};
Outline outline(const ShapeStyle& s, double w, double h, double tolerance = 0.25);

// The kind changes from `from` to `to`: the box size that fits. A line's height is its thickness (`lineThickness`
// pixels: a box height would make it a bar); leaving a line restores `remembered` (the height before it became a line,
// 0 = unknown: as wide as the box, an arrow half of it), so a star is not a flat sliver. Other changes keep the size
QSizeF sizeForKind(const ShapeStyle& s, ShapeKind to, double lineThickness, double remembered = 0);
// The thickness of a new line in a format `frameHeight` high (8 px at 1080)
double lineThickness(int frameHeight);

// Where a shape title is at clip frame t (keyframes included): centre in project pixels of a PW x PH frame (y down),
// size and rotation (clockwise degrees), how much of the stroke is drawn (0..1)
struct Place {
    QPointF center;
    double w = 0, h = 0, angle = 0;
    double reveal = 1;
};
Place placeAt(const Clip& c, double t, double PW, double PH);

// The outline placed: scaled into its box, turned, moved to the centre, then carried by a tracker (Masks::Follow,
// project pixels; nullptr = none). tolerance in project pixels
Outline placed(const ShapeStyle& s, const Place& p, const Masks::Follow* follow = nullptr, double tolerance = 0.25);

// The part of a polyline from its start up to `fraction` (0..1) of its length (a closed one goes round back to its
// first point); open as the result
QVector<QPointF> partial(const QVector<QPointF>& points, bool closed, double fraction);

// Bounds of a placed shape title with its stroke and shadow (project pixels)
QRectF bounds(const TitleStyle& t, const Outline& o);

// The outline as a mask polygon for MaskRaster (pixels of the picture it is rasterized into): soft = the feather
// (at least one pixel, an anti-aliased edge)
Masks::Geometry geometry(const QVector<QPointF>& points, double soft);

// Copies of clips got new ids (old -> new, paste, decompose): a copied shape following a clip copied with it follows
// the copy (a drawing too: it follows with the same fields). Only the clips with a new id are looked at
void remapFollow(Timeline& tl, const QHash<int, int>& ids);

// ---- Clip in a shape (effect "clip_shape"): the clip's own picture cut to a shape, with stroke and shadow ----
inline constexpr const char* kClipShapeEffect = "clip_shape";
// The shape of the effect on a picture of w x h pixels: centred at (x, y) percent offsets of the picture, `size`
// percent of the picture's shorter side high and `aspect` times as wide (1 = circle / square), corner radius percent
// of the smaller side of the shape
struct ClipShape {
    ShapeStyle shape;
    double size = 90, aspect = 1, x = 0, y = 0; // percent, width / height, percent
    double cornerPct = 20;
    double strokeWidth = 0; // pixels of the project format
    QColor strokeColor = Qt::white;
    bool shadow = false;
    double shadowSoft = 12, shadowOpacity = 0.6;
    double feather = 0;
};
// The effect's values at clip frame t (keyframes included); false = the clip has no enabled effect
bool clipShapeAt(const Clip& c, double t, ClipShape* out);
// Its outline in pixels of a w x h picture (y down); scale = picture pixels per project pixel (stroke, feather)
Outline clipShapeOutline(const ClipShape& cs, double w, double h);

} // namespace Shapes
