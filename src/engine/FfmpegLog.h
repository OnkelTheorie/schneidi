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

// Collects FFmpeg's error messages of the calling thread while it lives (a custom effect's filter chain checked at edit
// time: the reason goes back to the caller). Installs the callback if install() was not called (tests)
class Capture {
public:
    Capture();
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    // The messages so far, one per line, without trailing newlines
    QString text() const;

private:
    QString m_text;
    Capture* m_outer = nullptr;
    friend struct CaptureAccess;
};

} // namespace FfmpegLog
