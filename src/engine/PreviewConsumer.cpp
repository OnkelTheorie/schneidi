#include "engine/PreviewConsumer.h"

#include <Mlt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

struct PreviewConsumer::Impl {
    mlt_consumer consumer = nullptr;
    std::unique_ptr<Mlt::Consumer> wrapper;
    Clock clock;
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable cond;
    int refreshes = 0;                 // like sdl2_audio's refresh_count
    std::atomic<unsigned> purges{0};   // a frame fetched before a purge is not shown any more

    static Impl* of(mlt_consumer c) { return static_cast<Impl*>(c->child); }

    static int start(mlt_consumer c)
    {
        Impl* self = of(c);
        if (self->running) return 0;
        if (self->thread.joinable()) self->thread.join();
        self->running = true;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->refreshes = 0;
        }
        self->thread = std::thread([self] { self->run(); });
        return 0;
    }

    // mlt_consumer_stop calls this before it stops the read-ahead: the thread may sit in mlt_consumer_rt_frame, which
    // returns once the read-ahead delivers the next frame
    static int stop(mlt_consumer c)
    {
        Impl* self = of(c);
        if (self->running) {
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->running = false;
            }
            self->cond.notify_all();
        }
        if (self->thread.joinable() && self->thread.get_id() != std::this_thread::get_id()) self->thread.join();
        return 0;
    }

    static int isStopped(mlt_consumer c) { return of(c)->running ? 0 : 1; }

    static void purge(mlt_consumer c)
    {
        Impl* self = of(c);
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            ++self->purges;
        }
        self->cond.notify_all();
    }

    static void onPropertyChanged(mlt_properties, Impl* self, mlt_event_data data)
    {
        const char* name = mlt_event_data_to_string(data);
        if (!name || std::strcmp(name, "refresh") != 0) return;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->refreshes = self->refreshes <= 0 ? 1 : std::min(2, self->refreshes + 1);
        }
        self->cond.notify_all();
    }

    // Waits until the clock reaches the frame; false = stopped or purged meanwhile (do not show it)
    bool waitDue(int pos, double speed, unsigned purgedBefore)
    {
        const double fps = std::max(1.0, mlt_profile_fps(mlt_service_profile(MLT_CONSUMER_SERVICE(consumer))));
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (!running || purges != purgedBefore) return false;
            lock.unlock();
            const double now = clock ? clock() : NAN;
            lock.lock();
            if (std::isnan(now)) return true;
            // forward: frame pos is due at the clock pos; backward: once the clock is below pos + 1
            const double ahead = speed > 0 ? pos - now : now - (pos + 1);
            if (ahead <= 0) return true;
            const double seconds = std::min(ahead / (fps * std::abs(speed)), 0.05); // look again: the clock may stand
            cond.wait_for(lock, std::chrono::duration<double>(seconds));
        }
    }

    void show(mlt_frame frame)
    {
        mlt_events_fire(MLT_CONSUMER_PROPERTIES(consumer), "consumer-frame-show", mlt_event_data_from_frame(frame));
    }

    void run()
    {
        mlt_properties props = MLT_CONSUMER_PROPERTIES(consumer);
        while (running) {
            const unsigned purgedBefore = purges;
            mlt_frame frame = mlt_consumer_rt_frame(consumer);
            if (!frame) continue;
            mlt_properties fp = MLT_FRAME_PROPERTIES(frame);
            const double speed = mlt_properties_get_double(fp, "_speed");
            mlt_events_block(props, props);
            mlt_properties_set_int(props, "refresh", 0);
            mlt_events_unblock(props, props);
            if (speed != 0.0) {
                if (mlt_properties_get_int(fp, "rendered") && waitDue(mlt_frame_get_position(frame), speed, purgedBefore))
                    show(frame);
                mlt_frame_close(frame);
            } else {
                show(frame);
                mlt_frame_close(frame);
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (--refreshes <= 0) cond.wait(lock, [this] { return !running || refreshes > 0; });
                }
                mlt_consumer_purge(consumer); // the next refresh renders anew (the producer may have moved)
            }
        }
    }
};

PreviewConsumer::PreviewConsumer(Mlt::Profile& profile, Clock clock) : d(std::make_unique<Impl>())
{
    d->clock = std::move(clock);
    d->consumer = mlt_consumer_new(profile.get_profile());
    // mlt_consumer_start reads the profile from the service (the factory sets it for its consumers)
    mlt_service_set_profile(MLT_CONSUMER_SERVICE(d->consumer), profile.get_profile());
    d->consumer->child = d.get();
    d->consumer->start = Impl::start;
    d->consumer->stop = Impl::stop;
    d->consumer->is_stopped = Impl::isStopped;
    d->consumer->purge = Impl::purge;
    mlt_events_listen(MLT_CONSUMER_PROPERTIES(d->consumer), d.get(), "property-changed",
                      reinterpret_cast<mlt_listener>(Impl::onPropertyChanged));
    d->wrapper = std::make_unique<Mlt::Consumer>(d->consumer);
}

PreviewConsumer::~PreviewConsumer()
{
    mlt_consumer_stop(d->consumer);
    d->wrapper.reset();
    mlt_consumer_close(d->consumer);
}

Mlt::Consumer& PreviewConsumer::consumer() { return *d->wrapper; }
