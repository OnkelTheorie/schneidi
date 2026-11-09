// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Spotlight of the in-app tours (core/Tours.h, app/TourManager): a dim layer over the whole window with a soft-edged
// hole around each target, and a bubble beside it with the step's text, "3/7" and the buttons (End tour, Back, Next).
// Clicks inside a hole reach the real widget below (wait steps: "drag a clip into the timeline"), clicks on the dim
// part do nothing. The bubble keeps out of the targets and inside the window. The targets are asked again whenever the
// window changes size and a few times a second (a panel that moves, a splitter dragged), the hole follows.
// The same layer asks questions (showQuestion): "Shall I show you?" with Show me / Later (/ Don't offer tours), and
// before a tour whether to save the user's unsaved project.
// The bubble's size comes from its content: wide enough for its buttons (they never elide; a second row when one row
// would get too wide), its height from the layout at that width.

#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>

class QFrame;
class QLabel;
class QPushButton;

class TourOverlay : public QWidget {
    Q_OBJECT
public:
    // Covers `window` (a child of it, kept at its size and on top)
    explicit TourOverlay(QWidget* window);

    // Targets in this widget's coordinates, asked on every layout change (empty = no hole, the bubble in the middle)
    void setTargets(std::function<QVector<QRect>()> targets);
    // Where the bubble should not go (same coordinates, asked with the targets): `keep` never while there is room (the
    // controls the step's text names), `avoid` if it can (toolbars, tabs, the page bar)
    // The area whose corners are the bubble's home (window coordinates; empty = beside the target)
    void setHome(std::function<QRect()> home);
    // A tour begins: its bubble gets the size that fits every step's text (and the most buttons a step has), kept for
    // the whole tour
    void measureSteps(const QStringList& texts, const QStringList& offers);
    void setKeepFree(std::function<QVector<QRect>()> keep, std::function<QVector<QRect>()> avoid);
    // A tour step: text (rich text), "index+1/count", which buttons. waiting = the step waits for the user ("Skip" instead of
    // "Next"), offer = the text of an extra button (empty = none; "Load sample clips")
    void showStep(const QString& text, int index, int count, bool waiting, const QString& offer = {});
    // A question (no hole, bubble in the middle): its answers as buttons, left to right; `flat` ones go to the left
    // side, the primary one is the default; Esc picks the one marked `escape`
    struct Choice {
        QString name; // objectName of its button ("tour_later")
        QString text;
        std::function<void()> run;
        bool primary = false;
        bool flat = false;
        bool escape = false;
    };
    void showQuestion(const QString& title, const QString& text, const QString& hint, const QVector<Choice>& choices);
    void end(); // hide
    void refresh() { relayout(); } // targets and bubble anew now (tests; it also follows on a timer)

    // Where the bubble of `size` goes: beside the first (main) hole - below, above, right, left, the first place that
    // fits `area` without covering any hole -, else the same around all holes together, else clamped into `area`; no
    // hole = centered. `keep` counts like holes (never covered while there is room); `avoid` is kept free if
    // possible. `home` (the window's working area): a fixed corner of it first - bottom right, bottom left, top left,
    // top right (the viewer's right half shows the picture-in-picture example) -, so the bubble and its buttons
    // stay put; a corner that would cover a hole or a kept control is
    // skipped; beside the target only when no corner is free
    static QRect placeBubble(const QRect& area, const QVector<QRect>& holes, const QSize& size, int gap = 14,
                             const QVector<QRect>& keep = {}, const QVector<QRect>& avoid = {},
                             const QRect& home = {});
    // Narrowest and widest bubble (it grows from the narrowest for long texts and for its buttons)
    static constexpr int kMinWidth = 320;
    static constexpr int kMaxWidth = 520;

    QVector<QRect> holes() const { return m_holes; }
    QRect bubbleGeometry() const;
    QWidget* bubble() const;

signals:
    void nextClicked();
    void backClicked();
    void endClicked();   // End tour, Esc
    void offerClicked();

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void relayout();
    void clearButtons();
    QPushButton* addButton(const QString& name, const QString& text, bool primary, bool left);
    void arrangeButtons(bool twoRows); // the buttons into one row or two (left ones above)
    QSize bubbleSize(int atLeast = 0); // the bubble's size for its content (arranges the buttons), at least that wide
    QFrame* m_bubble = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_counter = nullptr;
    QLabel* m_text = nullptr;
    QLabel* m_hint = nullptr; // question: where the tours are found later
    class QHBoxLayout* m_row1 = nullptr; // one row: all buttons; two rows: the left ones (End tour)
    class QHBoxLayout* m_row2 = nullptr; // two rows: the rest, right-aligned
    struct Button {
        QPushButton* button;
        bool left;
    };
    QVector<Button> m_buttonList;
    std::function<void()> m_escape; // the question's Esc answer
    int m_arranged = 0;             // how the buttons are arranged now: 0 = not yet, 1 = one row, 2 = two rows
    std::function<QVector<QRect>()> m_targets;
    std::function<QVector<QRect>()> m_keep, m_avoid;
    std::function<QRect()> m_home;
    QSize m_steady; // the largest bubble of this tour so far: it never shrinks between steps (the buttons stay put)
    QVector<QRect> m_holes;
    QTimer m_follow;
    bool m_question = false;
};
