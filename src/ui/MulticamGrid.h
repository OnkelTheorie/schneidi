#pragma once
// Layout of the multicam grid (docs/plan-0.8.md): square grids 1x1 / 2x2 / 3x3 / 4x4 chosen by the number of angles
// (auto) or fixed, more angles than cells go on further pages. Shared by the grid tractor (engine/TimelineBuilder)
// and the viewer widget (ui/MulticamView), so picture, red frame and click hits line up.

#include <QPoint>
#include <QRect>
#include <QSize>

namespace MulticamGrid {

constexpr int kMaxColumns = 4;

// Columns (= rows) of the grid; fixed > 0 = chosen layout (clamped to 1..4), 0 = auto by the number of angles
int columns(int angles, int fixed = 0);
// Cells per page
int perPage(int angles, int fixed = 0);
// Pages (at least 1)
int pages(int angles, int fixed = 0);
// Cell `index` (0 = top left, row by row) of a page in `area`; the cells tile the area pixel-exactly.
// Null beyond the page.
QRect cell(const QRect& area, int angles, int index, int fixed = 0);
// Angle under `pos` on page `page`; -1 = outside or an empty cell
int angleAt(const QRect& area, int angles, int page, QPoint pos, int fixed = 0);
// Picture of size `source` fitted into `cell` keeping its aspect ratio (centered, bars at the sides or top/bottom);
// null for an empty source
QRect fit(const QRect& cell, QSize source);

} // namespace MulticamGrid
