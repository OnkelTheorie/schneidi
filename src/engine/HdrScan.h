// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Finds the HDR transfer (HLG/PQ) of the project's video files in the background (MediaProbe::hdrTransfer opens each
// file once). Projects from before 0.24 know none (MediaInfo::hdr comes from the project file), and a quick probe
// misses one stated only in the bitstream. Sets MediaInfo::hdr (Project::setMediaHdr: Media Pool mark, Inspector) and
// tells the proxies (an HDR file's proxy has its own name, ProxyManager::hdrFound). The GUI thread never opens a file
// for it: proxy and render cache keys only ask MediaProbe::hdrTransferCached.

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class Project;
class ProxyManager;

class HdrScan : public QObject {
    Q_OBJECT
public:
    // proxies may be null (tests)
    HdrScan(Project* project, ProxyManager* proxies, QObject* parent = nullptr);
    // Looks at the video files not looked at yet (after load, import, mediaChanged: done by itself)
    void scan();
    bool busy() const { return m_running > 0; }

signals:
    void finished(); // one batch done (tests)

private:
    void apply(const QStringList& paths, const QStringList& found);
    Project* m_project;
    ProxyManager* m_proxies;
    QSet<QString> m_seen;
    int m_running = 0;
};
