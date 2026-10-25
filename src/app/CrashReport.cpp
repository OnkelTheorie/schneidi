#include "app/CrashReport.h"

#include "app/Log.h"
#include "core/I18n.h"
#include "ui/FileManager.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#define SCHNEIDI_HAVE_BACKTRACE 1
#endif
#endif

namespace CrashReport {
namespace {

constexpr const char* kSeenKey = "crash/lastSeen";
constexpr int kKeep = 10;       // older reports are deleted
constexpr int kLogLines = 300;  // tail of the crashed run's log
constexpr const char* kLogMark = "\n--- log of the crashed run (schneidi.log) ---\n";

// Prepared at install(): the handler may neither allocate nor format (async-signal-safe)
QByteArray s_path;   // native encoding (POSIX) resp. UTF-16 via s_wpath (Windows)
QByteArray s_header; // version, system, start time
#ifdef Q_OS_WIN
std::wstring s_wpath;
#endif
std::atomic<bool> s_handling{false};
std::terminate_handler s_previousTerminate = nullptr;

#ifdef Q_OS_WIN
using Handle = HANDLE;
Handle openReport()
{
    return CreateFileW(s_wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
void put(Handle h, const char* s, size_t n)
{
    DWORD written = 0;
    if (h != INVALID_HANDLE_VALUE) WriteFile(h, s, DWORD(n), &written, nullptr);
}
void closeReport(Handle h)
{
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
#else
using Handle = int;
Handle openReport()
{
    return ::open(s_path.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
void put(Handle fd, const char* s, size_t n)
{
    while (fd >= 0 && n > 0) {
        const ssize_t w = ::write(fd, s, n);
        if (w <= 0) return;
        s += w;
        n -= size_t(w);
    }
}
void closeReport(Handle fd)
{
    if (fd >= 0) ::close(fd);
}
#endif

void put(Handle h, const char* s)
{
    put(h, s, std::strlen(s));
}

// Number as hex without printf (async-signal-safe)
[[maybe_unused]] void putHex(Handle h, unsigned long long v)
{
    char buf[19] = "0x";
    char digits[16];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v && n < 16);
    for (int i = 0; i < n; ++i) buf[2 + i] = digits[n - 1 - i];
    put(h, buf, size_t(2 + n));
}

const char* signalName(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access)";
    case SIGABRT: return "SIGABRT (abort)";
    case SIGFPE: return "SIGFPE (arithmetic error)";
    case SIGILL: return "SIGILL (illegal instruction)";
#ifdef SIGBUS
    case SIGBUS: return "SIGBUS (bus error)";
#endif
    default: return "unknown signal";
    }
}

// Writes header + reason (+ stack); returns the open handle for more lines, the caller closes it
Handle begin(const char* reason)
{
    const Handle h = openReport();
    put(h, s_header.constData(), size_t(s_header.size()));
    put(h, "Reason: ");
    put(h, reason);
    put(h, "\n");
    return h;
}

#ifdef Q_OS_WIN
// One line per address: the address, then module+offset (exe+offset for schneidi.exe; a release build has no debug
// info: nearest symbol in `nm -C schneidi.exe` below 0x140000000 + offset). Addresses in no module stay bare.
void putFrame(Handle h, unsigned long long pc)
{
    putHex(h, pc);
    HMODULE module = nullptr;
    if (pc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(pc), &module)) {
        put(h, "  ");
        wchar_t path[MAX_PATH];
        const DWORD n = module == GetModuleHandleW(nullptr) ? 0 : GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < n; ++i)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        char utf8[MAX_PATH * 3];
        const int len = n ? WideCharToMultiByte(CP_UTF8, 0, name, int(path + n - name), utf8, sizeof utf8, nullptr, nullptr) : 0;
        if (len > 0) put(h, utf8, size_t(len));
        else put(h, "exe");
        put(h, "+");
        putHex(h, pc - reinterpret_cast<unsigned long long>(module));
    }
    put(h, "\n");
}
#endif

// context (Windows exceptions): the faulting thread's registers – the stack then starts at the crash site instead of
// in this handler, and goes past a frame without unwind data (CaptureStackBackTrace stops there)
void putStack(Handle h, [[maybe_unused]] const void* context = nullptr)
{
#ifdef SCHNEIDI_HAVE_BACKTRACE
    put(h, "\nCall stack:\n");
    void* frames[64];
    backtrace_symbols_fd(frames, backtrace(frames, 64), h);
#elif defined(Q_OS_WIN)
    put(h, "\nCall stack:\n");
#if defined(_M_X64) || defined(__x86_64__)
    if (context) {
        CONTEXT c = *static_cast<const CONTEXT*>(context);
        for (int i = 0; i < 62 && (c.Rip || i == 0); ++i) { // the first may be 0 (a call through a null pointer)
            putFrame(h, c.Rip);
            DWORD64 imageBase = 0;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr)) {
                void* handlerData = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &frame, nullptr);
            } else { // leaf function or a jump to nowhere: the return address is on top of the stack
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
        }
        return;
    }
#endif
    void* frames[62];
    const USHORT n = CaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) putFrame(h, reinterpret_cast<unsigned long long>(frames[i]));
#else
    put(h, "\n(no call stack on this platform)\n");
#endif
}

void onSignal(int sig)
{
    if (!s_handling.exchange(true)) {
        const Handle h = begin(signalName(sig));
        putStack(h);
        closeReport(h);
    }
    // Default action (core dump, exit code) as without the handler
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void onTerminate()
{
    if (!s_handling.exchange(true)) {
        // Not in a signal: here the message of the exception may be fetched normally
        QByteArray reason = "uncaught C++ exception";
        if (const std::exception_ptr e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                reason += QByteArray(": ") + ex.what();
            } catch (...) {
            }
        }
        const Handle h = begin(reason.constData());
        putStack(h);
        closeReport(h);
    }
    if (s_previousTerminate) s_previousTerminate();
    std::abort();
}

