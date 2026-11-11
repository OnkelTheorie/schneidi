// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Licenses of the third-party code the running program uses. schneidi's own code never needs anything GPL; the GitHub
// downloads (since 0.21) bundle an FFmpeg with x264/x265 (GPL-2.0+), the GPL-free build (SCHNEIDI_NO_GPL) does not.
// Looks at what is really there, not at what the build meant to use: the license FFmpeg was configured with
// (avcodec_license() …), the GPL encoders inside it (x264/x265, also when linked statically) and every library mapped
// into the process (x264, rubberband …; Linux and Windows).
// `schneidi-cli --licenses` prints it; tests/test_license_free fails a SCHNEIDI_NO_GPL build that has GPL parts.

#include <QString>
#include <QStringList>
#include <QVector>

namespace Licenses {

struct Part {
    QString name;    // e.g. "libavcodec 61.19.101", "libx264.so.164"
    QString license; // short name as far as known ("LGPL-2.1+", "GPL-2+", "?")
    bool gpl = false;
    QString path;    // file, if any
};

// Everything found, GPL parts included
QVector<Part> report();
// Only the GPL parts of report()
QVector<Part> gplParts();
// License of a library by its file name ("libx264.so.164", "avcodec-61.dll"); empty = not known
// as GPL. Known GPL libraries only: everything else is treated as not GPL.
QString knownGplLicense(const QString& fileName);
// Built with SCHNEIDI_NO_GPL (the GPL-free packages)
bool builtWithoutGpl();

} // namespace Licenses
