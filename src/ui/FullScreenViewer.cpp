// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FullScreenViewer.h"

#include "engine/Engine.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>

FullScreenViewer::FullScreenViewer(Engine* engine, const QImage& picture, QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), m_image(picture)
{
    setObjectName("FullScreenViewer");
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(engine, &Engine::frameReady, this, [this](const QImage& img) {
        m_image = img;
        update();
    });
    m_hideCursor.setSingleShot(true);
    m_hideCursor.setInterval(1500);
    connect(&m_hideCursor, &QTimer::timeout, this, [this] { setCursor(Qt::BlankCursor); });
}

void FullScreenViewer::showOn(QScreen* screen)
{
    if (screen) {
        winId(); // the native window exists: it can be put on the screen before it is shown
        if (QWindow* w = windowHandle()) w->setScreen(screen);
        setGeometry(screen->geometry()); // also without a window manager that knows full screen
    }
    showFullScreen();
    raise();
    activateWindow();
    setFocus();
    m_hideCursor.start();
}

QRectF FullScreenViewer::pictureRect() const
{
    if (m_image.isNull()) return {};
    const QSizeF s = QSizeF(m_image.size()).scaled(size(), Qt::KeepAspectRatio);
    return QRectF(QPointF((width() - s.width()) / 2, (height() - s.height()) / 2), s);
}

void FullScreenViewer::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_image.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(pictureRect(), m_image);
}

void FullScreenViewer::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
        emit exitRequested();
        return;
    }
    QWidget::keyPressEvent(event);
}

void FullScreenViewer::mouseMoveEvent(QMouseEvent* event)
{
    unsetCursor();
    m_hideCursor.start();
    QWidget::mouseMoveEvent(event);
}

void FullScreenViewer::mouseDoubleClickEvent(QMouseEvent*)
{
    emit exitRequested();
}

void FullScreenViewer::closeEvent(QCloseEvent* event)
{
    // Closed from outside (window manager, the app quitting): the main window ends the mode and deletes it
    QWidget::closeEvent(event);
    emit exitRequested();
}
