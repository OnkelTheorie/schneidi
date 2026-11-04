// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Licenses.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <QSet>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#ifdef Q_OS_WIN
#include <windows.h>
// after windows.h: K32EnumProcessModules (kernel32, no psapi.lib needed)
#include <psapi.h>
#endif

namespace Licenses {
namespace {

// Libraries that are GPL (or make the process GPL) by their base name, without "lib" prefix, version and extension.
struct Known {
    const char* name;
    const char* license;
};
constexpr Known kGpl[] = {
    // FFmpeg's GPL dependencies (only linked with --enable-gpl) and other GPL libraries
    {"x264", "GPL-2+"},        {"x265", "GPL-2+"},        {"rubberband", "GPL-2+"},  {"vidstab", "GPL-2+"},
    {"postproc", "GPL-2+"},    {"xvidcore", "GPL-2+"},    {"frei0r", "GPL-2+"},      {"movit", "GPL-2+"},
    {"sox", "GPL-2+"},         {"fftw3", "GPL-2+"},       {"fftw3f", "GPL-2+"},      {"cdio", "GPL-3+"},
    {"cdio_paranoia", "GPL-3+"}, {"cdio_cdda", "GPL-3+"}, {"dvdread", "GPL-2+"},     {"dvdnav", "GPL-2+"},
    {"smbclient", "GPL-3+"},   {"davs2", "GPL-2+"},       {"xavs", "GPL-2+"},        {"xavs2", "GPL-2+"},
    {"vo-amrwbenc", "Apache-2.0 (needs GPL-3 FFmpeg)"},
};

// "libx264.so.164" / "libpostproc.so.58" / "x264-164.dll" / "avcodec-61.dll" -> "x264", "postproc", "x264", "avcodec"
QString baseName(const QString& fileName)
{
    QString n = fileName;
    if (n.startsWith("lib")) n = n.mid(3);
    static const QRegularExpression ext(R"((\.so(\.[0-9.]+)?|\.dll|\.dylib)$)", QRegularExpression::CaseInsensitiveOption);
    n.remove(ext);
    static const QRegularExpression dllVersion(R"(-[0-9]+$)"); // Windows: avcodec-61, x264-164
    n.remove(dllVersion);
    return n.toLower();
}

// Every shared library mapped into this process
QStringList loadedLibraries()
{
    QStringList out;
#if defined(Q_OS_LINUX)
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly)) return out;
    QSet<QString> seen;
    for (const QByteArray& line : maps.readAll().split('\n')) {
        const qsizetype slash = line.indexOf('/');
        if (slash < 0) continue;
        const QString path = QString::fromLocal8Bit(line.mid(slash)).trimmed();
        if (!path.contains(".so") || seen.contains(path)) continue;
        seen.insert(path);
        out << path;
    }
#elif defined(Q_OS_WIN)
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return out;
    const DWORD n = std::min<DWORD>(needed / sizeof(HMODULE), DWORD(std::size(modules)));
    for (DWORD i = 0; i < n; ++i) {
        wchar_t buf[MAX_PATH * 2];
        const DWORD len = GetModuleFileNameW(modules[i], buf, DWORD(std::size(buf)));
        if (len > 0) out << QDir::fromNativeSeparators(QString::fromWCharArray(buf, int(len)));
    }
#endif
    return out;
}

// FFmpeg's license string ("LGPL version 2.1 or later", "GPL version 2 or later", "nonfree and unredistributable")
Part ffmpegPart(const char* name, unsigned version, const char* license)
{
    const QString l = QString::fromUtf8(license);
    Part p;
    p.name = QString("%1 %2.%3.%4").arg(name).arg(version >> 16).arg((version >> 8) & 0xff).arg(version & 0xff);
    p.license = l;
    p.gpl = !l.startsWith("LGPL"); // "GPL …" and "nonfree …" both may not go into the bundle
    return p;
}

} // namespace

QString knownGplLicense(const QString& fileName)
{
    const QString n = baseName(fileName);
    for (const Known& k : kGpl)
        if (n == QLatin1String(k.name)) return QString::fromUtf8(k.license);
    return {};
}

QVector<Part> report()
{
    QVector<Part> out;
    out << ffmpegPart("libavcodec", avcodec_version(), avcodec_license())
        << ffmpegPart("libavformat", avformat_version(), avformat_license())
        << ffmpegPart("libavutil", avutil_version(), avutil_license())
        << ffmpegPart("libswscale", swscale_version(), swscale_license())
        << ffmpegPart("libswresample", swresample_version(), swresample_license());
    // Compiled in (third_party/rnnoise): the noise suppression of the voice cleaning (engine/VoiceClean)
    out << Part{"RNNoise 0.2 (built in)", "BSD-3-Clause", false, {}};
    // Ported into core/SfxWhoosh, core/SfxForge, core/SfxDsp.h (third_party/earshot/LICENSE): the built-in sounds of
    // op 'sfx'
    out << Part{"earshot sound recipes: whoosh, darkriser, shutter, knock, subdrop, banks (Leon Lin, ported in)", "MIT", false, {}};
    // Built in (assets/sfx/cc0, SOURCES.md): the recorded sounds of op 'sfx'
    out << Part{"Swishes Sound Pack by artisticdude, OpenGameArt (built in)", "CC0-1.0", false, {}};
    out << Part{"Impact Sounds by Kenney, www.kenney.nl (built in)", "CC0-1.0", false, {}};

    // Loaded libraries: GPL ones by name
    for (const QString& path : loadedLibraries()) {
        const QString file = QFileInfo(path).fileName();
        const QString gpl = knownGplLicense(file);
        if (gpl.isEmpty()) continue;
        Part p;
        p.name = file;
        p.path = path;
        p.license = gpl;
        p.gpl = true;
        out << p;
    }
    return out;
}

bool builtWithoutGpl()
{
#ifdef SCHNEIDI_NO_GPL
    return true;
#else
    return false;
#endif
}

QVector<Part> gplParts()
{
    QVector<Part> out;
    for (const Part& p : report())
        if (p.gpl) out << p;
    return out;
}

} // namespace Licenses
