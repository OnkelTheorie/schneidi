// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Project-open phase timer: SCHNEIDI_OPEN_TRACE=1 logs "open: <phase> <ms since the open began>" (and the gap to the
// previous mark) for every OpenTrace::mark() of MainWindow::openProject, then the first timeline paint, the viewer's
// first frame, "usable" (both shown and the event loop idle) and the longest GUI-thread stall of the next 3 s.
// SCHNEIDI_OPEN_TRACE=2 quits after that report (measuring runs); =3 opens the same project once more first, the
// File › Open path into a running app (SCHNEIDI_OPEN_TRACE_NEXT=<file>: that project instead), and quits after its
// report. Off: one cached flag test per mark.

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QImage>
#include <QWidget>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

namespace OpenTrace {

inline int level()
{
    static const int on = qEnvironmentVariableIntValue("SCHNEIDI_OPEN_TRACE");
    return on;
}
inline bool enabled() { return level() != 0; }

struct State {
    QElapsedTimer clock;
    qint64 last = 0;
    std::atomic<bool> sent{false}; // the picture engine has the new timeline
    std::mutex framesMutex;
    std::deque<bool> frames; // frames emitted, not delivered yet: emitted after sent?
    int round = 0; // 1 = the first open of this process, 2 = the reopen of SCHNEIDI_OPEN_TRACE=3
};
inline State& state()
{
    static State s;
    return s;
}

inline void mark(const char* phase)
{
    if (!enabled() || !state().clock.isValid()) return;
    State& s = state();
    const qint64 now = s.clock.elapsed();
    std::fprintf(stderr, "open%d: %-34s %6lld ms  (+%lld)\n", s.round, phase, static_cast<long long>(now),
                 static_cast<long long>(now - s.last));
    std::fflush(stderr);
    s.last = now;
}

// Follows one open until it is usable: the timeline view painted, the viewer showed a frame, then the event loop
// went idle. Measures GUI-thread stalls (awake -> aboutToBlock) for 3 s after that.
class Watch : public QObject {
public:
    // The engine's frame signal goes to frameEmitted (direct, the emitting thread) and frameDelivered (queued, GUI)
    Watch(QWidget* timelineView, std::function<void()> reopen)
        : QObject(timelineView), m_view(timelineView), m_reopen(std::move(reopen))
    {
        m_view->installEventFilter(this);
        connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, this, [this] { idle(); });
        // Stalls: how late a 5 ms heartbeat comes (the GUI thread was busy that long)
        m_beat.setInterval(5);
        m_beat.setTimerType(Qt::PreciseTimer);
        connect(&m_beat, &QTimer::timeout, this, [this] {
            if (m_usable) m_maxStall = std::max(m_maxStall, m_busy.elapsed() - 5);
            m_busy.start();
        });
    }
    // A frame leaves the engine: of the new timeline only once the engine has it (OpenTrace::timelineSent); queued
    // frames from before are delivered in order, so each delivery learns which kind it is
    static void frameEmitted()
    {
        State& s = state();
        std::lock_guard lock(s.framesMutex);
        s.frames.push_back(s.sent.load());
    }
    void frameDelivered(bool hasPicture)
    {
        bool fresh = false;
        {
            State& s = state();
            std::lock_guard lock(s.framesMutex);
            if (!s.frames.empty()) {
                fresh = s.frames.front();
                s.frames.pop_front();
            }
        }
        if (!fresh || !hasPicture || m_frame) return;
        m_frame = true;
        mark("viewer first frame");
    }
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == m_view && e->type() == QEvent::Paint && !m_painted) {
            m_painted = true;
            mark("timeline first paint");
        }
        return false;
    }

private:
    void idle()
    {
        if (!m_firstIdle) {
            m_firstIdle = true;
            mark("event loop first idle");
        }
        if (m_usable) return;
        if (!m_painted || !m_frame) return;
        m_usable = true;
        mark("usable (shown, event loop idle)");
        m_busy.start();
        m_beat.start();
        QTimer::singleShot(3000, this, [this] {
            std::fprintf(stderr, "open%d: %-34s %6lld ms\n", state().round, "longest GUI stall in next 3 s",
                         static_cast<long long>(m_maxStall));
            std::fflush(stderr);
            const int lvl = level();
            if (lvl == 3 && state().round == 1 && m_reopen) {
                std::function<void()> reopen = m_reopen;
                deleteLater();
                QTimer::singleShot(0, qApp, reopen);
            } else if (lvl >= 2) {
                QCoreApplication::exit(0);
            } else {
                deleteLater();
            }
        });
    }

    QPointer<QWidget> m_view;
    std::function<void()> m_reopen;
    QElapsedTimer m_busy;
    QTimer m_beat;
    qint64 m_maxStall = 0;
    bool m_painted = false, m_frame = false, m_firstIdle = false, m_usable = false;
};

// A frame with a picture in it (the empty timeline's black frame of before the open does not count): trace only
inline bool hasContent(const QImage& img)
{
    if (img.isNull()) return false;
    const int w = img.width(), h = img.height();
    for (const QPoint p : {QPoint(w / 2, h / 2), QPoint(w / 4, h / 4), QPoint(3 * w / 4, h / 4), QPoint(w / 4, 3 * h / 4),
                           QPoint(3 * w / 4, 3 * h / 4)})
        if (qGray(img.pixel(p)) > 8) return true;
    return false;
}

// Starts the clock of one open (MainWindow::openProject)
inline void begin()
{
    if (!enabled()) return;
    State& s = state();
    ++s.round;
    s.clock.start();
    s.last = 0;
    s.sent = false;
    mark("begin");
}

// The picture engine got the opened project's timeline (MainWindow: Engine::updateTimeline)
inline void timelineSent()
{
    if (!enabled() || !state().clock.isValid() || state().sent.exchange(true)) return;
    mark("engine got the new timeline");
}

} // namespace OpenTrace
