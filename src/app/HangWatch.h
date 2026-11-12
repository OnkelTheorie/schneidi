// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Hang watch: the GUI thread stops answering for longer than a few seconds (a disk at 100 %, a blocking call) -> a
// "HANG" line in the log with how long and where it is (the GUI thread's call stack, taken from a helper thread), a
// second stack if it still hangs after 15 s, and a line once it answers again. Windows shows "not responding" after
// 5 s; without this the log said nothing about it. A log with a hang is kept (app/Log.h, logs/kept/).
//
// Stacks: Linux/glibc (a signal to the GUI thread, backtrace()) and Windows x64 (the thread suspended only to copy
// the top of its stack, the copy scanned for return addresses after it runs again). Addresses are module+offset like
// in the crash report (app/CrashReport.h). Elsewhere: times only.

#include <QString>

#include <functional>

namespace HangWatch {

// GUI thread, after Log::install: prepares the watcher; it watches only after arm(). thresholdMs/report for tests
// (default: 3 s, the log).
void install(int thresholdMs = 3000, std::function<void(const QString&)> report = {});
// Right before the event loop runs (app.exec()): a long start is no hang (a splash that pumps events must not arm it)
void arm();
// Right after the event loop ended (quit-time waits for threads are no hang either); also done when the
// QCoreApplication goes
void uninstall();

} // namespace HangWatch
