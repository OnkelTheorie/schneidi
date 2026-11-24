// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

class QAction;
class QKeyEvent;

// J/K/L as every big editor has them (Premiere, Avid, DaVinci, Final Cut): J/L shuttle backwards/forwards, faster
// with each press (TimelineOptions::shuttleSteps: doubling or gentle 1-2-3-5-8), K stops. With K held down, a tap on
// J/L steps one frame back/forward and holding J/L plays at half speed backwards/forwards until either key is let go.
//
// The three keys are window actions (rebindable, QShortcut matching): an action only learns that it was triggered.
// Whether K (or J/L) is down comes from the key events: this object watches the press (ShortcutOverride) and the
// release of the actions' current keys as an application event filter - K from the menu or the live connection only
// stops. Leaving the app counts as letting go of everything.
class JklKeys : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<double()> speed;         // the current playback speed (0 = paused)
        std::function<void(double)> setSpeed;  // play at a speed (negative = backwards)
        std::function<void(int)> step;         // pause and move the playhead by frames
        std::function<void()> pause;
    };
    JklKeys(QAction* back, QAction* stop, QAction* forward, Hooks hooks, QObject* parent = nullptr);
    ~JklKeys() override;

    // The actions' triggers (MainWindowActions): K, J (direction -1), L (+1)
    void stopPressed();
    void shuttlePressed(int direction);

    // How long J/L has to stay down with K held for half speed instead of one frame (ms)
    static constexpr int kHoldMs = 250;
    bool stopHeld() const { return m_stopHeld; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static bool isKeyOf(const QAction* action, const QKeyEvent* e, bool exact);
    void releaseShuttle();
    void releaseAll();

    QPointer<QAction> m_back, m_stop, m_forward;
    Hooks m_hooks;
    bool m_stopHeld = false; // K is down
    int m_heldDir = 0;       // J (-1) or L (+1) went down while K was held and is still down
    bool m_slow = false;     // half speed of a held K+J/L is playing
    QTimer m_hold;           // J/L held long enough with K: half speed
};
