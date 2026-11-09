// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Spotlight of the in-app tours (core/Tours.h, app/TourManager): a dim layer over the whole window with a soft-edged
// hole around each target, and a bubble beside it with the step's text, "3/7" and the buttons (End tour, Back, Next).
// Clicks inside a hole reach the real widget below (wait steps: "drag a clip into the timeline"), clicks on the dim
// part do nothing. The bubble keeps out of the targets and inside the window. The targets are asked again whenever the
// window changes size and a few times a second (a panel that moves, a splitter dragged), the hole follows.
// The same layer asks the tour question (setQuestion): "Shall I show you?" with Show me / Later (/ Don't offer tours).

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
    // A tour step: text, "index+1/count", which buttons. waiting = the step waits for the user ("Skip" instead of
    // "Next"), offer = the text of an extra button (empty = none; "Load sample clips")
    void showStep(const QString& text, int index, int count, bool waiting, const QString& offer = {});
    // The question before a tour (no hole, bubble in the middle); firstStart adds "Don't offer tours"
    void showQuestion(const QString& title, const QString& text, bool firstStart);
    void end(); // hide

    // Where the bubble of `size` goes: below the targets' bounding box, else above, right, left (the first place that
    // fits `area` without covering the target), else clamped into `area`; no target = centered
    static QRect placeBubble(const QRect& area, const QRect& target, const QSize& size, int gap = 14);

    QVector<QRect> holes() const { return m_holes; }
    QRect bubbleGeometry() const;
    QWidget* bubble() const;

signals:
    void nextClicked();
    void backClicked();
    void endClicked();   // End tour, Esc
    void offerClicked();
    void showMe();       // question answers
    void later();
    void never();

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void relayout();
    void clearButtons();
    QPushButton* addButton(const QString& text, bool primary);
    QFrame* m_bubble = nullptr;
    QLabel* m_title = nullptr;
    QLabel* m_counter = nullptr;
    QLabel* m_text = nullptr;
    QLabel* m_hint = nullptr; // question: where the tours are found later
    class QHBoxLayout* m_buttons = nullptr;
    std::function<QVector<QRect>()> m_targets;
    QVector<QRect> m_holes;
    QTimer m_follow;
    bool m_question = false;
};
