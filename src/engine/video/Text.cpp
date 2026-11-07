// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Text.h"

#include "engine/video/Compose.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QLinearGradient>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

namespace Text {

namespace {

// How far the look's effects reach beyond the letters (pixels)
double lookReach(const Look& l)
{
    double r = l.edgeDepth;
    if (l.shadow.alpha() > 0) r = std::max(r, std::hypot(l.shadowX, l.shadowY) + l.shadowSoft);
    if (l.glow.alpha() > 0) r = std::max(r, l.glowRadius);
    return r;
}

// One pass of a box blur of radius r over a plane (rows then columns), in place; 3 passes come close to a gaussian
void boxPass(std::vector<uint8_t>& a, int w, int h, int r, std::vector<uint8_t>& tmp)
{
    if (r < 1) return;
    tmp.resize(a.size());
    const int win = 2 * r + 1;
    const uint32_t inv = (1u << 24) / uint32_t(win); // sum * inv >> 24 = sum / win
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = a.data() + size_t(y) * w;
        uint8_t* dst = tmp.data() + size_t(y) * w;
        uint32_t sum = 0;
        for (int i = -r; i <= r; ++i) sum += (i >= 0 && i < w) ? src[i] : 0;
        for (int x = 0; x < w; ++x) {
            dst[x] = uint8_t((uint64_t(sum) * inv) >> 24);
            const int in = x + r + 1, out = x - r;
            if (in < w) sum += src[in];
            if (out >= 0) sum -= src[out];
        }
    }
    std::vector<uint32_t> col(size_t(w), 0);
    for (int y = -r; y <= r; ++y)
        if (y >= 0 && y < h)
            for (int x = 0; x < w; ++x) col[x] += tmp[size_t(y) * w + x];
    for (int y = 0; y < h; ++y) {
        uint8_t* dst = a.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) dst[x] = uint8_t((uint64_t(col[x]) * inv) >> 24);
        const int in = y + r + 1, out = y - r;
        if (in < h)
            for (int x = 0; x < w; ++x) col[x] += tmp[size_t(in) * w + x];
        if (out >= 0)
            for (int x = 0; x < w; ++x) col[x] -= tmp[size_t(out) * w + x];
    }
}

void blurPlane(std::vector<uint8_t>& a, int w, int h, double reach)
{
    const int r = int(std::lround(reach / 3));
    if (r < 1) return;
    std::vector<uint8_t> tmp;
    for (int i = 0; i < 3; ++i) boxPass(a, w, h, r, tmp);
}

// A plane as a picture in one colour (its alpha times the plane); rows of the plane brighter at the top and darker
// at the bottom when `light` (the edge lit from above)
QImage tinted(const std::vector<uint8_t>& a, int w, int h, const QColor& c, double gain = 1.0, bool light = false,
              int inkTop = 0, int inkBottom = 0)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    const int ca = c.alpha();
    for (int y = 0; y < h; ++y) {
        double g = gain;
        if (light && inkBottom > inkTop) // 1.35 at the top of the letters to 0.65 at their bottom
            g *= 1.35 - 0.7 * std::clamp(double(y - inkTop) / (inkBottom - inkTop), 0.0, 1.0);
        const int r = std::min(255, int(c.red() * g + 0.5)), gr = std::min(255, int(c.green() * g + 0.5)),
                  b = std::min(255, int(c.blue() * g + 0.5));
        auto* row = reinterpret_cast<uint32_t*>(img.scanLine(y));
        const uint8_t* src = a.data() + size_t(y) * w;
        for (int x = 0; x < w; ++x) {
            const unsigned al = (src[x] * ca + 127) / 255;
            row[x] = (al << 24) | (((r * al + 127) / 255) << 16) | (((gr * al + 127) / 255) << 8) | ((b * al + 127) / 255);
        }
    }
    return img;
}

