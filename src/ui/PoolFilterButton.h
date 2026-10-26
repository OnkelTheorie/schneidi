#pragma once
#include "core/PoolFilter.h"

#include <QHash>
#include <QToolButton>

class QAction;
class QMenu;

// Filter button of the Media Pool (next to the search): a menu to tick kinds, flags and clip colors (it stays open
// while ticking) and "Reset filter". Filled with the primary color while a filter is active.
class PoolFilterButton : public QToolButton {
    Q_OBJECT
public:
    explicit PoolFilterButton(QWidget* parent = nullptr);

    const PoolFilter& filter() const { return m_filter; }
    void setFilter(const PoolFilter& filter); // emits changed() if it differs
    QMenu* filterMenu() const { return m_menu; }

signals:
    void changed(const PoolFilter& filter);

private:
    void toggle(QAction* action, bool on);
    void sync(); // ticks, look and tooltip from m_filter

    PoolFilter m_filter;
    QMenu* m_menu;
    QMenu* m_flagMenu;
    QMenu* m_colorMenu;
    QAction* m_reset;
    // Ticked entry -> what it stands for: kind bit, or flag/color id
    QHash<QAction*, unsigned> m_kindActions;
    QHash<QAction*, QString> m_flagActions;
    QHash<QAction*, QString> m_colorActions;
};
