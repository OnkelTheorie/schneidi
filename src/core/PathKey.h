// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The spelling of a file path used as a key (hashed into cache, notes, plan/backup folder and lock names): absolute
// and clean, and on Windows in lower case (file names and drive letters are case-insensitive there: "c:/x" and "C:/X"
// are one file and must give one key).

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace PathKey {

#ifdef Q_OS_WIN
inline constexpr bool kFoldCase = true;
#else
inline constexpr bool kFoldCase = false;
#endif

// foldCase: lower-case the key (the platform's default; a parameter for tests of both ways)
inline QString of(const QString& path, bool foldCase = kFoldCase)
{
    if (path.isEmpty()) return {};
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    return foldCase ? clean.toLower() : clean;
}

// How paths compare on this platform (foldCase as above)
inline Qt::CaseSensitivity sensitivity(bool foldCase = kFoldCase)
{
    return foldCase ? Qt::CaseInsensitive : Qt::CaseSensitive;
}

// `path` lies inside the folder `dir` (both cleaned; the folder itself is not inside it): "C:/Templates/x/a.png" is
// in "c:/templates/x" on Windows, a template saved there keeps its files
inline bool isInside(const QString& path, const QString& dir, bool foldCase = kFoldCase)
{
    if (path.isEmpty() || dir.isEmpty()) return false;
    QString d = QDir::cleanPath(dir);
    if (!d.endsWith('/')) d += '/';
    const QString p = QDir::cleanPath(path);
    return p.size() > d.size() && p.startsWith(d, sensitivity(foldCase));
}

} // namespace PathKey
