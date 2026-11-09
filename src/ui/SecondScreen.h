// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Preview on a second monitor (Workspace › Preview on Second Monitor, Ctrl+Shift+F): the picture full screen on
// another screen, without window chrome, letterboxed by the timeline's aspect (a portrait-rotated monitor shows a
// 9:16 timeline filling it). The timeline's picture is then only on the monitor. Two ways for the main window
// (Settings › General): the viewer's area stays as a source viewer (source clips opened by double click, else an empty
// page with a button to bring the preview back - Viewer::setProgramElsewhere) - or the viewer goes away and the panels
// around it take its room.
// SecondScreenPlan holds the choices (which screen next, what is left after a monitor went away, what the main window
// shows) without any QScreen, so the tests can feed it made-up screens (several, portrait, unplugged). ScreenOutput
// is the PreviewOutput on a real screen.

#include "ui/PreviewOutput.h"

#include <QPointer>
#include <QRect>
#include <QString>
#include <QVector>

class Engine;
class FullScreenViewer;
class QScreen;

class SecondScreenPlan {
public:
    struct Screen {
        QString name;   // QScreen::name(): stays the same while the monitor is plugged in
        QRect geometry; // desktop coordinates; a portrait-rotated monitor is taller than wide
    };
    // What the main window does with its viewer while the preview is on another screen
    enum class Mode {
        KeepArea,   // "show source clips": the area stays as a source viewer, the timeline only on the monitor
        HideViewer, // the viewer goes, the panels beside/below it close up
    };

    // The screens there are now. True = the screen the preview was on is gone: the target is cleared (the preview
    // comes back into the main window). A screen coming (back) never sets a target by itself.
    bool setScreens(const QVector<Screen>& screens);
    const QVector<Screen>& screens() const { return m_screens; }
    // Where the preview is now; empty = in the main window
    const QString& target() const { return m_target; }
    void setTarget(const QString& name) { m_target = name; }
    // The target after the key/menu entry: the next screen other than `mainScreen` (the one the main window is on),
    // in the order of the screen list; after the last one back into the main window (empty). With two screens that is
    // simply on/off.
    QString next(const QString& mainScreen) const;
    // The preview is on the monitor the main window is on (the window was dragged there, or its own monitor went away
    // and Qt moved it there): it would cover the main window, so it comes back into it
    bool coversMain(const QString& mainScreen) const { return !m_target.isEmpty() && m_target == mainScreen; }
    const Screen* screen(const QString& name) const;

    // The picture's rectangle on an area: as large as fits, the aspect kept, centered (black bars around)
    static QRectF fitted(const QSizeF& area, const QSizeF& frame);
    // Does the main window show its viewer?
    static bool viewerInWindow(Mode mode, bool outputOn) { return !outputOn || mode == Mode::KeepArea; }
    // Does the main window's viewer leave the timeline's picture to the output (shows source clips only)?
    static bool programElsewhere(Mode mode, bool outputOn) { return outputOn && mode == Mode::KeepArea; }
    // Does the output keep its last timeline picture instead of the engine's frame? While the engine shows a source
    // clip and the viewer's area stays (the source belongs into the main window, the monitor is the program)
    static bool holdsTimeline(Mode mode, bool sourceShown) { return mode == Mode::KeepArea && sourceShown; }

    // The user's choice (settings "viewer/secondScreen": "keep" - the default - or "hide")
    static Mode storedMode();
    static void storeMode(Mode mode);

private:
    QVector<Screen> m_screens;
    QString m_target;
};

// The preview full screen on a monitor: a FullScreenViewer fed by the main window (not by the engine directly, see
// SecondScreenPlan::holdsTimeline), not taking the keyboard focus. Unplugging the monitor or closing the window
// (Esc, double click, the window manager) emit lost().
class ScreenOutput : public PreviewOutput {
    Q_OBJECT
public:
    ScreenOutput(Engine* engine, QScreen* screen, const QImage& picture, QObject* parent = nullptr);
    ~ScreenOutput() override;

    QString name() const override;
    void showFrame(const QImage& frame, const QSize& projectSize) override;
    int wantedPreviewEdge() const override;

    QScreen* screen() const;
    FullScreenViewer* window() const { return m_window; }
    // Keys of these actions work in its window too (playback keys)
    void addActions(const QList<class QAction*>& actions);

private:
    FullScreenViewer* m_window; // no parent (FullScreenViewer.h): deleted by the destructor
    QPointer<QScreen> m_screen;
    QString m_name;
};
