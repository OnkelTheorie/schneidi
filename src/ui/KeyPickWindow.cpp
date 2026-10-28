// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/KeyPickWindow.h"

#include "core/I18n.h"
#include "ui/KeyColorPicker.h"

#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

KeyPickWindow::KeyPickWindow(KeyColorPicker* picker, const QSize& picture, QWidget* parent)
    : QWidget(parent, Qt::Window), m_picker(picker), m_picture(picture),
      m_center(QPointF(picture.width(), picture.height()) / 2)
{
    setObjectName("KeyPickWindow");
    setWindowTitle(T("Key-Farbe wählen"));
    setWindowModality(Qt::WindowModal); // the timeline and the Inspector wait until the color is picked
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    connect(picker, &ViewerOverlay::changed, this, qOverload<>(&QWidget::update));
}

QSize KeyPickWindow::sizeFor(const QSize& picture, const QSize& available)
{
    const QSize area = (QSizeF(available) * kScreenShare).toSize();
    if (picture.isEmpty()) return area;
    return picture.scaled(area, Qt::KeepAspectRatio).expandedTo(QSize(160, 90));
}

void KeyPickWindow::showOn(QScreen* screen)
{
    if (screen) {
        winId(); // the native window exists: it can be put on the screen before it is shown
        if (QWindow* w = windowHandle()) w->setScreen(screen);
        const QRect area = screen->availableGeometry();
        QRect r(QPoint(), sizeFor(m_picture, area.size()));
        r.moveCenter(area.center());
        setGeometry(r);
    }
    show();
    raise();
    activateWindow();
    setFocus();
}

double KeyPickWindow::fitScale() const
{
    if (m_picture.isEmpty()) return 1;
    return std::min(width() / double(m_picture.width()), height() / double(m_picture.height()));
}

QRectF KeyPickWindow::pictureRect() const
{
    if (m_picture.isEmpty()) return rect();
    const double scale = fitScale() * m_zoom;
    const QSizeF s = QSizeF(m_picture) * scale;
    return QRectF(QPointF(width() / 2.0 - m_center.x() * scale, height() / 2.0 - m_center.y() * scale), s);
}

void KeyPickWindow::clampCenter()
{
    // Zoomed in, the picture always fills the window (no black beyond its edges); fitting, it stays in the middle
    const double scale = fitScale() * m_zoom;
    const auto clamp = [](double c, double length, double view) {
        const double half = view / 2;
        return length <= view ? length / 2 : std::clamp(c, half, length - half);
    };
    m_center = {clamp(m_center.x(), m_picture.width(), width() / scale),
                clamp(m_center.y(), m_picture.height(), height() / scale)};
}

void KeyPickWindow::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    if (notches == 0 || m_picture.isEmpty()) return;
    const QPointF pos = event->position();
    const double before = fitScale() * m_zoom;
    const QPointF under = m_center + (pos - QPointF(width() / 2.0, height() / 2.0)) / before; // picture point
    m_zoom = std::clamp(m_zoom * std::pow(kZoomStep, notches), 1.0, kMaxZoom);
    const double after = fitScale() * m_zoom;
    m_center = under - (pos - QPointF(width() / 2.0, height() / 2.0)) / after; // the same point stays under the cursor
    clampCenter();
    if (m_picker) m_picker->hover(pos, pictureRect());
    update();
}

void KeyPickWindow::resizeEvent(QResizeEvent*)
{
    clampCenter();
}

void KeyPickWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_picker) m_picker->paint(p, pictureRect());
}

void KeyPickWindow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::MiddleButton) {
        m_dragFrom = event->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (m_picker) m_picker->mousePress(event, pictureRect());
}

void KeyPickWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragFrom.x() >= 0 && (event->buttons() & Qt::MiddleButton)) {
        m_center -= (event->position() - m_dragFrom) / (fitScale() * m_zoom);
        m_dragFrom = event->position();
        clampCenter();
        update();
        return;
    }
    if (m_picker && event->buttons() == Qt::NoButton) m_picker->hover(event->position(), pictureRect());
}

void KeyPickWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::MiddleButton) return;
    m_dragFrom = {-1, -1};
    setCursor(Qt::CrossCursor);
}

void KeyPickWindow::leaveEvent(QEvent*)
{
    if (m_picker) m_picker->leave();
}

void KeyPickWindow::closeEvent(QCloseEvent* event)
{
    // Closed from outside (window manager): the main window ends the pick and deletes the window
    QWidget::closeEvent(event);
    emit closeRequested();
}
