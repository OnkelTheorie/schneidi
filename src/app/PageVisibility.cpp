// SPDX-License-Identifier: AGPL-3.0-only
#include "PageVisibility.h"

#include <QSettings>

bool PageVisibility::setHidden(const QString& id, bool hidden)
{
    if (!m_order.contains(id) || isHidden(id) == hidden) return false;
    if (hidden) {
        if (!canHide(id)) return false;
        m_hidden.insert(id);
    } else {
        m_hidden.remove(id);
    }
    return true;
}

QString PageVisibility::nearestVisible(const QString& id) const
{
    const int at = int(m_order.indexOf(id));
    if (at < 0 || !isHidden(id)) return at < 0 ? QString() : id;
    for (int d = 1; d < m_order.size(); ++d) {
        if (at + d < m_order.size() && !isHidden(m_order[at + d])) return m_order[at + d];
        if (at - d >= 0 && !isHidden(m_order[at - d])) return m_order[at - d];
    }
    return id; // cannot happen: one page always stays shown
}

QStringList PageVisibility::hidden() const
{
    QStringList out;
    for (const QString& id : m_order)
        if (isHidden(id)) out << id;
    return out;
}

bool PageVisibility::setStart(const QString& id)
{
    if (!m_order.contains(id) || id == m_start) return false;
    m_start = id;
    return true;
}

void PageVisibility::load()
{
    m_hidden.clear();
    const QString start = QSettings().value(kStartKey).toString();
    m_start = m_order.contains(start) ? start : QString(kDefaultStart);
    const QStringList saved = QSettings().value(kSettingsKey).toStringList();
    for (const QString& id : saved)
        if (m_order.contains(id)) m_hidden.insert(id);
    if (visibleCount() < 1) m_hidden.clear();
}

void PageVisibility::save() const
{
    QSettings settings;
    if (m_hidden.isEmpty()) settings.remove(kSettingsKey);
    else settings.setValue(kSettingsKey, hidden());
    if (m_start == kDefaultStart) settings.remove(kStartKey); // the default stays the default when it changes
    else settings.setValue(kStartKey, m_start);
}
