// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Giving memory back to the system while the app is idle. The picture engine keeps frame buffers for reuse
// (Picture::releasePools); glibc keeps freed heap memory in the process. After playing 4K material, an export or a
// long AI session the app held GBs that nothing used any more.
//
// release() empties the picture engine's pools and trims glibc's heap. IdleRelease does that once nothing renders:
// not while the preview plays, an export runs or the render cache renders (the pools would only fill again).
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

// The picture engine's unused buffers and glibc's free heap back to the system
void release();
// Only glibc's free heap (after tearing down decoders: their frames came from the heap)
void trimHeap();
// Resident memory of this process in bytes (Linux; -1 = unknown)
qint64 residentBytes();
// The system runs short of memory: less than a tenth of it available (at least 256 MB, at most 2 GB kept free).
// core/SystemMemory: MemAvailable or the room a cgroup leaves, Windows the physical memory available; false where
// unknown. Decoders kept open for an instant Play are closed only then (Picture::Renderer::trim).
bool underPressure();
// Tests: 1 = under pressure, 0 = not, -1 = measure (the default)
void setPressureForTests(int state);
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
    // Idle now, right before the pools are emptied (this object's thread): drop what is kept only for speed
    void releasing();
    void released(qint64 before, qint64 after); // resident bytes before/after (-1 = unknown)

private:
    void check();

    std::function<MemoryRelease::Activity()> m_activity;
    QTimer m_timer;
    QThread* m_worker = nullptr; // release() runs here (freeing GBs takes a moment)
    bool m_off = false;          // SCHNEIDI_MEMORY_RELEASE_MS=0
    qint64 m_before = -1;
};
