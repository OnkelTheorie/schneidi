// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Drawing.h"

#include "core/Keyframes.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Drawing {

namespace {

// v to `per` steps of 1 (a power of ten): divided by an exact number, 0.125 stays 0.125
double rounded(double v, double per) { return std::round(v * per) / per; }

QString colorName(const QColor& c) { return c.alpha() == 255 ? c.name() : c.name(QColor::HexArgb); }

double segLen(QPointF a, QPointF b) { return std::hypot(b.x() - a.x(), b.y() - a.y()); }

double strokeLength(const Placed& s)
{
    if (s.points.size() < 2) return s.widths.isEmpty() ? 1.0 : std::max(1.0, s.widths[0]);
    double l = 0;
    for (int i = 1; i < s.points.size(); ++i) l += segLen(s.points[i - 1], s.points[i]);
    return std::max(l, 1e-6);
}

// The stroke from its start up to `len` of its length
Placed cut(const Placed& s, double len)
{
    if (s.points.size() < 2) return s;
    Placed out = s;
    out.points = {s.points[0]};
    out.widths = {s.widths.value(0)};
    for (int i = 1; i < s.points.size(); ++i) {
        const double l = segLen(s.points[i - 1], s.points[i]);
        if (l >= len) {
            const double k = l > 0 ? len / l : 0;
            out.points << s.points[i - 1] + (s.points[i] - s.points[i - 1]) * k;
            out.widths << s.widths.value(i - 1) + (s.widths.value(i) - s.widths.value(i - 1)) * k;
            return out;
        }
        len -= l;
        out.points << s.points[i];
        out.widths << s.widths.value(i);
    }
    return out;
}

// Distance of p to the segment a-b
double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double l2 = d.x() * d.x() + d.y() * d.y();
    double k = l2 > 0 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / l2 : 0;
    k = std::clamp(k, 0.0, 1.0);
    return segLen(p, a + d * k);
}

} // namespace

QJsonObject strokeJson(const DrawStroke& s)
{
    QJsonArray pts;
    for (const QPointF& p : s.points) pts << rounded(p.x(), 1e5) << rounded(p.y(), 1e5);
    QJsonObject o{{"points", pts}, {"color", colorName(s.color)}, {"width", rounded(s.width, 1e6)}};
    if (std::any_of(s.pressure.begin(), s.pressure.end(), [](float v) { return std::abs(v - 1.0f) > 5e-4f; })) {
        QJsonArray pr;
        for (const float v : s.pressure) pr << rounded(v, 1e3);
        o["pressure"] = pr;
    }
    if (s.opacity < 1) o["opacity"] = rounded(s.opacity, 1e3);
    return o;
}

bool strokeFromJson(const QJsonObject& o, DrawStroke* out, QString* error)
{
    DrawStroke s;
    const QJsonArray pts = o.value("points").toArray();
    for (int i = 0; i + 1 < pts.size(); i += 2) {
        const QJsonValue x = pts[i], y = pts[i + 1];
        if (!x.isDouble() || !y.isDouble()) continue;
        const double px = x.toDouble(), py = y.toDouble();
        if (!std::isfinite(px) || !std::isfinite(py) || px < -1 || px > 2 || py < -1 || py > 2) continue;
        s.points << QPointF(px, py);
    }
    if (s.points.isEmpty()) {
        if (error) *error = "a stroke needs points [x0, y0, x1, y1, …] (0..1 of the frame)";
        return false;
    }
    const QJsonArray pr = o.value("pressure").toArray();
    if (pr.size() == s.points.size())
        for (const QJsonValue& v : pr) s.pressure << float(std::clamp(v.toDouble(1), 0.0, 1.0));
    const QColor c(o.value("color").toString());
    if (c.isValid()) s.color = c;
    s.width = std::clamp(o.value("width").toDouble(s.width), 0.0, 1.0);
    s.opacity = std::clamp(o.value("opacity").toDouble(1), 0.0, 1.0);
    *out = s;
    return true;
}

QJsonObject toJson(const DrawingStyle& d)
{
    QJsonArray strokes;
    for (const DrawStroke& s : d.strokes) strokes << strokeJson(s);
    QJsonObject o{{"strokes", strokes}};
    if (d.reveal > 0) o["reveal"] = d.reveal;
    return o;
}

DrawingStyle fromJson(const QJsonObject& o)
{
    DrawingStyle d;
    for (const QJsonValue& v : o.value("strokes").toArray())
        if (DrawStroke s; strokeFromJson(v.toObject(), &s)) d.strokes << s;
    d.reveal = std::clamp(o.value("reveal").toDouble(0), 0.0, 3600.0);
    return d;
}

Placed placeStroke(const DrawStroke& s, double PW, double PH)
{
    Placed p;
    p.color = s.color;
    p.opacity = std::clamp(s.opacity, 0.0, 1.0);
    const double w = std::max(0.0, s.width) * PH;
    const bool pressure = s.pressure.size() == s.points.size();
    p.points.reserve(s.points.size());
    p.widths.reserve(s.points.size());
    for (int i = 0; i < s.points.size(); ++i) {
        p.points << QPointF(s.points[i].x() * PW, s.points[i].y() * PH);
        p.widths << w * (pressure ? std::clamp(double(s.pressure[i]), 0.0, 1.0) : 1.0);
    }
    return p;
}

