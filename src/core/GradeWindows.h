#pragma once
// Windows of the Color page (like DaVinci "Window": shape masks). A window limits its own correction (the same wheels
// and sliders as the grade) and an optional fill (blur, pixelate, solid color) to a rectangle or an ellipse with a
// soft edge. Windows belong to the clip's grade (effect "grade"), like DaVinci windows belong to a node: copying,
// resetting and switching off the grade includes them. Up to kMax per clip, parameters "w<n>.<key>" in the grade
// effect ("w1.x", "w1.liftY"), animatable as dynamic effect parameters (Keys::effectParam("grade", "w1.x")).
// Pure functions without MLT: the pixel work is in engine/ColorGrade.

#include "core/EffectRegistry.h"
#include "core/Types.h"

#include <QColor>
#include <QRect>
#include <QString>
#include <QVector>

namespace GradeWindows {

inline constexpr int kMax = 4; // windows per clip, numbered 1..kMax

enum class Shape { Rectangle, Ellipse };
enum class Fill { None, Blur, Pixelate, Color };

// Ids in the project file / CLI
const char* shapeId(Shape s);
const char* fillId(Fill f);
bool shapeFromId(const QString& id, Shape* s);
bool fillFromId(const QString& id, Fill* f);

// Parameter key in the grade effect ("w2.x") and its animatable parameter (all keys of the window, see params())
QString key(int n, const QString& name);
AnimParam param(int n, const QString& name);
// Window number and plain key of a grade parameter ("w2.x" -> 2, "x"); 0 = not a window parameter
int windowOf(const QString& gradeKey, QString* name = nullptr);

// Parameter descriptions for the grade effect (all windows; sparse: not written when the grade is created). grade =
// the grade's own parameters: each window gets their correction values (same keys, defaults and ranges).
QVector<EffectParam> params(const QVector<EffectParam>& grade);
// Correction keys of a window, same names and defaults as the grade's (liftY .. exposure, without LUT)
const QStringList& correctionKeys();
// Animatable keys of the window shape (x, y, width, height, rotation, softness) and the fill amount
const QStringList& geometryKeys();

bool exists(const Clip& c, int n);
QVector<int> list(const Clip& c); // existing window numbers, ascending
int freeNumber(const Clip& c);    // first unused number, 0 = all used

// New window n with defaults (centered, 40 % x 40 %); the grade must exist. Existing window: nothing.
void add(Clip& c, int n, Shape shape);
// Removes the window's values and keyframes
void remove(Clip& c, int n);

// Shape at clip frame t (keyframes interpolated). Units as stored: x, y, width, height in % of the picture width/height
// (x/y = center, y downward), rotation in degrees, softness = soft edge outside the shape in % of the picture height.
struct Geometry {
    Shape shape = Shape::Ellipse;
    double x = 50, y = 50, width = 40, height = 40;
    double rotation = 0, softness = 5;
    bool invert = false;
};
Geometry geometryAt(const Clip& c, int n, double t);

struct FillInfo {
    Fill fill = Fill::None;
    double amount = 50; // blur strength (like the blur effect) resp. block size, 0..100
    QColor color = Qt::black;
};
FillInfo fillAt(const Clip& c, int n, double t);

// Mask in pixels of a picture of the given size: weight() is 1 inside, 0 outside, smooth in the soft edge
// (inverted: the other way round). bounds() = pixels where the weight can be > 0 (whole picture when inverted).
class Mask {
public:
    Mask(const Geometry& g, int width, int height);
    float weight(int px, int py) const; // pixel center (px + 0.5, py + 0.5)
    QRect bounds() const { return m_bounds; }
    bool inverted() const { return m_invert; }

private:
    float inside(double lx, double ly) const; // weight without invert in local coordinates
    Shape m_shape;
    double m_cx, m_cy, m_hw, m_hh, m_cos, m_sin, m_soft;
    bool m_invert;
    QRect m_bounds;
};

} // namespace GradeWindows
