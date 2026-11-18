// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Color correction of the Color page (like DaVinci "Primaries – Color Wheels" + LUT), effect "grade" on the clip.
// calculation itself (no avfilter): lift may become negative, LUT paths need no FFmpeg escaping (Windows
// "C:\…"), keyframes/before-after without a rebuild. The pixel computation is a pure function.

#include "core/GradeQualifier.h"
#include "core/GradeWindows.h"
#include "core/Types.h"

#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <atomic>
#include <cstdint>
#include <memory>


namespace ColorGrade {

inline constexpr const char* EffectId = "grade";

// The primary correction: values as in DaVinci; wheels: [0] = master (Y), [1..3] = R/G/B. Also the correction of a
// window and of the qualifier.
struct Correction {
    double lift[4] = {0, 0, 0, 0};
    double gamma[4] = {0, 0, 0, 0};
    double gain[4] = {1, 1, 1, 1};
    double offset[4] = {25, 25, 25, 25};
    double contrast = 1.0, pivot = 0.435;
    double saturation = 50;  // 0 = gray, 50 = unchanged, 100 = double
    double temperature = 0;  // -4000..4000, + = warmer
    double tint = 0;         // -100..100, + = Magenta
    double exposure = 0;     // in stops
    bool isNeutral() const;
    bool operator==(const Correction&) const = default;
};

// Curves (core/GradeCurves), indexed by GradeCurves::Kind; empty = none
struct Curves {
    QVector<QPointF> points[7];
    bool isIdentity() const;     // all of them do nothing
    bool hasHueCurves() const;   // a hue/brightness vs saturation or hue vs hue curve does something
    bool operator==(const Curves&) const = default;
};

// Qualifier (core/GradeQualifier): its range and the correction inside it
struct Qualifier {
    bool on = false;
    int window = 0; // limited to this shape window (applyQualifierWindow, not in bake), 0 = the whole picture
    GradeQualifier::Range range;
    Correction correction;
    bool operator==(const Qualifier&) const = default;
};

// What the grade shows: the picture, or (preview only, the Color page's "show mask") the qualifier's selection as
// white on black
enum class View { Picture, QualifierMask };

// Everything of the grade that works per pixel (without the windows and the LUT): the primary correction, then the
// tone curves (master Y, then R/G/B), the saturation, the hue curves, the qualifier with its correction
struct Params : Correction {
    Curves curves;
    Qualifier qualifier;
    View view = View::Picture;
    bool isNeutral() const; // nothing changes the picture
    bool operator==(const Params&) const = default;
};

// Animatable parameters in the order of Params (wheels each Y/R/G/B, then the sliders)
const QVector<AnimParam>& animParams();

// Values at clip frame t (keyframes interpolated, with curves and qualifier); clip without "grade" = neutral
Params at(const Clip& c, double t);
// Animatable correction parameters of the qualifier, in the order of Params (like animParams())
const QVector<AnimParam>& qualifierParams();
// Correction of window n (core/GradeWindows) at clip frame t
Params windowAt(const Clip& c, int n, double t);
// Animatable correction parameters of window n, in the order of Params (like animParams())
const QVector<AnimParam>& windowParams(int n);
// Does the clip have an enabled correction that does something (values, keyframes, LUT or windows)?
bool active(const Clip& c);

// A window at one frame: shape, its correction and fill
struct Window {
    GradeWindows::Geometry geometry;
    Params params;
    GradeWindows::FillInfo fill;
    bool isNoop() const { return params.isNeutral() && fill.fill == GradeWindows::Fill::None; }
};
// Windows of the clip at clip frame t (in order; windows that change nothing are left out)
QVector<Window> windowsAt(const Clip& c, double t);
QString lutPath(const Clip& c); // empty = no LUT

struct LutTables; // a LUT prepared for the baked correction (fixed-point nodes, input mapping), see bake()

// 3D LUT resp. 1D LUT; values R fastest index
struct Lut {
    int size = 0;
    bool is3d = true;
    float domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    QVector<float> data; // size^3 * 3 (3D) resp. size * 3 (1D)
    // Pre-curve per channel (.csp "preLUT"): control points input -> output 0..1, empty = none
    QVector<float> shaperIn[3], shaperOut[3];
    // Made by parseLut/loadLut (prepare()); without them (a LUT built by hand) bake() makes its own each time
    std::shared_ptr<const LutTables> tables;
};
// Fills lut.tables (after changing size/data/domain/shaper by hand, so bake() does not redo it per call)
void prepare(Lut& lut);
// Formats: .cube (Adobe/Resolve), .3dl (Lustre/Flame/Nuke), .csp (cineSpace), Hald CLUT as image (.png/.tif/.bmp),
// For extensions see EffectFolders::lutSuffixes.
// Reads a LUT file (format by extension, also Qt resources ":/…") without a cache; nullptr + error on failure
std::shared_ptr<const Lut> parseLut(const QString& path, QString* error = nullptr);
// Like parseLut, but cached by path + modification time + size (preview/export). The file is looked at again at most
// every kLutRecheckMs per path (not per frame); one that cannot be reached any more (a network drive gone) keeps the
// LUT loaded before, a failure is retried once the file is there.
std::shared_ptr<const Lut> loadLut(const QString& path, QString* error = nullptr);
inline constexpr int kLutRecheckMs = 1500;
// The next loadLut of every path looks at its file at once (an export starts, the user picks a LUT)
void revalidateLuts();

// The per-pixel correction (params + LUT) computed once into tables: per channel what depends on one channel
// (exact for 8-bit input), saturation in fixed point, a 3D LUT as fixed-point nodes interpolated trilinearly.
// Cheap to make (~0.1 ms), so animated params get a new one per frame; holds the LUT's tables (a 3D LUT too big
// for them is computed per pixel, then the LUT must outlive the result).
struct Baked;
std::shared_ptr<const Baked> bake(const Params& p, const Lut* lut);
// Apply baked tables to an RGBA image (8 bit, rows without gaps); alpha stays. threads: 0 = a few for big
// pictures, otherwise this many
void apply(uint8_t* rgba, int width, int height, const Baked& b, int threads = 0);
// Apply the correction (= bake + apply). lut may be nullptr.
void apply(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut);
// The same without baking (the previous per-pixel code, floating point with a LUT): the reference for tests and
// benches. The baked result differs from it by at most about 1 code value, and only with a LUT.
void applyReference(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut);
// A qualifier limited to a shape window (p.qualifier.window > 0; bake() leaves it out): its correction (or with
// View::QualifierMask its selection) inside the window's mask `g` (geometry on this picture, see onPicture), on the
// picture after the primary correction and before the LUT
void applyQualifierWindow(uint8_t* rgba, int width, int height, const Params& p, const GradeWindows::Geometry& g);
// The whole grade of a clip (primary, curves, qualifier - in its window or not -, LUT, windows) on its picture.
// frame/fit as for onPicture. The Renderer's CPU path and the stills of the Color page use it.
void applyClip(uint8_t* rgba, int width, int height, const Clip& c, double t, const Params& p, const Lut* lut,
               QSizeF frame, QSizeF fit);

// Apply a window on top: inside its mask the picture gets the window's correction and fill (blur/pixelate/color),
// mixed by the soft edge. Only the window's area is computed.
void applyWindow(uint8_t* rgba, int width, int height, const Window& w);
// A window's geometry is in % of the whole frame, bars included (as the overlay and the tracker see it): the same
// window for a picture that stands for only the fitted part of the frame (`fit`, centred in `frame`), in % of that
// picture. In pixels the shape stays the same (soft edge and fill strength relative to the frame height as well).
Window onPicture(Window w, QSizeF frame, QSizeF fit);

} // namespace ColorGrade
