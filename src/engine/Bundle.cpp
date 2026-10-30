// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Bundle.h"

#include <QCoreApplication>
#include <QStandardPaths>

namespace Bundle {

bool verboseLogging()
{
    const QByteArray v = qgetenv("SCHNEIDI_VERBOSE");
    return !v.isEmpty() && v != "0";
}

QString tool(const QString& name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QString found = QStandardPaths::findExecutable(name, {appDir});
    if (found.isEmpty()) found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? name : found;
}

} // namespace Bundle
