// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Pump.h"

#include "core/SystemMemory.h"
#include "engine/audio/Wait.h"

#include <QThread>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace Picture {

namespace {

using SteadyClock = std::chrono::steady_clock;

// Keeping up (see the header): behind the sound by more than this (seconds) = catch up, by rendering or by a jump
constexpr double kLate = 0.1;
// What a jump costs (seconds from the jump until the frame at its target is there): the median of the last ones,
// this before the first
constexpr int kCostHistory = 5;
constexpr double kFirstJumpCost = 0.25;
constexpr double kMaxJumpCost = 5.0;
// Jump this much further than the cost says (a frame waiting a little for the clock beats one arriving late)
constexpr double kJumpMargin = 1.2;
// Idle workers: look for clips starting soon this often, drop unused decoders this often
constexpr auto kPrepareEvery = std::chrono::milliseconds(250);
constexpr auto kTrimEvery = std::chrono::seconds(10);
// Recently shown frames: at most 192 MB = ~95 frames of the 960x540 preview, ~23 at full 1080p; on a small machine
// 1/16 of its memory (see recentBudgetFor), at least 16 MB
constexpr int64_t kRecentBudget = 192ll * 1024 * 1024;
constexpr int64_t kMinRecentBudget = 16ll * 1024 * 1024;

double seconds(SteadyClock::duration d) { return std::chrono::duration<double>(d).count(); }

} // namespace

struct Pump::Impl {
    Clock clock;
    Show show;
    int workerCount = 2;
    std::vector<std::thread> workers;
    std::thread shower;

    mutable std::mutex m;
    std::condition_variable wake;  // workers: something to render
    std::condition_variable ready; // show thread: a frame came, or the state changed
    std::mutex showMutex;          // held while Show runs; seek/setSpeed/setContent take it first
    bool quit = false;

    std::shared_ptr<const Content> content;
    uint64_t generation = 0; // +1 when the content changes: renders of before are dropped
    uint64_t serial = 0;     // +1 at every seek/speed/content change: a frame taken before is not shown
    double speed = 0;
    int64_t next = 0;        // next frame to show (stopped: the frame to show)
    bool pendingStill = false;
    struct Rendered {
        QImage image;
        bool exact = true; // false: a close picture (scrubbing), the exact one follows
    };
    std::map<int64_t, Rendered> frames; // rendered for `generation`
    std::set<int64_t> inflight;         // being rendered for `generation`
    // Still renders in flight that can be cancelled (Content::renderWant), by position
    std::map<int64_t, std::shared_ptr<std::atomic<bool>>> cancels;
    bool scrub = false; // stopped: the pending still may be a close picture first

    // Recently shown frames of `generation` (LRU by stamp, within recentBudget bytes)
    struct Recent {
        QImage image;
        uint64_t stamp = 0;
    };
    std::map<int64_t, Recent> recent;
    int64_t recentBytes = 0, recentBudget = kRecentBudget;
    uint64_t stamp = 0;
    bool preparing = false;
    SteadyClock::time_point lastPrepare, lastTrim = SteadyClock::now();

    // Keeping up (show thread, under m)
    struct Jump {
        int64_t target = -1;
        SteadyClock::time_point start;
    } jump;
    double costs[kCostHistory] = {};
    int measured = 0;
    bool behind = false;
    SteadyClock::time_point behindSince;
    double behindBy = 0;
    double gain = NAN;
    double gainSpeed = 0;

    Stats stats;

    int direction() const { return speed > 0 ? 1 : speed < 0 ? -1 : 0; }
    int64_t step() const { return std::max<int64_t>(1, std::llround(std::abs(speed))); }
    int ahead() const { return speed == 0 ? 1 : workerCount + 2; }
    double fps() const { return content && content->fps > 0 ? content->fps : 25.0; }
    int64_t last() const { return content && content->last ? content->last() : -1; }
    bool inside(int64_t p) const
    {
        const int64_t l = last();
        return p >= 0 && (l < 0 || p <= l);
    }

