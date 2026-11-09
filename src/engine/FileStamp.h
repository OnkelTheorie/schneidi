// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Size and modification time of a media file, to notice a file overwritten under the same name (re-rendered,
// re-recorded): caches keyed by path alone kept showing the old picture everywhere in the app.
// recent() remembers the stamp per path and looks at the disk again at most every kRecheckMs (no stat per frame on the
// hot path); when it sees a change it tells the observers (on the calling thread) so caches drop what they keep of that
// file. Callers must not hold a lock an observer takes; observers must not add/remove observers. Readers that open a
// file store its stamp and compare it with recent() when they are reused.

#include <QString>
#include <QtGlobal>
#include <functional>

namespace FileStamp {

struct Stamp {
    qint64 size = -1;     // -1 = missing
    qint64 modified = 0;  // ms since the epoch
    bool operator==(const Stamp&) const = default;
};

constexpr int kRecheckMs = 1000;

// From the disk now (one stat); also updates the memory (and tells observers about a change)
Stamp now(const QString& path);
// From memory, looked up on the disk again when older than kRecheckMs; first call per path = now()
Stamp recent(const QString& path);

// Called with the path whose stamp changed (not on the first sighting). Returns an id for removeObserver.
int addObserver(std::function<void(const QString& path)> fn);
void removeObserver(int id);

// Tests: forget everything (the next recent() reads the disk)
void forget();

} // namespace FileStamp
