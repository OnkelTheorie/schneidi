// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Does playback keep up? Counts the pictures shown while playing at normal speed and compares them with the frame
// rate. The main window offers proxy media when the machine is too slow for the originals (a Raspberry Pi showed
// 1080p originals at 8 of 30 pictures/s, the proxies smoothly). Pure logic, the times come from outside.

#include <QtGlobal>

class PlaybackMeter {
public:
    static constexpr qint64 kWarmupMs = 1500; // the start of playback (first pictures, preroll) does not count
    static constexpr qint64 kWindowMs = 4000; // measured span
    static constexpr double kSlow = 0.7;      // fewer pictures than this share of the frame rate: visibly choppy
    // The window is judged per second as well: only when most of its seconds were slow does it count. A short dip
    // (an expensive transition, a jump) is not a machine too slow for the originals, and proxies would not help it.
    static constexpr qint64 kSliceMs = 1000;
    static constexpr double kSlowShare = 0.75; // share of the window's seconds that must be slow

    // Playback at normal speed starts (fps = frame rate of what plays); pause, another speed or a jump: stop()
    void start(double fps, qint64 nowMs)
    {
        m_fps = fps;
        m_windowStart = nowMs + kWarmupMs;
        m_open = false;
    }
    void stop() { m_fps = 0; }
    bool running() const { return m_fps > 0; }

    // A picture was shown. True when a full window was too slow (rate() tells how slow); the next window follows.
    bool frame(qint64 nowMs)
    {
        if (m_fps <= 0 || nowMs < m_windowStart) return false;
        if (!m_open) { // the first picture after the warmup opens the window
            m_open = true;
            openWindow(nowMs);
            return false;
        }
        ++m_frames;
        // Close the seconds that are over (a long stall closes several, each with no pictures: slow)
        for (; nowMs - m_sliceStart >= kSliceMs; m_sliceStart += kSliceMs, m_sliceFrames = 0) {
            ++m_slices;
            if (m_sliceFrames < kSlow * m_fps * kSliceMs / 1000.0) ++m_slowSlices;
        }
        ++m_sliceFrames;
        const qint64 span = nowMs - m_windowStart;
        if (span < kWindowMs) return false;
        m_rate = m_frames * 1000.0 / double(span);
        const bool mostlySlow = m_slowSlices >= kSlowShare * m_slices;
        openWindow(nowMs);
        return m_rate < kSlow * m_fps && mostlySlow;
    }
    double rate() const { return m_rate; } // pictures per second in the last window
    double fps() const { return m_fps; }

private:
    void openWindow(qint64 nowMs)
    {
        m_windowStart = m_sliceStart = nowMs;
        m_frames = m_sliceFrames = m_slices = m_slowSlices = 0;
    }

    double m_fps = 0;
    qint64 m_windowStart = 0;
    int m_frames = 0;
    qint64 m_sliceStart = 0;
    int m_sliceFrames = 0, m_slices = 0, m_slowSlices = 0;
    bool m_open = false;
    double m_rate = 0;
};
