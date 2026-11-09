// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/ProxyProgress.h"

#include "core/I18n.h"
#include "engine/ProxyManager.h"

#include <QFileInfo>
#include <algorithm>

bool ProxyBatch::update(const QStringList& now, const std::function<bool(const QString&)>& hasProxy)
{
    if (pending.isEmpty() && !now.isEmpty()) done = dropped = 0; // a new batch
    for (const QString& p : std::as_const(pending))
        if (!now.contains(p)) ++(hasProxy && hasProxy(p) ? done : dropped);
    const bool ended = !pending.isEmpty() && now.isEmpty();
    pending = now;
    return ended;
}

double ProxyBatch::fraction(int currentPercent) const
{
    if (total() <= 0) return 1.0;
    const double current = pending.isEmpty() ? 0.0 : std::clamp(currentPercent, 0, 100) / 100.0;
    return std::clamp((done + current) / total(), 0.0, 1.0);
}

ProxyProgress::ProxyProgress(ProxyManager* proxies) : QObject(proxies), m_proxies(proxies)
{
    setObjectName("proxy_progress");
    connect(proxies, &ProxyManager::queueChanged, this, &ProxyProgress::refresh);
    connect(proxies, &ProxyManager::progress, this, &ProxyProgress::refresh);
    connect(proxies, &ProxyManager::proxyChanged, this, &ProxyProgress::refresh);
    refresh();
}

ProxyProgress* ProxyProgress::of(ProxyManager* proxies)
{
    if (auto* p = proxies->findChild<ProxyProgress*>(QString(), Qt::FindDirectChildrenOnly)) return p;
    return new ProxyProgress(proxies);
}

int ProxyProgress::currentPercent() const
{
    const QString current = m_proxies->current();
    return current.isEmpty() ? 0 : std::max(0, m_proxies->progressOf(current));
}

QString ProxyProgress::text() const
{
    if (!active()) return {};
    const QString current = m_proxies->current().isEmpty() ? m_batch.pending.value(0) : m_proxies->current();
    return T("Proxy %1/%2 – %3 – %4 %")
        .arg(m_batch.done + 1)
        .arg(m_batch.total())
        .arg(QFileInfo(current).fileName())
        .arg(currentPercent());
}

QString ProxyProgress::waitingText() const
{
    const QStringList waiting = m_proxies->queued();
    if (waiting.isEmpty()) return {};
    QStringList lines{T("Wartend:")};
    constexpr int kMax = 12;
    for (int i = 0; i < std::min<int>(kMax, waiting.size()); ++i) lines << "  " + QFileInfo(waiting[i]).fileName();
    if (waiting.size() > kMax) lines << "  " + T("… und %1 weitere").arg(waiting.size() - kMax);
    return lines.join('\n');
}

QString ProxyProgress::finishedText() const
{
    if (m_batch.done == 0) return T("Proxy-Erzeugung abgebrochen");
    if (m_batch.dropped > 0) return T("Proxys fertig: %1 von %2").arg(m_batch.done).arg(m_batch.done + m_batch.dropped);
    return T("Proxys fertig");
}

void ProxyProgress::refresh()
{
    QStringList now;
    if (!m_proxies->current().isEmpty()) now << m_proxies->current();
    now << m_proxies->queued();
    const bool wasActive = m_batch.active();
    const bool ended = m_batch.update(now, [this](const QString& p) { return m_proxies->hasProxy(p); });
    if (ended) emit finished();
    else if (wasActive || m_batch.active()) emit changed();
}
