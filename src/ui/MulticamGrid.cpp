// SPDX-License-Identifier: AGPL-3.0-only
#include "ui/MulticamGrid.h"

#include <algorithm>
#include <cmath>

namespace MulticamGrid {

int columns(int angles, int fixed)
{
    if (fixed > 0) return std::min(fixed, kMaxColumns);
    for (int c = 1; c < kMaxColumns; ++c)
        if (angles <= c * c) return c;
    return kMaxColumns;
}

int perPage(int angles, int fixed)
{
    const int c = columns(angles, fixed);
    return c * c;
}

int pages(int angles, int fixed)
{
    const int per = perPage(angles, fixed);
    return std::max(1, (angles + per - 1) / per);
}

QRect cell(const QRect& area, int angles, int index, int fixed)
{
    const int c = columns(angles, fixed);
    if (index < 0 || index >= c * c) return {};
    const int col = index % c, row = index / c;
    // Edges at i * size / c: neighbors share them exactly, sizes differ by at most one pixel
    const int x0 = area.left() + col * area.width() / c, x1 = area.left() + (col + 1) * area.width() / c;
    const int y0 = area.top() + row * area.height() / c, y1 = area.top() + (row + 1) * area.height() / c;
    return QRect(x0, y0, x1 - x0, y1 - y0);
}

int angleAt(const QRect& area, int angles, int page, QPoint pos, int fixed)
{
    if (angles <= 0 || !area.contains(pos)) return -1;
    const int per = perPage(angles, fixed);
    for (int i = 0; i < per; ++i)
        if (cell(area, angles, i, fixed).contains(pos)) {
            const int angle = page * per + i;
            return angle < angles ? angle : -1;
        }
    return -1;
}

QRect fit(const QRect& cell, QSize source)
{
    if (source.isEmpty() || cell.isEmpty()) return {};
    const double scale = std::min(double(cell.width()) / source.width(), double(cell.height()) / source.height());
    const int w = std::clamp(int(std::lround(source.width() * scale)), 1, cell.width());
    const int h = std::clamp(int(std::lround(source.height() * scale)), 1, cell.height());
    return QRect(cell.left() + (cell.width() - w) / 2, cell.top() + (cell.height() - h) / 2, w, h);
}

} // namespace MulticamGrid
