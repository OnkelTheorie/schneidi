// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Drawing shapes (0.22, core/Shapes.h) into 8-bit RGBA pictures with straight alpha: a shape title (fill with a soft
// edge from the mask raster, a stroke that may draw itself, a drop shadow), the picture below a shape (blurred or a
// mosaic inside it, darkened outside it: a spotlight) and a clip cut to a shape (effect "clip_shape").

#include "core/Shapes.h"

#include <QColor>
#include <QPoint>

#include <cstdint>

namespace Picture::ShapePaint {

// Paints the shape title t whose outline `o` is in output pixels onto rgba (w x h), whose pixel (0, 0) is output pixel
// `origin`. reveal = drawn part of the stroke (0..1), scale = output pixels per project pixel, opacity 0..1, fill =
// the fill colour (transparent: no fill, e.g. a blur fill which `below` makes)
void paint(uint8_t* rgba, int w, int h, QPoint origin, const Shapes::Outline& o, const TitleStyle& t, double reveal,
           double scale, double opacity, const QColor& fill);

// What a shape does to the picture below it (rgba = the frame so far, w x h, outline `o` in its pixels): a blur or
// pixelate fill inside it, a spotlight outside it; k = how much (opacity and fades, 0..1)
void below(uint8_t* rgba, int w, int h, const Shapes::Outline& o, const ShapeStyle& s, double scale, double k);

// Cuts a clip's picture (w x h) to its clip shape: outside transparent, the stroke on the outline, the shadow
// behind. scale = picture pixels per project pixel
void clipShape(uint8_t* rgba, int w, int h, const Shapes::ClipShape& cs, double scale);

// The 8-bit coverage (255 = inside) of a closed outline with a soft edge `soft` (pixels) in a w x h plane; the rest 0.
// Returns where it can be > 0
QRect coverage(const QVector<QPointF>& points, double soft, int w, int h, uint8_t* plane);

} // namespace Picture::ShapePaint
