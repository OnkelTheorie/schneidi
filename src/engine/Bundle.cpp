#include "engine/Bundle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <iterator>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Bundle {
namespace {

void setPathEnv(const char* name, const QString& dir)
{
    if (QFileInfo(dir).isDir()) qputenv(name, pathForMlt(dir));
}

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

// Nicht paketierter Build unter Windows (MSYS2): MLT sucht Module und Daten sonst neben der .exe
QString devPrefix()
{
#if defined(Q_OS_WIN) && defined(SCHNEIDI_MLT_DEV_PREFIX)
    const QString p = QStringLiteral(SCHNEIDI_MLT_DEV_PREFIX);
    if (QFileInfo(p + "/lib/mlt").isDir()) return p;
#endif
    return {};
}

} // namespace

QString mltModuleDir()
{
    if (const QString p = bundledPrefix(); !p.isEmpty()) return p + "/lib/mlt-7";
    if (const QString p = devPrefix(); !p.isEmpty()) return p + "/lib/mlt";
    return {};
}

QByteArray pathForMlt(const QString& path)
{
    const QString native = QDir::toNativeSeparators(path);
#ifdef Q_OS_WIN
    // MLT (Modulordner) und die Umgebungsvariablen lesen Pfade in der ANSI-Codepage, nicht als UTF-8
    const QByteArray local = native.toLocal8Bit();
    if (QString::fromLocal8Bit(local) == native) return local;
    // Zeichen außerhalb der Codepage: kurzer 8.3-Pfad (nur ASCII), falls das Laufwerk ihn anbietet
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetShortPathNameW(reinterpret_cast<const wchar_t*>(native.utf16()), buf, DWORD(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return QString::fromWCharArray(buf, int(n)).toLocal8Bit();
    return local;
#else
    return native.toUtf8();
#endif
}

void prepareMltEnvironment()
{
    const auto set = setPathEnv;
    if (const QString p = bundledPrefix(); !p.isEmpty()) {
#ifdef Q_OS_WIN
        // MLT lädt Module mit dem Modulordner als Suchpfad: deren DLLs (ffmpeg, Qt, …) liegen im Programmordner
        const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
        SetDllDirectoryW(reinterpret_cast<const wchar_t*>(appDir.utf16()));
#endif
        set("MLT_DATA", p + "/share/mlt-7");
        set("MLT_PROFILES_PATH", p + "/share/mlt-7/profiles");
        set("MLT_PRESETS_PATH", p + "/share/mlt-7/presets");
        set("FREI0R_PATH", p + "/lib/frei0r-1");
    } else if (const QString d = devPrefix(); !d.isEmpty()) {
        set("MLT_DATA", d + "/share/mlt");
        set("MLT_PROFILES_PATH", d + "/share/mlt/profiles");
        set("MLT_PRESETS_PATH", d + "/share/mlt/presets");
        set("FREI0R_PATH", d + "/lib/frei0r-1");
    }
}

QString tool(const QString& name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QString found = QStandardPaths::findExecutable(name, {appDir});
    if (found.isEmpty()) found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? name : found;
}

} // namespace Bundle
