// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Shared parts of the `direct` command (CommandsDirect.cpp) and its moment collector (DirectMoments.cpp); `short`
// (CommandsShort.cpp) runs the recipe short through them.

#include "cli/CommandsDetail.h"
#include "core/Direction.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace Cli::detail {

// One moment of one recording, before picking
struct DirectMoment {
    int file = 0;      // index into the material
    double from = 0, to = 0; // seconds
    double score = 0;
    Direction::Reason reason;
    QStringList labels;
    QString source;    // "highlights" (kill feed / loud), "events" (AudioEvents), "picture" (pictureScores)
};

// The moments of one file (args: file, project?, game: id or "none", no_index?): its material line, the highlights'
// candidates (best first, as `highlights` gives them) and, for material that is no game, the moments of sound events
// (and of picture movement when nothing else stood out). A source that fails counts as none (not cached). Every
// source through its cache, the whole answer in AnalysisCache ("direct-moments"); the material index's face/sharpness
// added afterwards when it is cached (MediaIndex::cached, never built here). Throws CANCELED when canceled.
QJsonObject directMomentsFile(const QJsonObject& a, Context& ctx);

struct CollectedMoments {
    QVector<Direction::Material> material;
    QVector<DirectMoment> highlights; // per file in highlights' order, files in order
    QVector<DirectMoment> extra;      // events + picture, best first
};
// directMomentsFile for every file (in parallel, perFile); project may be empty; game: id, "none" or empty (none)
// index: the material index's bonus (not for recipe short)
CollectedMoments collectMoments(const QStringList& files, const QString& project, const QString& game, bool index,
                                Context& ctx);

// The session of a suggest made in memory (not stored): for `short`
// (found: what the collector found, before picking)
Direction::Session directSuggest(const QJsonObject& a, Context& ctx, CollectedMoments* found = nullptr);
// Applies a session as one edit (apply = {on, adjust, order, variant}); args: project?, plan?, dry_run?, and for
// `short` the private _created (the project was made just before: its empty first timeline becomes the cut)
QJsonObject directApply(const Direction::Session& s, const QJsonObject& apply, const QJsonObject& args, Context& ctx);

// What `short` does after its cut, on its vertical timeline, as ONE edit: word captions (style karaoke|word|off: from
// `words` = a `transcribe` words:true answer measured elsewhere, else whisper here when installed - never installed
// from here) and effect sounds (sfx 'mild': whooshes on the cuts, hits on the kills of `game`). done/notes get what
// happened; true = captions were put on (the render burns them in)
struct ShortFinish {
    QString captions = "karaoke";
    QString sfx = "off";
    QString game;     // empty = none
    QString language; // empty = detect
    std::optional<QJsonArray> words;
};
bool shortFinish(const QString& project, const QString& timeline, const ShortFinish& f, Context& ctx, QJsonArray* done,
                 QJsonArray* notes);

} // namespace Cli::detail
