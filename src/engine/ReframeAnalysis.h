// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Auto-Reframe, the measuring part: where the subject of a recording is over time, and where its shots change.
// Own heuristic, no trained model: on a small copy of the picture (longer side 128) a few times a second, motion
// against the previous sample, local detail and skin-coloured areas count, all relative to the rest of the picture;
// parts that never change while the rest does (a game HUD, a logo, a still background) lose their detail; a mild
// pull towards the middle. The weighted centre is the subject, its spread how sure that is (a game's busy picture
// spreads wide: the path then stays near the middle, where the crosshair is). Scene cuts: every frame, luma
// histogram and difference to the previous one.
//
// Per media file and range, cached in QStandardPaths::CacheLocation/reframe (named after the file, its size and
// time, and the analyzed seconds); a cached range that covers the request is reused. Runs in any thread, cancellable.

#include "core/ProjectFormat.h"
#include "core/Reframe.h"
#include "core/Types.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>

namespace ReframeAnalysis {

struct Sample {
    double seconds = 0;       // position in the file (as the decoder counts it from its first picture)
    double x = 0.5, y = 0.5;  // subject centre, 0..1 of the picture
    double weight = 0;        // how sure (0 = no idea)
    bool cut = false;         // a new shot starts here
};
using Samples = QVector<Sample>;

// The part of a file to measure (seconds, inclusive)
struct Need {
    QString file;
    double from = 0, to = 0;
};

QString cacheDir();
// What a clip shows of its file; nullopt for clips without one (titles, compound and multicam clips).
// fileLength: the file's length in project frames (MediaInfo::length; for speed changes, 0 = unknown).
std::optional<Need> need(const Clip& c, FrameRate rate, int fileLength);
// A cached result covering the request
std::optional<Samples> cached(const Need& n);
// Cached or measured now (blocking). progress: 0..1000
std::optional<Samples> analyze(const Need& n, QString* error = nullptr, const std::atomic<bool>* cancel = nullptr,
                               std::atomic<int>* progress = nullptr);

// The clip's targets for Reframe::path (clip frames, sorted), from the samples of its file
QVector<Reframe::Target> targets(const Clip& c, FrameRate rate, int fileLength, const Samples& samples);

// Camera paths (Reframe::path) of clips in a timeline of `format`'s size, ready for Editor::reframeClips: measures what
// each clip shows (or takes it from the cache), blocking. lengthOf(file) = its length in project frames (speed changes).
// progress 0..1000 over all clips. A clip whose file cannot be read gets no path (it stays centered) and its error
// goes to *errors. Cancelled: empty.
QHash<int, QVector<Reframe::Key>> paths(const QVector<Clip>& clips, const ProjectFormat& format, double smoothness,
                                        const std::function<int(const QString&)>& lengthOf, QStringList* errors = nullptr,
                                        const std::atomic<bool>* cancel = nullptr, std::atomic<int>* progress = nullptr);

} // namespace ReframeAnalysis
