#include "ui/KeyPickWindow.h"

#include "core/I18n.h"
#include "ui/KeyColorPicker.h"

#include <QCloseEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>

KeyPickWindow::KeyPickWindow(KeyColorPicker* picker, const QSize& picture, QWidget* parent)
    : QWidget(parent, Qt::Window), m_picker(picker), m_picture(picture)
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

QRectF KeyPickWindow::pictureRect() const
{
    if (m_picture.isEmpty()) return rect();
    const QSizeF s = QSizeF(m_picture).scaled(size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void KeyPickWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_picker) m_picker->paint(p, pictureRect());
}

void KeyPickWindow::mousePressEvent(QMouseEvent* event)
{
    if (m_picker) m_picker->mousePress(event, pictureRect());
}

void KeyPickWindow::mouseMoveEvent(QMouseEvent* event)
{
    if (m_picker && event->buttons() == Qt::NoButton) m_picker->hover(event->position(), pictureRect());
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
