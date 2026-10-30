// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Bundled runtime (AppImage on Linux, program folder on Windows): ffmpeg/ffprobe and other helper programs live with
// the program instead of in the system.

#include <QString>

namespace Bundle {

// Verbose messages? Environment variable SCHNEIDI_VERBOSE (main sets it with --verbose)
bool verboseLogging();
// Path of an external program (e.g. "ffmpeg"): first next to the program, then in PATH; otherwise the bare name
QString tool(const QString& name);

} // namespace Bundle
