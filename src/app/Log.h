// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Log file for feedback (Windows has no terminal): Qt messages, FFmpeg warnings and
// ffmpeg errors end up in <AppData>/logs/schneidi.log. Rotated at startup, the last 3 runs are kept.

#include <QString>

namespace Log {

// Call after setting the program name (the path depends on it); still forwards to stderr
void install();
// A line from elsewhere (level: 4-5 characters, e.g. "AV-W"; engine/FfmpegLog hands FFmpeg's messages in)
void message(const char* level, const QString& text);
QString directory();

} // namespace Log
