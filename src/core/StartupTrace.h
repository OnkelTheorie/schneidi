// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Startup phase timer: SCHNEIDI_STARTUP_TRACE=1 logs "startup: <phase> <ms since main>" (and the gap to the previous
// mark) for every StartupTrace::mark(). Off: one cached flag test per mark, nothing else.
// SCHNEIDI_STARTUP_TRACE=2 also quits the app 2.5 s after the main window's first paint (measuring runs).

#include <QByteArray>
#include <QElapsedTimer>
#include <QtGlobal>
#include <cstdio>

namespace StartupTrace {

inline QElapsedTimer& clock()
{
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t;
}

inline bool enabled()
{
    static const bool on = qEnvironmentVariableIntValue("SCHNEIDI_STARTUP_TRACE") != 0;
    return on;
}

// Milliseconds since the first call (main() calls start() first thing)
inline qint64 elapsed() { return clock().elapsed(); }
inline void start() { (void)clock(); }

inline void mark(const char* phase)
{
    if (!enabled()) return;
    static qint64 last = 0;
    const qint64 now = clock().elapsed();
    // stderr directly: the log may not be installed yet, and the numbers must not wait for a buffer
    std::fprintf(stderr, "startup: %-34s %6lld ms  (+%lld)\n", phase, static_cast<long long>(now),
                 static_cast<long long>(now - last));
    std::fflush(stderr);
    last = now;
}

} // namespace StartupTrace
