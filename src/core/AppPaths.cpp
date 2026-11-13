// SPDX-License-Identifier: AGPL-3.0-only
#include "core/AppPaths.h"

#include "core/SettingsIsolation.h"

#include <QCoreApplication>
#include <QDir>

namespace AppPaths {

QString writable(QStandardPaths::StandardLocation type)
{
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    const QString isolated = qEnvironmentVariable(SettingsIsolation::kVariable);
    if (!isolated.isEmpty()) {
        // The test's own folder, below it the subfolder SettingsIsolation::apply gives the variable when unset
        auto base = [&](const char* variable, const char* sub) {
            const QString value = qEnvironmentVariable(variable);
            return QDir::cleanPath(QDir(value.isEmpty() ? QDir(isolated).filePath(sub) : value).absolutePath());
        };
        // Like Qt on Linux: <base>/<organization>/<application>, empty names left out
        auto withApp = [](QString path) {
            for (const QString& name : {QCoreApplication::organizationName(), QCoreApplication::applicationName()})
                if (!name.isEmpty()) path += '/' + name;
            return path;
        };
        switch (type) {
        case QStandardPaths::AppDataLocation:
        case QStandardPaths::AppLocalDataLocation: return withApp(base("XDG_DATA_HOME", "data"));
        case QStandardPaths::GenericDataLocation: return base("XDG_DATA_HOME", "data");
        case QStandardPaths::AppConfigLocation: return withApp(base("XDG_CONFIG_HOME", "config"));
        case QStandardPaths::ConfigLocation:
        case QStandardPaths::GenericConfigLocation: return base("XDG_CONFIG_HOME", "config");
        case QStandardPaths::CacheLocation: return withApp(base("XDG_CACHE_HOME", "cache"));
        case QStandardPaths::GenericCacheLocation: return base("XDG_CACHE_HOME", "cache");
        default: break;
        }
    }
#endif
    return QStandardPaths::writableLocation(type);
}

void setProgramNames()
{
    QCoreApplication::setApplicationName(QStringLiteral("schneidi"));
#ifdef Q_OS_WIN
    QCoreApplication::setOrganizationName(QStringLiteral("schneidi"));
#endif
}

} // namespace AppPaths
