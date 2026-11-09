// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The in-app tours in the window: Help › Tours, the one question at the first start (and at a page's first opening,
// for tours offered there), the spotlight (ui/TourOverlay) on the real widgets, the conditions wait steps wait for.
// The tours themselves are data (core/Tours.h), what was answered is in the settings (TourState).

#include "core/Tours.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <functional>

class Project;
class QMainWindow;
class QMenu;
class QAction;
class TourOverlay;

class TourManager : public QObject {
    Q_OBJECT
public:
    // What the window gives the tours (all in the window's coordinates)
    struct Host {
        std::function<QString()> page;                    // the shown page ("media", "edit", "comp", "color", "deliver")
        std::function<void(const QString&)> showPage;     // switch to a page
        std::function<int()> playhead;                    // timeline frame
        std::function<void()> loadSample;                 // the built-in sample clips into the Media Pool
        QHash<QString, std::function<QRect()>> areas;     // "area:<name>" targets
    };
    TourManager(QMainWindow* window, Project* project, Host host);

    // Help › Tours: every tour, "Offer tours" (on/off) and "Offer all tours again"
    void addMenu(QMenu* help);
    // At the end of the start (not in test runs: TourState::suppressed): the start tour's question, once
    void offerAtStart();
    // A page opened by the user: its tour's question, once (never while a tour runs or a question is open)
    void pageOpened(const QString& page);

    void start(const QString& id);
    void stop(); // ends a running tour or question quietly
    bool asking() const { return !m_asking.isEmpty(); }
    TourRunner* runner() { return &m_runner; }
    TourOverlay* overlay() const { return m_overlay; }
    // The rectangles a target string stands for (window coordinates; empty = nothing visible of it)
    QVector<QRect> resolve(const QString& target) const;

public slots:
    void poke(); // something changed a wait condition could depend on (project, playhead, page)

private:
    void ask(const Tour& tour, bool firstStart);
    void showStep();
    void finished(bool completed);
    bool holds(const QString& condition) const;
    void begin(const TourStep& step);
    int clipCount() const;
    QAction* findAction(const QString& id) const;

    QMainWindow* m_window;
    Project* m_project;
    Host m_host;
    TourOverlay* m_overlay;
    TourRunner m_runner;
    QString m_asking;    // the tour asked about now
    QString m_startPage; // the page shown when the tour began (shown again at its end)
    // What "changed" compares with, taken when a step is entered
    int m_basePlayhead = 0;
    int m_baseClips = 0;
    QPointer<QAction> m_offerAction;
};
