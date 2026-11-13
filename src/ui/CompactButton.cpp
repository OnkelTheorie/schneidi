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
    // Elided while that keeps a few letters more than the short form ("▦ Media…"), else the short form ("▦")
    const QString elided = fontMetrics().elidedText(text(), Qt::ElideRight, std::max(0, textWidth(text()) - missing));
    if (m_compact.isEmpty() || elided.size() >= m_compact.size() + 4) return elided;
    return m_compact;
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
    // Shortened: the full text as the tooltip, before the button's own tooltip (which may not name it)
    if (e->type() == QEvent::ToolTip && shownText() != text()) {
        QString full = text();
        full.remove(QLatin1Char('&'));
        if (!toolTip().isEmpty()) full += "\n" + toolTip();
        QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), full, this);
        return true;
    }
    return QToolButton::event(e);
}
