// SPDX-License-Identifier: AGPL-3.0-only
#include "app/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QSysInfo>

#include <algorithm>
#include <mutex>

namespace Log {
namespace {

constexpr int kKeep = 3; // schneidi.log, schneidi.1.log, schneidi.2.log

std::mutex s_mutex;
QFile s_file;
QtMessageHandler s_previous = nullptr;

void write(const char* level, const QString& text)
{
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
    QFile::remove(name(kKeep - 1));
    for (int i = kKeep - 2; i >= 0; --i) QFile::rename(name(i), name(i + 1));
    s_file.setFileName(name(0));
    // Still there: another instance holds it open (Windows refuses the rename), so append instead of wiping its log
    const QIODevice::OpenMode mode = QFile::exists(name(0)) ? QIODevice::Append : QIODevice::Truncate;
    if (!s_file.open(QIODevice::WriteOnly | mode | QIODevice::Text)) return;
    write("INFO ", QString("schneidi %1 gestartet (%2, Qt %3)")
                       .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(), qVersion()));
    s_previous = qInstallMessageHandler(qtHandler);
}

void message(const char* level, const QString& text)
{
    write(level, text);
}

} // namespace Log
