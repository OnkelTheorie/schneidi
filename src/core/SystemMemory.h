// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Physical memory of the machine, for choosing how much work runs at once (render threads on a small board like the
// Raspberry Pi Zero 2 W with 415 MB: four render threads there make it swap and take twice as long as two).
// Linux: /proc/meminfo (MemTotal, MemAvailable), lowered to the room the process's control groups leave (cgroup v2
// memory.max/memory.high, cgroup v1 memory.limit_in_bytes: a container, a systemd unit or scope with a memory cap - /proc/meminfo shows the whole
// machine there, and the cap kills or throttles the process long before that is used), Windows: GlobalMemoryStatusEx, macOS: sysctl hw.memsize (total
// only). Unknown values are 0 = no limit.

#include <QtGlobal>

namespace SystemMemory {

struct Info {
    qint64 total = 0;     // physical RAM in bytes (0 = unknown)
    qint64 available = 0; // RAM available for new work without swapping, in bytes (0 = unknown)
    qint64 limit = 0;     // the smallest cgroup limit above the process, in bytes (0 = none known; Linux)
};

// Read now (not cached: what is available changes). SCHNEIDI_AVAILABLE_MB (a number) replaces the available amount,
// SCHNEIDI_TOTAL_MB the RAM (also capping the cgroup limit and the available amount): tests, trying a small machine's
// choices on a big one (the Pi Zero 2 W: SCHNEIDI_TOTAL_MB=415).
Info read();

// /proc/meminfo text -> Info (kB values); split off for tests
Info parseMeminfo(const QByteArray& text);

// Room the cgroup limits leave, in bytes (0 = no limit known): `self` is the text of /proc/self/cgroup, `root` the
// cgroup mount (/sys/fs/cgroup; cgroup v1: its memory controller at <root>/memory, memory.limit_in_bytes minus
// memory.usage_in_bytes plus the total_* file cache of memory.stat, "unlimited" = no limit). Every level from the process's group up to the root counts: limit (the smaller of
// memory.max and memory.high) minus memory.current, plus the file cache there that can be dropped (active_file and
// inactive_file of memory.stat, as MemAvailable counts it). A group above its limit even without that cache (memory.high
// set below its use) has no room: 1 byte. Split off for tests (a folder tree in place of the mount).
// limit (optional): the smallest of those limits (0 = none).
qint64 cgroupRoom(const QByteArray& self, const QString& root, qint64* limit = nullptr);

} // namespace SystemMemory
