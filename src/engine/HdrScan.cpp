// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/HdrScan.h"
#include "core/Project.h"
#include "engine/HdrTone.h"
#include "engine/MediaProbe.h"
#include "engine/ProxyManager.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>

HdrScan::HdrScan(Project* project, ProxyManager* proxies, QObject* parent)
    : QObject(parent), m_project(project), m_proxies(proxies)
{
    connect(project, &Project::mediaChanged, this, &HdrScan::scan, Qt::QueuedConnection);
    connect(project, &Project::loaded, this, [this] {
        m_seen.clear(); // another project: its files (the same ones again cost a cache lookup)
        scan();
    }, Qt::QueuedConnection);
}

void HdrScan::scan()
{
    QStringList paths;
    for (const MediaInfo& m : m_project->media())
        if (m.hasVideo && !m.isImage && !m_seen.contains(m.path)) {
            m_seen.insert(m.path);
            paths << m.path;
        }
    if (paths.isEmpty()) return;
    ++m_running;
    QThreadPool::globalInstance()->start([guard = QPointer<HdrScan>(this), paths] {
        QStringList found;
        for (const QString& path : paths) found << QString::fromLatin1(HdrTone::name(MediaProbe::hdrTransfer(path)));
        QMetaObject::invokeMethod(qApp, [guard, paths, found] {
            if (guard) guard->apply(paths, found);
        }, Qt::QueuedConnection);
    });
}

void HdrScan::apply(const QStringList& paths, const QStringList& found)
{
    --m_running;
    for (int i = 0; i < paths.size(); ++i) {
        // A missing file keeps what the project says (offline: nothing to look at)
        if (!QFileInfo::exists(paths[i])) continue;
        m_project->setMediaHdr(paths[i], found[i]);
        if (!found[i].isEmpty() && m_proxies) m_proxies->hdrFound(paths[i]);
    }
    emit finished();
}
