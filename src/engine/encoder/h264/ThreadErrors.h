// Exceptions on the encoder's own threads (worker pool, wavefront rows, entropy writer, lookahead
// and first-pass helpers): caught where they happen, the first one kept and rethrown on the
// caller's thread at its next call, so a failure (e.g. bad_alloc on a 4K picture) ends the encode
// instead of the program (an uncaught exception on a std::thread calls std::terminate).
#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace schneidi::h264 {

class ThreadErrors {
public:
    // Inside a catch block: keeps the exception being handled (the first one wins).
    void capture() noexcept
    {
        std::lock_guard<std::mutex> l(m_);
        if (!first_)
            first_ = std::current_exception();
        failed_.store(true, std::memory_order_release);
    }
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    void rethrow()
    {
        std::exception_ptr e;
        {
            std::lock_guard<std::mutex> l(m_);
            e = first_;
        }
        if (e)
            std::rethrow_exception(e);
    }
    void clear()
    {
        std::lock_guard<std::mutex> l(m_);
        first_ = nullptr;
        failed_.store(false, std::memory_order_release);
    }

private:
    std::mutex m_;
    std::exception_ptr first_;
    std::atomic<bool> failed_{false};
};

// Runs fn on `threads` threads (the calling one included), joins them all, then rethrows the
// first exception any of them threw. fn must not wait for work another thread may abandon.
template <class F>
void runOnThreads(int threads, F&& fn)
{
    ThreadErrors errors;
    auto run = [&] {
        try {
            fn();
        } catch (...) {
            errors.capture();
        }
    };
    std::vector<std::thread> workers;
    try {
        for (int t = 1; t < threads; ++t)
            workers.emplace_back(run);
    } catch (...) { // no thread: the ones there (and this one) do the work
        errors.capture();
    }
    run();
    for (auto& t : workers)
        t.join();
    errors.rethrow();
}

// Fault injection for tests: the n-th pass through a point throws std::runtime_error there.
enum class FailPoint { FirstPassWorker, LookaheadThread, WaveRow, Writer, Count };
namespace detail {
inline std::atomic<int> gFailCountdown[int(FailPoint::Count)] = {};
}
// after = 1: the first pass through the point throws; 0 switches it off.
inline void injectFailure(FailPoint p, int after) { detail::gFailCountdown[int(p)].store(after, std::memory_order_relaxed); }
inline void failurePoint(FailPoint p)
{
    std::atomic<int>& c = detail::gFailCountdown[int(p)];
    if (c.load(std::memory_order_relaxed) > 0 && c.fetch_sub(1, std::memory_order_relaxed) == 1)
        throw std::runtime_error("h264: injected failure");
}

} // namespace schneidi::h264
