// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// In-app tours (Help › Tours): short guided walks through the real interface. A tour is data (assets/tours/*.json in
// the resources): steps with a target, a text (German dictionary key, translated on display with T()) and optionally a
// condition the step waits for ("drag a clip into the timeline" goes on by itself once a clip is there). No widgets
// here: the TourRunner steps through a tour, TourState remembers in the settings what was offered and answered. The
// spotlight (ui/TourOverlay) and the wiring into the window (app/TourManager) live in the GUI.

#include <QObject>
#include <QString>
#include <QVector>
#include <functional>

struct TourStep {
    // What the spotlight shows. Empty = nothing (the bubble in the middle). Otherwise one or more targets joined by '|':
    //   "<name>"           a widget by its "tourTarget" property or objectName (the first visible one)
    //   "area:<name>"      a part of a widget the window computes (e.g. "area:playhead")
    //   "menu:<action id>" the menu in the menu bar holding that action (ids of the key bindings)
    QString target;
    QString text; // German dictionary key
    QString page; // the page the step needs ("media", "edit", "comp", "color", "deliver"; empty = the current one)
    // Condition the step waits for (empty = none): the tour goes on by itself once it holds; "Skip" goes on anyway.
    // The window knows the names (app/TourManager.cpp: media_imported, clip_on_timeline, playhead_moved, ...)
    QString wait;
    // An extra button the step offers while the window says it is needed ("clips" = the built-in sample clips in an
    // empty project, "comp_example" = a small example graph; the window knows the names, app/MainWindowTours.cpp)
    QString offer;
    QString action; // done by the window when the step is entered (e.g. "comp_nodes": the Comp page's node view)
    // Controls the text tells the user to use beside the target (same form as `target`, no hole): the bubble keeps
    // off them
    QString keep;
    // The timeline tool the step needs, switched to when it is entered ("select", "trim", "blade"; empty = as it is).
    // The user's own tool comes back when the tour ends
    QString tool;
    // What the step needs on the practice project, made sure of when it is entered ("clip" = a video clip on the
    // timeline: placed when there is none, e.g. the step asking to drag one was skipped). Clips the user deleted are
    // brought back at any step once the timeline had some (app/TourManager.cpp)
    QString needs;
};

struct Tour {
    QString id;    // "first_steps", also the settings key
    QString title; // German dictionary key ("Erste Schritte")
    int order = 0; // place in Help › Tours
    // When it is offered unasked (once): "start" = at the first start, "page:<id>" = when that page first opens,
    // empty = only from the menu
    QString offerOn;
    QString group; // where Help › Tours lists it: empty = the tours, "guides" = the submenu with the task guides
    // The practice project the tour runs on (never the user's own clips; the window builds it from the built-in sample
    // clips, app/MainWindowTours.cpp): "pool" = the clips in the Media Pool and an empty timeline, "timeline" (default)
    // = also one after another on the timeline with the playhead in the first, "stack" = like "timeline" plus a clip on
    // a track above the first one (picture in picture, keys)
    QString practice = QStringLiteral("timeline");
    QVector<TourStep> steps;
};

namespace Tours {

// A tour from its JSON (format in the comments of Tours.cpp); unreadable = empty id, `error` says why
Tour parse(const QByteArray& json, QString* error = nullptr);
// The built-in tours (":/tours/*.json"), by order
QVector<Tour> builtIn();
// One of them by id (empty id = none)
Tour find(const QString& id);

} // namespace Tours

// What the user answered, per tour, in the settings (tours/offer, tours/state/<id>). Asked exactly once: any answer
// (or starting it from the menu) means never asked again; "Don't offer tours" at the first start stops every question
// (Help › Tours › "Offer tours" turns it on again). Test runs never ask (setSuppressed).
namespace TourState {

enum class Answer { Show, Later, Never };

// Test runs (--screenshot, --demo, --actions, offscreen) and runs started for an AI: never ask
void setSuppressed(bool on);
bool suppressed();

bool offerTours();          // the global switch (default on)
void setOfferTours(bool on);
QString state(const QString& id); // "" = never asked, "later", "shown"
// Ask about this tour now? Only when offering is on, not suppressed and never answered
bool shouldAsk(const QString& id);
void answer(const QString& id, Answer a);
void markShown(const QString& id); // started from the menu: no question about it later
void resetAll();                   // Help › Tours › "Offer tours again": every tour is asked about once more

} // namespace TourState

// Steps through one tour. The window feeds it: conditions (a function answering "does condition X hold now?") and
// check() whenever something changed that a condition could depend on (core signals). Wait steps whose condition
// becomes true while shown go on by themselves; one that already holds when entered is just a normal step.
class TourRunner : public QObject {
    Q_OBJECT
public:
    explicit TourRunner(QObject* parent = nullptr);
    // begin: called when a step is entered (the window remembers what "changed" means, e.g. the playhead now);
    // holds: whether a condition holds now
    void setConditions(std::function<void(const TourStep&)> begin, std::function<bool(const QString&)> holds);

    void start(const Tour& tour);
    bool running() const { return m_index >= 0; }
    const Tour& tour() const { return m_tour; }
    int index() const { return m_index; } // 0-based, -1 = not running
    int count() const { return int(m_tour.steps.size()); }
    const TourStep* step() const;
    bool waiting() const { return m_waiting; } // the shown step waits for its condition

    void next();  // also skips a waiting step
    void back();
    void finish(); // Esc, "End tour"; finished(false) unless it was the last step

public slots:
    void check(); // re-test a waiting step's condition

signals:
    void stepChanged(int index);
    void waitingChanged(bool waiting);
    void finished(bool completed); // completed = went past the last step

private:
    void enter(int index);
    Tour m_tour;
    int m_index = -1;
    bool m_waiting = false;
    std::function<void(const TourStep&)> m_begin;
    std::function<bool(const QString&)> m_holds;
};
