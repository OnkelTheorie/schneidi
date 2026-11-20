// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QObject>

// Keeps the screen on (no screen saver, no display power-off) while active - e.g. while the timeline plays: watching a
// cut without touching mouse or keyboard must not blank the monitor.
// Linux: org.freedesktop.ScreenSaver.Inhibit over D-Bus (KDE, GNOME, Xfce, …; without D-Bus nothing happens);
// Windows: SetThreadExecutionState(ES_DISPLAY_REQUIRED); elsewhere nothing.
class ScreenAwake : public QObject {
    Q_OBJECT
public:
    explicit ScreenAwake(QObject* parent = nullptr);
    ~ScreenAwake() override;

    void setActive(bool active);
    bool active() const { return m_active; }
    // An inhibition is held right now (Linux: the desktop answered the request); for tests and the log
    bool holding() const;

private:
    void release();

    bool m_active = false;
    bool m_waiting = false; // Linux: Inhibit sent, no answer yet
    uint m_cookie = 0;      // Linux: the desktop's cookie of the held inhibition (0 = none)
    bool m_windowsHeld = false;
};
