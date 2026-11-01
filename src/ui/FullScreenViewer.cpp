// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FullScreenViewer.h"

#include "engine/Engine.h"
#include "ui/FrameView.h"

#include <QCloseEvent>
#include <QKeyEvent>
#include <QScreen>
#include <QVBoxLayout>

FullScreenViewer::FullScreenViewer(Engine* engine, const QImage& picture)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint)
{
    setObjectName("FullScreenViewer");
    setWindowTitle("schneidi");
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    m_view = new FrameView(this);
    m_view->setObjectName("full_screen_frame");
    m_view->setBackground(Qt::black);
    m_view->setFrameSize(engine->frameSize());
    m_view->setFrame(picture);
    m_view->installEventFilter(this); // pointer hiding and double click also over the picture
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_view);
    connect(engine, &Engine::frameReady, this, [this, engine](const QImage& img) {
        m_view->setFrameSize(engine->frameSize());
        m_view->setFrame(img);
    });
    m_hideCursor.setSingleShot(true);
    m_hideCursor.setInterval(1500);
    // On the picture: its own pointer (hover over the picture sets one) would win over the window's
    connect(&m_hideCursor, &QTimer::timeout, this, [this] {
        setCursor(Qt::BlankCursor);
        m_view->setIdleCursor(Qt::BlankCursor);
    });
}

void FullScreenViewer::showOn(QScreen* screen)
{
    if (screen) {
        setScreen(screen); // not winId() + windowHandle()->setScreen(): see the header (native window)
        setGeometry(screen->geometry()); // also without a window manager that knows full screen
    }
    showFullScreen();
    raise();
    activateWindow();
    setFocus();
    m_hideCursor.start();
}

const QImage& FullScreenViewer::picture() const
{
    return m_view->frame();
}

QRectF FullScreenViewer::pictureRect() const
{
    return m_view->frameRect();
}

bool FullScreenViewer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_view) {
        if (event->type() == QEvent::MouseMove) {
            unsetCursor();
            m_view->setIdleCursor(Qt::ArrowCursor);
            m_hideCursor.start();
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            emit exitRequested();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
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
