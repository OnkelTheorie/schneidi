// SPDX-License-Identifier: AGPL-3.0-only
#include "core/GradeCurves.h"

#include <algorithm>
#include <cmath>

namespace GradeCurves {

namespace {

struct Name { Kind kind; const char* key; const char* cli; };
constexpr Name kNames[] = {
    {Kind::Y, "curveY", "y"},           {Kind::R, "curveR", "r"},           {Kind::G, "curveG", "g"},
    {Kind::B, "curveB", "b"},           {Kind::HueHue, "hueHue", "hue_hue"}, {Kind::HueSat, "hueSat", "hue_sat"},
    {Kind::LumSat, "lumSat", "lum_sat"},
};

// Tangents of a monotone cubic (Fritsch-Carlson) through xs/ys (strictly increasing xs, at least 2 points)
std::vector<double> tangents(const std::vector<double>& xs, const std::vector<double>& ys)
{
    const size_t n = xs.size();
    std::vector<double> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i) d[i] = (ys[i + 1] - ys[i]) / (xs[i + 1] - xs[i]);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i) m[i] = d[i - 1] * d[i] <= 0 ? 0.0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    return m;
}

double hermite(const std::vector<double>& xs, const std::vector<double>& ys, const std::vector<double>& m, double x)
{
    const size_t i = size_t(std::upper_bound(xs.begin(), xs.end(), x) - xs.begin());
    const size_t k = std::clamp<size_t>(i, 1, xs.size() - 1) - 1; // xs[k] <= x < xs[k + 1]
    const double h = xs[k + 1] - xs[k], t = (x - xs[k]) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * ys[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * ys[k + 1]
           + (t3 - t2) * h * m[k + 1];
}

// The curve prepared for evaluation: knots (hue curves with the neighbours of the wrap) and their tangents
struct Spline {
    std::vector<double> xs, ys, m;
    bool constant = false;
    double value = 0;

    Spline(const Points& points, Kind k)
    {
        if (points.size() == 1) {
            constant = true;
            value = points[0].y();
            return;
        }
        if (periodic(k)) { // one turn before and after: the curve is smooth across red
            Points pts = points;
            if (pts.size() > 1 && pts.last().x() - pts.first().x() > 1 - kMinGap) pts.removeLast(); // 0 and 1 = same hue
            if (pts.size() == 1) {
                constant = true;
                value = pts[0].y();
                return;
            }
            const QPointF last = pts.last();
            const QPointF first = pts.first();
            xs.push_back(last.x() - 1);
            ys.push_back(last.y());
            for (const QPointF& p : pts) {
                xs.push_back(p.x());
                ys.push_back(p.y());
            }
            xs.push_back(first.x() + 1);
            ys.push_back(first.y());
        } else {
            for (const QPointF& p : points) {
                xs.push_back(p.x());
                ys.push_back(p.y());
            }
        }
        m = tangents(xs, ys);
    }
    double operator()(Kind k, double x) const
    {
        if (constant) return value;
        if (!periodic(k)) {
            if (x <= xs.front()) return ys.front();
            if (x >= xs.back()) return ys.back();
        } else {
            x -= std::floor(x);
        }
        return hermite(xs, ys, m, x);
    }
};

} // namespace

const char* key(Kind k)
{
    for (const Name& n : kNames)
        if (n.kind == k) return n.key;
    return kNames[0].key;
}

bool fromKey(const QString& key, Kind* k)
{
    for (const Name& n : kNames)
        if (key == QLatin1String(n.key)) {
            *k = n.kind;
            return true;
        }
    return false;
}

const char* cliName(Kind k)
{
    for (const Name& n : kNames)
        if (n.kind == k) return n.cli;
    return kNames[0].cli;
}

bool fromCliName(const QString& name, Kind* k)
{
    for (const Name& n : kNames)
        if (name == QLatin1String(n.cli)) {
            *k = n.kind;
            return true;
        }
    return false;
}

Points normalized(Points points)
{
    for (QPointF& p : points) {
        p.setX(std::clamp(std::isfinite(p.x()) ? p.x() : 0.0, 0.0, 1.0));
        p.setY(std::clamp(std::isfinite(p.y()) ? p.y() : 0.0, 0.0, 1.0));
    }
    std::stable_sort(points.begin(), points.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
    Points out;
    for (const QPointF& p : points) {
        if (!out.isEmpty() && p.x() - out.last().x() < kMinGap) {
            out.last() = p; // the later one wins (a point dragged onto another)
            continue;
        }
        out << p;
    }
    return out;
}

Points parse(const QString& text)
{
    Points out;
    for (const QString& pair : text.split(' ', Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(',');
        if (xy.size() != 2) return {};
        bool okx = false, oky = false;
        const double x = xy[0].toDouble(&okx), y = xy[1].toDouble(&oky);
        if (!okx || !oky) return {};
        out << QPointF(x, y);
    }
    return normalized(out);
}

QString format(const Points& points)
{
    QStringList parts;
    for (const QPointF& p : normalized(points))
        parts << QString::number(p.x(), 'g', 5) + ',' + QString::number(p.y(), 'g', 5);
    return parts.join(' ');
}

bool isIdentity(const Points& points, Kind k)
{
    if (points.isEmpty()) return true;
    if (tone(k)) { // the line y = x needs its ends there too (one point alone is a flat line)
        if (points.size() < 2) return false;
        return std::all_of(points.begin(), points.end(), [](const QPointF& p) { return std::abs(p.y() - p.x()) < 1e-6; })
               && points.first().x() < 1e-6 && points.last().x() > 1 - 1e-6;
    }
    return std::all_of(points.begin(), points.end(), [](const QPointF& p) { return std::abs(p.y() - 0.5) < 1e-6; });
}

double eval(const Points& points, Kind k, double x)
{
    if (isIdentity(points, k)) return neutral(k, x);
    return std::clamp(Spline(points, k)(k, x), 0.0, 1.0);
}

std::vector<float> table(const Points& points, Kind k, int n)
{
    n = std::max(1, n);
    std::vector<float> out(size_t(n) + 1);
    if (isIdentity(points, k)) {
        for (int i = 0; i <= n; ++i) out[size_t(i)] = float(neutral(k, double(i) / n));
        return out;
    }
    const Spline s(points, k);
    for (int i = 0; i <= n; ++i) out[size_t(i)] = float(std::clamp(s(k, double(i) / n), 0.0, 1.0));
    return out;
}

} // namespace GradeCurves
