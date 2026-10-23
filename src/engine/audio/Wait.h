#pragma once
// Own audio engine (docs/plan-0.10-ton.md): waits that keep their time. With MSYS2's thread library (winpthreads)
// std::condition_variable::wait_for and std::this_thread::sleep_for/sleep_until wake in Windows' ~15.6 ms grid, even
// after timeBeginPeriod(1), and sleep_for below 1 ms returns at once (measured, docs/dev/ton.md). The picture follows
// the sound clock to the millisecond and the timer device paces blocks of 10 ms, so they wait through these helpers:
// on Windows a high-resolution waitable timer (~0.5 ms), elsewhere the standard waits, which are exact there.

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace Audio {

// Sleeps `d` (nothing for d <= 0)
void sleepFor(std::chrono::nanoseconds d);

// Sleeps until `due`
void sleepUntil(std::chrono::steady_clock::time_point due);

// cv.wait_for(lock, d) that keeps its time. May return early (a notify, or on Windows after at most 1 ms with the lock
// released meanwhile, so a notify is seen within 1 ms): callers check their condition in a loop.
void waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, std::chrono::nanoseconds d);

} // namespace Audio
