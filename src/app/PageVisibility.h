// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which pages the page bar at the bottom shows (right-click on the bar, Workspace › Visible Pages). Pages are named by
// stable ids ("media", "edit", …) in bar order. Default: all shown. At least one page always stays shown. Saved in
// QSettings under kSettingsKey as the list of hidden ids (ids a newer/older version does not know are ignored).
// The widget side (MainWindow::updatePageButtons) shows the current page's button even when it is hidden: a key or
// menu entry that goes to a hidden page shows it for as long as it is the current page.

#include <QSet>
#include <QString>
#include <QStringList>

class PageVisibility
{
public:
    static constexpr const char* kSettingsKey = "pages/hidden";

    explicit PageVisibility(QStringList order) : m_order(std::move(order)) {}

    const QStringList& order() const { return m_order; }
    bool isHidden(const QString& id) const { return m_hidden.contains(id); }
    int visibleCount() const { return int(m_order.size()) - int(m_hidden.size()); }
    // False for the last shown page (its checkbox is disabled) and for unknown ids
    bool canHide(const QString& id) const { return m_order.contains(id) && !isHidden(id) && visibleCount() > 1; }
    // False = refused (the last shown page, an unknown id) or nothing changed
    bool setHidden(const QString& id, bool hidden);
    void showAll() { m_hidden.clear(); }
    // `id` itself when shown; else the closest shown page in bar order, the one to the right first on a tie
    QString nearestVisible(const QString& id) const;
    QStringList hidden() const; // in bar order

    void load();       // from QSettings; all hidden (edited by hand) = all shown
    void save() const; // to QSettings

private:
    QStringList m_order;
    QSet<QString> m_hidden;
};
