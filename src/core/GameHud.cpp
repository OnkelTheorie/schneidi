// SPDX-License-Identifier: AGPL-3.0-only
#include "core/GameHud.h"

#include <QDir>
#include <QFileInfo>

#include <cmath>

namespace GameHud {

const QVector<Game>& games()
{
    // CS2 (1080p recordings): kill feed lines from y 0.07 down, ~0.03 apart, right edge at x 0.99; the player's own
    // lines have a thin red frame. Its right part is enough to see a new line: further left lies game picture
    // (red signs, awnings) that a wider region took for kills.
    // Valorant: the player's lines carry a yellow mark at their left.
    static const QVector<Game> list{
        {"cs2", "Counter-Strike 2", QRectF(0.88, 0.06, 0.115, 0.19), "#d8444a", 40, QRectF(0.70, 0.055, 0.30, 0.19)},
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
