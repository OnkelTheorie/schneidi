// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Text.h"

#include "engine/video/Compose.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace Text {

QRectF bounds(const Params& tp)
{
    const QStringList lines = tp.text.split('\n');
    const QFontMetricsF fm(tp.font);
    double blockW = 0;
    for (const QString& line : lines) blockW = std::max(blockW, fm.horizontalAdvance(line));
    const double blockH = fm.lineSpacing() * (lines.size() - 1) + fm.height();
    const double ext = tp.pad + tp.room / 2;
    const double cw = blockW + 2 * ext, ch = blockH + 2 * ext;
    double k = 1.0;
    if (cw > 0 && tp.area.width() > 0) k = std::min(k, tp.area.width() / cw);
    if (ch > 0 && tp.area.height() > 0) k = std::min(k, tp.area.height() / ch);
    const double ox = tp.halign == 0 ? tp.area.left() : tp.area.left() + (tp.halign == 1 ? (tp.area.width() - cw * k) / 2
                                                                                         : tp.area.width() - cw * k);
    const double oy = tp.valign == 0 ? tp.area.top() : tp.area.top() + (tp.valign == 1 ? (tp.area.height() - ch * k) / 2
                                                                                       : tp.area.height() - ch * k);
    // Overhang of glyphs (italics, side bearings), the box centred on the ink and the outline's outer half
    const double margin = (std::max(tp.outline, tp.room) + 0.5 * tp.font.pixelSize() + 2 * tp.pad) * k + 4;
    return QRectF(ox, oy, cw * k, ch * k).adjusted(-margin, -margin, margin, margin);
}

void draw(uint8_t* rgba, int w, int h, const Params& tp)
{
    const QString& text = tp.text;
    const QFont& font = tp.font;
    const QRectF& area = tp.area;
    const int halign = tp.halign, valign = tp.valign;
    const QColor &fg = tp.fg, &bg = tp.bg, &ol = tp.outlineColor;
    const double outline = tp.outline, pad = tp.pad, room = tp.room;
    // The block: lines one below the other at the font's line spacing, each aligned within the widest. Around it the
    // outline's outer half and the box margin; the whole is shrunk to fit the area when larger (a box margin or an
    // outline makes the text a little smaller, as qtext did) and aligned in it.
    const QStringList lines = text.split('\n');
    const QFontMetricsF fm(font);
    double blockW = 0;
    for (const QString& line : lines) blockW = std::max(blockW, fm.horizontalAdvance(line));
    const double blockH = fm.lineSpacing() * (lines.size() - 1) + fm.height();
    const double ext = pad + room / 2;
    const double cw = blockW + 2 * ext, ch = blockH + 2 * ext;
    double k = 1.0;
    if (cw > 0 && area.width() > 0) k = std::min(k, area.width() / cw);
    if (ch > 0 && area.height() > 0) k = std::min(k, area.height() / ch);
    const double ox = halign == 0 ? area.left() : halign == 1 ? area.left() + (area.width() - cw * k) / 2
                                                               : area.right() - cw * k;
    const double oy = valign == 0 ? area.top() : valign == 1 ? area.top() + (area.height() - ch * k) / 2
                                                             : area.bottom() - ch * k;
    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        const double adv = fm.horizontalAdvance(lines[i]);
        const double x = ext + (halign == 0 ? 0.0 : halign == 1 ? (blockW - adv) / 2 : blockW - adv);
        path.addText(QPointF(x, ext + fm.ascent() + i * fm.lineSpacing()), font, lines[i]);
    }
    QImage img(rgba, w, h, w * 4, QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(ox, oy);
    p.scale(k, k);
    if (bg.alpha() > 0) {
        // As wide as the layout, but centred on the letters as drawn, not on their advances: the side bearings differ
        // per font (the box stood 17 px left and 9 px right of a subtitle with another system font)
        const QRectF ink = path.boundingRect();
        const double x0 = ink.isEmpty() ? 0.0 : ink.center().x() - cw / 2;
        p.fillRect(QRectF(x0, 0, cw, ch), bg);
    }
    if (outline > 0 && ol.alpha() > 0) p.strokePath(path, QPen(ol, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, fg);
    p.end();
}

} // namespace Text
