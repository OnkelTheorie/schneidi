// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SystemMemory.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QList>
#include <QStringList>
#include <QString>

#include <algorithm>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX // std::min/std::max
#endif
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

// A limit file ("max" = none): bytes, or 0 = none/unknown. cgroup v1 writes "no limit" as a huge number
// (PAGE_COUNTER_MAX pages, 0x7ffffffffffff000 on 4-KB pages): anything from 2^62 bytes on counts as none
qint64 limitOf(const QByteArray& text)
{
    bool ok = false;
    const qint64 v = text.trimmed().toLongLong(&ok);
    return ok && v > 0 && v < (qint64(1) << 62) ? v : 0;
}

// A number read from a usage file (0 = unknown); no "unlimited" cut-off
qint64 amountOf(const QByteArray& text)
{
    bool ok = false;
    const qint64 v = text.trimmed().toLongLong(&ok);
    return ok && v > 0 ? v : 0;
}

// The files of one cgroup version
struct Layout {
    QStringList limits; // the smallest of these counts
    QString usage;
    QList<QStringList> cacheKeys; // memory.stat lines (with a trailing space) of droppable file cache; the first set
                                  // of which any line is there counts
};

// Walks from `group` (a path below `mount`) up to the mount, the smallest room any level leaves (see cgroupRoom)
qint64 roomIn(const QString& mount, const QByteArray& group, const Layout& files, qint64* limitOut)
{
    qint64 room = 0;
    QString dir = QDir::cleanPath(mount + QString::fromUtf8(group));
    const QString top = QDir::cleanPath(mount);
    for (;;) {
        qint64 limit = 0;
        for (const QString& name : files.limits)
            if (const qint64 l = limitOf(readSmall(dir + '/' + name)); l && (!limit || l < limit)) limit = l;
        if (limit) {
            const qint64 current = amountOf(readSmall(dir + '/' + files.usage));
            // The page cache the kernel drops when the group needs the memory, like MemAvailable counts it: both
            // file LRU lists (active_file too: a recording just read or written sits there, often gigabytes)
            qint64 cache = 0;
            const QList<QByteArray> stat = readSmall(dir + "/memory.stat").split('\n');
            for (const QStringList& keys : files.cacheKeys) {
                bool found = false;
                for (const QByteArray& line : stat)
                    for (const QString& key : keys)
                        if (line.startsWith(key.toLatin1())) {
                            cache += amountOf(line.mid(key.size()));
                            found = true;
                        }
                if (found) break;
            }
            const qint64 used = current - std::min(cache, current);
            // Above the limit even without the cache (memory.high lowered below what the group uses: the kernel
            // throttles every allocation and reclaims until it is back under it): no room - 1 byte, never 0 (that
            // means "no limit known"), so the caller takes the least it can
            const qint64 here = std::max<qint64>(1, limit - used);
            if (limitOut) *limitOut = *limitOut ? std::min(*limitOut, limit) : limit;
            room = room ? std::min(room, here) : here;
        }
        if (dir.size() <= top.size()) break;
        const qsizetype slash = dir.lastIndexOf('/');
        if (slash < top.size()) break;
        dir = dir.left(slash);
    }
    return room;
}

bool safeGroup(const QByteArray& group)
{
    return !group.isEmpty() && group.startsWith('/') && !group.contains("/..");
}

} // namespace

qint64 cgroupRoom(const QByteArray& self, const QString& root, qint64* limitOut)
{
    if (limitOut) *limitOut = 0;
    // cgroup v2 line: "0::/user.slice/.../app.scope"; v1 lines name their controllers between the colons
    // ("4:memory:/user.slice", "7:cpu,memory:/docker/abc"), the memory controller mounted at <root>/memory
    QByteArray v2, v1;
    for (const QByteArray& line : self.split('\n')) {
        if (line.startsWith("0::")) {
            v2 = line.mid(3).trimmed();
            continue;
        }
        const QList<QByteArray> parts = line.split(':');
        if (parts.size() >= 3 && parts[1].split(',').contains("memory"))
            v1 = line.mid(parts[0].size() + parts[1].size() + 2).trimmed();
    }
    qint64 room = 0;
    const auto take = [&](qint64 here) {
        if (here) room = room ? std::min(room, here) : here;
    };
    // (A hybrid system has both: the v2 tree then has no memory controller, its files are missing and count as none)
    if (safeGroup(v2))
        take(roomIn(root, v2, {{"memory.max", "memory.high"}, "memory.current", {{"active_file ", "inactive_file "}}},
                    limitOut));
    if (safeGroup(v1)) {
        // usage_in_bytes and the total_* lines of memory.stat count the group with its children (hierarchical);
        // older kernels without them: the group's own lines
        take(roomIn(root + "/memory", v1,
                    {{"memory.limit_in_bytes"},
                     "memory.usage_in_bytes",
                     {{"total_active_file ", "total_inactive_file "}, {"active_file ", "inactive_file "}}},
                    limitOut));
    }
    return room;
}

Info read()
{
    Info info;
#if defined(Q_OS_LINUX) || defined(Q_OS_ANDROID)
    QFile f(QStringLiteral("/proc/meminfo"));
    if (f.open(QIODevice::ReadOnly)) info = parseMeminfo(f.readAll());
    if (const qint64 room = cgroupRoom(readSmall(QStringLiteral("/proc/self/cgroup")), QStringLiteral("/sys/fs/cgroup"),
                                       &info.limit))
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
    // A small machine on a big one: SCHNEIDI_TOTAL_MB is the RAM (and caps a cgroup limit and what is available),
    // SCHNEIDI_AVAILABLE_MB what is free
    bool ok = false;
    if (const qint64 mb = qEnvironmentVariable("SCHNEIDI_TOTAL_MB").toLongLong(&ok); ok && mb > 0) {
        const qint64 bytes = mb * 1024 * 1024;
        info.total = bytes;
        if (info.limit > bytes) info.limit = bytes;
        info.available = info.available > 0 ? std::min(info.available, bytes) : bytes;
    }
    const qint64 mb = qEnvironmentVariable("SCHNEIDI_AVAILABLE_MB").toLongLong(&ok);
    if (ok && mb > 0) info.available = mb * 1024 * 1024;
    return info;
}

} // namespace SystemMemory
