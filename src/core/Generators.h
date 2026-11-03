// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Title generators beyond plain text (TitleKind): the animated counter's number at a frame and the progress bar's
// fill and place, plus the speed curves (MotionEase) transitions and counters share. Pure data functions in project
// pixels; the picture engine draws them (engine/picture/Renderer drawTitle), `check` looks at the bar's place.

#include "core/Types.h"

#include <QChar>
#include <QFontMetricsF>

#include <functional>
#include <QRectF>
#include <QString>

namespace Generators {

// Position 0..1 of a motion at linear time x (0..1, clamped) along the curve (cubic: In x^3, Out 1 - (1-x)^3)
double ease(MotionEase e, double x);

// Counter: the value at clip frame t of a clip `length` frames long (first frame countFrom, last frame countTo, along
// countEase), rounded to `decimals` places
double counterValue(const TitleStyle& s, int t, int length);
// A number with `decimals` places, the decimal point and (grouping) thousands separator given
QString formatNumber(double v, int decimals, bool grouping, QChar point, QChar group);
// Counter text at clip frame t: prefix + number + suffix, separators by the title's `numbers` (not the UI language:
// the same project renders the same everywhere)
QString counterText(const TitleStyle& s, int t, int length);
// Separators of the UI language (for a new counter)
NumberStyle numberStyleOfLanguage();
// The widest text the counter shows (its first or last value): the block keeps this size while the number grows
QString counterWidest(const TitleStyle& s, const QFontMetricsF& fm);
// The same with any width measure (e.g. a font-independent estimate)
QString counterWidest(const TitleStyle& s, const std::function<double(const QString&)>& width);

// Progress bar: how full it is (0..1) at timeline frame f for a clip `c` in a video running from `begin` to `end`
// (exclusive; the whole timeline, or the exported range: the bar fills over what the viewer gets): Timeline span =
// from its first to its last frame, Clip span = over the clip
double progress(const TitleStyle& s, const Clip& c, int f, int begin, int end);
// The track (whole bar) and its filled part in project pixels of a W x H picture
QRectF progressTrack(const TitleStyle& s, double W, double H);
QRectF progressFill(const TitleStyle& s, double W, double H, double fraction);

} // namespace Generators
