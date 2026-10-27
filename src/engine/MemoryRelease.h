#pragma once
// Giving memory back to the system while the app is idle. MLT keeps every image buffer it ever handed out in its pool
// (mlt_pool) for reuse and never frees one; glibc keeps freed heap memory in the process. After playing 4K material,
// an export or a long AI session the app held GBs that nothing used any more.
//
// release() empties MLT's pool of the buffers nobody holds (mlt_pool_purge locks each pool, so MLT threads may keep
// running) and trims glibc's heap. IdleRelease does that once nothing renders: not while the preview plays, an export
// runs or the render cache renders (purging then only makes MLT allocate the same buffers again).
// What stays (tests/bench_memory): malloc_trim gives back free pages inside every arena, but the free top of a thread's
// own arena only above glibc's trim threshold (raised up to 64 MB per arena): with many decoder/render threads a few
// hundred MB (3 HD sources after an export: 1490 MB -> 596 MB, with GLIBC_TUNABLES=glibc.malloc.arena_max=2 -> 142 MB).

#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>

class QThread;

namespace MemoryRelease {

// What runs right now (the policy's input)
struct Activity {
    bool playing = false;   // preview plays or shuttles
    bool exporting = false; // an export renders (Exporter::anyRunning)
    bool caching = false;   // the render cache renders a clip
};
// The policy: memory is given back only while nothing renders
bool allowed(const Activity& a);

// MLT's unused pool buffers and glibc's free heap back to the system (no-op for MLT before Mlt::Factory::init)
void release();
// Only glibc's free heap (after tearing down decoders: their frames came from the heap, not from MLT's pool)
void trimHeap();
// Resident memory of this process in bytes (Linux; -1 = unknown)
qint64 residentBytes();
// How often release() ran (tests)
int releases();

// Something rendered or freed frames: the IdleRelease of the running app (if any) gives memory back once idle.
// Thread-safe; nothing happens without an IdleRelease (schneidi-cli releases after each MCP tool itself).
void requestRelease();

} // namespace MemoryRelease

class IdleRelease : public QObject {
    Q_OBJECT
public:
    // activity: what runs right now (asked on this object's thread when the delay is over). Registers itself as the
    // one MemoryRelease::requestRelease() reaches (the newest one wins).
    explicit IdleRelease(std::function<MemoryRelease::Activity()> activity, QObject* parent = nullptr);
    ~IdleRelease() override;

    // Gives memory back once nothing rendered for delayMs() (each call starts the wait anew). Busy then: tries again
    // after the same wait, until it is idle.
    void request();
    bool pending() const { return m_timer.isActive(); }
    // Default 4 s: a quick stop/play does not throw away the buffers the next play needs anyway
    void setDelayMs(int ms) { m_timer.setInterval(ms); }
    int delayMs() const { return m_timer.interval(); }

signals:
    void released(qint64 before, qint64 after); // resident bytes before/after (-1 = unknown)

private:
    void check();

    std::function<MemoryRelease::Activity()> m_activity;
    QTimer m_timer;
    QThread* m_worker = nullptr; // release() runs here (freeing GBs takes a moment)
    bool m_off = false;          // SCHNEIDI_MEMORY_RELEASE_MS=0
    qint64 m_before = -1;
};
