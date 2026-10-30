// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// MLT's log messages (warning level and above) into the program's log, so the log itself (app/Log) needs no MLT
// header and this file goes away with MLT.

#include <QString>

namespace MltLog {

// error: an MLT error (else a warning); text: the message, with the reporting service's name in front
using Sink = void (*)(bool error, const QString& text);
// Before MLT initialization (missing modules end up in the log too); also forwards every message to stderr
void install(Sink sink);

} // namespace MltLog
