#include "ui/KeyColorPicker.h"

#include "app/Theme.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

KeyColorPicker::KeyColorPicker(const QImage& picture, QObject* parent)
    : ViewerOverlay(parent), m_picture(picture.convertToFormat(QImage::Format_RGBA8888))
{
    qApp->installEventFilter(this);
}

Timeline KeyColorPicker::isolate(const Timeline& tl, int clipId)
{
    Timeline out = tl;
    bool found = false;
    for (Track& t : out.video) {
        t.clips.removeIf([clipId](const Clip& c) { return c.id != clipId; });
        if (t.clips.isEmpty()) continue;
        found = true;
        t.hidden = false;
        Clip& c = t.clips.first();
        c.enabled = true;
        // The key renders after the other fixed stages (stabilization, grade) and before the movable effects
        c.effects.removeIf([](const EffectInstance& e) {
            return e.effectId == "chromakey" || EffectRegistry::movable(e.effectId);
        });
        c.transform.opacity = 100;
        c.transform.compositeOn = true;
        Keys::clear(c, AnimParam::Opacity);
        c.fadeIn = c.fadeOut = 0;
        c.transIn = c.transOut = 0;
    }
    for (Track& t : out.audio) t.clips.clear();
    for (SubtitleTrack& t : out.subtitles) t.cues.clear();
    if (!found) return Timeline();
    return out;
}

QColor KeyColorPicker::sample(const QImage& rgba, const QPointF& at, int radius)
{
    const QImage img = rgba.format() == QImage::Format_RGBA8888 ? rgba : rgba.convertToFormat(QImage::Format_RGBA8888);
    const int cx = int(std::floor(at.x())), cy = int(std::floor(at.y()));
    long r = 0, g = 0, b = 0, n = 0;
    for (int y = std::max(0, cy - radius); y <= std::min(img.height() - 1, cy + radius); ++y) {
        const uchar* line = img.constScanLine(y);
        for (int x = std::max(0, cx - radius); x <= std::min(img.width() - 1, cx + radius); ++x) {
            const uchar* px = line + x * 4;
            if (px[3] < 128) continue;
            r += px[0];
            g += px[1];
            b += px[2];
            ++n;
        }
    }
    if (n == 0) return {};
    return QColor(int((r + n / 2) / n), int((g + n / 2) / n), int((b + n / 2) / n));
}

QImage KeyColorPicker::loupe(const QImage& rgba, const QPointF& at, int radius)
{
    const QImage img = rgba.format() == QImage::Format_RGBA8888 ? rgba : rgba.convertToFormat(QImage::Format_RGBA8888);
    const int size = 2 * radius + 1;
    QImage out(size, size, QImage::Format_RGBA8888);
    out.fill(Qt::transparent);
    const int x0 = int(std::floor(at.x())) - radius, y0 = int(std::floor(at.y())) - radius;
    for (int y = 0; y < size; ++y) {
        if (y0 + y < 0 || y0 + y >= img.height()) continue;
        const auto* in = reinterpret_cast<const quint32*>(img.constScanLine(y0 + y));
        auto* line = reinterpret_cast<quint32*>(out.scanLine(y));
        for (int x = 0; x < size; ++x)
            if (x0 + x >= 0 && x0 + x < img.width()) line[x] = in[x0 + x];
    }
    return out;
}

namespace {
constexpr double kGap = 14, kSwatch = 30, kSwatchGap = 4; // cursor to loupe, color square beside it
}

QRectF KeyColorPicker::loupeRect(const QPointF& pos, const QRectF& frame)
{
    const double side = (2 * kLoupeRadius + 1) * kLoupeZoom;
    const double width = side + kSwatchGap + kSwatch; // the color square to its right
    QPointF tl = pos + QPointF(kGap, kGap);
    if (tl.x() + width > frame.right()) tl.setX(pos.x() - kGap - width);
    if (tl.y() + side > frame.bottom()) tl.setY(pos.y() - kGap - side);
    // Whole pixels: the magnified pixels stay sharp squares
    return QRectF(QPointF(std::round(tl.x()), std::round(tl.y())), QSizeF(side, side));
}

QPointF KeyColorPicker::imagePoint(const QPointF& pos, const QRectF& frame) const
{
    if (frame.isEmpty()) return {-1, -1};
    return {(pos.x() - frame.x()) / frame.width() * m_picture.width(),
            (pos.y() - frame.y()) / frame.height() * m_picture.height()};
}

