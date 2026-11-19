// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Items side by side, wrapping into another row when the width runs out (a tool bar that must fit any language and
// font size without widening its window). Its minimum width is the widest item; the height follows the width
// (heightForWidth), and its minimum height is the height of the rows at the width it has (a splitter or a low window
// that ignores heightForWidth never cuts off a wrapped row). Items keep their size hint; put widgets that belong together into one widget (a group). An item sits in the middle
// of its row's height, or at its top when its alignment says Qt::AlignTop (setAlignment(widget, Qt::AlignTop)).

#include <QLayout>
#include <QVector>

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int hSpacing = 4, int vSpacing = 2);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int count() const override { return int(m_items.size()); }
    QLayoutItem* itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect& rect) override;

private:
    int arrange(const QRect& rect, bool apply) const; // the height used
    QVector<QLayoutItem*> m_items;
    int m_h, m_v;
    int m_rowsHeight = 0; // height of the rows at the last setGeometry width (0 = not laid out yet: one row)
};
