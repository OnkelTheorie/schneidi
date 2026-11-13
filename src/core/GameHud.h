// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Where known games show their kill feed, for the analyses (highlights: kills from the player's own kill feed lines)
// and for Auto-Reframe (the kill feed kept readable above a 9:16 crop that cuts it away). Regions are fractions of
// the game's picture (16:9 recordings: the whole picture; with black bars around a 4:3 game: the part between them).

#include <QRectF>
#include <QString>
#include <QVector>

namespace GameHud {

struct Game {
    QString id;   // "cs2" (CLI, settings)
    QString name; // "Counter-Strike 2"
    // The part of the kill feed where the player's own lines show their mark, and the mark's colour: as small as the
    // lines allow, so the game picture behind (a red sign, a menu) is not counted as a kill
    QRectF mark;
    QString color;
    int tolerance = 0; // largest difference per channel; 0 = the analysis' default
    // The whole kill feed (several lines under each other, right-aligned): what a short keeps readable
    QRectF feed;
    // Lines framed by a thin outline in the mark's colour (CS2): found by that shape instead of the amount of colour,
    // in `frameArea` (the feed's whole height: the game picture behind does not count). Top edge to bottom edge as
    // part of the picture height (0 = no frame: the colour is counted), the right edge's x as part of the width
    QRectF frameArea;
    double frameHeight = 0;
    double frameRight = 0;
    // How far left a framed line can reach (long names, an assist: "A + B  weapon  C"), part of the width: the names'
    // team colours are read up to there (a teamkill: the victim in the player's own team colour)
    double frameLeft = 0;
    // Every line of the feed (the player's and everyone else's): a box of `lineHeight` (part of the picture height)
    // whose right edge is at `frameRight`, the first one's top between `firstLineMin` and `firstLineMax` (HUD layouts
    // differ), the next ones `linePitch` below each other. 0 = not known: the kept kill feed stays a fixed region.
    // Read by engine/KillFeedLines, so the kept copy in a short shows only the lines that are there.
    double lineHeight = 0;
    double linePitch = 0;
    double firstLineMin = 0, firstLineMax = 0;
};

const QVector<Game>& games();
const Game* find(const QString& id); // case-insensitive; nullptr = unknown
// The game from the folder (a clip tool's "cs2/") or the file name ("cs2_clip_…", "valorant-…"); nullptr = none
const Game* guess(const QString& file);
// "x,y,w,h" with up to four decimals (CLI arguments and output)
QString regionText(const QRectF& r);

} // namespace GameHud