void KeyColorPicker::paint(QPainter& p, const QRectF& frame)
{
    p.save();
    p.fillRect(frame, Qt::black);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(frame, m_picture);

    // Hint at the top
    const QString text = T("Pipette: Klick ins Bild übernimmt die Key-Farbe, Rechtsklick oder Esc bricht ab");
    QFont font = p.font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1));
    p.setFont(font);
    const QRectF hint = p.boundingRect(frame.adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignHCenter | Qt::TextWordWrap,
                                       text).adjusted(-6, -3, 6, 3);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(hint, 3, 3);
    p.setPen(Theme::text);
    p.drawText(hint, Qt::AlignCenter | Qt::TextWordWrap, text);

    // Beside the cursor (flipped at the edges): the loupe with the pixels around it from the full-resolution frame,
    // the pixel under the cursor framed, the area a click averages outlined; right of it the color a click takes
    if (m_hover.x() >= 0 && frame.contains(m_hover)) {
        const QRectF lens = loupeRect(m_hover, frame);
        const QImage patch = loupe(m_picture, imagePoint(m_hover, frame));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        // Checkerboard where the clip does not cover the frame (as transparency is shown elsewhere)
        static const QBrush checker = [] {
            constexpr int cell = kLoupeZoom / 2;
            QImage tile(2 * cell, 2 * cell, QImage::Format_RGB32);
            tile.fill(QColor(110, 110, 110));
            QPainter tp(&tile);
            tp.fillRect(cell, 0, cell, cell, QColor(70, 70, 70));
            tp.fillRect(0, cell, cell, cell, QColor(70, 70, 70));
            tp.end();
            return QBrush(tile);
        }();
        p.setBrushOrigin(lens.topLeft());
        p.fillRect(lens, checker);
        p.drawImage(lens, patch);
        auto cellRect = [&](int radius) {
            const double c = kLoupeRadius - radius;
            return QRectF(lens.x() + c * kLoupeZoom, lens.y() + c * kLoupeZoom, (2 * radius + 1) * kLoupeZoom,
                          (2 * radius + 1) * kLoupeZoom);
        };
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DashLine));
        p.drawRect(cellRect(kSampleRadius).adjusted(0, 0, -1, -1));
        p.setPen(QPen(Qt::black, 1));
        p.drawRect(cellRect(0).adjusted(-1, -1, 0, 0));
        p.setPen(QPen(Qt::white, 1));
        p.drawRect(cellRect(0).adjusted(-2, -2, 1, 1));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(Qt::black, 3));
        p.drawRect(lens);
        p.setPen(QPen(Qt::white, 1.2));
        p.drawRect(lens);
        if (m_hoverColor.isValid()) {
            const QRectF box(lens.right() + kSwatchGap, lens.bottom() - kSwatch, kSwatch, kSwatch);
            p.setBrush(m_hoverColor);
            p.setPen(QPen(Qt::black, 3));
            p.drawRoundedRect(box, 3, 3);
            p.setPen(QPen(Qt::white, 1.2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 3, 3);
        }
    }
    p.restore();
}

bool KeyColorPicker::mousePress(QMouseEvent* e, const QRectF& frame)
{
    if (e->button() == Qt::RightButton) {
        emit cancelled();
        return true;
    }
    if (e->button() != Qt::LeftButton) return true;
    const QColor c = sample(m_picture, imagePoint(e->position(), frame), kSampleRadius);
    if (c.isValid()) emit picked(c); // outside the clip's picture nothing happens
    return true;
}

Qt::CursorShape KeyColorPicker::cursorAt(const QPointF&, const QRectF&) const { return Qt::CrossCursor; }

void KeyColorPicker::hover(const QPointF& pos, const QRectF& frame)
{
    m_hover = pos;
    m_hoverColor = sample(m_picture, imagePoint(pos, frame), kSampleRadius);
    emit changed();
}

void KeyColorPicker::leave()
{
    m_hover = QPointF(-1, -1);
    emit changed();
}

bool KeyColorPicker::eventFilter(QObject* watched, QEvent* event)
{
    const bool key = event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride;
    if (key && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        // Esc is the pipette's while it is on: taken before the window's shortcuts (the large viewer's Esc), which
        // would otherwise get it first
        if (event->type() == QEvent::ShortcutOverride) event->accept();
        else emit cancelled();
        return true;
    }
    return ViewerOverlay::eventFilter(watched, event);
}
