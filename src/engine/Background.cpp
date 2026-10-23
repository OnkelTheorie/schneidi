#include "engine/Background.h"

#include <QProcess>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#if defined(Q_OS_LINUX)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace Background {

namespace {
std::atomic<int> g_foreground{0};
std::mutex g_mutex;
std::condition_variable g_done;
} // namespace

Foreground::Foreground() { ++g_foreground; }

Foreground::~Foreground()
{
    if (--g_foreground == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done.notify_all();
    }
}

bool foregroundBusy() { return g_foreground.load() > 0; }

void yieldToForeground()
{
    if (!foregroundBusy()) return;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_done.wait_for(lock, std::chrono::seconds(20), [] { return g_foreground.load() == 0; });
}

void lowerThisThread(Level level)
{
#if defined(Q_OS_LINUX)
    // On Linux every thread has its own nice value (PRIO_PROCESS with the thread id)
    const int nice = level == Level::Low ? 10 : 19;
    (void)setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice);
#else
    if (QThread* t = QThread::currentThread())
        t->setPriority(level == Level::Low ? QThread::LowPriority : QThread::LowestPriority);
#endif
}

void lowerProcess(QProcess& process)
{
#if defined(Q_OS_WIN)
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* a) { a->flags |= BELOW_NORMAL_PRIORITY_CLASS; });
#elif defined(Q_OS_UNIX)
    process.setChildProcessModifier([] { (void)::nice(10); });
#else
    Q_UNUSED(process);
#endif
}

} // namespace Background
