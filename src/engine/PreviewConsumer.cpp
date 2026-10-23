#include "engine/PreviewConsumer.h"
#include "engine/audio/Wait.h"

#include <Mlt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using SteadyClock = std::chrono::steady_clock;

// Keeping up (see the header). Behind the sound by more than this (seconds): catch up, by rendering or by a jump
constexpr double kLate = 0.1;
// What a jump costs (seconds from the seek until the first frame at the target): the median of the last measured
// ones (it varies with the distance from the keyframe before the target; one slow jump, e.g. into a clip opened just
// then, must not make the next ones overshoot), this before the first
constexpr int kCostHistory = 5;
constexpr double kFirstJumpCost = 0.25;
constexpr double kMaxJumpCost = 5.0;
// Jump this much further than the cost says (a frame waiting a little for the clock beats one arriving late)
constexpr double kJumpMargin = 1.2;
// The first frames after a jump may still be from before it (rendered meanwhile): frames up to this many steps past
// the target count as arrived, and after this many others the jump is given up (the producer could not go there)
constexpr int kArriveSlack = 4;
constexpr int kJumpPatience = 25;

double seconds(SteadyClock::duration d) { return std::chrono::duration<double>(d).count(); }

} // namespace

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

    // A jump while playing (seek() or a catch-up), under `mutex`: frames until the target are from before it
    struct Jump {
        int target = -1; // -1 = none
        int direction = 0;
        SteadyClock::time_point start;
        int skipped = 0;
        bool reached = false; // a frame at the target came (maybe not rendered)
    } jump;
    // Consumer thread only. Measured jump costs (seconds, ring), and the picture behind the sound: since when, by
    // how much, and how fast rendering made up for it the last time (seconds per second, < 0 = fell further behind;
    // NaN = not seen yet at this speed)
    double costs[kCostHistory] = {};
    int measured = 0;
    bool behind = false;
    SteadyClock::time_point behindSince;
    double behindBy = 0;
    double gain = NAN;
    double gainSpeed = 0;

    static Impl* of(mlt_consumer c) { return static_cast<Impl*>(c->child); }

    mlt_producer producer() const
    {
        mlt_service s = mlt_service_producer(MLT_CONSUMER_SERVICE(consumer));
        return s ? MLT_PRODUCER(s) : nullptr;
    }

    double fps() const { return std::max(1.0, mlt_profile_fps(mlt_service_profile(MLT_CONSUMER_SERVICE(consumer)))); }

    static int start(mlt_consumer c)
    {
        Impl* self = of(c);
        if (self->running) return 0;
        if (self->thread.joinable()) self->thread.join();
        self->running = true;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->refreshes = 0;
            self->jump = {};
        }
        self->behind = false;
        self->gain = NAN;
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
        const double fps = this->fps();
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
            Audio::waitFor(cond, lock, std::chrono::nanoseconds(int64_t(seconds * 1e9))); // exact on Windows too
        }
    }

    double jumpCost() const
    {
        const int n = std::min(measured, kCostHistory);
        if (n == 0) return kFirstJumpCost;
        double sorted[kCostHistory];
        std::copy(costs, costs + n, sorted);
        std::sort(sorted, sorted + n);
        return n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
    }

    // A jump in flight: false while the frames are still from before it (not shown). Its cost counts until the first
    // picture there: MLT may skip rendering the target itself, then the next frame pays for decoding it.
    bool arrived(int pos, double speed, bool rendered)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (jump.target < 0) return true;
        if (!jump.reached) {
            const int past = jump.direction * (pos - jump.target);
            jump.reached = past >= 0 && past <= kArriveSlack * std::max(1, int(std::lround(std::abs(speed))));
            if (!jump.reached) {
                if (++jump.skipped < kJumpPatience) return false;
                jump = {}; // never came (the producer could not go there)
                behind = false;
                return true;
            }
        }
        if (rendered) {
            costs[measured++ % kCostHistory] = std::min(kMaxJumpCost, seconds(SteadyClock::now() - jump.start));
            jump = {};
            behind = false;
        }
        return true;
    }

    // After showing frame `pos` while playing: jump ahead if the picture is behind the sound and rendering would not
    // catch up by itself before a jump would be done
    void keepUp(int pos, double speed, unsigned purgedBefore)
    {
        const double now = clock ? clock() : NAN;
        const double rate = fps() * std::abs(speed); // clock frames per second
        // seconds behind: forward frame pos is due at clock pos, backward while the clock is above pos + 1
        const double late = (speed > 0 ? now - pos : pos + 1 - now) / rate;
        const SteadyClock::time_point t = SteadyClock::now();
        if (speed != gainSpeed) { // rendering 4x or backwards keeps up differently
            gain = NAN;
            gainSpeed = speed;
        }
        if (!(late > kLate)) { // in time, or no clock
            behind = false;
            return;
        }
        if (!behind) {
            // just fell behind: decide with how rendering did the last time (unknown: see how it goes on)
            behind = true;
            behindSince = t;
            behindBy = late;
        } else if (t > behindSince) {
            gain = (behindBy - late) / seconds(t - behindSince);
        }
        const double cost = jumpCost();
        if (std::isnan(gain) || (gain > 0 && late / gain <= cost)) return;
        const double lead = std::min(cost * kJumpMargin, kMaxJumpCost) * rate; // the clock runs on meanwhile
        const int direction = speed > 0 ? 1 : -1;
        {
            std::lock_guard<std::mutex> lock(mutex);
            mlt_producer p = producer();
            // a seek came since this frame was fetched: it wins
            if (!running || purges != purgedBefore || jump.target >= 0 || !p) return;
            const int last = std::max(0, int(mlt_producer_get_length(p)) - 1);
            const int target = std::clamp(int(direction > 0 ? std::ceil(now + lead) : std::floor(now - lead)), 0, last);
            if (direction * (target - pos) <= 0) return; // at the end already
            mlt_producer_seek(p, target);
            jump = {target, direction, t};
        }
        behind = false;
        mlt_consumer_purge(consumer); // what was rendered before the jump
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
            // Block our own listener (its owner is this, not props): otherwise resetting "refresh" counts as a new
            // refresh and the stopped preview renders the same frame endlessly (a whole core while idle)
            mlt_events_block(props, this);
            mlt_properties_set_int(props, "refresh", 0);
            mlt_events_unblock(props, this);
            if (speed != 0.0) {
                const int pos = mlt_frame_get_position(frame);
                const bool rendered = mlt_properties_get_int(fp, "rendered");
                const bool shown = arrived(pos, speed, rendered) && rendered && waitDue(pos, speed, purgedBefore);
                if (shown) show(frame);
                mlt_frame_close(frame);
                if (shown) keepUp(pos, speed, purgedBefore);
            } else {
                behind = false;
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

void PreviewConsumer::seek(int position)
{
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        mlt_producer p = d->producer();
        if (!p) return;
        mlt_producer_seek(p, position);
        ++d->purges; // under the lock: a catch-up decided on a frame from before sees it
        const double speed = mlt_producer_get_speed(p);
        d->jump = {};
        if (speed != 0.0) d->jump = {position, speed > 0 ? 1 : -1, SteadyClock::now()};
    }
    mlt_consumer_purge(d->consumer);
}
