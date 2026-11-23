// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// A calm switch of the whole window (layout form, fold/unfold): the window's picture is frozen on top, the change
// happens underneath, then the picture fades away (~200 ms). A change that takes longer than ~0.5 s shows a small
// spinner on the frozen picture (when it lets the event loop run). Reduced motion (the setting ui/reduceMotion,
// Android's "remove animations", SCHNEIDI_REDUCE_MOTION=1): the picture simply goes, no fade.

#include <QPixmap>
#include <QWidget>

class QTimer;

class SwitchCurtain : public QWidget {
public:
    // Covers `window` with its own picture (nullptr when it is not shown: nothing to cover)
    static SwitchCurtain* cover(QWidget* window);
    // The change is done: fade out and go (at once with reduced motion)
    void reveal();
    static bool reducedMotion();
    static constexpr int kFadeMs = 200, kSpinnerAfterMs = 500;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    explicit SwitchCurtain(QWidget* window);
    QPixmap m_picture;
    QTimer* m_spin = nullptr;
    int m_angle = -1; // < 0: no spinner yet
    double m_opacity = 1.0;
};
