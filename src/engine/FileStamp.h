// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Size and modification time of a media file, to notice a file overwritten under the same name (re-rendered,
// re-recorded): caches keyed by path alone kept showing the old picture everywhere in the app.
// recent() remembers the stamp per path and looks at the disk again at most every kRecheckMs (no stat per frame on the
// hot path). A change counts once the new stamp has held still for kSettleMs (the writer is done): a recording that
// still grows (a watch folder, a recorder's output on the timeline) is left alone - its readers are not reopened every
// second - until it stops growing. Then the observers are told (on the calling thread, no FileStamp lock held: they
// may take their own locks and add or remove observers) so caches drop what they keep of that file. Readers that open
// a file store the confirmed stamp and compare it with recent() when they are reused.

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
constexpr int kSettleMs = 2000;

// The confirmed stamp after looking at the disk now (one stat); a change that has settled tells the observers
Stamp now(const QString& path);
// The same from memory, looked up on the disk again when older than kRecheckMs; first call per path = now()
Stamp recent(const QString& path);
// Is `path` seen changing without the change having settled yet? (Callers look again after kSettleMs)
bool changing(const QString& path);

// Called with the path whose stamp changed (not on the first sighting). Returns an id for removeObserver, which
// waits for a call of it running on another thread (it may be called from inside the observer itself).
int addObserver(std::function<void(const QString& path)> fn);
void removeObserver(int id);

// Tests: forget everything (the next recent() reads the disk); a shorter settle time
void forget();
void setSettleMsForTest(int ms);

} // namespace FileStamp
