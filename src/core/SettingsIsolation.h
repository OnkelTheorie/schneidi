// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Test runs never touch the user's settings: SCHNEIDI_TEST_SETTINGS=<folder> sends every QSettings of this process to
// an INI file in that folder (never the registry or ~/.config) and the data/config/cache folders below it. The test
// harness sets it for every test (tests/CMakeLists.txt, Check::isolateSettings); schneidi and schneidi-cli honour it
// at startup, so every program a test starts is isolated as well. Without the variable nothing changes.

#include <QString>

namespace SettingsIsolation {

constexpr const char* kVariable = "SCHNEIDI_TEST_SETTINGS";

// Call first in main(), before any QSettings/QStandardPaths use (works before the application object exists).
// Settings: <folder>/settings/<organization>/<application>.ini. Data, config, cache: on Linux/BSD <folder>/data,
// /config, /cache through the XDG variables (only where they are unset: a test may point a child elsewhere); on
// Windows and macOS, where XDG does nothing, QStandardPaths' test mode (never the real AppData/Library folders).
// Returns the folder (absolute), empty = not set (nothing changed).
QString apply();

} // namespace SettingsIsolation
