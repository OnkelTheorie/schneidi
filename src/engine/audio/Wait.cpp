#include "engine/audio/Wait.h"

#include <QtGlobal>

#include <algorithm>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace Audio {

#ifdef Q_OS_WIN
namespace {

// One timer per thread (Windows 10 1803+); without it Sleep, which is no finer than the system timer
struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    ~Timer()
    {
        if (handle) CloseHandle(handle);
    }
};

} // namespace

void sleepFor(std::chrono::nanoseconds d)
{
    if (d <= std::chrono::nanoseconds::zero()) return;
    thread_local Timer timer;
    if (!timer.handle) {
        Sleep(DWORD(std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(d).count())));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, d.count() / 100); // relative, in 100 ns
    if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer.handle, INFINITE);
}

void sleepUntil(std::chrono::steady_clock::time_point due) { sleepFor(due - std::chrono::steady_clock::now()); }

void waitFor(std::condition_variable&, std::unique_lock<std::mutex>& lock, std::chrono::nanoseconds d)
{
    lock.unlock();
    sleepFor(std::min<std::chrono::nanoseconds>(d, std::chrono::milliseconds(1)));
    lock.lock();
}
#else
void sleepFor(std::chrono::nanoseconds d)
{
    if (d > std::chrono::nanoseconds::zero()) std::this_thread::sleep_for(d);
}

void sleepUntil(std::chrono::steady_clock::time_point due) { std::this_thread::sleep_until(due); }

void waitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lock, std::chrono::nanoseconds d)
{
    cv.wait_for(lock, d);
}
#endif

} // namespace Audio
