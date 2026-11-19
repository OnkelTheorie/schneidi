// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Which pages the page bar at the bottom shows (right-click on the bar, Workspace › Visible Pages). Pages are named by
// stable ids ("media", "edit", …) in bar order. Default: all shown. At least one page always stays shown. Saved in
// QSettings under kSettingsKey as the list of hidden ids (ids a newer/older version does not know are ignored).
// The widget side (MainWindow::updatePageButtons) shows the current page's button even when it is hidden: a key or
// menu entry that goes to a hidden page shows it for as long as it is the current page.
// The start page (kStartKey, Settings › General › Interface): the page shown when the app starts and on New Project,
// not when a project is opened. Default "edit"; an unknown saved id counts as the default.

#include <QSet>
#include <QString>
#include <QStringList>

class PageVisibility
{
public:
    static constexpr const char* kSettingsKey = "pages/hidden";
    static constexpr const char* kStartKey = "pages/start";
    static constexpr const char* kDefaultStart = "edit";

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
    // The chosen start page (saved id, default kDefaultStart) and the page to show for it: itself, or the nearest
    // shown one when it is hidden
    QString start() const { return m_start; }
    QString startShown() const { return nearestVisible(m_start); }
    bool setStart(const QString& id); // false = unknown id or unchanged

    void load();       // from QSettings; all hidden (edited by hand) = all shown
    void save() const; // to QSettings (the hidden pages and the start page)

private:
    QStringList m_order;
    QSet<QString> m_hidden;
    QString m_start = kDefaultStart;
};
