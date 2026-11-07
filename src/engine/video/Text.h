// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Text (titles, subtitles): draws lines of text with Qt's QPainter (LGPL) onto a picture. The text block is aligned
// in an area (halign within it, valign top/middle/bottom), with an optional box behind it (bg, pad) and an outline
// (pen width, centered on the glyph edges). `room` = the outline width the layout makes room for (stacked layers of
// one title share it and so the scale and origin).

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPair>
#include <QRectF>
#include <QString>
#include <QVector>

#include <cstdint>

namespace Text {

// Fill and effects of a styled title (0.19, core TitleLook in pixels of the picture). Order back to front: box,
// glow, shadow, edge (extrusion), outline, letters. The silhouette the effects use is the letters with their outline.
struct Look {
    QVector<QPair<double, QColor>> stops; // gradient fill (2..4 stops, sorted); empty = no gradient
    double gradAngle = 90;                // degrees clockwise, 90 = top to bottom over the text block
    QImage texture;                       // picture fill (null = none; wins over the gradient)
    double texScale = 1, texX = 0, texY = 0; // scale 1 = as wide as the text block; offset in pixels
    double edgeDepth = 0, edgeAngle = 45; // extrusion length (0 = none), its direction
    QColor edgeColor{60, 60, 60};
    bool edgeLight = true;
    double shadowX = 0, shadowY = 0, shadowSoft = 0;
    QColor shadow = Qt::transparent;      // colour with the opacity in its alpha (transparent = none)
    double glowRadius = 0;
    QColor glow = Qt::transparent;        // colour with the strength in its alpha (transparent = none)

    bool styledFill() const { return !texture.isNull() || stops.size() >= 2; }
    bool behind() const { return edgeDepth > 0 || shadow.alpha() > 0 || (glow.alpha() > 0 && glowRadius > 0); }
};

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
    // Styled title (null = plain, drawn exactly as before). A title with an outline is drawn as two calls (outline
    // layer, then letters): lookBehind = this call draws glow/shadow/edge (the first), lookFill = this call fills
    // the letters with the gradient/texture instead of fg (the letters' call)
    const Look* look = nullptr;
    bool lookBehind = true, lookFill = true;
};
// Draws onto an RGBA picture (straight alpha) of w x h
void draw(uint8_t* rgba, int w, int h, const Params& p);
// Where draw() can put pixels (box, outline and glyph overhang included, generously): draw only needs a picture that
// large, shifted
QRectF bounds(const Params& p);

// Word captions: lines of words, each word with its own colour, size and opacity (karaoke highlight, pop-in). The
// layout gives a larger word its larger width (the others move aside, a full space stays between words); the block's
// size counts the widest word grown by `reserve`, so it does not change size while the highlight moves. Lines one
// below the other, each aligned like Params::halign; the block is aligned in `area` and shrunk to fit it when larger.
struct Word {
    QString text;
    QColor fg = Qt::white;
    double scale = 1.0, opacity = 1.0;
};
struct WordParams {
    QVector<QVector<Word>> lines;
    QRectF area;
    QFont font; // pixel size set
    int halign = 1, valign = 2;
    QColor outlineColor = Qt::black, bg = Qt::transparent;
    double outline = 0, pad = 0; // outline: pen width
    QColor shadow = Qt::transparent; // drop shadow (offset by a tenth of the font size down and right)
    double reserve = 1.0; // the largest scale a word grows to (block size, see above)
};
void drawWords(uint8_t* rgba, int w, int h, const WordParams& p);


} // namespace Text
