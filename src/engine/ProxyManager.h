#pragma once
// Proxy media like in DaVinci ("Generate Proxy Media"): small H.264 copies of the originals
// for a smooth preview. Created in the background via ffmpeg (QProcess, one file after the other),
// stored in ~/.cache/schneidi/proxies. The file name derives from path, size and modification time
// of the original -> the project file does not need to know anything about proxies. Export always uses the originals.

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

class ProxyManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSize = 960; // longer image edge of the proxy (pixels)

    explicit ProxyManager(QObject* parent = nullptr);
    ~ProxyManager() override;

    static QString cacheDir();
    // Location of the proxy for an original (regardless of whether it exists yet)
    static QString proxyPath(const QString& original);
    bool hasProxy(const QString& original) const;
    // For the preview: proxy if enabled and present, otherwise the original
    QString resolve(const QString& original) const;

    // "Use proxy media if available" (saved, default on)
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    void generate(const QStringList& originals); // into the queue (existing/waiting ones are skipped)
    void remove(const QStringList& originals);   // delete proxy (a running generation is canceled)
    void cancel(const QStringList& originals);
    void cancelAll();

    bool isPending(const QString& original) const; // waiting or currently running
    int progressOf(const QString& original) const; // 0..100, -1 = not in progress
    int pendingCount() const { return m_queue.size() + (m_current.isEmpty() ? 0 : 1); }
    QString current() const { return m_current; }

signals:
    void progress(const QString& original, int percent);
    // Proxy created, deleted or generation finished/canceled -> refresh display/preview
    void proxyChanged(const QString& original);
    void failed(const QString& original, const QString& message);
    void enabledChanged(bool on);
    void queueChanged(); // queue/progress changed (status display)

private:
    void startNext();
    void launch(const QString& original, const QStringList& args, double durationUs, const QString& error);
    void onOutput();
    void onFinished(int exitCode, bool crashed);
    void stopCurrent();

    bool m_enabled = true;
    QStringList m_queue;
    QString m_current;      // original whose proxy is currently being created
    QString m_currentPart;  // half-finished file (only renamed at the end)
    double m_durationUs = 0;
    int m_progress = 0;
    QString m_errorTail;    // last ffmpeg messages for the error display
    QProcess* m_process = nullptr;
    class QThread* m_probeThread = nullptr; // probes the next source
    int m_job = 0;                          // counted up on start/cancel; stale probes expire
};
