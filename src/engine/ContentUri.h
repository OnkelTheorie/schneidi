// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Android's content:// addresses (the system's file picker hands them out instead of paths): FFmpeg opens only files,
// so a content address is opened once through Android's ContentResolver and read as /proc/self/fd/<n> (the same file,
// no copy). On import a content address that is a real file the app may read (a video in Movies with the media
// permission) becomes that path; else it stays the address (its read permission kept across restarts) and its media
// gets the provider's display name. Everything here does nothing on other platforms (no content addresses there).

#include <QString>
#include <QStringList>

namespace ContentUri {

bool is(const QString& path); // "content://…"
// A new descriptor of its own for the address (each reader its own file offset; the caller closes it); -1 = refused
int openFd(const QString& uri);
// The file a content address stands for, when the app may read it by path (else empty)
QString localPath(const QString& uri);
// The provider's name for it ("cs2_clip.mkv"), else the address's last part
QString displayName(const QString& uri);
// For import: each content address as its file path when readable, else as it is with its read permission kept
// (takePersistableUriPermission: the project opens after a restart too)
QStringList forImport(const QStringList& paths);

} // namespace ContentUri