// Glow, shadow and edge of the silhouette (letters + outline pen) under the painter's current transform
void drawBehind(QPainter& p, const QPainterPath& path, double outlinePen, const Look& l)
{
    const QTransform tr = p.transform();
    const double half = outlinePen / 2;
    const QRectF dev = tr.mapRect(path.boundingRect().adjusted(-half, -half, half, half));
    if (dev.isEmpty()) return;
    const int margin = int(std::ceil(std::max(l.shadow.alpha() > 0 ? l.shadowSoft : 0.0,
                                              l.glow.alpha() > 0 ? l.glowRadius : 0.0))) + 2;
    const int x0 = int(std::floor(dev.left())) - margin, y0 = int(std::floor(dev.top())) - margin;
    const int w = int(std::ceil(dev.right())) + margin - x0, h = int(std::ceil(dev.bottom())) + margin - y0;
    if (w <= 0 || h <= 0 || double(w) * h > 64e6) return;
    QImage sil(w, h, QImage::Format_ARGB32_Premultiplied);
    sil.fill(0);
    {
        QPainter mp(&sil);
        mp.setRenderHint(QPainter::Antialiasing);
        mp.setTransform(tr * QTransform::fromTranslate(-x0, -y0));
        mp.fillPath(path, Qt::white);
        if (outlinePen > 0) mp.strokePath(path, QPen(Qt::white, outlinePen, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    }
    std::vector<uint8_t> mask(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(sil.constScanLine(y));
        for (int x = 0; x < w; ++x) mask[size_t(y) * w + x] = uint8_t(row[x] >> 24);
    }
    p.save();
    p.resetTransform();
    if (l.glow.alpha() > 0 && l.glowRadius > 0) {
        std::vector<uint8_t> b = mask;
        blurPlane(b, w, h, l.glowRadius);
        const QImage g = tinted(b, w, h, l.glow);
        p.drawImage(QPoint(x0, y0), g);
        p.drawImage(QPoint(x0, y0), g); // twice: a glow that shows also around thin letters
    }
    if (l.shadow.alpha() > 0) {
        std::vector<uint8_t> b = mask;
        blurPlane(b, w, h, l.shadowSoft);
        p.drawImage(QPointF(x0 + l.shadowX, y0 + l.shadowY), tinted(b, w, h, l.shadow));
    }
    if (l.edgeDepth > 0) {
        // Copies one pixel apart along the main axis of the direction, the far third darker (depth)
        const double a = l.edgeAngle * M_PI / 180.0;
        const double dx = std::cos(a), dy = std::sin(a);
        const double step = 1.0 / std::max({std::abs(dx), std::abs(dy), 1e-6});
        const int n = std::clamp(int(std::ceil(l.edgeDepth / step)), 1, 400);
        const int inkTop = int(dev.top()) - y0, inkBottom = int(dev.bottom()) - y0;
        const QImage tones[3] = {tinted(mask, w, h, l.edgeColor, 0.6, l.edgeLight, inkTop, inkBottom),
                                 tinted(mask, w, h, l.edgeColor, 0.8, l.edgeLight, inkTop, inkBottom),
                                 tinted(mask, w, h, l.edgeColor, 1.0, l.edgeLight, inkTop, inkBottom)};
        for (int i = n; i >= 1; --i) {
            const double d = std::min(l.edgeDepth, i * step);
            const int tone = std::min(2, (n - i) * 3 / n);
            p.drawImage(QPoint(x0 + int(std::lround(dx * d)), y0 + int(std::lround(dy * d))), tones[tone]);
        }
    }
    p.restore();
}

// The letters' brush of a styled fill: texture or gradient over the ink (local coordinates of the block)
QBrush fillBrush(const Look& l, const QRectF& ink, double blockW, double k)
{
    if (!l.texture.isNull()) {
        QBrush b(l.texture);
        const double s = std::max(1e-3, blockW * std::max(0.01, l.texScale)) / std::max(1, l.texture.width());
        b.setTransform(QTransform::fromScale(s, s) * QTransform::fromTranslate(l.texX / k, l.texY / k));
        return b;
    }
    const double a = l.gradAngle * M_PI / 180.0;
    const QPointF d(std::cos(a), std::sin(a));
    const double half = (std::abs(d.x()) * ink.width() + std::abs(d.y()) * ink.height()) / 2;
    QLinearGradient g(ink.center() - d * half, ink.center() + d * half);
    for (const auto& s : l.stops) g.setColorAt(std::clamp(s.first, 0.0, 1.0), s.second);
    return QBrush(g);
}

} // namespace

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
    double margin = (std::max(tp.outline, tp.room) + 0.5 * tp.font.pixelSize() + 2 * tp.pad) * k + 4;
    if (tp.look) margin += lookReach(*tp.look);
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
    const Look* look = tp.look;
    if (look && tp.lookBehind && look->behind()) drawBehind(p, path, outline > 0 && ol.alpha() > 0 ? outline : 0.0, *look);
    if (outline > 0 && ol.alpha() > 0) p.strokePath(path, QPen(ol, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (look && tp.lookFill && look->styledFill()) p.fillPath(path, fillBrush(*look, path.boundingRect(), cw, k));
    else p.fillPath(path, fg);
    p.end();
}

void drawWords(uint8_t* rgba, int w, int h, const WordParams& tp)
{
    const QFont& font = tp.font;
    const QFontMetricsF fm(font);
    const double space = fm.horizontalAdvance(QLatin1Char(' '));
    const double reserve = std::max(1.0, tp.reserve);
    // A word takes its own width, a larger word (highlight, pop overshoot) its larger width: the words next to it
    // move aside and keep a full space to it (laid out at the normal width and grown around its centre, a larger
    // word covered the spaces: "Thisis word"). The size of the block (fit into the area, the box) counts the largest
    // word grown by `reserve`, so the text does not change size or wrap while the highlight moves.
    const auto slotOf = [&](const Word& word) { return fm.horizontalAdvance(word.text) * std::max(1.0, word.scale); };
    QVector<double> lineW;
    double blockW = 0;
    for (const QVector<Word>& line : tp.lines) {
        double lw = 0, natural = 0, widest = 0;
        for (int i = 0; i < line.size(); ++i) {
            const double adv = fm.horizontalAdvance(line[i].text);
            lw += (i ? space : 0.0) + slotOf(line[i]);
            natural += (i ? space : 0.0) + adv;
            widest = std::max(widest, adv);
        }
        lineW << lw;
        blockW = std::max({blockW, lw, natural + widest * (reserve - 1)});
    }
    if (tp.lines.isEmpty() || blockW <= 0) return;
    const double blockH = fm.lineSpacing() * (tp.lines.size() - 1) + fm.height();
    const double ext = tp.pad + tp.outline / 2;
    const double cw = blockW + 2 * ext, ch = blockH + 2 * ext;
    const QRectF& area = tp.area;
    double k = 1.0;
    if (area.width() > 0) k = std::min(k, area.width() / cw);
    if (area.height() > 0) k = std::min(k, area.height() / ch);
    const double ox = tp.halign == 0 ? area.left() : tp.halign == 1 ? area.left() + (area.width() - cw * k) / 2
                                                                    : area.right() - cw * k;
    const double oy = tp.valign == 0 ? area.top() : tp.valign == 1 ? area.top() + (area.height() - ch * k) / 2
                                                                   : area.bottom() - ch * k;
    struct Placed {
        QPainterPath path;
        const Word* word;
    };
    std::vector<Placed> placed;
    for (int l = 0; l < tp.lines.size(); ++l) {
        const double baseline = ext + fm.ascent() + l * fm.lineSpacing();
        double x = ext + (tp.halign == 0 ? 0.0 : tp.halign == 1 ? (blockW - lineW[l]) / 2 : blockW - lineW[l]);
        // Right-to-left text (Arabic, Hebrew): the first word on the right (each word's letters are shaped by Qt)
        const QVector<Word>& line = tp.lines[l];
        QString joined;
        for (const Word& word : line) joined += word.text + ' ';
        const bool rtl = joined.isRightToLeft();
        for (int i = 0; i < line.size(); ++i) {
            const Word& word = line[rtl ? line.size() - 1 - i : i];
            const double adv = fm.horizontalAdvance(word.text), slot = slotOf(word);
            QPainterPath path;
            path.addText(QPointF(x + (slot - adv) / 2, baseline), font, word.text);
            if (word.scale != 1.0) {
                const QPointF c(x + slot / 2, baseline - fm.ascent() + fm.height() / 2);
                QTransform t;
                t.translate(c.x(), c.y());
                t.scale(word.scale, word.scale);
                t.translate(-c.x(), -c.y());
                path = t.map(path);
            }
            placed.push_back({path, &word});
            x += slot + space;
        }
    }
    QImage img(rgba, w, h, w * 4, QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(ox, oy);
    p.scale(k, k);
    if (tp.bg.alpha() > 0) p.fillRect(QRectF(0, 0, cw, ch), tp.bg);
    const double off = font.pixelSize() / 10.0;
    if (tp.shadow.alpha() > 0)
        for (const Placed& pl : placed) {
            p.setOpacity(pl.word->opacity);
            p.fillPath(pl.path.translated(off, off), tp.shadow);
            if (tp.outline > 0)
                p.strokePath(pl.path.translated(off, off), QPen(tp.shadow, tp.outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
    if (tp.outline > 0 && tp.outlineColor.alpha() > 0)
        for (const Placed& pl : placed) {
            p.setOpacity(pl.word->opacity);
            const double width = tp.outline * pl.word->scale;
            p.strokePath(pl.path, QPen(tp.outlineColor, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
    for (const Placed& pl : placed) {
        p.setOpacity(pl.word->opacity);
        p.fillPath(pl.path, pl.word->fg);
    }
    p.end();
}

} // namespace Text
