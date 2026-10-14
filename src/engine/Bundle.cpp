#include "engine/Bundle.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <framework/mlt.h>

#include <clocale>
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

// Candidates for <prefix>: program folder itself (Windows) and its parent folder (…/usr/bin in the AppImage)
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

// Unpackaged build on Windows (MSYS2): MLT otherwise looks for modules and data next to the .exe
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
    // MLT (module folder) and the environment variables read paths in the ANSI code page, not as UTF-8
    const QByteArray local = native.toLocal8Bit();
    if (QString::fromLocal8Bit(local) == native) return local;
    // Characters outside the code page: short 8.3 path (ASCII only), if the drive offers it
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
    // schneidi does not use jackrack (LADSPA/LV2/VST2 plugins, JACK): do not load it. Saves searching the
    // plugin folders at startup, and older MLT versions otherwise report it as a warning without LADSPA plugins
    // "No LADSPA plugins were found! Check your LADSPA_PATH …" (also VST_PATH). A value set by the user
    // MLT_REPOSITORY_DENY is kept (list with ':', also on Windows – MLT separates it that way there too).
    QByteArray deny = qgetenv("MLT_REPOSITORY_DENY");
    if (!deny.split(':').contains("libmltjackrack"))
        qputenv("MLT_REPOSITORY_DENY", deny.isEmpty() ? QByteArray("libmltjackrack") : deny + ":libmltjackrack");

    const auto set = setPathEnv;
    if (const QString p = bundledPrefix(); !p.isEmpty()) {
#ifdef Q_OS_WIN
        // MLT loads modules with the module folder as search path: their DLLs (ffmpeg, Qt, …) lie in the program folder
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

bool verboseLogging()
{
    const QByteArray v = qgetenv("SCHNEIDI_VERBOSE");
    return !v.isEmpty() && v != "0";
}

void configureMltLogging()
{
    // FFmpeg warnings go directly to stderr (not into the log) and are noise in normal operation, e.g. "[opus] Could
    // not update timestamps for skipped/discarded samples" (decoder; MLT counts the samples itself, audio stays in sync)
    // or "[mp4] Timestamps are unset in a packet for stream 1 … making some up" (MLT writes an empty packet without timestamp after flushing the
    // audio encoder; the file is correct). MLT only takes over its own level once,
    // at the first avformat service, as the FFmpeg level – create that one here with the desired level.
    const bool more = verboseLogging();
    mlt_log_set_level(more ? MLT_LOG_WARNING : MLT_LOG_ERROR); // MLT and FFmpeg levels are numbered the same
    if (mlt_profile profile = mlt_profile_init(nullptr)) {
        if (mlt_consumer c = mlt_factory_consumer(profile, "avformat", nullptr)) mlt_consumer_close(c);
        mlt_profile_close(profile);
    }
    mlt_log_set_level(more ? MLT_LOG_INFO : MLT_LOG_WARNING);
}

void pinNumericLocale()
{
    if (mlt_profile profile = mlt_profile_init(nullptr)) {
        if (mlt_transition t = mlt_factory_transition(profile, "qtblend", nullptr)) mlt_transition_close(t);
        mlt_profile_close(profile);
    }
    std::setlocale(LC_NUMERIC, "C");
}

QString tool(const QString& name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QString found = QStandardPaths::findExecutable(name, {appDir});
    if (found.isEmpty()) found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? name : found;
}

} // namespace Bundle
