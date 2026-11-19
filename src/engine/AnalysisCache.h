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

// The stored result, nullopt = none (never stored, the file changed, another version, unreadable entry)
std::optional<QJsonObject> get(const QString& kind, const QString& file, const QJsonObject& options, int version);
// Stores `result` (nothing happens when the file is gone or the folder cannot be written: a cache never fails a command)
void put(const QString& kind, const QString& file, const QJsonObject& options, int version, const QJsonObject& result);

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
