// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <QLabel>
#include <QFontMetricsF>
#include <QPainter>
#include <cmath>

// One line of text that never widens its layout: too long, it ends in "…" (or loses its middle, ElideMiddle) and the
// tooltip holds all of it. setFullText() instead of setText(); the contents margins (and padding) are kept free.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(Qt::TextElideMode mode = Qt::ElideRight) : m_mode(mode) {}
    void setFullText(const QString& text)
    {
        m_text = text;
        setToolTip(text);
        updateGeometry();
        update();
    }
    const QString& fullText() const { return m_text; }
    QSize minimumSizeHint() const override { return QSize(0, fontMetrics().height()) + margins(); }
    // Rounded up and one pixel more: at exactly the integer advance elidedText() may already cut the text
    QSize sizeHint() const override
    {
        const int w = m_text.isEmpty() ? 0 : int(std::ceil(QFontMetricsF(font()).horizontalAdvance(m_text))) + 1;
        return QSize(w, fontMetrics().height()) + margins();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setPen(palette().color(foregroundRole()));
        p.setFont(font());
        const QRect r = contentsRect();
        p.drawText(r, int(alignment() | Qt::AlignVCenter), fontMetrics().elidedText(m_text, m_mode, r.width()));
    }

private:
    // Contents margins, frame and a style sheet's padding (QLabel#PanelTitle): all that lies outside contentsRect()
    QSize margins() const { return size() - contentsRect().size(); }

    QString m_text;
    Qt::TextElideMode m_mode;
};
