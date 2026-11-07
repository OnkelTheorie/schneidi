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
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    QStandardPaths::setTestModeEnabled(true);
#else
    for (const auto& [name, sub] : {std::pair{"XDG_CONFIG_HOME", "config"}, std::pair{"XDG_DATA_HOME", "data"},
                                    std::pair{"XDG_CACHE_HOME", "cache"}})
        if (qEnvironmentVariableIsEmpty(name)) qputenv(name, QDir::toNativeSeparators(dir.filePath(sub)).toLocal8Bit());
#endif
    return dir.absolutePath();
}

} // namespace SettingsIsolation
