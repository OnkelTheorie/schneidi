// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Pictures for hover scrub over thumbnails (Media Storage, Media Pool): a few pictures spread over a video file, decoded
// in one background thread with the own video decoder (engine/video/Reader) and kept in memory per state of the file
// (modification time and size: a file rendered again, or a recording that could not be read until it was finished,
// is looked at anew). Asking never blocks: picture() returns what is there and requests the rest, ready() reports a
// new picture. The newest request goes first, old ones are dropped once the mouse has moved on.

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWaitCondition>
#include <deque>

class QThread;

class ScrubFrames : public QObject {
    Q_OBJECT
public:
    static constexpr int kSlots = 12; // pictures per file: slot i shows the frame at (i + 0.5) / kSlots of its length

    // Pictures fit into `size` (aspect ratio kept, no borders)
    explicit ScrubFrames(const QSize& size, QObject* parent = nullptr);
    ~ScrubFrames() override;

    static int slotAt(double fraction); // 0..1 across the thumbnail -> slot
    // Picture of the slot, else the nearest slot already decoded (null if none yet); requests the slot if missing
    QImage picture(const QString& path, int slot);
    bool has(const QString& path, int slot);
    bool failed(const QString& path); // no picture possible (no video stream, unreadable): no hover scrub
    int decoded() const;              // pictures decoded so far (tests)

signals:
    void ready(const QString& path, int slot); // a new picture, or the file failed (queued from the worker thread)

private:
    struct Job {
        QString path;
        QString file; // fileKey(path) when asked
        int slot;
    };
    static QString fileKey(const QString& path); // path with modification time and size
    static QString key(const QString& file, int slot);
    void run();

    const QSize m_size;
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    bool m_quit = false;
    QCache<QString, QImage> m_pictures; // key(file, slot)
    std::deque<Job> m_jobs;             // front = newest
    QSet<QString> m_pending;            // key(file, slot)
    QSet<QString> m_failed;             // fileKey()
    int m_decoded = 0;
    QThread* m_thread = nullptr;
};
