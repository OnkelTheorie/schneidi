// SPDX-License-Identifier: AGPL-3.0-only
#include "app/HangWatch.h"

#include "app/Log.h"

#include <QCoreApplication>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#if defined(_M_X64) || defined(__x86_64__)
#define SCHNEIDI_HANG_STACK_WIN 1
#endif
#elif defined(__GLIBC__)
#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <pthread.h>
#define SCHNEIDI_HANG_STACK_GLIBC 1
#endif

namespace HangWatch {
namespace {

constexpr int kBeatMs = 500;          // the GUI thread's heartbeat
constexpr int kLookMs = 250;          // how often the watcher looks
constexpr qint64 kSecondStackMs = 15000;
constexpr qint64 kSleptMs = 2000;     // the watcher itself did not run this long: the machine slept, no hang
constexpr int kMaxFrames = 48;

std::atomic<qint64> s_beat{-1}; // ms on the steady clock of the GUI thread's last heartbeat, -1 = not armed yet
std::atomic<bool> s_stop{false};
std::thread s_thread;
std::mutex s_mutex;
std::condition_variable s_wake;
QTimer* s_timer = nullptr;

qint64 nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef SCHNEIDI_HANG_STACK_GLIBC
pthread_t s_gui;
int s_signal = 0; // 0: no stacks (the signal is someone else's)
void* s_frames[kMaxFrames];
std::atomic<int> s_frameCount{-1};

void onStackSignal(int)
{
    // backtrace() was called once at install (loads libgcc: the first call may allocate)
    s_frameCount.store(backtrace(s_frames, kMaxFrames), std::memory_order_release);
}

void setUpStacks()
{
    s_gui = pthread_self();
    const int sig = SIGRTMIN + 5;
    struct sigaction old{};
    if (sigaction(sig, nullptr, &old) != 0 || old.sa_handler != SIG_DFL) return; // in use: leave it alone
    struct sigaction sa{};
    sa.sa_handler = onStackSignal;
    sa.sa_flags = SA_RESTART; // the GUI thread's blocking read/write goes on instead of failing with EINTR
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, nullptr) != 0) return;
    void* warm[1];
    backtrace(warm, 1);
    s_signal = sig;
}

QStringList guiStack()
{
    if (!s_signal) return {};
    s_frameCount.store(-1);
    if (pthread_kill(s_gui, s_signal) != 0) return {};
    for (int i = 0; i < 50 && s_frameCount.load(std::memory_order_acquire) < 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const int n = s_frameCount.load(std::memory_order_acquire);
    if (n <= 2) return {};
    QStringList out;
    if (char** names = backtrace_symbols(s_frames, n)) {
        for (int i = 2; i < n; ++i) out << QString::fromLocal8Bit(names[i]); // 0: the handler, 1: the signal frame
        std::free(names);
    }
    return out;
}
#elif defined(SCHNEIDI_HANG_STACK_WIN)
HANDLE s_gui = nullptr;
ULONG_PTR s_stackLow = 0, s_stackHigh = 0;

void setUpStacks()
{
    if (s_gui) CloseHandle(s_gui);
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_gui,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        s_gui = nullptr;
    GetCurrentThreadStackLimits(&s_stackLow, &s_stackHigh);
}

// module+offset, like the crash report ("exe" for schneidi.exe)
QString frameText(DWORD64 pc)
{
    QString text = QStringLiteral("0x%1").arg(pc, 0, 16);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(pc), &module)) {
        QString name = QStringLiteral("exe");
        if (module != GetModuleHandleW(nullptr)) {
            wchar_t path[MAX_PATH];
            const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
            name = QString::fromWCharArray(path, int(n)).section(QLatin1Char('\\'), -1);
        }
        text += QStringLiteral("  %1+0x%2").arg(name).arg(pc - reinterpret_cast<DWORD64>(module), 0, 16);
    }
    return text;
}

QStringList guiStack()
{
    if (!s_gui || SuspendThread(s_gui) == DWORD(-1)) return {};
    // While it is suspended: no allocation, no lock (it may hold the heap's) - addresses into a fixed array only
    DWORD64 pcs[kMaxFrames];
    int n = 0;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(s_gui, &c)) {
        while (n < kMaxFrames && c.Rip && c.Rsp >= s_stackLow && c.Rsp < s_stackHigh) {
            pcs[n++] = c.Rip;
            DWORD64 imageBase = 0;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr)) {
                void* handlerData = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &frame, nullptr);
            } else { // leaf function: the return address is on top of the stack
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
        }
    }
    ResumeThread(s_gui);
    QStringList out;
    for (int i = 0; i < n; ++i) out << frameText(pcs[i]);
    return out;
}
#else
void setUpStacks() {}
QStringList guiStack() { return {}; }
#endif

QString stackText()
{
    const QStringList frames = guiStack();
    return frames.isEmpty() ? QStringLiteral("(no call stack on this platform)")
                            : QStringLiteral("GUI thread call stack:\n  ") + frames.join(QStringLiteral("\n  "));
}

QString seconds(qint64 ms)
{
    return QString::number(double(ms) / 1000.0, 'f', 1);
}

void watch(qint64 threshold, std::function<void(const QString&)> report)
{
    qint64 lastLook = nowMs();
    qint64 quietUntil = 0;
    bool hanging = false, secondStack = false;
    qint64 hangBeat = 0;
    std::unique_lock<std::mutex> lock(s_mutex);
    while (!s_wake.wait_for(lock, std::chrono::milliseconds(kLookMs), [] { return s_stop.load(); })) {
        const qint64 now = nowMs();
        // This thread did not run either (the machine slept, everything starved): the GUI thread gets a fresh start
        if (now - lastLook > kSleptMs) quietUntil = now + threshold;
        lastLook = now;
        const qint64 beat = s_beat.load();
        if (beat < 0) continue;
        if (hanging) {
            if (beat != hangBeat) {
                report(QStringLiteral("GUI thread responsive again after %1 s").arg(seconds(beat - hangBeat)));
                hanging = false;
            } else if (!secondStack && now - hangBeat > kSecondStackMs) {
                secondStack = true;
                report(QStringLiteral("GUI thread still not responding after %1 s\n%2")
                           .arg(seconds(now - hangBeat), stackText()));
            }
            continue;
        }
        if (now < quietUntil || now - beat <= threshold) continue;
        hanging = true;
        secondStack = false;
        hangBeat = beat;
        report(QStringLiteral("GUI thread not responding for %1 s\n%2").arg(seconds(now - beat), stackText()));
    }
}

} // namespace

void install(int thresholdMs, std::function<void(const QString&)> report)
{
    uninstall();
    if (!report) report = [](const QString& text) { Log::message("HANG ", text); };
    setUpStacks();
    s_beat = -1;
    s_stop = false;
    s_timer = new QTimer(QCoreApplication::instance());
    s_timer->setInterval(kBeatMs);
    QObject::connect(s_timer, &QTimer::timeout, [] { s_beat = nowMs(); });
    s_timer->start();
    s_thread = std::thread(watch, qint64(thresholdMs), std::move(report));
    static bool once = false;
    if (!once) qAddPostRoutine(uninstall); // before the QCoreApplication is gone (the report may use Qt)
    once = true;
}

void uninstall()
{
    if (!s_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stop = true;
    }
    s_wake.notify_all();
    s_thread.join();
    delete s_timer;
    s_timer = nullptr;
}

} // namespace HangWatch
