// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The program's own command line (src/main.cpp): what is left for import after the options.

#include <QStringList>

namespace LaunchArgs {

// Options of schneidi that take a value (removed with it)
inline const QStringList& valueOptions()
{
    static const QStringList v = {"--screenshot", "--wait", "--lang", "--format", "--storage", "--design", "--size", "--actions"};
    return v;
}

// Files and folders to open/import from the arguments (without the program name): everything except schneidi's options
// and Qt's own window-system options. Qt leaves some of those in QCoreApplication::arguments() (X11 -geometry
// 1600x900+0+0): taken as files they were reported "not readable" in a modal box that blocked a test run's start.
QStringList files(const QStringList& args);

// Whether a launch with these arguments first offers its files to a schneidi that already runs (cli/Live.h handOver)
// instead of opening a window of its own: files given, no test run (--demo, --actions), no --new-instance. The caller
// checks --screenshot and SCHNEIDI_LIVE itself.
bool handsOver(const QStringList& args);

// The arguments of the restart after a design/language change: the same project again, in a window of its own. Never
// handed over: with a second schneidi running, the project would land in that one and this window would be gone.
QStringList restart(const QString& projectPath);

} // namespace LaunchArgs
