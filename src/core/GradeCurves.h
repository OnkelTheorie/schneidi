// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Curves of the Color page grade: tone curves (master Y, R, G, B: input -> output brightness) and the hue curves
// (hue vs hue, hue vs saturation, brightness vs saturation). Stored in the grade effect as text parameters
// ("curveY" = "0,0 0.4,0.5 1,1": x,y pairs in 0..1, C locale), not animatable. The pixel work is in
// engine/ColorGrade; this is the shape: a monotone cubic through the points (no overshoot, so a curve never bends
// past its points), tone curves flat beyond their first and last point, hue curves wrapping around (0 = 1 = red).
// Hue curves: y = 0.5 is neutral (hue vs hue: +-0.5 = +-180 degrees; the saturation curves: y * 2 = factor).

#include <QPointF>
#include <QString>
#include <QVector>
#include <vector>

namespace GradeCurves {

using Points = QVector<QPointF>;

enum class Kind { Y, R, G, B, HueHue, HueSat, LumSat };
inline constexpr int kCount = 7;

// Parameter key in the grade effect ("curveY", "hueSat")
const char* key(Kind k);
bool fromKey(const QString& key, Kind* k);
// CLI/MCP name ("y", "r", "hue_sat")
const char* cliName(Kind k);
bool fromCliName(const QString& name, Kind* k);
// Hue curves wrap around (x = hue 0..1)
inline bool periodic(Kind k) { return k == Kind::HueHue || k == Kind::HueSat; }
inline bool tone(Kind k) { return k == Kind::Y || k == Kind::R || k == Kind::G || k == Kind::B; }
// Output of the untouched curve at x
inline double neutral(Kind k, double x) { return tone(k) ? x : 0.5; }

// Text form <-> points (sorted by x, clamped to 0..1, points closer than kMinGap in x merged; bad text = none)
Points parse(const QString& text);
QString format(const Points& points);
Points normalized(Points points);
inline constexpr double kMinGap = 0.004;

// The curve does nothing (no points, or every point on the neutral line)
bool isIdentity(const Points& points, Kind k);
// Curve value at x (0..1); identity: neutral(k, x)
double eval(const Points& points, Kind k, double x);
// n + 1 samples over 0..1 (eval at i / n), for the pixel loops
std::vector<float> table(const Points& points, Kind k, int n);

} // namespace GradeCurves
