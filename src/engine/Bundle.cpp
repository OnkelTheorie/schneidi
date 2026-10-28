// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Bundle.h"

#include "engine/audio/Volume.h"
#include "engine/video/Compose.h"
#include "engine/video/Still.h"
#include "engine/video/Text.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
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
    const auto deny = [](const QByteArray& module) {
        const QByteArray list = qgetenv("MLT_REPOSITORY_DENY");
        if (!list.split(':').contains(module))
            qputenv("MLT_REPOSITORY_DENY", list.isEmpty() ? module : list + ':' + module);
    };
    deny("libmltjackrack");
    // MLT's Qt modules are GPL as a whole (0.14: no third-party GPL code in the bundled build). Their services that
    // schneidi used have own replacements registered under the same names (qtblend, qtcrop: engine/video/Compose;
    // qimage: Still; qtext: Text), so nothing of them is loaded. glaxnimate (Lottie, a Qt module too) and plusgpl
    // (burningtv, lumaliftgaingamma, rotoscoping, cbrts) are not used. xine (GPL, deinterlace: the loader's first
    // choice) is not bundled but stays allowed on systems: denying it changed frames in tests (test_retime, test_compose).
    // SCHNEIDI_MLT_QT=1 loads the Qt 6 module anyway: the image comparison against the old services (tests/test_compose).
    // Without that, systems with MLT for Qt 5 and Qt 6 side by side (Debian, Ubuntu): both Qt modules offer qimage,
    // qtblend, qtext … and the Qt 5 one won. It pulled Qt 5 into this Qt 6 program (a .webp picture then fell through
    // to avformat without Qt 5's WebP plugin): the Qt 5 one stays denied in any case.
    deny("libmltqt");
    deny("libmltglaxnimate");
    deny("libmltglaxnimate-qt6");
    deny("libmltplusgpl");
    if (!qEnvironmentVariableIsSet("SCHNEIDI_MLT_QT")) deny("libmltqt6");
    // More GPL modules schneidi does not need: normalize ("volume": own filter engine/audio/Volume; "audiolevel": own
    // meter), resample (the loader's "resampler"; swresample of the avformat module converts already), plus (GPL
    // through interp.h in its affine transition; nothing of it is used), vid.stab (own stabilizer), movit and sox
    // (link GPL libraries, unused). Licenses::report() lists what is loaded.
    for (const char* module : {"libmltnormalize", "libmltresample", "libmltplus", "libmltvidstab", "libmltmovit", "libmltsox"})
        deny(module);
#ifdef SCHNEIDI_NO_GPL
    // The GPL-free build loads no GPL module at all, also not from a system MLT (see the comment on xine above);
    // rubberband too: the MLT sound path (fallback) then keeps no pitch, the own engine does (engine/audio/Stretch)
    deny("libmltxine");
    deny("libmltrubberband");
#endif

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // MLT's Qt module (only with SCHNEIDI_MLT_QT) refuses to load without $DISPLAY / $WAYLAND_DISPLAY, even when a Qt
    // application already exists – then the timeline gets invalid transitions and crashes. On a server (no display,
    // QT_QPA_PLATFORM=offscreen) our application is already there and MLT uses it instead of opening one itself, so
    // the check only has to be passed: a placeholder name that nobody connects to.
    if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
        && qobject_cast<QGuiApplication*>(QCoreApplication::instance()) && QGuiApplication::platformName() == "offscreen")
        qputenv("DISPLAY", ":schneidi-offscreen");
#endif

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

void prepareMltCaches()
{
    // Own services first: the loader below reads its tables, and with them the producer for pictures
    Compose::registerServices();
    Volume::registerService();
    Still::registerService();
    Text::registerService();
    // every name MLT 7.30 uses with mlt_service_cache_* (grep over src/)
    for (const char* name : {"producer_avformat", "link_avdeinterlace", "link_swresample", "qimage.image", "qimage.alpha",
                             "qimage.qimage", "pixbuf.image", "pixbuf.alpha", "pixbuf.pixbuf", "pango.image"})
        mlt_service_cache_get_size(nullptr, name); // creates the cache (and the list of caches) if missing
    // Service metadata (the module's YAML) is loaded on first use and stored without a lock (mlt_repository_metadata).
    // timewarp asks for its clip producer's metadata while it opens: two speed clips opened in parallel before an
    // export crashed in producer_timewarp_init (2026-10-22). Load it here for every producer a file can open as, once
    // (6.5 ms; the frei0r generators would add 20 ms and are never opened from a file)
    const mlt_repository repository = mlt_factory_repository();
    const mlt_properties producers = mlt_repository_producers(repository);
    for (int i = 0, n = mlt_properties_count(producers); i < n; ++i)
        if (const char* name = mlt_properties_get_name(producers, i); qstrncmp(name, "frei0r.", 7) != 0)
            mlt_repository_metadata(repository, mlt_service_producer_type, name);
    // The loader (every file goes through it) reads loader.dict (file name -> producer) and loader.ini (normalizing
    // filters) on first use into statics, without a lock (producer_loader.c): crashed in producer_loader_init with
    // files opened in parallel. One producer with a service prefix loads loader.ini, one without loads loader.dict
    // (it need not exist).
    const mlt_profile profile = mlt_profile_init(nullptr);
    for (const char* resource : {"color:black", "schneidi-warm-up.invalid"})
        if (const mlt_producer p = mlt_factory_producer(profile, "loader", resource)) mlt_producer_close(p);
    mlt_profile_close(profile);
}

QString tool(const QString& name)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QString found = QStandardPaths::findExecutable(name, {appDir});
    if (found.isEmpty()) found = QStandardPaths::findExecutable(name);
    return found.isEmpty() ? name : found;
}

} // namespace Bundle
