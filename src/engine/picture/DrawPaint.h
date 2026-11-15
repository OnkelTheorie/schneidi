// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Painting drawing titles (0.22, core/Drawing.h) into 8-bit RGBA pictures with straight alpha: each stroke with round
// ends and joins in its colour and opacity (a stroke crossing itself is not darker where it crosses), a pen's
// pressure as a varying width.

#include "core/Drawing.h"

#include <QImage>
#include <QPoint>
#include <QRect>

#include <cstdint>

namespace Picture::DrawPaint {

// The strokes (output pixels) painted within `reach`: straight RGBA, `box` = where it lies in output pixels (empty
// image = nothing). put() lays it onto rgba (w x h, its pixel (0, 0) at output pixel `origin`)
struct Painted {
    QImage straight;
    QRect box;
};
Painted render(const QVector<Drawing::Placed>& strokes, const QRect& reach);
void put(uint8_t* rgba, int w, int h, QPoint origin, const Painted& painted, double opacity = 1);

// Paints `strokes` (output pixels) onto rgba (w x h), whose pixel (0, 0) is output pixel `origin`, with opacity 0..1
void paint(uint8_t* rgba, int w, int h, QPoint origin, const QVector<Drawing::Placed>& strokes, double opacity = 1);

} // namespace Picture::DrawPaint
