#include "core/GradeWindows.h"

#include "core/I18n.h"
#include "core/Keyframes.h"

#include <algorithm>
#include <cmath>

namespace GradeWindows {

namespace {

constexpr const char* kGrade = "grade";

struct ShapeName { Shape shape; const char* id; };
constexpr ShapeName kShapes[] = {{Shape::Rectangle, "rectangle"}, {Shape::Ellipse, "ellipse"}};
struct FillName { Fill fill; const char* id; };
constexpr FillName kFills[] = {{Fill::None, "none"}, {Fill::Blur, "blur"}, {Fill::Pixelate, "pixelate"}, {Fill::Color, "color"}};

double num(const Clip& c, int n, const char* name, double t) { return Keys::valueAt(c, param(n, name), t); }

} // namespace

const char* shapeId(Shape s)
{
    for (const auto& x : kShapes)
        if (x.shape == s) return x.id;
    return kShapes[0].id;
}

const char* fillId(Fill f)
{
    for (const auto& x : kFills)
        if (x.fill == f) return x.id;
    return kFills[0].id;
}

bool shapeFromId(const QString& id, Shape* s)
{
    for (const auto& x : kShapes)
        if (id == QLatin1String(x.id)) {
            *s = x.shape;
            return true;
        }
    return false;
}

bool fillFromId(const QString& id, Fill* f)
{
    for (const auto& x : kFills)
        if (id == QLatin1String(x.id)) {
            *f = x.fill;
            return true;
        }
    return false;
}

QString key(int n, const QString& name) { return QStringLiteral("w%1.%2").arg(n).arg(name); }

AnimParam param(int n, const QString& name) { return Keys::effectParam(kGrade, key(n, name)); }

int windowOf(const QString& gradeKey, QString* name)
{
    if (gradeKey.size() < 4 || gradeKey[0] != 'w' || gradeKey[2] != '.') return 0;
    const int n = gradeKey[1].digitValue();
    if (n < 1 || n > kMax) return 0;
    if (name) *name = gradeKey.mid(3);
    return n;
}

const QStringList& correctionKeys()
{
    static const QStringList keys = {"liftY",   "liftR",   "liftG",   "liftB",   "gammaY",     "gammaR",
                                     "gammaG",  "gammaB",  "gainY",   "gainR",   "gainG",      "gainB",
                                     "offsetY", "offsetR", "offsetG", "offsetB", "contrast",   "pivot",
                                     "saturation", "temperature", "tint", "exposure"};
    return keys;
}

const QStringList& geometryKeys()
{
    static const QStringList keys = {"x", "y", "width", "height", "rotation", "softness", "amount"};
    return keys;
}

bool followsContent(AnimParam p)
{
    static const QStringList placement = {"x", "y", "width", "height", "rotation"};
    QString effectId, k, name;
    return Keys::effectParamOf(p, &effectId, &k) && effectId == QLatin1String(kGrade) && windowOf(k, &name) > 0
           && placement.contains(name);
}

QVector<EffectParam> params(const QVector<EffectParam>& grade)
{
    QVector<EffectParam> out;
    for (int n = 1; n <= kMax; ++n) {
        const QString prefix = T("Fenster %1").arg(n) + ' ';
        const auto add = [&](EffectParam p, bool animatable) {
            p.label = prefix + p.label;
            p.key = key(n, p.key);
            p.anim = animatable ? Keys::effectParam(kGrade, p.key) : AnimParam::Count;
            p.sparse = true;
            out << p;
        };
        const auto choice = [&](const char* k, const QString& label, const QString& def, const QStringList& ids) {
            EffectParam p;
            p.key = k;
            p.label = label;
            p.type = EffectParam::Choice;
            p.defaultValue = def;
            p.choices = ids;
            add(p, false);
        };
        const auto number = [&](const char* k, const QString& label, double def, double min, double max, double step,
                                 int decimals) {
            EffectParam p;
            p.key = k;
            p.label = label;
            p.defaultValue = def;
            p.min = min;
            p.max = max;
            p.step = step;
            p.decimals = decimals;
            add(p, true);
        };
        QStringList shapes, fills;
        for (const auto& s : kShapes) shapes << s.id;
        for (const auto& f : kFills) fills << f.id;
        choice("shape", T("Form"), "ellipse", shapes);
        number("x", T("Position X"), 50, -100, 200, 0.1, 2);
        number("y", T("Position Y"), 50, -100, 200, 0.1, 2);
        number("width", T("Breite"), 40, 0, 300, 0.1, 2);
        number("height", T("Höhe"), 40, 0, 300, 0.1, 2);
        number("rotation", T("Drehung"), 0, -360, 360, 0.1, 1);
        number("softness", T("Weichheit"), 5, 0, 100, 0.05, 2);
        {
            EffectParam p;
            p.key = "invert";
            p.label = T("Invertieren");
            p.type = EffectParam::Bool;
            p.defaultValue = false;
            add(p, false);
        }
        choice("fill", T("Füllung"), "none", fills);
        number("amount", T("Stärke"), 50, 0, 100, 0.25, 2);
        {
            EffectParam p;
            p.key = "color";
            p.label = T("Farbe");
            p.type = EffectParam::Color;
            p.defaultValue = QColor(Qt::black);
            add(p, false);
        }
        for (const EffectParam& g : grade)
            if (correctionKeys().contains(g.key)) add(g, true);
    }
    return out;
}

bool exists(const Clip& c, int n)
{
    const EffectInstance* e = EffectRegistry::instance(c, kGrade);
    return e && n >= 1 && n <= kMax && e->params.contains(key(n, "shape"));
}

QVector<int> list(const Clip& c)
{
    QVector<int> out;
    for (int n = 1; n <= kMax; ++n)
        if (exists(c, n)) out << n;
    return out;
}

int freeNumber(const Clip& c)
{
    for (int n = 1; n <= kMax; ++n)
        if (!exists(c, n)) return n;
    return 0;
}

void add(Clip& c, int n, Shape shape)
{
    EffectInstance* e = EffectRegistry::instance(c, kGrade);
    if (!e || n < 1 || n > kMax || exists(c, n)) return;
    // Shape values written explicitly: the file then does not depend on today's defaults
    const Geometry g;
    e->params[key(n, "shape")] = QString(shapeId(shape));
    e->params[key(n, "x")] = g.x;
    e->params[key(n, "y")] = g.y;
    e->params[key(n, "width")] = g.width;
    e->params[key(n, "height")] = g.height;
    e->params[key(n, "rotation")] = g.rotation;
    e->params[key(n, "softness")] = g.softness;
    e->params[key(n, "invert")] = g.invert;
    e->params[key(n, "fill")] = QString(fillId(Fill::None));
}

void remove(Clip& c, int n)
{
    if (EffectInstance* e = EffectRegistry::instance(c, kGrade))
        e->params.removeIf([&](const auto& it) { return windowOf(it.key()) == n; });
    c.keys.removeIf([&](const auto& it) {
        QString effectId, k;
        return Keys::effectParamOf(it.key(), &effectId, &k) && effectId == QLatin1String(kGrade) && windowOf(k) == n;
    });
}

Geometry geometryAt(const Clip& c, int n, double t)
{
    Geometry g;
    shapeFromId(EffectRegistry::value(c, kGrade, key(n, "shape")).toString(), &g.shape);
    g.x = num(c, n, "x", t);
    g.y = num(c, n, "y", t);
    g.width = std::max(0.0, num(c, n, "width", t));
    g.height = std::max(0.0, num(c, n, "height", t));
    g.rotation = num(c, n, "rotation", t);
    g.softness = std::max(0.0, num(c, n, "softness", t));
    g.invert = EffectRegistry::value(c, kGrade, key(n, "invert")).toBool();
    return g;
}

FillInfo fillAt(const Clip& c, int n, double t)
{
    FillInfo f;
    fillFromId(EffectRegistry::value(c, kGrade, key(n, "fill")).toString(), &f.fill);
    f.amount = std::clamp(num(c, n, "amount", t), 0.0, 100.0);
    f.color = EffectRegistry::value(c, kGrade, key(n, "color")).value<QColor>();
    return f;
}

// ---- Mask ----

Mask::Mask(const Geometry& g, int width, int height)
    : m_shape(g.shape), m_invert(g.invert)
{
    const double rad = g.rotation * M_PI / 180.0;
    m_cos = std::cos(rad);
    m_sin = std::sin(rad);
    m_cx = g.x / 100.0 * width;
    m_cy = g.y / 100.0 * height;
    m_hw = std::max(0.5, g.width / 100.0 * width / 2);
    m_hh = std::max(0.5, g.height / 100.0 * height / 2);
    m_soft = std::max(1.0, g.softness / 100.0 * height); // at least one pixel: smooth edge without stair steps
    const QRect all(0, 0, width, height);
    if (m_invert) {
        m_bounds = all;
        return;
    }
    const double ex = std::abs(m_hw * m_cos) + std::abs(m_hh * m_sin) + m_soft + 1;
    const double ey = std::abs(m_hw * m_sin) + std::abs(m_hh * m_cos) + m_soft + 1;
    const int x0 = int(std::floor(m_cx - ex)), y0 = int(std::floor(m_cy - ey));
    const int x1 = int(std::ceil(m_cx + ex)), y1 = int(std::ceil(m_cy + ey));
    m_bounds = QRect(QPoint(x0, y0), QPoint(x1, y1)).intersected(all);
}

float Mask::inside(double lx, double ly) const
{
    double d = 0; // distance outside the shape (pixels)
    if (m_shape == Shape::Rectangle) {
        const double ox = std::abs(lx) - m_hw, oy = std::abs(ly) - m_hh;
        if (ox <= 0 && oy <= 0) return 1;
        d = std::hypot(std::max(ox, 0.0), std::max(oy, 0.0));
    } else {
        const double ux = lx / m_hw, uy = ly / m_hh;
        const double r = std::sqrt(ux * ux + uy * uy);
        if (r <= 1) return 1;
        // Distance to the ellipse ~ (r - 1) / |grad r| (exact for circles, close enough for a soft edge)
        const double gx = ux / m_hw, gy = uy / m_hh;
        const double grad = std::sqrt(gx * gx + gy * gy) / r;
        d = grad > 0 ? (r - 1) / grad : 0;
    }
    if (d >= m_soft) return 0;
    const double f = 1 - d / m_soft;
    return float(f * f * (3 - 2 * f)); // smoothstep
}

float Mask::weight(int px, int py) const
{
    const double dx = px + 0.5 - m_cx, dy = py + 0.5 - m_cy;
    // Rotation counterclockwise on screen (y downward), like the Inspector's rotation
    const double lx = dx * m_cos - dy * m_sin;
    const double ly = dx * m_sin + dy * m_cos;
    const float w = inside(lx, ly);
    return m_invert ? 1 - w : w;
}

} // namespace GradeWindows
