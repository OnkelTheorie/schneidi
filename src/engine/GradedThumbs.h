#pragma once
// Thumbnails with the Color page grade: the timeline filmstrip shows each clip's look, the Media Pool shows timelines
// with their first picture graded. Sources in the Media Pool stay neutral on purpose: a source has no grade of its own,
// several clips of one source can be graded differently.
//
// The grade is the pure pixel function of engine/ColorGrade (correction, LUT, windows) applied to the small source
// thumbnail, never an MLT graph. Graded images are made in a worker thread and cached by (source thumbnail, evaluated
// grade): scrolling and zooming reuse them, undo finds the previous look again. Until a graded image is ready a tile
// keeps what it showed before (its previous look; a new tile a graded neighbor of the clip, else the ungraded
// thumbnail), so nothing flickers or goes blank.
// A changed grade is picked up once it has been still for kSettleMs: dragging a color wheel does not regrade on every
// step, pasting a grade onto many clips only queues work. Other effects (color, blur, frei0r …) are not shown.

#include "core/Types.h"
#include "engine/ColorGrade.h"

#include <QCache>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QWaitCondition>
#include <deque>

class Project;
class QThread;
class QThreadPool;
class QTimer;

class GradedThumbs : public QObject {
    Q_OBJECT
public:
    static constexpr int kSettleMs = 300;

    // What the grade does to a picture at one clip frame (keyframes evaluated), with a hash of all of it
    struct Spec {
        ColorGrade::Params params;
        QString lut; // LUT file, empty = none
        QVector<ColorGrade::Window> windows;
        quint64 hash = 0; // 0 = the grade changes nothing
        bool isNull() const { return hash == 0; }
        bool operator==(const Spec& o) const { return hash == o.hash; }
    };
    // Grade of clip c at clip frame t; null when the clip has no enabled grade or it changes nothing there
    static Spec specAt(const Clip& c, double t);
    // Are grade values or windows of the clip animated (keyframes)?
    static bool animated(const Clip& c);
    // The picture with the grade (any size and format, result RGB32). Pure, any thread; null spec = unchanged copy.
    static QImage apply(const QImage& image, const Spec& spec);

    // Graded filmstrip on/off (menu Timeline › Timeline view, default on): off = the source thumbnails as before
    static void setEnabled(bool on);
    static bool enabled();

    explicit GradedThumbs(QObject* parent = nullptr);
    ~GradedThumbs() override;

    // Grade of one clip while drawing its tiles: get it once per clip and paint, then image() per tile.
    // False (clip without grade, or switched off) = draw the thumbnails as they are; nothing else is done for it.
    class ClipGrade {
    public:
        explicit operator bool() const { return m_owner != nullptr; }
        // raw = thumbnail drawn for clip frame t (tile position): the graded one if ready, else what to show meanwhile
        QImage image(const QImage& raw, int t) const;

    private:
        friend class GradedThumbs;
        GradedThumbs* m_owner = nullptr;
        const Clip* m_clip = nullptr; // valid while drawing the clip
        bool m_animated = false;
        Spec m_spec;           // not animated: the same everywhere in the clip
        qint64 m_settledAt = 0; // grade counts as still from this time on (m_clock)
    };
    ClipGrade forClip(const Clip& c);

    void clear(); // project format changed: the thumbnails come anew

    // For tests and benchmarks
    int computed() const;      // graded images made so far
    bool idle() const;         // nothing queued or being computed
    int cacheCount() const { return int(m_done.count()); }
    // Tiles drawn so far: with their graded picture; while theirs is computed with their previous look, a graded
    // neighbor of the clip, or ungraded
    int tilesGraded() const { return m_tilesGraded; }
    int tilesPrevious() const { return m_tilesPrevious; }
    int tilesNeighbor() const { return m_tilesNeighbor; }
    int tilesUngraded() const { return m_tilesUngraded; }

signals:
    void updated(); // new graded images, or a changed grade has settled: redraw

private:
    struct Key {
        qint64 raw;   // QImage::cacheKey of the source thumbnail
        quint64 grade; // Spec::hash
        bool operator==(const Key&) const = default;
    };
    struct TileKey {
        int clip;
        int t;
        bool operator==(const TileKey&) const = default;
    };
    friend size_t qHash(const Key& k, size_t seed) { return qHashMulti(seed, k.raw, k.grade); }
    friend size_t qHash(const TileKey& k, size_t seed) { return qHashMulti(seed, k.clip, k.t); }
    struct Job {
        Key key;
        QImage raw;
        Spec spec;
        int generation = 0;
    };
    // Last state of a clip's grade seen while drawing (compared cheaply: unchanged data stays shared)
    struct Memo {
        QString mediaPath;
        QVector<EffectInstance> effects;
        QMap<AnimParam, KeyTrack> keys;
        bool animated = false;
        Spec spec;
        qint64 settledAt = 0;
    };

    QImage image(const ClipGrade& g, const QImage& raw, int t);
    void queue(const Key& key, const QImage& raw, const Spec& spec);
    void finished(const Key& key, const QImage& img, int generation);
    void loop();

    QElapsedTimer m_clock;
    QHash<int, Memo> m_memos;            // by clip id (UI thread)
    QCache<Key, QImage> m_done;           // graded images (UI thread)
    QCache<TileKey, QImage> m_shown;      // last graded image per tile, shown while its new one is computed
    struct Look {
        quint64 grade;
        QImage image;
    };
    QCache<int, Look> m_looks; // by clip id: its last graded tile, shown on its new tiles until theirs are computed
    QTimer* m_settle;                     // repaint once a changed grade has settled
    int m_generation = 0;
    int m_tilesGraded = 0, m_tilesPrevious = 0, m_tilesNeighbor = 0, m_tilesUngraded = 0;

    mutable QMutex m_mutex; // guards the members below
    QWaitCondition m_cond;
    std::deque<Job> m_jobs; // front = newest request
    QSet<Key> m_pending;
    bool m_busy = false;
    bool m_quit = false;
    int m_computed = 0;
    QThread* m_thread = nullptr;
};

// Pictures of timelines and compound clips in the Media Pool: the first picture of the timeline (the topmost visible
// video clip where the earliest one starts) with that clip's grade. Made in a worker (opening a file takes 20–700 ms),
// anew only when that frame or its grade changed (looked at kSettleMs after the timeline changed).
class TimelinePosters : public QObject {
    Q_OBJECT
public:
    // What a poster shows: file frame and the grade there; empty path = the timeline has no picture
    struct Source {
        QString path;
        int frame = 0;
        GradedThumbs::Spec grade;
        bool operator==(const Source&) const = default;
    };
    static Source sourceOf(const Project& project, const Timeline& tl);

    TimelinePosters(Project* project, const QSize& size, QObject* parent = nullptr);
    ~TimelinePosters() override;

    // Current picture of the sequence (null: not ready yet, or no picture); the first call asks for it
    QImage poster(int sequenceId);

signals:
    void changed(int sequenceId);

private:
    struct Entry {
        Source wanted;  // what it should show now
        Source shown;   // what `image` shows
        QImage raw;     // ungraded frame of shown.path/frame
        QImage image;
        bool busy = false;
    };
    void refresh();
    void start(int sequenceId);
    void done(int sequenceId, const Source& source, const QImage& raw, const QImage& image, int generation);

    Project* m_project;
    QSize m_size;
    QHash<int, Entry> m_entries;
    QThreadPool* m_pool;
    QTimer* m_settle;
    int m_generation = 0;
};
