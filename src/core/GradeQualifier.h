// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Qualifier of the Color page: a selection by colour (hue, saturation, brightness ranges with soft edges) that limits
// its own correction (the same wheels and sliders as the grade) to the picked colours - a face warmer, a sky bluer.
// One per clip, part of the grade effect like the windows: parameters "q.<key>" ("q.hue", "q.liftY"), animatable as
// dynamic effect parameters. The key is a pure function of the colour after the primary correction and the curves
// (a node after the primary), so it bakes into the grade's 3D table: preview and export, CPU and GPU alike.
// The pixel work is in engine/ColorGrade.
//
// Measures: hue in degrees (0 = red, 120 = green, 240 = blue), saturation = chroma (max - min of R, G, B) in %,
// brightness = Rec. 709 luma in %. A range is fully selected between low and high and fades to nothing over `soft`
// beyond (hue: `width` degrees around `hue`, fading over `hueSoft` degrees; width 360 = every hue).

#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/Types.h"

#include <QColor>
#include <QString>
#include <QStringList>
#include <QVector>

namespace GradeQualifier {

// Layer number on the Color page / in Editor::setGradeKeyframe (after the windows' numbers)
inline constexpr int kLayer = GradeWindows::kMax + 1;

QString key(const QString& name);    // "q.hue"
AnimParam param(const QString& name); // animatable parameter of a key
bool isKey(const QString& gradeKey);  // a "q.…" key of the grade
// Range keys (hue, hueWidth, hueSoft, satLow, satHigh, satSoft, lumLow, lumHigh, lumSoft)
const QStringList& rangeKeys();
// Parameter descriptions for the grade effect (sparse): "invert", the range keys and the correction (same keys,
// defaults and ranges as the grade's own, GradeWindows::correctionKeys)
QVector<EffectParam> params(const QVector<EffectParam>& grade);

bool exists(const Clip& c);
// The shape window the qualifier is limited to (its key times the window's mask), 0 = the whole picture; a window
// that no longer exists counts as 0
int windowOf(const Clip& c);
// New qualifier with the default range (skin tones), written explicitly; the grade must exist. Existing: nothing
void add(Clip& c);
void remove(Clip& c); // its values and keyframes

struct Range {
    double hue = 25, hueWidth = 40, hueSoft = 30;
    double satLow = 8, satHigh = 60, satSoft = 8;
    double lumLow = 15, lumHigh = 90, lumSoft = 10;
    bool invert = false;
    bool operator==(const Range&) const = default;
    // Selection weight 0..1 of a colour (0..1 per channel)
    float weight(float r, float g, float b) const;
};
Range rangeAt(const Clip& c, double t);
// A range around a picked colour (its hue +-15 degrees, its saturation and brightness with some room)
Range around(const QColor& color);
// 1 inside [lo, hi], fading (smoothstep) to 0 over `soft` beyond
float band(double v, double lo, double hi, double soft);
// The hue part of a range's weight at hue h (degrees)
float hueWeight(const Range& r, double h);
// Hue (degrees), saturation (chroma %) and brightness (luma %) of a colour, as the key measures them
void measure(float r, float g, float b, double* hue, double* sat, double* lum);

} // namespace GradeQualifier
