// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Drawing titles (0.22, TitleKind::Drawing): strokes painted in the viewer (or given to op 'draw') as point lists in
// 0..1 of the frame (DrawStroke in core/Types.h). Here: their file form, where they are at a clip frame (title
// position, tracker), the part a self-drawing ("reveal") drawing has drawn, light smoothing of a painted stroke, which
// strokes an eraser touches and their bounds. Pure data in project pixels (y down); the picture engine paints them
// (engine/picture/DrawPaint).

#include "core/Masks.h"
#include "core/Types.h"

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace Drawing {

// ---- file / CLI form: {strokes:[{points:[x0,y0,x1,y1,…], pressure?:[…], color, width, opacity?}], reveal?} ----
// points rounded to 1/100000 of the frame, pressure to 1/1000 (left out when all 1), width in fractions of the frame
// height. Unknown keys are ignored when read (a newer version's); points outside -1..2 or not numbers are dropped
QJsonObject toJson(const DrawingStyle& d);
DrawingStyle fromJson(const QJsonObject& o);
QJsonObject strokeJson(const DrawStroke& s);
// false = no usable points (error says why); lenient about the rest (defaults)
bool strokeFromJson(const QJsonObject& o, DrawStroke* s, QString* error = nullptr);

// ---- placed in project pixels ----
struct Placed {
    QVector<QPointF> points;  // project pixels (y down)
    QVector<double> widths;   // the full width at each point (pixels)
    QColor color;
    double opacity = 1;
};
// The strokes of drawing title c at clip frame t in a PW x PH frame: moved by the title position (keyframes included)
// and carried by a tracker (Masks::Follow in project pixels; nullptr = none)
QVector<Placed> place(const Clip& c, double t, double PW, double PH, const Masks::Follow* follow = nullptr);
// One stroke placed without a title position (points of 0..1 times the frame)
Placed placeStroke(const DrawStroke& s, double PW, double PH);

// How much of a self-drawing drawing is drawn at clip frame t (0..1): 1 when it does not reveal
double revealFraction(const DrawingStyle& d, double t, double fps);
// The strokes up to `fraction` of their summed length, drawn in order (the last one partly, its widths interpolated);
// a dot counts as long as it is wide
QVector<Placed> revealed(const QVector<Placed>& strokes, double fraction);
// The clip frame (from its start) by which stroke i is drawn whole (reveal on), -1 = out of range
double strokeDoneAt(const DrawingStyle& d, int i, double PW, double PH, double fps);

// Bounds of the strokes with their width (project pixels); empty = nothing drawn
QRectF bounds(const QVector<Placed>& strokes);

// ---- painting ----
// Light smoothing of a painted stroke (points in 0..1 of a PW x PH frame): points closer than `minPixels` to the one
// before are dropped, then the inner points (and pressures) are averaged with their neighbours twice (1-2-1). The
// first and last points stay where they are
void smooth(DrawStroke& s, double PW, double PH, double minPixels = 1.5);
// The strokes (indices) an eraser touches: its path in project pixels with a radius (pixels); a stroke counts when it
// comes within its own half width plus the radius of the path
QVector<int> hits(const DrawingStyle& d, const QVector<QPointF>& path, double radius, double PW, double PH);

// The format changed (core/ProjectFormat scaleTimeline): the strokes keep their shape around the frame centre, scaled
// by s (the factor sizes get), their widths too
void scaleFormat(DrawingStyle& d, double fromW, double fromH, double toW, double toH, double s);

} // namespace Drawing
