// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/SecondScreen.h"

#include "core/I18n.h"
#include "ui/FullScreenViewer.h"

#include <QAction>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>

#include <algorithm>

bool SecondScreenPlan::setScreens(const QVector<Screen>& screens)
{
    m_screens = screens;
    if (m_target.isEmpty() || screen(m_target)) return false;
    m_target.clear();
    return true;
}

QString SecondScreenPlan::next(const QString& mainScreen) const
{
    QVector<QString> others;
    for (const Screen& s : m_screens)
        if (s.name != mainScreen) others << s.name;
    if (others.isEmpty()) return {};
    if (m_target.isEmpty()) return others.first();
    const auto at = std::find(others.begin(), others.end(), m_target);
    if (at == others.end()) return others.first(); // the target is the main window's screen now: start over
    return at + 1 == others.end() ? QString() : *(at + 1);
}

const SecondScreenPlan::Screen* SecondScreenPlan::screen(const QString& name) const
{
    for (const Screen& s : m_screens)
        if (s.name == name) return &s;
    return nullptr;
}

QRectF SecondScreenPlan::fitted(const QSizeF& area, const QSizeF& frame)
{
    if (area.isEmpty() || frame.isEmpty()) return {};
    const QSizeF size = frame.scaled(area, Qt::KeepAspectRatio);
    return {QPointF((area.width() - size.width()) / 2, (area.height() - size.height()) / 2), size};
}

SecondScreenPlan::Mode SecondScreenPlan::storedMode()
{
    return QSettings().value("viewer/secondScreen").toString() == "hide" ? Mode::HideViewer : Mode::KeepArea;
}

void SecondScreenPlan::storeMode(Mode mode)
{
    QSettings().setValue("viewer/secondScreen", mode == Mode::HideViewer ? "hide" : "keep");
}

ScreenOutput::ScreenOutput(Engine* engine, QScreen* screen, const QImage& picture, QObject* parent)
    : PreviewOutput(parent)
    , m_screen(screen)
    , m_name(screen ? screen->name() : QString())
{
    m_window = new FullScreenViewer(engine, picture, false); // no parent: see FullScreenViewer.h; deleted here
    m_window->setObjectName("SecondScreenViewer");
    m_window->setWindowTitle(T("schneidi – Vorschau"));
    m_window->showOn(screen, false);
    connect(m_window, &FullScreenViewer::exitRequested, this, &PreviewOutput::lost);
    connect(m_window, &FullScreenViewer::previewEdgeWanted, this, &PreviewOutput::previewEdgeWanted);
    // Unplugged: Qt moves the window to a screen that is left - over the main window. It comes back there instead
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* gone) {
        if (gone == m_screen || !m_screen) emit lost();
    });
}

ScreenOutput::~ScreenOutput()
{
    if (m_window) {
        m_window->hide();
        m_window->deleteLater();
    }
}

QString ScreenOutput::name() const
{
    return m_name;
}

void ScreenOutput::showFrame(const QImage& frame, const QSize& projectSize)
{
    if (m_window) m_window->setPicture(frame, projectSize);
}

int ScreenOutput::wantedPreviewEdge() const
{
    return m_window ? m_window->wantedPreviewEdge() : 0;
}

QScreen* ScreenOutput::screen() const
{
    return m_screen;
}

void ScreenOutput::addActions(const QList<QAction*>& actions)
{
    if (m_window) m_window->addActions(actions);
}
