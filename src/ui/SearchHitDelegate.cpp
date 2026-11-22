// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/SearchHitDelegate.h"

#include "app/Theme.h"

#include <QApplication>
#include <QListView>
#include <QPainter>
#include <QPoint>
#include <QTextLayout>

namespace {

QFont subtitleFont(const QFont& base)
{
    QFont f = base;
    if (base.pixelSize() > 0) f.setPixelSize(std::max(8, base.pixelSize() * 85 / 100));
    else f.setPointSizeF(std::max(6.0, base.pointSizeF() * 0.85));
    return f;
}

} // namespace

QVariant SearchHitDelegate::highlights(const QVector<NameMatch::Range>& ranges)
{
    if (ranges.isEmpty()) return {};
    QVariantList out;
    for (const NameMatch::Range& r : ranges) out << QPoint(r.start, r.length);
    return out;
}

int SearchHitDelegate::subtitleHeight(const QFont& font)
{
    return QFontMetrics(subtitleFont(font)).height();
}

QSize SearchHitDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    if (!index.data(kSubtitleRole).toString().isEmpty()) s.rheight() += subtitleHeight(option.font);
    return s;
}

void SearchHitDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    const QVariantList marks = index.data(kHighlightRole).toList();
    const QString subtitle = index.data(kSubtitleRole).toString();
    if (marks.isEmpty() && subtitle.isEmpty()) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QWidget* w = opt.widget;
    QStyle* style = w ? w->style() : QApplication::style();
    const bool selected = opt.state & QStyle::State_Selected;
    const QColor textColor = opt.palette.color(QPalette::Normal, selected ? QPalette::HighlightedText : QPalette::Text);
    const QString text = opt.text;
    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w);
    // The style draws background, icon and focus as always, its text invisible (the layout stays the same)
    opt.palette.setColor(QPalette::Text, Qt::transparent);
    opt.palette.setColor(QPalette::HighlightedText, Qt::transparent);
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

    const bool iconMode = opt.decorationPosition == QStyleOptionViewItem::Top;
    // Icon view: from the text's top down to the end of the item; list view: the text's place, name and subtitle as
    // one block in its middle
    QRect area = textRect;
    if (iconMode) {
        // down to the end of the grid cell (the item's own rect is only as high as Qt measured its text)
        const auto* view = qobject_cast<const QListView*>(w);
        const int cell = view && view->gridSize().isValid() ? view->gridSize().height() : opt.rect.height();
        area.setBottom(opt.rect.top() + std::max(cell, opt.rect.height()) - 1);
    }
    const QFont subFont = subtitleFont(opt.font);
    const int subH = subtitle.isEmpty() ? 0 : QFontMetrics(subFont).height();
    const Qt::Alignment hAlign = opt.displayAlignment & Qt::AlignHorizontal_Mask;

    QString shown = text;
    shown.replace('\n', QChar::LineSeparator);
    QTextLayout layout(shown, opt.font);
    QTextOption to;
    to.setWrapMode((opt.features & QStyleOptionViewItem::WrapText) ? QTextOption::WrapAtWordBoundaryOrAnywhere
                                                                    : QTextOption::NoWrap);
    to.setAlignment(hAlign);
    layout.setTextOption(to);
    QList<QTextLayout::FormatRange> formats;
    for (const QVariant& v : marks) {
        const QPoint r = v.toPoint();
        if (r.x() < 0 || r.y() <= 0 || r.x() >= shown.size()) continue;
        QTextLayout::FormatRange f;
        f.start = r.x();
        f.length = std::min<int>(r.y(), int(shown.size()) - r.x());
        f.format.setFontWeight(QFont::Bold);
        if (!selected) f.format.setForeground(Theme::primary);
        formats << f;
    }
    layout.setFormats(formats);
    const int nameRoom = std::max(0, area.height() - subH);
    qreal y = 0;
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(area.width());
        if (y > 0 && y + line.height() * 0.6 > nameRoom) { // no room left (a few pixels may go): the rest is cut
            line.setPosition(QPointF(0, 100000));
            continue;
        }
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();
    const int top = iconMode ? area.top() : area.top() + std::max(0, (area.height() - int(y) - subH) / 2);

    painter->save();
    painter->setClipRect(area);
    painter->setPen(textColor);
    layout.draw(painter, QPointF(area.left(), top));
    if (!subtitle.isEmpty()) {
        painter->setFont(subFont);
        painter->setPen(selected ? textColor : Theme::textDim);
        const QRect subRect(area.left(), top + int(y), area.width(), subH);
        painter->drawText(subRect, int(hAlign) | Qt::AlignVCenter,
                          QFontMetrics(subFont).elidedText(subtitle, Qt::ElideMiddle, area.width()));
    }
    painter->restore();
}
