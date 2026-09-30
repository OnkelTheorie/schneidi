#include "app/Log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QSysInfo>

#include <framework/mlt.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
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
    s_file.write(line);
    s_file.flush(); // bei einem Absturz soll alles bis dahin drinstehen
}

void qtHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
    static const char* names[] = {"DEBUG", "WARN ", "CRIT ", "FATAL", "INFO "};
    write(names[std::clamp(int(type), 0, 4)], msg);
    if (s_previous) s_previous(type, ctx, msg);
}

void mltHandler(void* service, int level, const char* fmt, va_list args)
{
    if (level > mlt_log_get_level()) return;
    char buf[2048];
    va_list copy;
    va_copy(copy, args);
    std::vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);
    // Meist UTF-8, unter Windows Pfade teils in der ANSI-Codepage
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    QString text = utf8(QByteArrayView(buf));
    if (utf8.hasError()) text = QString::fromLocal8Bit(buf);
    if (service) {
        const char* id = mlt_properties_get(MLT_SERVICE_PROPERTIES(static_cast<mlt_service>(service)), "mlt_service");
        if (id) text = QString("[%1] %2").arg(QString::fromUtf8(id), text);
    }
    write(level <= MLT_LOG_ERROR ? "MLT-E" : "MLT-W", text);
    std::fputs(buf, stderr);
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
    if (!s_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
    write("INFO ", QString("schneidi %1 gestartet (%2, Qt %3)")
                       .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(), qVersion()));
    s_previous = qInstallMessageHandler(qtHandler);
}

void installMlt()
{
    mlt_log_set_callback(mltHandler);
}

} // namespace Log
