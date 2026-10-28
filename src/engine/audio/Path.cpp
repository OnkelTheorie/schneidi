// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/audio/Path.h"

#include <QSettings>
#include <QString>

namespace Audio {

bool ownPath()
{
    QString choice = qEnvironmentVariable("SCHNEIDI_AUDIO").trimmed().toLower();
    if (choice.isEmpty()) choice = QSettings().value("audio/engine").toString().trimmed().toLower();
    return choice != QLatin1String("mlt");
}

void setOwnPath(bool own)
{
    QSettings().setValue("audio/engine", own ? "own" : "mlt");
}

} // namespace Audio
