// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SystemMemory.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QString>

#include <algorithm>

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace SystemMemory {

Info parseMeminfo(const QByteArray& text)
{
    Info info;
    for (const QByteArray& line : text.split('\n')) {
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        const QByteArray key = line.left(colon);
        if (key != "MemTotal" && key != "MemAvailable") continue;
        const QList<QByteArray> parts = line.mid(colon + 1).simplified().split(' ');
        bool ok = false;
        const qint64 kb = parts.value(0).toLongLong(&ok);
        if (!ok || kb <= 0) continue;
        (key == "MemTotal" ? info.total : info.available) = kb * 1024;
    }
    return info;
}

namespace {

QByteArray readSmall(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.read(1 << 16) : QByteArray();
}

// A limit file ("max" = none): bytes, or 0 = none/unknown
qint64 limitOf(const QByteArray& text)
{
    bool ok = false;
    const qint64 v = text.trimmed().toLongLong(&ok);
    return ok && v > 0 ? v : 0;
}

} // namespace

qint64 cgroupRoom(const QByteArray& self, const QString& root)
{
    // cgroup v2 line: "0::/user.slice/.../app.scope" (v1 lines have a controller name between the colons)
    QByteArray group;
    for (const QByteArray& line : self.split('\n'))
        if (line.startsWith("0::")) group = line.mid(3).trimmed();
    if (group.isEmpty() || !group.startsWith('/') || group.contains("/..")) return 0;
    qint64 room = 0;
    QString dir = QDir::cleanPath(root + QString::fromUtf8(group));
    const QString top = QDir::cleanPath(root);
    for (;;) {
        qint64 limit = limitOf(readSmall(dir + "/memory.max"));
        if (const qint64 high = limitOf(readSmall(dir + "/memory.high")); high && (!limit || high < limit)) limit = high;
        if (limit) {
            const qint64 current = limitOf(readSmall(dir + "/memory.current"));
            qint64 cache = 0;
            for (const QByteArray& line : readSmall(dir + "/memory.stat").split('\n'))
                if (line.startsWith("inactive_file ")) cache = limitOf(line.mid(14));
            const qint64 here = std::max<qint64>(1, limit - current + std::min(cache, current));
            room = room ? std::min(room, here) : here;
        }
        if (dir.size() <= top.size()) break;
        const qsizetype slash = dir.lastIndexOf('/');
        if (slash < top.size()) break;
        dir = dir.left(slash);
    }
    return room;
}

Info read()
{
    Info info;
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    QFile f(QStringLiteral("/proc/meminfo"));
    if (f.open(QIODevice::ReadOnly)) info = parseMeminfo(f.readAll());
    if (const qint64 room = cgroupRoom(readSmall(QStringLiteral("/proc/self/cgroup")), QStringLiteral("/sys/fs/cgroup")))
        info.available = info.available ? std::min(info.available, room) : room;
#elif defined(Q_OS_WIN)
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        info.total = qint64(m.ullTotalPhys);
        info.available = qint64(m.ullAvailPhys);
    }
#elif defined(Q_OS_MACOS)
    int64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0 && bytes > 0) info.total = qint64(bytes);
#endif
    bool ok = false;
    const qint64 mb = qEnvironmentVariable("SCHNEIDI_AVAILABLE_MB").toLongLong(&ok);
    if (ok && mb > 0) info.available = mb * 1024 * 1024;
    return info;
}

} // namespace SystemMemory
