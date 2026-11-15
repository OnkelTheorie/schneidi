// SPDX-License-Identifier: AGPL-3.0-only
#include "core/SettingsIsolation.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <utility>

namespace SettingsIsolation {

QString apply()
{
    const QString value = qEnvironmentVariable(kVariable);
    if (value.isEmpty()) return {};
    const QDir dir(QDir(value).absolutePath());
    // The default QSettings() of the whole program takes the default format: an INI file, also on Windows (no
    // registry) and macOS (no plist)
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.filePath("settings"));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, dir.filePath("settings-system"));
    // Data, config, cache: the XDG variables on every platform (Qt reads them on Linux/BSD, AppPaths::writable on
    // Windows/macOS), only where unset: a test may point a child elsewhere
    for (const auto& [name, sub] : {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
                                    std::pair{"XDG_CACHE_HOME", "cache"}})
        if (qEnvironmentVariableIsEmpty(name)) qputenv(name, QDir::toNativeSeparators(dir.filePath(sub)).toLocal8Bit());
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    // What still asks QStandardPaths itself (Qt, a call that bypasses AppPaths): Qt's test folders, never the real
    // AppData/Library folders
    QStandardPaths::setTestModeEnabled(true);
#endif
    return dir.absolutePath();
}

} // namespace SettingsIsolation
