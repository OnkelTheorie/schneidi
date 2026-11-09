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
        // "area:<name>" targets: a part of a widget (in that widget's coordinates; no widget = not there)
        struct Area {
            QWidget* widget = nullptr;
            QRect rect;
        };
        QHash<QString, std::function<Area()>> areas;
        QHash<QString, std::function<bool()>> conditions; // wait conditions beyond the built-in ones
        // Where the bubble had better not sit (toolbars, tabs, the page bar, the graph's nodes), window coordinates
        std::function<QVector<QRect>()> avoid;
        // The working area (the pages, without the bars): the bubble's home is one of its corners
        std::function<QRect()> home;
        std::function<void(const QString&)> setTool; // a step's timeline tool ("select", "trim", "blade")
        QHash<QString, std::function<void()>> actions;    // a step's "action", done when it is entered
        // A step's "offer": the button's text (German key), whether it is needed now, what it does
        struct Offer {
            QString text;
            std::function<bool()> needed;
            std::function<void()> run;
        };
        QHash<QString, Offer> offers;
        // Tour mode: enter = the tour's practice project (Tour::practice) and a fixed layout instead of the user's
        // project and panels; leave = the user's project, page, panels and sizes back exactly as they were
        std::function<void(const Tour&)> enter;
        std::function<void()> leave;
        // The user's project has unsaved changes (asked about before a tour), save = save it (false = not saved)
        std::function<bool()> unsaved;
        std::function<bool()> save;
        // Why no tour can start now (a render, an AI change waiting for review ...; shown in the status bar), empty = it can
        std::function<QString()> blocked;
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
    // A step's text as the bubble shows it (rich text): translated, "{key:<action id>}" replaced by the action's key in
    // the user's current key set (bold), or by the action's name when it has no key
    QString stepText(const TourStep& step) const;
    TourOverlay* overlay() const { return m_overlay; }
    // The rectangles a target string stands for (window coordinates; empty = nothing visible of it)
    QVector<QRect> resolve(const QString& target) const;
    // The same with the widget each one belongs to (the widget itself, the one holding an area, the menu bar)
    struct Target {
        QString part; // the part of the target string
        QRect rect;   // window coordinates
        QWidget* widget = nullptr;
    };
    QVector<Target> targets(const QString& target) const;

signals:
    void runningChanged(bool running); // a tour began / ended (after leave)

public slots:
    void poke(); // something changed a wait condition could depend on (project, playhead, page)

private:
    void ask(const Tour& tour, bool firstStart);
    void launch(const Tour& tour); // into tour mode and the first step
    void enterStep(); // a step entered: its page, its action
    void showStep();  // the bubble for the step as it is now (waiting, offer)
    void finished(bool completed);
    bool holds(const QString& condition) const;
    void begin(const TourStep& step);
    int clipCount() const;
    QVector<int> timelineShape() const;
    QAction* findAction(const QString& id) const;
    QWidget* widgetFor(const QString& part) const; // a widget target ("<name>"), the first visible one

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
    QVector<int> m_baseTimeline; // every clip's place and length (a trim changes it)
    QPointer<QAction> m_offerAction;
};
