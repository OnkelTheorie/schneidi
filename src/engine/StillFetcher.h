#pragma once
// Single frames from media files for the trim view (two-up/four-up while trimming, like DaVinci).
// Decodes in its own thread with its own producers, so the preview consumer is not disturbed.
// Latest request wins: request() replaces everything still waiting, so dragging an edit only decodes
// the frames that are current when the worker gets to them. Results are kept in a small cache
// (dragging back and forth over the same frames is instant).

#include "core/ProjectFormat.h"

#include <QCache>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWaitCondition>

class QThread;

class StillFetcher : public QObject {
    Q_OBJECT
public:
    struct Request {
        QString path; // media file (original; a proxy is used if one exists)
        int frame = 0;
    };

    explicit StillFetcher(QObject* parent = nullptr);
    ~StillFetcher() override;

    // Project format: frames count in its rate, images come in its aspect ratio (drops cache + producers)
    void setFormat(const ProjectFormat& format);
    // Longer image edge of the delivered frames (default 960, like the preview)
    void setMaxEdge(int pixels);

    // Already decoded frame or a null image
    QImage cached(const QString& path, int frame) const;
    // Replaces all waiting requests; frames already in the cache are not decoded again (ask cached() first)
    void request(const QVector<Request>& requests);
    // Waiting requests (for tests)
    int pending() const;
    // Frames decoded so far (for tests: the worker only decodes what is still current)
    int decodedCount() const;

signals:
    void ready(const QString& path, int frame, const QImage& image); // queued from the worker thread

private:
    static QString key(const QString& path, int frame);
    void loop();

    mutable QMutex m_mutex;
    QWaitCondition m_cond;
    bool m_quit = false;
    ProjectFormat m_format;
    int m_generation = 0; // counts format changes; results for an older format are dropped
    int m_maxEdge = 960;
    QVector<Request> m_jobs; // front = next
    QCache<QString, QImage> m_cache;
    int m_decoded = 0;
    QThread* m_thread = nullptr;
};
