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

    // Color under the cursor in a square beside it (flipped at the edges)
    if (m_hover.x() >= 0 && m_hoverColor.isValid() && frame.contains(m_hover)) {
        constexpr double size = 30, gap = 14;
        QPointF tl = m_hover + QPointF(gap, gap);
        if (tl.x() + size > frame.right()) tl.setX(m_hover.x() - gap - size);
        if (tl.y() + size > frame.bottom()) tl.setY(m_hover.y() - gap - size);
        const QRectF box(tl, QSizeF(size, size));
        p.setBrush(m_hoverColor);
        p.setPen(QPen(Qt::black, 3));
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QPen(Qt::white, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 3, 3);
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
    const QColor c = sample(m_picture, imagePoint(e->position(), frame));
    if (c.isValid()) emit picked(c); // outside the clip's picture nothing happens
    return true;
}

Qt::CursorShape KeyColorPicker::cursorAt(const QPointF&, const QRectF&) const { return Qt::CrossCursor; }

void KeyColorPicker::hover(const QPointF& pos, const QRectF& frame)
{
    m_hover = pos;
    m_hoverColor = sample(m_picture, imagePoint(pos, frame));
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
