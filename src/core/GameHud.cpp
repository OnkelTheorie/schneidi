// SPDX-License-Identifier: AGPL-3.0-only
#include "core/GameHud.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace GameHud {

const QVector<Game>& games()
{
    // CS2 (1080p recordings): kill feed lines 31 px tall from y 75 (0.069) down (lower with some HUD layouts: 95),
    // 34 px (0.0315) apart, right edge at x 0.99; the player's own lines have a thin red frame (1 px edges in rows 74
    // and 104, columns 1908-1909). Its right part is enough to see a new line: further left lies game picture (red
    // signs, awnings) that a wider region took for kills when counting the colour. Found by the frame's shape, the
    // area reaches down to y 430 (ten lines: on a full server the feed is longer than five and a new line shows at
    // its bottom; red signs and the hit tint behind do not count there). Shown in a short: x from 0.78 (long names
    // reach 0.81), five lines (the fifth ends at y 241 = 0.223; the text is a little smaller than with four).
    // A line with an assist and two long names starts at about x 0.8 (1585); read from 0.55 for longer names.
    // Valorant: the player's lines carry a yellow mark at their left.
    static const QVector<Game> list{
        {"cs2", "Counter-Strike 2", QRectF(0.88, 0.06, 0.115, 0.19), "#d8444a", 40, QRectF(0.78, 0.06, 0.22, 0.172),
         QRectF(0.88, 0.06, 0.115, 0.34), 30.0 / 1080, 1908.5 / 1920, 0.55},
        {"valorant", "Valorant", QRectF(0.70, 0.04, 0.22, 0.23), "#e7e371", 0, QRectF(0.70, 0.04, 0.295, 0.24)},
    };
    return list;
}

const Game* find(const QString& id)
{
    for (const Game& g : games())
        if (id.trimmed().compare(g.id, Qt::CaseInsensitive) == 0) return &g;
    return nullptr;
}

const Game* guess(const QString& file)
{
    const QFileInfo fi(file);
    const QString dir = fi.absoluteDir().dirName().toLower(), name = fi.fileName().toLower();
    for (const Game& g : games())
        if (dir == g.id || name.startsWith(g.id + "_") || name.startsWith(g.id + "-") || name.startsWith(g.id + " ")) return &g;
    return nullptr;
}

QString regionText(const QRectF& r)
{
    const auto n = [](double v) { return QString::number(std::round(v * 10000) / 10000); };
    return QString("%1,%2,%3,%4").arg(n(r.x()), n(r.y()), n(r.width()), n(r.height()));
}

} // namespace GameHud
