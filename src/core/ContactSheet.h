// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Contact sheet: still images of a video in a grid, each with its label (timecode), as one picture. The same for the
// app (File › Save Contact Sheet) and `frames --sheet` (lets an AI look at the material).

#include <QImage>
#include <QStringList>
#include <QVector>

namespace ContactSheet {

// count frames spread evenly over [from, to): the middle of each of count equal parts
QVector<int> spread(int from, int to, int count);

// columns 0 = 2–4 depending on the count. Tiles of different sizes (sources with their own crop) share the largest
// cell and are fitted into it, centered.
QImage compose(const QVector<QImage>& images, const QStringList& labels, int columns = 0);

} // namespace ContactSheet
