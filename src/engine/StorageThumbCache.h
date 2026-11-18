// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Media Storage thumbnails on disk: a folder opened again (or after a restart) shows its previews at once instead of
// decoding every file anew. Small JPEGs in QStandardPaths::CacheLocation/storage-thumbs, named after the file's path,
// size and modification time (a changed file gets a new name, its old picture is pruned later). Thread-safe (no shared
// state), any thread. A missing or unwritable folder only means nothing is cached.

#include <QFileInfo>
#include <QImage>
#include <QString>

namespace StorageThumbCache {

QString dir();
QString fileFor(const QFileInfo& file); // where the picture of `file` is (or would be) stored
// The stored picture of `file`, null without one
QImage load(const QFileInfo& file);
// Stores the picture of `file` (written to a temporary file first: a reader never sees half a picture)
bool store(const QFileInfo& file, const QImage& picture);
// Drops pictures not used for 60 days, then the oldest beyond 64 MB. At most once a day unless `force`; returns the
// number of files removed. Slow on a big folder: call it in a background thread.
int prune(bool force = false);

} // namespace StorageThumbCache
