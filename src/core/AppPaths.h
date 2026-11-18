// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Writable folders of schneidi (data, config, cache): use AppPaths::writable instead of
// QStandardPaths::writableLocation for these, so test runs never reach the user's folders on any platform.
// Normally it is exactly QStandardPaths::writableLocation. In a test run (SCHNEIDI_TEST_SETTINGS, see
// core/SettingsIsolation.h) the data, config and cache folders follow XDG_DATA_HOME, XDG_CONFIG_HOME and
// XDG_CACHE_HOME on every platform, with the Linux layout (<XDG_DATA_HOME>/<organization>/<application> ...): Qt
// reads these variables only on Linux/BSD, so on Windows (AppData) and macOS (Library) a test and the
// schneidi/schneidi-cli it starts would otherwise share one folder, and a test could not point a child elsewhere.

#include <QStandardPaths>
#include <QString>

namespace AppPaths {

QString writable(QStandardPaths::StandardLocation type);

// The names of schneidi and schneidi-cli (they share backups, effects, settings): application "schneidi", on Windows
// also the organization "schneidi" (without one QSettings cannot write the registry; on Linux the missing name stays,
// otherwise existing settings would be gone). Tests that share folders with a schneidi/schneidi-cli they start call
// it too. Works before the application object exists.
void setProgramNames();

} // namespace AppPaths
