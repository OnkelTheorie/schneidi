#pragma once
// Color correction of the Color page (like DaVinci "Primaries – Color Wheels" + LUT), effect "grade" on the clip.
// calculation itself (no avfilter): lift may become negative, LUT paths need no FFmpeg escaping (Windows
// "C:\…"), keyframes/before-after without a rebuild. The pixel computation is a pure function (testable without MLT).

#include "core/GradeWindows.h"
#include "core/Types.h"

#include <QString>
#include <QVector>
#include <framework/mlt_types.h>
#include <atomic>
#include <cstdint>
#include <memory>

namespace Mlt {
class Producer;
}

namespace ColorGrade {

inline constexpr const char* EffectId = "grade";

// Values as in DaVinci; wheels: [0] = master (Y), [1..3] = R/G/B
struct Params {
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
};

// Animatable parameters in the order of Params (wheels each Y/R/G/B, then the sliders)
const QVector<AnimParam>& animParams();

// Values at clip frame t (keyframes interpolated); clip without "grade" = neutral
Params at(const Clip& c, double t);
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

// 3D LUT resp. 1D LUT; values R fastest index
struct Lut {
    int size = 0;
    bool is3d = true;
    float domainMin[3] = {0, 0, 0}, domainMax[3] = {1, 1, 1};
    QVector<float> data; // size^3 * 3 (3D) resp. size * 3 (1D)
    // Pre-curve per channel (.csp "preLUT"): control points input -> output 0..1, empty = none
    QVector<float> shaperIn[3], shaperOut[3];
};
// Formats: .cube (Adobe/Resolve), .3dl (Lustre/Flame/Nuke), .csp (cineSpace), Hald CLUT as image (.png/.tif/.bmp),
// For extensions see EffectFolders::lutSuffixes.
// Reads a LUT file (format by extension, also Qt resources ":/…") without a cache; nullptr + error on failure
std::shared_ptr<const Lut> parseLut(const QString& path, QString* error = nullptr);
// Like parseLut, but cached by path + modification time (preview/export)
std::shared_ptr<const Lut> loadLut(const QString& path, QString* error = nullptr);

// Apply the correction to an RGBA image (8 bit, rows without gaps); alpha stays. lut may be nullptr.
void apply(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut);
// Apply a window on top: inside its mask the picture gets the window's correction and fill (blur/pixelate/color),
// mixed by the soft edge. Only the window's area is computed.
void applyWindow(uint8_t* rgba, int width, int height, const Window& w);

// Attach the filter to an excerpt (a = clip frame at the start, for keyframes). bypass: preview switch
// "Before/After" (true = without correction), nullptr = always apply (export). Nothing to do -> no filter.
void attach(Mlt::Producer& cut, const Clip& c, int a, const std::shared_ptr<std::atomic<bool>>& bypass);
// Filter made by attach(): give `live` the state of `fresh` (same clip, new values) while the preview may be
// rendering; false = not both such filters
bool transfer(mlt_filter live, mlt_filter fresh);

} // namespace ColorGrade
