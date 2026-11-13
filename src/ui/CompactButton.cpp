// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/CompactButton.h"

#include <QEvent>
#include <QHelpEvent>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolTip>

void CompactButton::setCompactText(const QString& text)
{
    m_compact = text;
    updateGeometry();
    update();
}

bool CompactButton::showsText() const
{
    // The style the button really draws: without an icon a tool button shows its text (style IconOnly or not)
    QStyleOptionToolButton opt;
    initStyleOption(&opt);
    return !text().isEmpty() && opt.toolButtonStyle != Qt::ToolButtonIconOnly;
}

int CompactButton::textWidth(const QString& s) const
{
    return fontMetrics().size(Qt::TextShowMnemonic, s).width();
}

QSize CompactButton::minimumSizeHint() const
{
    // The full hint less the text it does not need: the button's padding, icon and menu arrow stay
    const QSize full = sizeHint();
    if (!showsText()) return full;
    const QString shortest = m_compact.isEmpty() ? QString(text().left(1) + QChar(0x2026)) : m_compact;
    const int saved = textWidth(text()) - textWidth(shortest);
    return saved > 0 ? QSize(full.width() - saved, full.height()) : full;
}

QString CompactButton::shownText() const
{
    const int missing = sizeHint().width() - width();
    if (missing <= 0 || !showsText()) return text();
    if (!m_compact.isEmpty()) return m_compact;
    return fontMetrics().elidedText(text(), Qt::ElideRight, std::max(0, textWidth(text()) - missing));
}

void CompactButton::paintEvent(QPaintEvent*)
{
    // What QToolButton::paintEvent draws, with the text that fits
    QStylePainter p(this);
    QStyleOptionToolButton opt;
    initStyleOption(&opt);
    opt.text = shownText();
    p.drawComplexControl(QStyle::CC_ToolButton, opt);
}

bool CompactButton::event(QEvent* e)
{
    // Shortened and without a tooltip of its own: the full text as the tooltip
    if (e->type() == QEvent::ToolTip && toolTip().isEmpty() && shownText() != text()) {
        QString full = text();
        full.remove(QLatin1Char('&'));
        QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), full, this);
        return true;
    }
    return QToolButton::event(e);
}
