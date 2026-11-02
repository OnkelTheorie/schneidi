// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Black bars baked into a recording: a 4:3 game in a 16:9 video (pillarbox, black left and right), a wide film in a
// 16:9 frame (letterbox, black top and bottom). The result is the content rect, the part of the picture that is not a
// bar, as fractions of the picture as displayed (0..1; the whole picture when there are no bars).
//
// A few pictures spread over the file are shrunk to a gray 320 x 240 copy. A column (row) is a bar when in every one of
// them next to none of its pixels is brighter than black: a dark scene goes bright somewhere in one of the samples, a
// HUD sits inside the game picture, a small overlay in the bar (a few pixels of a column) does not count. Bars are
// taken from the edges inwards only; thinner than 1.5 % of the side they are left (an encoder's edge). A picture that
// is dark all through has no bars (nothing to tell them apart).
//
// Per file (path, size, time), cached in QStandardPaths::CacheLocation/bars and in memory. Runs in any thread.

#include <QRectF>
#include <QString>

#include <atomic>
#include <optional>

namespace BarDetect {

QString cacheDir();
// Cached result, if there is one
std::optional<QRectF> cached(const QString& path);
// Cached or measured now (blocking, about a dozen decoded pictures); nullopt = the file cannot be read (or canceled)
std::optional<QRectF> content(const QString& path, QString* error = nullptr, const std::atomic<bool>* cancel = nullptr);
// Is `content` less than the whole picture?
bool hasBars(const QRectF& content);

} // namespace BarDetect