QVector<Placed> place(const Clip& c, double t, double PW, double PH, const Masks::Follow* follow)
{
    const QPointF shift(Keys::valueAt(c, AnimParam::TitlePosX, t), -Keys::valueAt(c, AnimParam::TitlePosY, t));
    double fs = 1, fc = 1, fn = 0;
    if (follow) {
        fs = follow->scale > 0 && std::isfinite(follow->scale) ? follow->scale : 1.0;
        const double fa = std::isfinite(follow->angle) ? follow->angle * std::numbers::pi / 180 : 0.0;
        fc = std::cos(fa);
        fn = std::sin(fa);
    }
    QVector<Placed> out;
    out.reserve(c.title.drawing.strokes.size());
    for (const DrawStroke& s : c.title.drawing.strokes) {
        Placed p = placeStroke(s, PW, PH);
        for (QPointF& q : p.points) {
            q += shift;
            if (follow) {
                const QPointF d = (q - follow->from) * fs;
                q = follow->to + QPointF(d.x() * fc - d.y() * fn, d.x() * fn + d.y() * fc);
            }
        }
        if (fs != 1)
            for (double& w : p.widths) w *= fs;
        out << p;
    }
    return out;
}

double revealFraction(const DrawingStyle& d, double t, double fps)
{
    if (d.reveal <= 0 || fps <= 0) return 1;
    return std::clamp(t / (d.reveal * fps), 0.0, 1.0);
}

QVector<Placed> revealed(const QVector<Placed>& strokes, double fraction)
{
    if (fraction >= 1) return strokes;
    QVector<Placed> out;
    if (fraction <= 0) return out;
    double total = 0;
    for (const Placed& s : strokes) total += strokeLength(s);
    double left = total * fraction;
    for (const Placed& s : strokes) {
        const double l = strokeLength(s);
        if (l <= left) {
            out << s;
            left -= l;
            continue;
        }
        if (s.points.size() >= 2 && left > 0) out << cut(s, left);
        break;
    }
    return out;
}

double strokeDoneAt(const DrawingStyle& d, int i, double PW, double PH, double fps)
{
    if (i < 0 || i >= d.strokes.size()) return -1;
    if (d.reveal <= 0) return 0;
    double total = 0, upTo = 0;
    for (int k = 0; k < d.strokes.size(); ++k) {
        const double l = strokeLength(placeStroke(d.strokes[k], PW, PH));
        total += l;
        if (k <= i) upTo += l;
    }
    return total > 0 ? upTo / total * d.reveal * fps : 0;
}

QRectF bounds(const QVector<Placed>& strokes)
{
    QRectF r;
    for (const Placed& s : strokes)
        for (int i = 0; i < s.points.size(); ++i) {
            const double h = s.widths.value(i) / 2 + 1;
            const QRectF b(s.points[i].x() - h, s.points[i].y() - h, 2 * h, 2 * h);
            r = r.isNull() ? b : r.united(b);
        }
    return r;
}

void smooth(DrawStroke& s, double PW, double PH, double minPixels)
{
    const bool pressure = s.pressure.size() == s.points.size();
    QVector<QPointF> pts;
    QVector<float> pr;
    for (int i = 0; i < s.points.size(); ++i) {
        const QPointF p = s.points[i];
        // the last point always stays (where the pen left the picture)
        if (!pts.isEmpty() && i + 1 < s.points.size()
            && std::hypot((p.x() - pts.last().x()) * PW, (p.y() - pts.last().y()) * PH) < minPixels)
            continue;
        if (!pts.isEmpty() && i + 1 == s.points.size() && pts.size() >= 2
            && std::hypot((p.x() - pts.last().x()) * PW, (p.y() - pts.last().y()) * PH) < minPixels) {
            pts.last() = p; // the end replaces a point too close before it
            if (pressure) pr.last() = s.pressure[i];
            continue;
        }
        pts << p;
        if (pressure) pr << s.pressure[i];
    }
    for (int pass = 0; pass < 2 && pts.size() > 2; ++pass) {
        QVector<QPointF> next = pts;
        QVector<float> nextPr = pr;
        for (int i = 1; i + 1 < pts.size(); ++i) {
            next[i] = (pts[i - 1] + 2 * pts[i] + pts[i + 1]) / 4;
            if (pressure) nextPr[i] = (pr[i - 1] + 2 * pr[i] + pr[i + 1]) / 4;
        }
        pts = next;
        pr = nextPr;
    }
    s.points = pts;
    s.pressure = pressure ? pr : QVector<float>();
}

QVector<int> hits(const DrawingStyle& d, const QVector<QPointF>& path, double radius, double PW, double PH)
{
    QVector<int> out;
    if (path.isEmpty()) return out;
    for (int i = 0; i < d.strokes.size(); ++i) {
        const Placed s = placeStroke(d.strokes[i], PW, PH);
        bool hit = false;
        for (int k = 0; k < s.points.size() && !hit; ++k) {
            const double reach = radius + s.widths.value(k) / 2;
            // the stroke's segment k-1..k against each eraser point; its point k against each eraser segment
            for (int e = 0; e < path.size() && !hit; ++e) {
                const QPointF a = k > 0 ? s.points[k - 1] : s.points[k];
                if (segDist(path[e], a, s.points[k]) <= reach) hit = true;
                else if (e > 0 && segDist(s.points[k], path[e - 1], path[e]) <= reach) hit = true;
            }
        }
        if (hit) out << i;
    }
    return out;
}

void scaleFormat(DrawingStyle& d, double fromW, double fromH, double toW, double toH, double s)
{
    if (fromW <= 0 || fromH <= 0 || toW <= 0 || toH <= 0 || s <= 0) return;
    const double kx = fromW * s / toW, ky = fromH * s / toH, kw = fromH * s / toH;
    if (std::abs(kx - 1) < 1e-12 && std::abs(ky - 1) < 1e-12) return;
    for (DrawStroke& st : d.strokes) {
        for (QPointF& p : st.points) p = QPointF(0.5 + (p.x() - 0.5) * kx, 0.5 + (p.y() - 0.5) * ky);
        st.width *= kw;
    }
}

} // namespace Drawing
