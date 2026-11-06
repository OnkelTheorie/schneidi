// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Physical memory of the machine, for choosing how much work runs at once (render threads on a small board like the
// Raspberry Pi Zero 2 W with 415 MB: four render threads there make it swap and take twice as long as two).
// Linux: /proc/meminfo (MemTotal, MemAvailable), lowered to the room the process's control groups leave (cgroup v2
// memory.max/memory.high: a container, a systemd unit or scope with a memory cap - /proc/meminfo shows the whole
// machine there, and the cap kills or throttles the process long before that is used), Windows: GlobalMemoryStatusEx, macOS: sysctl hw.memsize (total
// only). Unknown values are 0 = no limit.

#include <QtGlobal>

namespace SystemMemory {

struct Info {
    qint64 total = 0;     // physical RAM in bytes (0 = unknown)
    qint64 available = 0; // RAM available for new work without swapping, in bytes (0 = unknown)
};

// Read now (not cached: what is available changes). SCHNEIDI_AVAILABLE_MB (a number) replaces the available amount
// (tests, trying a small machine's choice on a big one).
Info read();

// /proc/meminfo text -> Info (kB values); split off for tests
Info parseMeminfo(const QByteArray& text);

// Room the cgroup v2 limits leave, in bytes (0 = no limit known): `self` is the text of /proc/self/cgroup, `root` the
// cgroup2 mount (/sys/fs/cgroup). Every level from the process's group up to the root counts: limit (the smaller of
// memory.max and memory.high) minus memory.current, plus the file cache there that can be dropped (inactive_file of
// memory.stat). Split off for tests (a folder tree in place of the mount).
qint64 cgroupRoom(const QByteArray& self, const QString& root);

} // namespace SystemMemory
