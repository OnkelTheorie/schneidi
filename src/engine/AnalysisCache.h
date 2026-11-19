// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Results of analyses of one media file, kept on disk so looking at the same material again (a second recipe, the
// same command with other settings that do not change the measurement) does not decode it again.
//
// One entry per (kind, file, options, version): the key is the sha1 of the file's absolute path (PathKey), its size and
// modification time, the options (compact JSON: every setting that changes the result) and the version (raise it when
// the analysis gives other results for the same file). A file that is overwritten or still grows gets another size or
// time, so its old entries are simply never asked for again. Written atomically (QSaveFile) into
// AppPaths::writable(CacheLocation)/analysis/<kind>/<key>.json. Thread-safe (distinct keys write distinct files; two
// writers of the same key write the same content). No memory cache: callers that ask often keep their own.
//
// Users: `silence` (file mode) and loudPeaks (`peaks`, `highlights`) through the per-frame peaks, pictureScores
// (`scenes`, `highlights` without a kill feed), AudioEvents (`peaks` events, the material index).

#include <QJsonObject>
#include <QString>

#include <optional>
#include <vector>

namespace AnalysisCache {

// Where one result of one file is stored, and the file as it was when the key was taken
struct Key {
    QString entry;    // the cache file; empty = nothing to key on (the media file is gone)
    QString file;     // the media file (absolute)
    qint64 size = -1; // its size and modification time (ms) at key()
    qint64 modified = 0;
    int version = 0;
    bool valid() const { return !entry.isEmpty(); }
};
// Take the key BEFORE measuring and store with it: put() leaves out a result of a file that changed meanwhile
// (overwritten while it was read: the old content must not be stored under the new size and time)
Key key(const QString& kind, const QString& file, const QJsonObject& options, int version);
// The stored result, nullopt = none (never stored, the file changed, another version, unreadable entry)
std::optional<QJsonObject> get(const Key& key);
// Stores `result`; nothing happens when the file changed since key(), is gone, or the folder cannot be written (a
// cache never fails a command)
void put(const Key& key, const QJsonObject& result);

// How many get() calls found a result (this process; for tests and measurements)
int hits();

// Keeps the folder below `capBytes`: the entries used longest ago go first, down to 80 % of the cap. Runs once per
// process by itself on a low-priority pool thread at the first key() (cap kDefaultCap), and also removes the folder of
// the old audio events cache (before 0.25). Returns the number of files removed.
inline constexpr qint64 kDefaultCap = 2LL * 1024 * 1024 * 1024;
int prune(qint64 capBytes = kDefaultCap);

// The folder of all kinds (CacheLocation/analysis)
QString folder();

// Long number lists (a value per frame) inside a result: exact and small as base64 of the raw values (little-endian),
// instead of a JSON array (a 40 min recording at 60 fps: 1 MB instead of 7, read in a few ms). Unpacking something
// else (missing, damaged) gives an empty list.
QString pack(const std::vector<float>& values);
QString pack(const std::vector<double>& values);
std::vector<float> unpackFloats(const QJsonValue& packed);
std::vector<double> unpackDoubles(const QJsonValue& packed);

} // namespace AnalysisCache
