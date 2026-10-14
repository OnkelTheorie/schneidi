#pragma once
// Render cache like DaVinci ("Render Cache Clip Output" in the right-click menu of a timeline clip,
// Playback > Render Cache: Off / Smart / User). A video clip with all clip effects, transform,
// speed and keyframes is rendered in the background into a quickly decodable file (ProRes, project resolution);
// the preview then plays this file instead of original + effects. Export and still always render
// from the originals.
//
// The file name is a hash of everything that determines the clip's image (clip as in the project file without
// position/audio/fades/transitions, size + modification time of the source, project format) -> every change to the clip
// invalidates the cache, moving does not. Fades and transitions still act live on top of the cache
// (transitions and titles are not cached). Location like the proxies under QStandardPaths::CacheLocation.

#include "core/ProjectFormat.h"
#include "core/Types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class QThread;

class RenderCache : public QObject {
    Q_OBJECT
public:
    enum class Mode { Off, Smart, User };

    explicit RenderCache(QObject* parent = nullptr);
    ~RenderCache() override;

    static QString cacheDir();

    // Off = never; User = only clips with "Render-Cache Clip-Ausgabe"; Smart = additionally clips with
    // expensive effects or changed speed (saved, default User)
    Mode mode() const { return m_mode; }
    void setMode(Mode mode);

    void setFormat(const ProjectFormat& format);
    const ProjectFormat& format() const { return m_format; }

    // Should the clip (on a video track) be cached in the mode? Titles never.
    static bool wanted(const Clip& c, Mode mode);
    bool wanted(const Clip& c) const { return wanted(c, m_mode); }
    // Smart: effects or speed/reverse
    static bool isExpensive(const Clip& c);
    // Only what determines the image (without position, ID, audio, fades, transitions, active switch, cache marker)
    static Clip normalized(const Clip& c);
    static QString key(const Clip& c, const ProjectFormat& format);
    QString path(const Clip& c) const; // location (regardless of whether already rendered)

    // Preview: finished cache file if the mode caches the clip and it matches the current state; otherwise empty
    QString resolve(const Clip& c) const;

    // Display above the timeline: cached (blue) and not yet cached (red) ranges; done = rendered
    // part from the clip start (1 = done)
    struct Span {
        int start = 0, end = 0; // timeline frames, end exclusive
        double done = 0;
    };
    QVector<Span> spans(const Timeline& tl) const;

    // Timeline changed: queue missing clips (starts after a short rest), discard outdated jobs
    void sync(const Timeline& tl);
    // Rendering pauses during playback (and while an export is running)
    void setPlaying(bool on);
    void clear(); // "Delete render cache": all files gone (wanted clips are rendered again)

    bool isRendering() const { return m_thread != nullptr; }
    int pendingCount() const;

    // Renders the output of a clip (see TimelineBuilder::buildClipOutput) synchronously into `file`.
    // With alpha (ProRes 4444) if the clip has transparency, otherwise ProRes 422 LT. cancel/paused may be null.
    static bool render(const Clip& c, const ProjectFormat& format, const QString& file, QString* error,
                       const std::atomic<bool>* cancel = nullptr, const std::function<bool()>& paused = {},
                       std::atomic<int>* progress = nullptr);

signals:
    // Cache file finished/deleted or mode changed -> rebuild preview, refresh display
    void cacheChanged();
    void progressChanged(); // progress of the running clip (display)
    void modeChanged(RenderCache::Mode mode);
    void failed(const QString& message);

private:
    struct Job {
        QString key;
        Clip clip; // normalized
    };
    void startNext();
    void cancelCurrent();
    void onFinished(bool ok, const QString& error);

    Mode m_mode = Mode::User;
    ProjectFormat m_format;
    QVector<Job> m_jobs;        // wanted and not yet finished, in timeline order
    QHash<QString, bool> m_failed; // keys whose rendering failed (do not retry endlessly)
    QHash<QString, bool> m_reported; // already reported error messages (e.g. ffmpeg missing: only once)
    QTimer m_idle;              // short rest after changes before rendering
    QTimer m_poll;              // query progress
    QThread* m_thread = nullptr;
    QString m_currentKey;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress; // 0..1000
    int m_lastProgress = -1;
    std::atomic<bool> m_playing{false};
};
