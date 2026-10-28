#pragma once
#include "engine/TimelineBuilder.h"

#include <QSize>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Preview: bring the decoder (video and audio) to a jump spot (clip start, transition) in the background beforehand.
// With material with long GOPs (OBS, phone) the decoder has to decode up to several seconds from the last keyframe
// when jumping – in the middle of playback the image then froze for up to ~1 s. Here the frame before the spot is
// decoded in time, after which playback reads on in one go. Forward playback only.
class Preroll {
public:
    Preroll();
    ~Preroll();

    // After every rebuild of the preview (spots from TimelineBuilder::prerollPoints); waits for a decode that is
    // currently running (it read a producer the new graph may share)
    void setPoints(std::vector<PrerollPoint> points, int lookahead, QSize size);
    // Shown frame (consumer thread) and speed; speed <= 0 = prepare nothing
    void update(int position, double speed);
    // Forget everything and wait for a decode that is currently running (before tearing down profile/producers)
    void clear();

private:
    void run();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    bool m_busy = false; // currently decoding (without lock)
    std::vector<PrerollPoint> m_points;
    std::vector<bool> m_done;
    int m_lookahead = 0; // Frames
    QSize m_size;
    int m_position = 0;
    double m_speed = 0.0;
    bool m_quit = false;
    std::thread m_thread;
};
