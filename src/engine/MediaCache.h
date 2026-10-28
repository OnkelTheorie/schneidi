// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Thumbnails (filmstrips) and waveforms for the timeline.
// Computed asynchronously in own threads, the UI only queries and
// is notified via updated() when something new is ready.
//
// Filmstrips fast: one pass per file that only decodes the keyframes (engine/video/Thumbnails, inside the program: no
// process per file; one image per keyframe, ~20 ms instead of ~150–700 ms for an exactly sought 4K frame). One file at a
// time. Exact frames are only loaded afterwards if the keyframe is further away than the tile spacing (zoomed in far).

#include "core/ProjectFormat.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWaitCondition>
#include <deque>
#include <memory>

class GradedThumbs;
class QThread;

struct Waveform {
    static constexpr int kBucketsPerFrame = 8;
    // levels[0]: one peak value (0..255) per bucket, levels[k]: 2 buckets combined each
    QVector<QVector<quint8>> levels;
    int frames = 0;     // this many frames are already computed
    bool complete = false;
};

class MediaCache : public QObject {
    Q_OBJECT
public:
    static constexpr int kThumbHeight = 90;

    explicit MediaCache(QObject* parent = nullptr);
    ~MediaCache() override;

    // Returns the image for (file, source frame) if already computed; otherwise it is requested.
    // Until then the image of the previous keyframe comes (or an empty one). tolerance: by this many frames the
    // keyframe may lie before the wanted frame without the exact frame being computed.
    QImage thumbnail(const QString& path, int frame, int tolerance = 1);
    // Only look up, request nothing (for substitute images from coarser levels)
    QImage cachedThumbnail(const QString& path, int frame);

    // Waveform (possibly still incomplete) or nullptr; requests it if needed. stream: audio stream of the file (0 = first)
    std::shared_ptr<const Waveform> waveform(const QString& path, int stream = 0);

    // Project settings changed: frames count in the new frame rate, images in the new aspect ratio
    // -> discard everything computed (waveforms on disk are separated by frame rate)
    void setFormat(const ProjectFormat& format);

    // Filmstrip tiles with each clip's Color page grade (engine/GradedThumbs); its updates come as updated()
    GradedThumbs* graded() const { return m_graded; }

signals:
    void updated(); // queued from the worker threads

private:
    struct ThumbJob {
        QString path;
        int frame;
    };
    struct KeyIndex {
        QVector<int> frames; // source frames of the already decoded keyframes (ascending)
        bool done = false;
        bool failed = false; // no key frame pictures (no video, unreadable) -> exact frames only
    };
    static QString thumbKey(const QString& path, int frame);
    void queueThumb(const QString& path, int frame);
    void thumbLoop();
    void stripLoop();
    void waveLoop();

    QMutex m_mutex;
    QWaitCondition m_thumbCond;
    QWaitCondition m_waveCond;
    bool m_quit = false;
    ProjectFormat m_format;
    int m_generation = 0; // counts format changes; older results from the threads are discarded

    QCache<QString, QImage> m_thumbs;
    std::deque<ThumbJob> m_thumbJobs; // front = newest request (processed first)
    QSet<QString> m_thumbPending;

    QHash<QString, KeyIndex> m_keys;
    std::deque<QString> m_stripJobs;
    QWaitCondition m_stripCond;

    QHash<QString, std::shared_ptr<const Waveform>> m_waves;
    std::deque<QString> m_waveJobs;
    QSet<QString> m_waveRequested;

    QThread* m_thumbThread = nullptr;
    QThread* m_stripThread = nullptr;
    QThread* m_waveThread = nullptr;
    GradedThumbs* m_graded = nullptr;
};
