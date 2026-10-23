#pragma once
// Crash file (local, no telemetry): on a crash (SIGSEGV/SIGABRT/…, on Windows an unhandled exception, an
// uncaught C++ exception) a report with version, system and call stack is written to <AppData>/crashes.
// At the next start the log of the crashed run is appended and schneidi asks whether to look at the report;
// the user sends it themselves (e.g. attached to a GitHub issue).
//
// Stack addresses are offsets into the module (Linux "schneidi(+0x1234)"): `addr2line -e schneidi -f -C 0x1234`
// with the same build resolves them.

#include <QStringList>

class QWidget;

namespace CrashReport {

// Call right after Log::install() (the previous log is then schneidi.1.log): appends it to an unfinished report
// of the previous run, then installs the handlers for this run.
void install();
QString directory();
// Reports not yet shown to the user (oldest first)
QStringList pending();
// Asks about the newest pending report (view, copy, open folder) and marks all as seen
void offer(QWidget* parent);

} // namespace CrashReport
