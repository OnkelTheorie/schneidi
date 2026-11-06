// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// ZIP archives for sharing templates (core/TemplateLibrary.h): writing (stored, no compression - templates are mostly
// media that are compressed already) and reading (stored and deflate, the two methods every zip tool writes). Own
// code with Qt only: GNU tar (the archive tool of extensions on Linux) cannot read ZIP files, and neither zlib nor
// Qt's private zip classes are available on every build.
//
// Reading is strict, an archive comes from someone else: entries with an absolute path ("/x", "C:\x", "\\server"),
// a ".." part, a part another system reads as another name (a dot or space at its end - ".. " is ".." on Windows -,
// < > " | ? * :, control characters, Windows device names such as "CON" or "nul.txt"), a symbolic link, encryption
// or ZIP64 sizes are refused before anything is written, and every file is checked against its CRC-32.

#include <QString>
#include <QVector>

#include <optional>

namespace Zip {

struct Entry {
    QString name;          // as in the archive ('/' separated)
    quint16 method = 0;    // 0 stored, 8 deflate
    quint16 flags = 0;
    quint32 crc = 0;
    quint64 compressedSize = 0, size = 0;
    quint64 offset = 0;    // of its local header
    bool dir = false;      // name ends in '/'
    bool symlink = false;
};

// One file to write: its name in the archive and the file on disk
struct Source {
    QString name;
    QString file;
};

// A safe relative path for an entry name ("a/b.mp4"; '\' counts as separator), empty = unsafe (absolute, drive
// letter, an unsafe part as above, empty)
QString safeName(const QString& name);

// A file other programs leave in archives that is no content: macOS ("__MACOSX/…", ".DS_Store", "._name"), Windows
// ("Thumbs.db", "desktop.ini"). Takes a name as safeName gives it. extract skips these.
bool junk(const QString& name);

// Writes the files stored (no compression). Files of 4 GiB or more are refused (no ZIP64).
bool write(const QString& zipPath, const QVector<Source>& files, QString* error = nullptr);
// The entries of an archive (its central directory)
std::optional<QVector<Entry>> entries(const QString& zipPath, QString* error = nullptr);
// Every entry into `dir` (created), names stripped of `prefix` (entries outside it and junk are skipped). Refuses the
// whole archive when one entry is unsafe (see above) or two files would be one on a case-insensitive file system
// ("a.mp4" and "A.mp4", a file "x" and a folder "X/"); on failure what was written stays for the caller to remove.
// maxTotal: limit of all uncompressed sizes together (a "zip bomb")
bool extract(const QString& zipPath, const QString& dir, const QString& prefix = {}, QString* error = nullptr,
             quint64 maxTotal = quint64(16) << 30);

} // namespace Zip
