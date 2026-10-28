// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Stutter of a recording (core/Stutter.h) from the time stamps of its video packets: libavformat reads the packets of
// the first video stream, nothing is decoded (a 30 s 1080p60 capture in well under a second). Results are kept in
// memory and on disk (CacheLocation/gaps, by path, size and modification time), so `probe`, `highlights`, `check` and
// the Deliver page ask for a file once. Thread-safe (`highlights`/`probe` look at several files in parallel).

#include "core/Stutter.h"

#include <QString>

#include <optional>

namespace FrameGaps {

// nullopt = no video stream (sound only, still image) or the file cannot be read
std::optional<Stutter::Scan> scan(const QString& path);

// For tests: forget what is cached in memory (the disk cache is keyed by size and modification time anyway)
void clearMemoryCache();

} // namespace FrameGaps
