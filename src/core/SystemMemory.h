// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Physical memory of the machine, for choosing how much work runs at once (render threads on a small board like the
// Raspberry Pi Zero 2 W with 415 MB: four render threads there make it swap and take twice as long as two).
// Linux: /proc/meminfo (MemTotal, MemAvailable), Windows: GlobalMemoryStatusEx, macOS: sysctl hw.memsize (total
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

} // namespace SystemMemory
