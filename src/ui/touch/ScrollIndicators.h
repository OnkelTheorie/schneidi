// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <QList>
#include <functional>
#include <QPointer>
#include <QWidget>

class QAbstractScrollArea;
class QScrollBar;
class QVariantAnimation;

// A phone's scroll indicator: a thin bar over the content's edge that shows only while the content moves and fades
// out after it (as phone apps do), instead of a finger-wide scroll bar that always takes its room. It mirrors a
// (hidden) QScrollBar: range, page step and value; a press on it while shown drags like the bar (a mouse in a narrow
// window keeps a way to drag). Transparent to the mouse while hidden.
class ScrollIndicator : public QWidget {
    Q_OBJECT
public:
    // `host` is the widget it lies over, `area` the part of it the bar runs along (host coordinates, nullptr = the
    // whole host; a function so it follows resizes)
    ScrollIndicator(QScrollBar* bar, QWidget* host, std::function<QRect()> area = {});

    // Shows it when the bar's value or range differs from the last one seen (a sync that blocks the bar's signals
    // calls this itself)
    void poke();
    bool shown() const { return m_opacity > 0.0; }
    static bool grabbable(); // takes presses while shown (a mouse); never under a finger (touch mode)
    static constexpr int kHit = 14;  // the touch/mouse zone at the edge while shown
    static constexpr int kThin = 4;  // the bar as painted
    static constexpr int kHideMs = 900; // shown this long after the last move

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void place();
    void setShown(bool on);
    QRect handleRect() const; // in own coordinates
    QPointer<QScrollBar> m_bar;
    std::function<QRect()> m_area;
    QVariantAnimation* m_fade;
    class QTimer* m_idle;
    double m_opacity = 0.0;
    int m_lastValue = -1, m_lastMax = -1, m_lastPage = -1;
    int m_dragFrom = -1, m_dragValue = 0; // press position along the bar, the value then
};

// Every scroll area of a window gets ScrollIndicators instead of its scroll bars while this lives (the phone form);
// areas made later too (seen when shown). Gone again = the scroll bars' policies as they were.
class ScrollIndicators : public QObject {
    Q_OBJECT
public:
    explicit ScrollIndicators(QWidget* window);
    ~ScrollIndicators() override;
    void adopt(QAbstractScrollArea* area);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Adopted {
        QPointer<QAbstractScrollArea> area;
        Qt::ScrollBarPolicy h, v;
        QList<QPointer<ScrollIndicator>> indicators;
    };
    QWidget* m_window;
    QList<Adopted> m_adopted;
};
