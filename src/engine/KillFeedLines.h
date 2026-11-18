// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// The lines a game's kill feed shows over time (every line, not only the player's: analyses/highlights read those),
// for the kill feed a reframe keeps at the top of a short (Reframe::Keep::feed): the copy shows only the lines there
// are and is hidden while there are none.
//
// A line is found by its box: games draw each line on a semi-transparent box (CS2: dark, red for the player's own
// death) whose right edge is fixed (GameHud::Game::frameRight) and whose height is known (lineHeight). Per candidate
// top row the columns from the right edge leftwards are tested: the box's top rows differ from the picture just above
// it and its bottom rows from the picture just below, while its top and bottom rows look alike (the box is flat
// from top to bottom, the game picture behind is not cut at exactly these rows); the box ends at the right edge (a
// horizontal edge of the game picture runs on). A run of such columns from the right edge is a line, its end the
// line's left edge. Over a black picture (the death screen) the box does not show: there a line is its bright text
// in the middle rows with dark rows above and below. The first line's top lies in a range (HUD layouts differ), the
// next ones follow `linePitch` apart; the first slot without a line ends the feed.
//
// Sampled 10 times a second over the part of the file a clip shows, at the scale of a 1080p picture. Per file, game,
// content (black bars) and range cached in QStandardPaths::CacheLocation/killfeed. Runs in any thread, cancellable.

#include "core/GameHud.h"
#include "core/ProjectFormat.h"
#include "core/Reframe.h"
#include "core/Types.h"

#include <QHash>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>

namespace KillFeedLines {

struct Sample {
    double seconds = 0; // position in the file
    int lines = 0;
    // The widest line's left edge and the last line's bottom edge, fractions of the game picture (lines > 0)
    double left = 1, bottom = 0;
    bool operator==(const Sample&) const = default;
};
using Samples = QVector<Sample>;

// Can the lines of this game's kill feed be found (its line geometry is known)?
bool canMeasure(const GameHud::Game& game);
// The lines in one picture of the game (the whole game picture, any size); `seconds` stays 0
Sample detect(const QImage& picture, const GameHud::Game& game);

QString cacheDir();

// Cached or measured now (blocking): the samples over [from, to] seconds of `file`, whose game picture is `content`
// (fractions; engine/BarDetect). progress: 0..1000. nullopt = cannot read the file, or cancelled.
std::optional<Samples> measure(const QString& file, double from, double to, const QRectF& content,
                               const GameHud::Game& game, QString* error = nullptr,
                               const std::atomic<bool>* cancel = nullptr, const std::function<void(int)>& progress = {});

// The feed over clip `c` (clip frames, fractions of the whole recording) from samples of its file
Reframe::Feed feedOf(const Clip& c, FrameRate rate, int fileLength, const Samples& samples, const QRectF& content,
                     const GameHud::Game& game);

// The measured feed of every clip that a reframe keeps a kill feed of (Reframe::keepsOf with the same arguments)
// when its game's lines can be found; a clip whose file cannot be read gets none (its copy stays a fixed region) and
// its error goes to *errors. lengthOf(file) = its length in project frames. progress 0..1000 over all clips.
// Cancelled: empty.
QHash<int, Reframe::Feed> feeds(const QVector<Clip>& clips, const ProjectFormat& format, Reframe::Mode mode,
                                const QHash<QString, QRectF>& contents, const Reframe::KeepChoice& choice,
                                const std::function<int(const QString&)>& lengthOf, QStringList* errors = nullptr,
                                const std::atomic<bool>* cancel = nullptr,
                                const std::function<void(int)>& progress = {});
// Gives the kill feed keeps of each clip its feed
void attach(QHash<int, QVector<Reframe::Keep>>& keeps, const QHash<int, Reframe::Feed>& feeds);

} // namespace KillFeedLines
