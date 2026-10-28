#pragma once
// Video stabilization (effect "stabilize") with schneidi's own analysis and warp (engine/StabilizeMotion).
// An analysis pass measures the camera motion from frame to frame before anything can be applied. It runs in the
// background (one at a time, own MLT producer, no preview involved); its result (.stab: one motion per frame) is
// cached in QStandardPaths::CacheLocation/stabilize, named after everything that changes it (source file +
// size/time, speed, frame rate, aspect ratio, shakiness/accuracy) plus the analyzed range. A cached range that covers
// the clip is reused (trimming shorter, moving, splitting need no new analysis). Until the result exists the clip
// plays unstabilized; export runs missing analyses first (Exporter), never silently without. Smoothing, zoom and
// border are applied by an own MLT filter on the clip (attach) and change live, without a new analysis.
// Results of older versions (vid.stab's .trf) are not read: such clips are analyzed again by themselves.
//
// Frames: positions of the producer the timeline reads (the timewarp producer for speed changes, so speed and
// reverse work too). Not supported: speed ramps, freeze frames, titles, compound clips.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <framework/mlt_types.h>

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <memory>
#include <optional>

namespace Mlt {
class Producer;
class Profile;
} // namespace Mlt
class QThread;

class Stabilizer : public QObject {
    Q_OBJECT
public:
    static constexpr const char* EffectId = "stabilize";

    struct Request {
        QString file;            // original media file (never the proxy)
        double speed = 1.0;      // timewarp speed (negative = reverse), 1 = original
        FrameRate rate;          // project frame rate (frames of the producer)
        int width = 960, height = 540; // analysis size: aspect ratio of the project, longer side 960
        int from = 0, to = 0;    // range to analyze (producer frames, inclusive; `to` is clamped to the file)
        int needFrom = 0, needTo = 0; // a cached result must cover at least this (the clip's in..out)
        int shakiness = 5, accuracy = 9;
        QString base() const;    // file name prefix: hash of everything except the range
        QString key() const;     // base + needed range (queue, cancel)
        bool operator==(const Request&) const = default;
    };
    struct Result {
        QString path;
        int from = 0, to = -1;
        bool valid() const { return !path.isEmpty(); }
    };
    enum class State { Unsupported, Missing, Queued, Running, Done, Failed, Cancelled };

    static QString cacheDir();
    // Request for a stabilized clip (effect present, also when switched off); nullopt = not possible for this clip
    static std::optional<Request> request(const Clip& c, const ProjectFormat& format);
    static std::optional<Request> request(const Clip& c, Mlt::Profile& profile);
    // Why a clip cannot be stabilized (empty = it can)
    static QString unsupportedReason(const Clip& c);
    // Cached result covering the request (thread-safe); invalid = analysis missing
    static Result find(const Request& r);
    // Does a clip use border "keep" (the picture before fills the uncovered parts)? Then frames must render in order
    static bool needsFrameOrder(const Timeline& tl);
    // Requests of all enabled stabilizations in the timeline (and its nested sequences) without a result
    static QVector<Request> missing(const Timeline& tl, const ProjectFormat& format);
    // Analyze synchronously in the calling thread (any thread). progress: 0..1000
    static bool analyze(const Request& r, QString* error, const std::atomic<bool>* cancel = nullptr,
                        std::atomic<int>* progress = nullptr);
    // Timeline: the stabilizing filter on the cut of clip c (its first filter); false = no result (yet) or not possible
    static bool attach(Mlt::Profile& profile, Mlt::Producer& cut, const Clip& c);
    // Live update (LiveUpdate): takes over the fresh filter's values (smoothing, zoom, border, a new result) without a
    // rebuild; false = not two stabilizing filters
    static bool transfer(mlt_filter live, mlt_filter fresh);

    // Background analyses of the preview (main thread). Created on first use.
    static Stabilizer* instance();
    State state(const Request& r, double* progress = nullptr) const;
    QString error(const Request& r) const;
    // The analyses the shown timeline needs (Engine, after each build): queues the missing ones (not those the user
    // cancelled or that failed – start() retries them), drops queued ones that are no longer needed and stops a
    // running one that is not needed any more (e.g. shakiness dragged on)
    void sync(const QVector<Request>& requests);
    void start(const Request& r);
    void cancel(const Request& r);

signals:
    void stateChanged();  // queue or progress changed (at most ~5 times a second while running)
    void resultReady();   // an analysis finished -> build the preview again

private:
    explicit Stabilizer(QObject* parent = nullptr);
    ~Stabilizer() override;
    void next();
    bool runningUncancelled(const QString& key) const;

    QVector<Request> m_queue;
    std::optional<Request> m_current;
    QThread* m_thread = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress;
    std::shared_ptr<QString> m_error;
    QSet<QString> m_cancelled;         // keys the user cancelled
    QHash<QString, QString> m_failed;  // key -> error
    QTimer m_poll;
};
