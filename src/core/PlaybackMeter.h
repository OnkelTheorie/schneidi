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
            m_windowStart = nowMs;
            m_frames = 0;
            return false;
        }
        ++m_frames;
        const qint64 span = nowMs - m_windowStart;
        if (span < kWindowMs) return false;
        m_rate = m_frames * 1000.0 / double(span);
        m_windowStart = nowMs;
        m_frames = 0;
        return m_rate < kSlow * m_fps;
    }
    double rate() const { return m_rate; } // pictures per second in the last window
    double fps() const { return m_fps; }

private:
    double m_fps = 0;
    qint64 m_windowStart = 0;
    int m_frames = 0;
    bool m_open = false;
    double m_rate = 0;
};
