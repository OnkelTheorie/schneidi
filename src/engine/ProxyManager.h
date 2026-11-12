// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Proxy media like in DaVinci ("Generate Proxy Media"): small H.264 copies of the originals
// for a smooth preview. Created in the background via ffmpeg (QProcess, one file after the other),
// stored in ~/.cache/schneidi/proxies. The file name derives from path, size and modification time
// of the original -> the project file does not need to know anything about proxies. Export always uses the originals.
// A note next to each proxy (<hash>.json) names its original; cleaning up the folder: engine/ProxyCleanup.

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QProcess;

class ProxyManager : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSize = 960; // longer image edge of the proxy (pixels) at the default quality (Small)
    // "Full resolution": larger than any picture, so the proxy keeps the original's size (only a lighter codec)
    static constexpr int kFullSize = 1 << 16;

    // Proxy quality (Settings › General, "proxy/quality", for all projects): the longer edge of the proxy.
    // Small = 960 px (540p, the default), Medium = 1280 (720p), Large = 1920 (1080p), Full = the original's size.
    // The size is part of the proxy's name: another quality means other files (old ones are "other size" for cleanup).
    enum class Quality { Small, Medium, Large, Full };
    static Quality quality(); // the stored one (read once, then kept)
    // Reads the stored quality again (the app may have changed it since: a long-running schneidi-cli/MCP process
    // before cleaning up the proxy folder)
    static void reloadQuality();
    static int maxSizeOf(Quality q);
    static int maxSize() { return maxSizeOf(quality()); }
    // Short name for the viewer and the settings: "540p", "720p", "1080p", "volle Auflösung"
    static QString qualityLabel(Quality q);
    // Stores the quality; a proxy being made right now is started again at the new size. Emits qualityChanged.
    // Existing proxies of the old quality stay on the disk (regenerate: generate() with withProxy(…, old size)).
    void setQuality(Quality q);

    // The ffmpeg call that makes the proxy of `original` (output "-"), for tests; empty when none can be made
    static QStringList proxyArguments(const QString& original, int maxSize);
    // Filters an ffmpeg program lists (`ffmpeg -filters`), kept per file and modification time once listing worked
    static QStringList programFilters(const QString& program);

    explicit ProxyManager(QObject* parent = nullptr);
    ~ProxyManager() override;

    static QString cacheDir();
    // Location of the proxy for an original (regardless of whether it exists yet)
    static QString proxyPath(const QString& original);
    // … for a proxy of the given longer edge (maxSizeOf)
    static QString proxyPath(const QString& original, int maxSize);
    // Of `originals` the ones that have a proxy of that size on the disk (what to regenerate after a quality change)
    static QStringList withProxy(const QStringList& originals, int maxSize);
    bool hasProxy(const QString& original) const;
    // For thumbnails, filmstrips and stills: proxy if present (regardless of the switch, much faster to decode and
    // seek; same frame rate and aspect ratio), otherwise the original
    static QString decodeSource(const QString& original);
    // For the preview: proxy if enabled and present, otherwise the original
    QString resolve(const QString& original) const;

    // "Use proxy media if available" (saved, default on)
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);

    void generate(const QStringList& originals); // into the queue (existing/waiting ones are skipped)
    // Of `originals` the ones without a proxy that are not waiting or running (what "Generate" would queue)
    QStringList missing(const QStringList& originals) const;

    // Proxies made right on import (Settings › General, "proxy/onImport"): never (the default), for heavy video only
    // (heavyVideo) or for every video file
    enum class ImportRule { Never, Heavy, Always };
    static ImportRule importRule();
    static void setImportRule(ImportRule rule);
    // Video the preview struggles with: 4K and more (longer edge from 3840 px), H.265/HEVC or AV1, more than 8 bits
    static bool heavyVideo(int width, int height, const QString& codec, int bitDepth);
    // Does `rule` want a proxy of this file? Reads its description (no picture decoded): stills, sound-only files
    // and unreadable files never
    static bool wantedOnImport(ImportRule rule, const QString& path);
    // Newly imported files (app: the Media Pool, drag and drop, watch folders, the live MCP/CLI import): queues the
    // ones the stored rule wants. The files are read in a helper thread; the queue fills a moment later - without the
    // files `stillThere` says are gone by then (removed again, another project), and nothing at all after cancelAll()
    // or dropImports() meanwhile
    void importedFiles(const QStringList& paths, std::function<bool(const QString&)> stillThere = {});
    void dropImports() { ++m_importRun; } // another project: the files being read are not its imports
    void remove(const QStringList& originals);   // delete proxy (a running generation is canceled)
    void cancel(const QStringList& originals);
    void cancelAll();

    bool isPending(const QString& original) const; // waiting or currently running
    int progressOf(const QString& original) const; // 0..100, -1 = not in progress
    int pendingCount() const { return m_queue.size() + (m_current.isEmpty() ? 0 : 1); }
    QString current() const { return m_current; }
    QStringList queued() const { return m_queue; } // waiting, in order (without the current one)
    // File being written right now (half-finished proxy; empty = none): cleaning up never touches it
    QString writing() const { return m_currentPart; }

signals:
    void progress(const QString& original, int percent);
    // Proxy created, deleted or generation finished/canceled -> refresh display/preview
    void proxyChanged(const QString& original);
    void failed(const QString& original, const QString& message);
    void enabledChanged(bool on);
    void queueChanged(); // queue/progress changed (status display)
    void qualityChanged(); // setQuality: other proxy files now (the preview resolves again)

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
    int m_currentSize = 0;  // longer edge the current proxy is made at (maxSize() when it started)
    double m_durationUs = 0;
    int m_progress = 0;
    QString m_errorTail;    // last ffmpeg messages for the error display
    QProcess* m_process = nullptr;
    class QThread* m_probeThread = nullptr; // probes the next source
    int m_job = 0;                          // counted up on start/cancel; stale probes expire
    int m_importRun = 0;                    // counted up by cancelAll/dropImports: import probes running expire
};
