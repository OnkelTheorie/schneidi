#include "engine/Bundle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace Bundle {
namespace {

// Kandidaten für <prefix>: Programmordner selbst (Windows) und dessen Elternordner (…/usr/bin im AppImage)
QStringList prefixes()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    return {appDir, QDir::cleanPath(appDir + "/..")};
}

QString bundledPrefix()
{
    for (const QString& p : prefixes())
        if (QFileInfo(p + "/lib/mlt-7").isDir()) return p;
    return {};
}

} // namespace

QString mltModuleDir()
{
    const QString p = bundledPrefix();
    return p.isEmpty() ? QString() : p + "/lib/mlt-7";
}

void prepareMltEnvironment()
{
    const QString p = bundledPrefix();
    if (p.isEmpty()) return;
    const auto set = [](const char* name, const QString& dir) {
        if (QFileInfo(dir).isDir()) qputenv(name, QDir::toNativeSeparators(dir).toUtf8());
    };
    set("MLT_DATA", p + "/share/mlt-7");
    set("MLT_PROFILES_PATH", p + "/share/mlt-7/profiles");
    set("MLT_PRESETS_PATH", p + "/share/mlt-7/presets");
    set("FREI0R_PATH", p + "/lib/frei0r-1");
}

QString tool(const QString& name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QString found = QStandardPaths::findExecutable(name, {appDir});
    if (found.isEmpty()) found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? name : found;
}

} // namespace Bundle
