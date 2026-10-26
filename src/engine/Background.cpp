#include "engine/Background.h"

#include <QProcess>
#include <QSettings>
#include <QThread>

#include <algorithm>

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

void lowerProcess(QProcess& process, Level level)
{
#if defined(Q_OS_WIN)
    const DWORD flag = level == Level::Low ? BELOW_NORMAL_PRIORITY_CLASS : IDLE_PRIORITY_CLASS;
    process.setCreateProcessArgumentsModifier([flag](QProcess::CreateProcessArguments* a) { a->flags |= flag; });
#elif defined(Q_OS_UNIX)
    const int nice = level == Level::Low ? 10 : 19;
    process.setChildProcessModifier([nice] { (void)::nice(nice); });
#else
    Q_UNUSED(process);
    Q_UNUSED(level);
#endif
}

int chosenThreads() { return std::max(0, QSettings().value("performance/threads", 0).toInt()); }

void setChosenThreads(int threads) { QSettings().setValue("performance/threads", std::max(0, threads)); }

int threads(int automatic)
{
    const int chosen = chosenThreads();
    return chosen > 0 ? std::min(chosen, std::max(1, QThread::idealThreadCount())) : std::max(1, automatic);
}

} // namespace Background
