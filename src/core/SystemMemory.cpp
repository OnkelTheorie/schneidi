// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SystemMemory.h"

#include <QByteArray>
#include <QFile>
#include <QList>

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

Info read()
{
    Info info;
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    QFile f(QStringLiteral("/proc/meminfo"));
    if (f.open(QIODevice::ReadOnly)) info = parseMeminfo(f.readAll());
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
