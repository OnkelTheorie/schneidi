// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Item look of search hits in the Media Pool and the Media Storage lists: the matched parts of the name in bold and in
// the accent color (core/NameMatch::ranges, kept in kHighlightRole) and a small dim line under the name (kSubtitleRole:
// the folder resp. bin a hit is in). Items with neither are drawn as usual.

#include "core/NameMatch.h"

#include <QStyledItemDelegate>
#include <QVariant>

class SearchHitDelegate : public QStyledItemDelegate {
public:
    static constexpr int kHighlightRole = Qt::UserRole + 40; // QVariantList of QPoint(start, length) in the item text
    static constexpr int kSubtitleRole = Qt::UserRole + 41;  // QString

    using QStyledItemDelegate::QStyledItemDelegate;

    static QVariant highlights(const QVector<NameMatch::Range>& ranges);
    // Height of the subtitle line for `font` (what an item grows by)
    static int subtitleHeight(const QFont& font);

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
