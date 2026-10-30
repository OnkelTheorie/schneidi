// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text (titles, subtitles) as an own MLT filter instead of MLT's qtext (Qt module, GPL): draws lines of text with
// Qt's QPainter (LGPL) onto the frame's picture. Same properties as qtext, so the timeline code sets them unchanged:
// "argument" (text, lines split at \n), "geometry" (profile pixels "x y w h", keyframed: the area the text block is
// aligned in), "family", "size" (pixels), "weight" (400/700), "style" (normal/italic), "halign" (left/center/right:
// the lines within the block and the block in the area), "valign" (top/middle/bottom), "fgcolour" (keyframed),
// "bgcolour" (box behind the block), "pad" (box margin), "olcolour", "outline" (pen width, centered on the glyph
// edges). Own addition: "layout_outline" (the outline width the layout makes room for, default "outline": stacked
// layers of one title share it and so the scale and origin). Font, outline and margin scale with the requested size (a preview frame looks like the export one).

#include <QColor>
#include <QFont>
#include <QRectF>
#include <QString>

#include <cstdint>

namespace Mlt {
class Filter;
class Profile;
} // namespace Mlt

namespace Text {

// One text block, already in pixels of the picture it is drawn on (the filter scales the properties above by
// requested size / profile size): pure drawing, also used by the own picture engine (engine/picture)
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

Mlt::Filter filter(Mlt::Profile& profile);
// With MLT's factory: "schneidi_text", and as "qtext" where MLT's Qt module is not loaded
void registerService();

} // namespace Text
