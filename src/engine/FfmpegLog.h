// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// FFmpeg's log messages into the program's log, so the log itself (app/Log) needs no FFmpeg header.
// Normal: errors only; verbose (Bundle::verboseLogging): warnings too.

#include <QString>

namespace FfmpegLog {

// error: an FFmpeg error (else a warning); text: the message, with the reporting component's name in front
using Sink = void (*)(bool error, const QString& text);
// Sets FFmpeg's level and callback; also forwards every message to stderr
void install(Sink sink);

} // namespace FfmpegLog