    // Under m: the next frame a worker should render, -1 = none
    int64_t pick() const
    {
        if (!content || !content->render) return -1;
        if (speed == 0) {
            // A still behind the end too: the content shows what is there (nothing: black), never the frame shown before
            if (pendingStill && !frames.count(next) && !inflight.count(next) && next >= 0) return next;
            return -1;
        }
        const int64_t s = step() * direction();
        for (int k = 0; k < ahead(); ++k) {
            const int64_t p = next + k * s;
            if (!inside(p)) break;
            if (!frames.count(p) && !inflight.count(p)) return p;
        }
        return -1;
    }

    // Under m: drops rendered frames the show thread will not take any more
    void prune()
    {
        if (speed == 0) {
            for (auto it = frames.begin(); it != frames.end();)
                it = it->first != next ? (++stats.skipped, frames.erase(it)) : std::next(it);
            return;
        }
        const int dir = direction();
        const int64_t span = step() * ahead();
        for (auto it = frames.begin(); it != frames.end();) {
            const int64_t off = dir * (it->first - next);
            it = off < 0 || off >= span ? (++stats.skipped, frames.erase(it)) : std::next(it);
        }
    }

    void workerLoop()
    {
        std::unique_lock lock(m);
        while (!quit) {
            if (takeRecent()) ready.notify_all();
            const int64_t p = pick();
            if (p < 0) {
                const auto now = SteadyClock::now();
                std::shared_ptr<const Content> c = content;
                if (c && speed > 0 && c->prepare && !preparing && now - lastPrepare >= kPrepareEvery) {
                    preparing = true;
                    lastPrepare = now;
                    const int64_t at = next;
                    lock.unlock();
                    c->prepare(at);
                    lock.lock();
                    preparing = false;
                    continue;
                }
                if (c && c->trim && now - lastTrim >= kTrimEvery) {
                    lastTrim = now;
                    const int64_t shown = next;
                    lock.unlock();
                    c->trim(shown);
                    lock.lock();
                    continue;
                }
                wake.wait_for(lock, kPrepareEvery);
                continue;
            }
            inflight.insert(p);
            const uint64_t gen = generation;
            std::shared_ptr<const Content> c = content;
            std::shared_ptr<std::atomic<bool>> cancel;
            Want want;
            if (speed == 0 && c->renderWant) {
                cancel = std::make_shared<std::atomic<bool>>(false);
                cancels[p] = cancel;
                want.nearest = scrub;
                want.cancel = cancel.get();
            }
            lock.unlock();
            QImage img = cancel ? c->renderWant(p, want) : c->render(p);
            lock.lock();
            ++stats.rendered;
            if (cancel) {
                auto it = cancels.find(p);
                if (it != cancels.end() && it->second == cancel) cancels.erase(it);
            }
            if (gen != generation) continue; // content changed meanwhile
            inflight.erase(p);
            if (cancel && cancel->load()) { // the position moved on
                ++stats.cancelled;
                wake.notify_all(); // it may be wanted again meanwhile
                continue;
            }
            if (cancel && !want.exact && speed != 0) { // a scrub's close picture, finished after playback started
                wake.notify_all();                      // the exact one is rendered for it
                continue;
            }
            frames[p] = {std::move(img), !cancel || want.exact};
            prune();
            ready.notify_all();
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

    // Under m, after showing `pos` while playing: jump ahead if the picture is behind the sound and rendering would
    // not catch up by itself before a jump would be done
    void keepUp(int64_t pos, double now)
    {
        const double rate = fps() * std::abs(speed); // clock frames per second
        const double late = (speed > 0 ? now - pos : pos + 1 - now) / rate;
        const SteadyClock::time_point t = SteadyClock::now();
        if (speed != gainSpeed) { // rendering 4x or backwards keeps up differently
            gain = NAN;
            gainSpeed = speed;
        }
        if (!(late > kLate) || jump.target >= 0) { // in time, no clock, or a jump on its way
            if (jump.target < 0) behind = false;
            return;
        }
        if (!behind) {
            behind = true;
            behindSince = t;
            behindBy = late;
        } else if (t > behindSince) {
            gain = (behindBy - late) / seconds(t - behindSince);
        }
        const double cost = jumpCost();
        if (std::isnan(gain) || (gain > 0 && late / gain <= cost)) return;
        const double lead = std::min(cost * kJumpMargin, kMaxJumpCost) * rate; // the clock runs on meanwhile
        const int dir = direction();
        int64_t target = int64_t(dir > 0 ? std::ceil(now + lead) : std::floor(now - lead));
        const int64_t l = last();
        target = std::max<int64_t>(0, l >= 0 ? std::min(target, l) : target);
        if (dir * (target - pos) <= 0) return; // at the end already
        next = target;
        jump = {target, t};
        behind = false;
        ++stats.jumps;
        prune();
        wake.notify_all();
    }

    void showLoop()
    {
        std::unique_lock lock(m);
        while (!quit) {
            if (!content) {
                ready.wait(lock);
                continue;
            }
            if (speed == 0) {
                auto it = pendingStill ? frames.find(next) : frames.end();
                if (it == frames.end()) {
                    ready.wait(lock);
                    continue;
                }
                Rendered r = std::move(it->second);
                frames.erase(it);
                if (r.exact) {
                    pendingStill = false;
                } else {
                    scrub = false; // the exact picture next (still pending)
                    ++stats.nearest;
                    wake.notify_all();
                }
                const int64_t at = next;
                const uint64_t gen = generation;
                QImage keep = r.exact ? r.image : QImage();
                if (deliver(lock, std::move(r.image), at, 0.0)) remember(at, std::move(keep), gen);
                continue;
            }
            const int dir = direction();
            const double now = clock ? clock() : NAN;
            auto it = frames.find(next);
            if (it != frames.end() && !it->second.exact) { // never played (nor kept as exact): render it again
                frames.erase(it);
                wake.notify_all();
                it = frames.end();
            }
            if (it == frames.end()) {
                // Overdue while a later frame that is due already is there (workers finish out of order): skip
                if (!std::isnan(now)) {
                    int64_t best = -1;
                    for (const auto& [p, img] : frames)
                        if (dir * (p - next) > 0 && (dir > 0 ? now >= p : now <= p + 1)
                            && (best < 0 || dir * (p - best) > 0))
                            best = p;
                    if (best >= 0) {
                        next = best;
                        prune();
                        continue;
                    }
                }
                Audio::waitFor(ready, lock, std::chrono::milliseconds(20));
                continue;
            }
            // forward: frame `next` is due at clock `next`; backward once the clock is below next + 1
            const double early = std::isnan(now) ? 0 : dir > 0 ? next - now : now - (next + 1);
            if (early > 0) {
                const double s = std::min(early / (fps() * std::abs(speed)), 0.05); // look again: the clock may stand
                Audio::waitFor(ready, lock, std::chrono::nanoseconds(int64_t(s * 1e9)));
                continue;
            }
            const int64_t pos = next;
            QImage img = std::move(it->second.image);
            frames.erase(it);
            const uint64_t gen = generation;
            QImage keep = img;
            if (jump.target >= 0 && dir * (pos - jump.target) >= 0) { // the jump arrived: what it cost
                costs[measured++ % kCostHistory] = std::min(kMaxJumpCost, seconds(SteadyClock::now() - jump.start));
                jump = {};
                behind = false;
            }
            next = pos + dir * step();
            // Faster than 1x the step may jump over the end: the end frame is shown (the engine stops there)
            if (const int64_t end = last(); dir > 0 && end >= 0 && pos < end && next > end) next = end;
            if (dir < 0 && pos > 0 && next < 0) next = 0;
            prune();
            wake.notify_all();
            const double s = speed;
            const bool shown = deliver(lock, std::move(img), pos, s);
            if (shown) remember(pos, std::move(keep), gen);
            if (shown && !std::isnan(now)) keepUp(pos, clock ? clock() : NAN);
        }
    }

    // Lock held on entry and exit: hands the frame to Show unless a seek/speed/content change came in between
    bool deliver(std::unique_lock<std::mutex>& lock, QImage img, int64_t pos, double s)
    {
        const uint64_t before = serial;
        lock.unlock();
        bool shown = false;
        {
            std::lock_guard showLock(showMutex);
            {
                std::lock_guard again(m);
                shown = serial == before && !quit;
            }
            if (shown && show) show(img, pos, s);
        }
        lock.lock();
        if (shown) ++stats.shown;
        else ++stats.skipped;
        return shown && serial == before;
    }

    // Under m: frames due soon that were shown recently go to `frames` without rendering; true if any did
    bool takeRecent()
    {
        if (recent.empty() || !content) return false;
        bool any = false;
        auto take = [&](int64_t p) {
            if (frames.count(p) || inflight.count(p)) return;
            auto it = recent.find(p);
            if (it == recent.end()) return;
            frames[p] = {it->second.image, true};
            it->second.stamp = ++stamp;
            ++stats.recent;
            any = true;
        };
        if (speed == 0) {
            if (pendingStill && next >= 0) take(next);
            return any;
        }
        const int64_t s = step() * direction();
        for (int k = 0; k < ahead(); ++k) {
            const int64_t p = next + k * s;
            if (!inside(p)) break;
            take(p);
        }
        return any;
    }

    // Under m: keeps a shown frame (rendered for generation gen) among the recent ones, the oldest go over budget
    void remember(int64_t pos, QImage image, uint64_t gen)
    {
        if (image.isNull() || gen != generation || recentBudget <= 0) return;
        auto [it, added] = recent.try_emplace(pos);
        if (!added) recentBytes -= it->second.image.sizeInBytes();
        it->second.image = std::move(image);
        it->second.stamp = ++stamp;
        recentBytes += it->second.image.sizeInBytes();
        while (recentBytes > recentBudget && !recent.empty()) {
            auto oldest = recent.begin();
            for (auto i = recent.begin(); i != recent.end(); ++i)
                if (i->second.stamp < oldest->second.stamp) oldest = i;
            recentBytes -= oldest->second.image.sizeInBytes();
            recent.erase(oldest);
        }
    }

    // Under m: the content changed - recent frames and still renders in flight are of before
    void forgetRendered()
    {
        recent.clear();
        recentBytes = 0;
        for (auto& [p, c] : cancels) c->store(true);
    }

    void resetKeepUp()
    {
        jump = {};
        behind = false;
    }
};

Pump::Pump(Clock clock, Show show, int workers) : d(std::make_unique<Impl>())
{
    d->clock = std::move(clock);
    d->show = std::move(show);
    d->recentBudget = recentBudgetFor(SystemMemory::read());
    d->workerCount = workers > 0 ? workers : std::clamp(QThread::idealThreadCount() / 3, 2, 4);
    for (int i = 0; i < d->workerCount; ++i) d->workers.emplace_back([this] { d->workerLoop(); });
    d->shower = std::thread([this] { d->showLoop(); });
}

Pump::~Pump()
{
    {
        std::lock_guard lock(d->m);
        d->quit = true;
    }
    d->wake.notify_all();
    d->ready.notify_all();
    for (std::thread& t : d->workers) t.join();
    d->shower.join();
}

void Pump::setContent(std::shared_ptr<const Content> content, int64_t position, double speed)
{
    std::shared_ptr<const Content> c = content;
    int dir = 0;
    {
        std::lock_guard showLock(d->showMutex);
        std::lock_guard lock(d->m);
        d->content = std::move(content);
        ++d->generation;
        ++d->serial;
        d->forgetRendered();
        d->scrub = false;
        d->frames.clear();
        d->inflight.clear();
        d->speed = speed;
        d->next = std::max<int64_t>(0, position);
        d->pendingStill = speed == 0;
        d->resetKeepUp();
        dir = d->direction();
    }
    if (c && c->direction) c->direction(dir);
    d->wake.notify_all();
    d->ready.notify_all();
}

void Pump::setSpeed(double speed, int64_t position)
{
    std::shared_ptr<const Content> c;
    int dir = 0;
    bool turned = false;
    {
        std::lock_guard showLock(d->showMutex);
        std::lock_guard lock(d->m);
        const bool sameWay = speed != 0 && d->speed != 0 && (speed > 0) == (d->speed > 0);
        turned = d->direction() != (speed > 0 ? 1 : speed < 0 ? -1 : 0);
        d->speed = speed;
        if (!sameWay) {
            d->next = std::max<int64_t>(0, position);
            d->pendingStill = speed == 0;
            d->scrub = false;
            d->resetKeepUp();
            ++d->serial;
        }
        d->prune();
        dir = d->direction();
        c = d->content;
    }
    if (turned && c && c->direction) c->direction(dir);
    d->wake.notify_all();
    d->ready.notify_all();
}

void Pump::seek(int64_t position, bool scrub)
{
    {
        std::lock_guard showLock(d->showMutex);
        std::lock_guard lock(d->m);
        d->next = std::max<int64_t>(0, position);
        d->pendingStill = d->speed == 0;
        d->scrub = scrub && d->speed == 0;
        d->resetKeepUp();
        ++d->serial;
        for (auto& [p, c] : d->cancels) // the newest seek wins: renders for other positions stop early
            if (p != d->next) c->store(true);
        d->prune();
        if (d->takeRecent()) d->ready.notify_all();
    }
    d->wake.notify_all();
    d->ready.notify_all();
}

void Pump::refresh()
{
    {
        std::lock_guard lock(d->m);
        ++d->generation;
        d->forgetRendered();
        d->frames.clear();
        d->inflight.clear();
        if (d->speed == 0) d->pendingStill = true;
    }
    d->wake.notify_all();
    d->ready.notify_all();
}

int Pump::workers() const { return d->workerCount; }

Pump::Stats Pump::stats() const
{
    std::lock_guard lock(d->m);
    Stats s = d->stats;
    s.jumpCost = d->jumpCost();
    s.recentBytes = d->recentBytes;
    return s;
}

void Pump::dropRecent()
{
    std::map<int64_t, Impl::Recent> gone; // freed outside the lock
    {
        std::lock_guard lock(d->m);
        gone.swap(d->recent);
        d->recentBytes = 0;
    }
}

int64_t Pump::recentBudgetFor(const SystemMemory::Info& memory)
{
    // The whole is the machine's RAM or the cgroup limit above the process (container, systemd scope), the smaller.
    // A fixed 192 MB filled within ~3 s of playback and made a 415-MB Raspberry Pi Zero 2 W swap: the sound device and
    // the decoders stalled for seconds, the picture stood still (0.18-0.21)
    qint64 total = memory.total;
    if (memory.limit > 0 && (total <= 0 || memory.limit < total)) total = memory.limit;
    if (total <= 0) return kRecentBudget; // unknown: as on a usual computer
    return std::clamp<int64_t>(total / 16, kMinRecentBudget, kRecentBudget);
}

void Pump::setRecentBudget(int64_t bytes)
{
    std::lock_guard lock(d->m);
    d->recentBudget = bytes;
    if (bytes <= 0) {
        d->recent.clear();
        d->recentBytes = 0;
    }
}

} // namespace Picture
