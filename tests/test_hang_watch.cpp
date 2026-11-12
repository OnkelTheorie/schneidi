// SPDX-License-Identifier: AGPL-3.0-only
// Hang watch (app/HangWatch.h): a GUI thread blocked longer than the threshold is reported once with where it is
// (the call stack on Linux/glibc and Windows x64) and once more when it answers again; an idle event loop is no hang.
// Log keeping (app/Log.h): the previous run's log is copied to logs/kept/ when it did not end normally or hung, and
// not when it ended normally; at most 10 are kept.

#include "check.h"

#include "app/HangWatch.h"
#include "app/Log.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QThread>

namespace {

QMutex g_mutex;
QStringList g_reports;

QStringList reports()
{
    QMutexLocker lock(&g_mutex);
    return g_reports;
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (!t.hasExpired(ms)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

// A function of its own so the stack has something of this program to show
Q_NEVER_INLINE void blockGuiThread(int ms)
{
    QThread::msleep(ms);
}

void hangIsReported()
{
    HangWatch::install(1200, [](const QString& text) {
        QMutexLocker lock(&g_mutex);
        g_reports << text;
    });
    spin(1500); // armed (first heartbeat), then idle: nothing
    CHECK(reports().isEmpty());
    blockGuiThread(2500);
    spin(1200); // the heartbeat comes again
    const QStringList r = reports();
    if (!CHECK(r.size() == 2)) {
        for (const QString& s : r) std::printf("       report: %s\n", qPrintable(s));
        HangWatch::uninstall();
        return;
    }
    CHECK(r[0].startsWith("GUI thread not responding for"));
    CHECK(r[1].startsWith("GUI thread responsive again after"));
#if (defined(Q_OS_LINUX) && defined(__GLIBC__)) || (defined(Q_OS_WIN) && (defined(_M_X64) || defined(__x86_64__)))
    // a stack with several frames, and the GUI thread's own (its sleep), not the watcher's
    CHECK(r[0].contains("GUI thread call stack:"));
    CHECK(r[0].count('\n') >= 4);
    CHECK(r[0].contains("sleep"));
#endif
    HangWatch::uninstall();
}

void writeLog(const QString& path, const QByteArray& lastLine)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write("2026-11-12 15:35:57.397 INFO  schneidi 0.21.0 gestartet\n");
    f.write("2026-11-12 15:36:10.000 INFO  something\n");
    f.write(lastLine);
}

int keptCount()
{
    return int(QDir(QDir(Log::directory()).filePath("kept")).entryList({"schneidi-*.log"}, QDir::Files).size());
}

void troubledLogsAreKept()
{
    QDir(Log::directory()).removeRecursively();
    QDir().mkpath(Log::directory());
    const QString current = QDir(Log::directory()).filePath("schneidi.log");

    writeLog(current, "2026-11-12 15:40:00.000 INFO  schneidi ended normally\n");
    Log::install();
    CHECK(keptCount() == 0);

    writeLog(current, "2026-11-12 15:40:00.000 INFO  still running when it went\n"); // crashed / killed
    Log::install();
    const QDir kept(QDir(Log::directory()).filePath("kept"));
    CHECK(keptCount() == 1);
    CHECK(kept.exists("schneidi-2026-11-12_15-35-57-unclean.log"));

    writeLog(current, "2026-11-12 15:40:00.000 INFO  schneidi ended normally after a hang\n");
    QFile::remove(kept.filePath("schneidi-2026-11-12_15-35-57-unclean.log"));
    Log::install();
    CHECK(kept.exists("schneidi-2026-11-12_15-35-57-hang.log"));

    // at most 10
    for (int i = 0; i < 12; ++i) {
        QFile f(kept.filePath(QString("schneidi-2026-01-%1_00-00-00-unclean.log").arg(i + 10)));
        f.open(QIODevice::WriteOnly);
    }
    writeLog(current, "no end\n");
    Log::install();
    CHECK(keptCount() == 10);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("hang-watch");
    hangIsReported();
    troubledLogsAreKept();
    return Check::result();
}
