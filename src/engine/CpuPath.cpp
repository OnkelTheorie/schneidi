// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/CpuPath.h"

#include <QSettings>
#include <QString>

namespace CpuPath {

bool on()
{
    static const bool value = [] {
        const QString env = qEnvironmentVariable("SCHNEIDI_CPU_PATH").trimmed();
        if (!env.isEmpty()) return env != QLatin1String("0");
        return QSettings().value("playback/cpuPath", false).toBool();
    }();
    return value;
}

void setOn(bool on)
{
    QSettings().setValue("playback/cpuPath", on);
}

} // namespace CpuPath
