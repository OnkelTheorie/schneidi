#pragma once
// Background work (thumbnails, waveforms, scopes, caches, proxies) must not slow down playback, opening a project or
// the window. QThread priorities do nothing on Linux: threads run as SCHED_OTHER, which has no thread priority
// (only IdlePriority switches to SCHED_IDLE). There the nice value of the thread is what counts.

class QProcess;

namespace Background {

enum class Level {
    Low,    // visible soon (thumbnails, scopes): Linux nice 10
    Lowest, // may wait (waveforms, render cache, stabilizer): Linux nice 19
};

// Lowers the calling thread. Linux: nice of this thread (it cannot be raised again without rights, so only for
// threads that do nothing else afterwards: own threads, own pools). Elsewhere: QThread priority.
void lowerThisThread(Level level);

// Starts the child process (ffmpeg) at low priority: nice 10 / BELOW_NORMAL_PRIORITY_CLASS. Call before start().
void lowerProcess(QProcess& process);

// Work the user waits for right now (opening files for the preview, building it): while one is alive, background
// jobs hold back before their next step (a low priority alone still leaves them a share of a weak CPU).
// Any thread; nests. Release it where the work ends, not in a queued call (quitting would wait for it).
class Foreground {
public:
    Foreground();
    ~Foreground();
    Foreground(const Foreground&) = delete;
    Foreground& operator=(const Foreground&) = delete;
};
bool foregroundBusy();
// Background threads: waits while foreground work runs (at most 20 s, then it goes on anyway)
void yieldToForeground();

} // namespace Background