#ifdef Q_OS_WIN
LONG WINAPI onException(EXCEPTION_POINTERS* info)
{
    if (!s_handling.exchange(true)) {
        const Handle h = begin("unhandled Windows exception");
        const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
        if (r) {
            put(h, "Code: ");
            putHex(h, r->ExceptionCode);
            put(h, "\nAddress: ");
            putFrame(h, reinterpret_cast<unsigned long long>(r->ExceptionAddress));
        }
        putStack(h, info ? info->ContextRecord : nullptr);
        closeReport(h);
    }
    return EXCEPTION_CONTINUE_SEARCH; // Windows error dialog / WER as usual
}
#endif

QString logTail()
{
    QFile f(QDir(Log::directory()).filePath("schneidi.1.log"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    if (lines.size() > kLogLines) lines = lines.mid(lines.size() - kLogLines);
    return lines.join('\n');
}

// Reports of earlier runs that have no log yet get the log of the previous run (the crashed one)
void attachPreviousLog()
{
    const QDir dir(directory());
    const QStringList files = dir.entryList({"crash-*.txt"}, QDir::Files, QDir::Name);
    if (files.isEmpty()) return;
    QFile f(dir.filePath(files.last()));
    if (!f.open(QIODevice::ReadWrite | QIODevice::Text)) return;
    if (f.readAll().contains(QByteArray(kLogMark).trimmed())) return;
    const QString tail = logTail();
    f.write(kLogMark);
    f.write(tail.isEmpty() ? QByteArray("(no log)\n") : tail.toUtf8());
}

void removeOld()
{
    const QDir dir(directory());
    QStringList files = dir.entryList({"crash-*.txt"}, QDir::Files, QDir::Name);
    while (files.size() > kKeep) QFile::remove(dir.filePath(files.takeFirst()));
}

} // namespace

QString directory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("crashes");
}

void install()
{
    QDir().mkpath(directory());
    attachPreviousLog();
    removeOld();

    const QDateTime now = QDateTime::currentDateTime();
    // Name by start time: sorts chronologically; reports are compared by name (kSeenKey)
    const QString path = QDir(directory()).filePath(QString("crash-%1.txt").arg(now.toString("yyyyMMdd-HHmmss")));
    s_path = QFile::encodeName(path);
#ifdef Q_OS_WIN
    s_wpath = QDir::toNativeSeparators(path).toStdWString();
#endif
    s_header = QString("schneidi crash report\n"
                       "Version: %1\nSystem: %2 (%3, %4)\nQt: %5\nStarted: %6\n")
                   .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(),
                        QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture(), qVersion(),
                        now.toString(Qt::ISODate))
                   .toUtf8();

#ifdef SCHNEIDI_HAVE_BACKTRACE
    void* warm[1];
    backtrace(warm, 1); // loads libgcc now: the first call may allocate, not allowed in the handler
#endif
#ifndef Q_OS_WIN
    // A stack overflow (endless recursion) leaves no stack for the handler: it gets its own (main thread, where the
    // UI recurses; other threads keep the plain handler)
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof altStack;
    const bool onOwnStack = sigaltstack(&ss, nullptr) == 0;
#endif
    for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL
#ifdef SIGBUS
                          , SIGBUS
#endif
         }) {
#ifdef Q_OS_WIN
        std::signal(sig, onSignal);
#else
        struct sigaction sa{};
        sa.sa_handler = onSignal;
        sa.sa_flags = onOwnStack ? SA_ONSTACK : 0;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
#endif
    }
    s_previousTerminate = std::set_terminate(onTerminate);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(onException);
#endif
}

QStringList pending()
{
    const QDir dir(directory());
    const QString seen = QSettings().value(kSeenKey).toString();
    const QString own = QFileInfo(QFile::decodeName(s_path)).fileName(); // this run's (not crashed yet)
    QStringList out;
    for (const QString& f : dir.entryList({"crash-*.txt"}, QDir::Files, QDir::Name))
        if (f > seen && f != own) out << dir.filePath(f);
    return out;
}

void offer(QWidget* parent)
{
    const QStringList files = pending();
    if (files.isEmpty()) return;
    const QString file = files.last();
    QSettings().setValue(kSeenKey, QFileInfo(file).fileName()); // ask only once
    QFile f(file);
    const QString text = f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();

    QMessageBox box(QMessageBox::Warning, "schneidi", T("schneidi ist beim letzten Mal abgestürzt."), QMessageBox::NoButton,
                    parent);
    box.setInformativeText(T("Ein Bericht wurde lokal gespeichert, nichts wurde gesendet. Wenn du helfen möchtest, "
                             "hänge ihn an eine Fehlermeldung an (der Bericht enthält Version, System, Aufrufliste "
                             "und das Protokoll mit Dateipfaden)."));
    box.setDetailedText(text);
    QPushButton* folder = box.addButton(T("Im Ordner zeigen"), QMessageBox::ActionRole);
    QPushButton* copy = box.addButton(T("Text kopieren"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    // Action buttons should not close the dialog
    folder->disconnect();
    copy->disconnect();
    QObject::connect(folder, &QPushButton::clicked, &box, [file] { FileManager::reveal(file); });
    QObject::connect(copy, &QPushButton::clicked, &box, [text] { QApplication::clipboard()->setText(text); });
    box.exec();
}

} // namespace CrashReport
