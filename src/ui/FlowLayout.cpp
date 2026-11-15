// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/FlowLayout.h"

#include <QWidget>

#include <algorithm>

FlowLayout::FlowLayout(QWidget* parent, int hSpacing, int vSpacing) : QLayout(parent), m_h(hSpacing), m_v(vSpacing) {}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* item = takeAt(0)) delete item;
}

void FlowLayout::addItem(QLayoutItem* item) { m_items << item; }

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= m_items.size()) return nullptr;
    return m_items.takeAt(index);
}

QSize FlowLayout::minimumSize() const
{
    // The widest item (everything else wraps), the height of one row (the real one: heightForWidth)
    QSize s;
    for (const QLayoutItem* item : m_items)
        if (!item->isEmpty()) s = s.expandedTo(item->minimumSize().expandedTo(item->sizeHint()));
    const QMargins m = contentsMargins();
    return s + QSize(m.left() + m.right(), m.top() + m.bottom());
}

QSize FlowLayout::sizeHint() const
{
    // All in one row
    int w = 0, h = 0, n = 0;
    for (const QLayoutItem* item : m_items) {
        if (item->isEmpty()) continue;
        w += item->sizeHint().width() + (n++ ? m_h : 0);
        h = std::max(h, item->sizeHint().height());
    }
    const QMargins m = contentsMargins();
    return QSize(w + m.left() + m.right(), h + m.top() + m.bottom());
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    arrange(rect, true);
}

int FlowLayout::arrange(const QRect& rect, bool apply) const
{
    const QMargins m = contentsMargins();
    const QRect area = rect.marginsRemoved(m);
    int y = area.y();
    QVector<std::pair<QLayoutItem*, QSize>> row; // the items of the row being filled
    int rowW = 0, rowH = 0;
    const auto flush = [&] { // places the row (each item centred in the row's height)
        if (apply) {
            int x = area.x();
            for (const auto& [item, s] : row) {
                const int w = std::min(s.width(), area.width());
                item->setGeometry(QRect(x, y + (rowH - s.height()) / 2, w, s.height()));
                x += w + m_h;
            }
        }
        if (!row.isEmpty()) y += rowH + m_v;
        row.clear();
        rowW = rowH = 0;
    };
    for (QLayoutItem* item : m_items) {
        if (item->isEmpty()) continue;
        const QSize s = item->sizeHint().expandedTo(item->minimumSize());
        if (!row.isEmpty() && rowW + m_h + s.width() > area.width()) flush(); // no room left: the next row
        rowW += (row.isEmpty() ? 0 : m_h) + s.width();
        rowH = std::max(rowH, s.height());
        row.append({item, s});
    }
    flush();
    const int used = y > area.y() ? y - m_v - area.y() : 0;
    return used + m.top() + m.bottom();
}
