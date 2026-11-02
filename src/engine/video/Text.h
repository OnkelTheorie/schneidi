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
#include <QVector>

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
