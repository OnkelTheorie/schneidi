// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// How far the proxies of the current batch are: the files queued since the queue was last empty. Overall progress =
// finished files plus the running file's share, over all files of the batch. A file that left the queue without a
// proxy (canceled, failed) leaves the batch instead of counting as done. Read by the status bar's background jobs
// indicator (ui/BackgroundJobs) and the Media Pool's header - one object per ProxyManager (ProxyProgress::of).

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

class ProxyManager;

// The counting alone (no Qt objects; tests feed it fake queues)
struct ProxyBatch {
    QStringList pending; // current one first, then the waiting ones
    int done = 0;        // finished with a proxy in this batch
    int dropped = 0;     // left without a proxy (canceled, failed)
    int total() const { return done + int(pending.size()); }
    bool active() const { return !pending.isEmpty(); }
    // The queue now (current first). hasProxy tells whether a file that left the queue was finished. Returns true
    // when this update ended the batch (the queue ran empty); done/dropped then still describe the batch until the
    // next update starts a new one.
    bool update(const QStringList& now, const std::function<bool(const QString&)>& hasProxy);
    // 0..1: finished files plus the running one's percent
    double fraction(int currentPercent) const;
};

class ProxyProgress : public QObject {
    Q_OBJECT
public:
    explicit ProxyProgress(ProxyManager* proxies);
    // The one for `proxies` (made on first use, a child of it)
    static ProxyProgress* of(ProxyManager* proxies);

    const ProxyBatch& batch() const { return m_batch; }
    bool active() const { return m_batch.active(); }
    int currentPercent() const;
    double fraction() const { return m_batch.fraction(currentPercent()); }
    // "Proxy 2/5 – clip.mp4 – 34 %"
    QString text() const;
    // The waiting files, one per line ("Waiting:" first); empty when none waits
    QString waitingText() const;
    // After the batch: "Proxies done" ("done: 2 of 5" when some were canceled or failed, "canceled" when none was made)
    QString finishedText() const;

signals:
    void changed();  // while active: count, file or percent changed
    void finished(); // the queue ran empty (finishedText says how)

private:
    void refresh();
    ProxyManager* m_proxies;
    ProxyBatch m_batch;
};
