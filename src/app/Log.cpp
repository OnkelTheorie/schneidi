// SPDX-License-Identifier: AGPL-3.0-only
#include "app/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

namespace Log {
namespace {

constexpr int kKeep = 3; // schneidi.log, schneidi.1.log, schneidi.2.log
constexpr int kKeptMax = 10; // logs/kept/
// The last line of a run that ended normally (exit() or a return from main; not a crash, a kill, std::_Exit)
constexpr const char* kEnded = "schneidi ended normally";
constexpr const char* kEndedAfterHang = "schneidi ended normally after a hang";
constexpr const char* kHangLevel = "HANG";

std::mutex s_mutex;
QFile s_file;
QtMessageHandler s_previous = nullptr;
std::atomic<bool> s_hung{false};
// Held while this process owns schneidi.log: a second instance (--new-instance, a test run) neither rotates the log
// of the one that runs nor judges it (its end line is not written yet), it appends to it
QLockFile* s_owner = nullptr;

void write(const char* level, const QString& text)
{
    if (qstrncmp(level, kHangLevel, 4) == 0) s_hung = true;
    const QByteArray line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz ").toUtf8() + level + ' '
                            + text.trimmed().toUtf8() + '\n';
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_file.isOpen()) return;
    // Another instance may write to the same file (Windows: it could not be rotated): append at its real end, not at
    // the offset this process last wrote to, so the two do not overwrite each other's lines
    s_file.seek(s_file.size());
    s_file.write(line);
    s_file.flush(); // in a crash everything up to then should be in there
}

void qtHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    static const char* names[] = {"DEBUG", "WARN ", "CRIT ", "FATAL", "INFO "};
    write(names[std::clamp(int(type), 0, 4)], msg);
    if (s_previous) s_previous(type, ctx, msg);
}

// The previous run's log (before the rotation): copied to logs/kept/ when it did not end normally or hung. Only its
// end is read (the end line says both), at every start.
void keepIfTroubled(const QDir& dir, const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const qint64 size = f.size();
    if (size == 0) return;
    f.seek(std::max<qint64>(0, size - 512));
    const QByteArray tail = f.readAll();
    f.seek(0);
    const QByteArray first = f.readLine(64); // "yyyy-MM-dd HH:mm:ss.zzz ..." of its start
    f.close();
    const bool hung = tail.contains(kEndedAfterHang);
    if (tail.contains(kEnded) && !hung) return;
    const QDir kept(dir.filePath("kept"));
    QDir().mkpath(kept.path());
    QString stamp = QString::fromUtf8(first.left(19)).replace(' ', '_').replace(':', '-');
    if (stamp.size() != 19) stamp = QFileInfo(path).lastModified().toString("yyyy-MM-dd_HH-mm-ss");
    QFile::copy(path, kept.filePath(QString("schneidi-%1-%2.log").arg(stamp, hung ? "hang" : "unclean")));
    QStringList files = kept.entryList({"schneidi-*.log"}, QDir::Files, QDir::Name);
    while (files.size() > kKeptMax) QFile::remove(kept.filePath(files.takeFirst()));
}

void writeEnd()
{
    if (s_owner) write("INFO ", s_hung ? kEndedAfterHang : kEnded); // the owner's end only
}

} // namespace

QString directory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("logs");
}

void install()
{
    const QDir dir(directory());
    QDir().mkpath(dir.path());
    const auto name = [&](int i) { return dir.filePath(i == 0 ? "schneidi.log" : QString("schneidi.%1.log").arg(i)); };
    if (!s_owner) {
        auto* lock = new QLockFile(dir.filePath("schneidi.log.lock")); // left to the process end on purpose
        // Never stale by age (a run of hours keeps it); a lock whose process is gone (a crash) is taken over
        lock->setStaleLockTime(0);
        if (lock->tryLock(0)) s_owner = lock;
        else delete lock;
    }
    if (s_owner) {
        keepIfTroubled(dir, name(0));
        QFile::remove(name(kKeep - 1));
        for (int i = kKeep - 2; i >= 0; --i) QFile::rename(name(i), name(i + 1));
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_file.isOpen()) s_file.close(); // installed again (tests)
    }
    s_file.setFileName(name(0));
    // Still there: another instance holds it open (Windows refuses the rename), so append instead of wiping its log
    const QIODevice::OpenMode mode = QFile::exists(name(0)) ? QIODevice::Append : QIODevice::Truncate;
    if (!s_file.open(QIODevice::WriteOnly | mode | QIODevice::Text)) return;
    write("INFO ", QString("schneidi %1 gestartet (%2, Qt %3)")
                       .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(), qVersion()));
    s_previous = qInstallMessageHandler(qtHandler);
    // Registered after s_file/s_mutex exist: runs before their destructors
    static bool once = false;
    if (!once) std::atexit(writeEnd);
    once = true;
}

void message(const char* level, const QString& text)
{
    write(level, text);
}

} // namespace Log
