// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text (titles, subtitles): draws lines of text with Qt's QPainter (LGPL) onto a picture. The text block is aligned
// in an area (halign within it, valign top/middle/bottom), with an optional box behind it (bg, pad) and an outline
// (pen width, centered on the glyph edges). `room` = the outline width the layout makes room for (stacked layers of
// one title share it and so the scale and origin).

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>

#include <cstdint>

namespace Text {

// One text block, already in pixels of the picture it is drawn on (engine/picture scales the title by output size /
// project size)
struct Params {
    QString text;              // lines split at \n
    QRectF area;               // where the block is aligned
    QFont font;                // pixel size set
    int halign = 0, valign = 0; // 0 left/top, 1 center/middle, 2 right/bottom
    QColor fg = Qt::white, bg = Qt::transparent, outlineColor = Qt::black;
    double outline = 0, pad = 0;
    double room = 0;           // outline width the layout makes room for (layout_outline)
};
// Draws onto an RGBA picture (straight alpha) of w x h
void draw(uint8_t* rgba, int w, int h, const Params& p);
// Where draw() can put pixels (box, outline and glyph overhang included, generously): draw only needs a picture that
// large, shifted
QRectF bounds(const Params& p);


} // namespace Text
