// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Preroll.h"

#include <Mlt.h>
#include <memory>

namespace {
// Distance to the spot: closer than this, playback may already be reading the producer (lead-in buffer)
constexpr int kMinDistance = 8;
// Lead-in buffer of the consumer: the producer can still be read this far behind freeFrom
constexpr int kBusyMargin = 4;
} // namespace

Preroll::Preroll() : m_thread([this] { run(); }) {}

Preroll::~Preroll()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void Preroll::setPoints(std::vector<PrerollPoint> points, int lookahead, QSize size)
{
    std::vector<PrerollPoint> old;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        old.swap(m_points); // release outside the lock
        m_points = std::move(points);
        m_done.assign(m_points.size(), false);
        m_lookahead = lookahead;
        m_size = size;
    }
    m_wake.notify_all();
    // A decode still running belongs to the old graph: its "free from" no longer holds, and the new graph reuses the
    // cached producers. Playback starts right after this, so it must not read a producer this thread still decodes.
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return !m_busy; });
}

void Preroll::clear()
{
    setPoints({}, 0, {});
}

void Preroll::update(int position, double speed)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Jumped (back or far ahead): spots already prepared no longer apply
        if (position < m_position || position > m_position + m_lookahead) m_done.assign(m_points.size(), false);
        m_position = position;
        m_speed = speed;
    }
    m_wake.notify_all();
}

void Preroll::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_quit) {
        // Next due spot: within look-ahead, producer no longer in use
        int next = -1;
        if (m_speed > 0.0) {
            for (size_t i = 0; i < m_points.size(); ++i) {
                const PrerollPoint& p = m_points[i];
                if (p.frame > m_position + m_lookahead) break; // sorted
                if (m_done[i] || p.frame < m_position + kMinDistance) continue;
                if (p.freeFrom + kBusyMargin > m_position) continue; // is still being read
                next = int(i);
                break;
            }
        }
        if (next < 0) {
            m_wake.wait(lock);
            continue;
        }
        m_done[next] = true;
        m_busy = true;
        std::shared_ptr<Mlt::Producer> producer = m_points[next].producer;
        const int source = m_points[next].source;
        const bool audio = m_points[next].audio;
        const QSize size = m_size;
        lock.unlock();
        // Decode the frame before it: afterwards the decoder expects exactly `source` and reads on without a jump
        const int at = std::max(0, source - 1);
        producer->seek(at);
        std::unique_ptr<Mlt::Frame> frame(producer->get_frame());
        if (frame && audio) {
            mlt_audio_format fmt = mlt_audio_float;
            int freq = 48000, channels = 2;
            int samples = mlt_audio_calculate_frame_samples(float(producer->get_fps()), freq, at);
            frame->get_audio(fmt, freq, channels, samples);
        } else if (frame) {
            mlt_image_format fmt = mlt_image_yuv422;
            int w = size.width(), h = size.height();
            frame->get_image(fmt, w, h);
        }
        frame.reset(); // give up references before reporting: afterwards the engine may tear down producers and profile
        producer.reset();
        lock.lock();
        m_busy = false;
        m_idle.notify_all();
    }
}
