// SPDX-License-Identifier: AGPL-3.0-only
#include "core/GradeQualifier.h"

#include "core/I18n.h"
#include "core/Keyframes.h"

#include <algorithm>
#include <cmath>

namespace GradeQualifier {

namespace {

constexpr const char* kGrade = "grade";

} // namespace

QString key(const QString& name) { return QStringLiteral("q.") + name; }

AnimParam param(const QString& name) { return Keys::effectParam(kGrade, key(name)); }

bool isKey(const QString& gradeKey) { return gradeKey.startsWith(QLatin1String("q.")); }

const QStringList& rangeKeys()
{
    static const QStringList keys = {"hue",    "hueWidth", "hueSoft", "satLow", "satHigh",
                                     "satSoft", "lumLow",  "lumHigh", "lumSoft"};
    return keys;
}

QVector<EffectParam> params(const QVector<EffectParam>& grade)
{
    QVector<EffectParam> out;
    const QString prefix = T("Qualifier") + ' ';
    const auto add = [&](EffectParam p, bool animatable) {
        p.label = prefix + p.label;
        p.key = key(p.key);
        p.anim = animatable ? Keys::effectParam(kGrade, p.key) : AnimParam::Count;
        p.sparse = true;
        out << p;
    };
    const Range def;
    const auto number = [&](const char* k, const QString& label, double value, double min, double max) {
        EffectParam p;
        p.key = k;
        p.label = label;
        p.defaultValue = value;
        p.min = min;
        p.max = max;
        p.step = 0.2;
        p.decimals = 1;
        add(p, true);
    };
    number("hue", T("Farbton"), def.hue, 0, 360);
    number("hueWidth", T("Farbton-Breite"), def.hueWidth, 0, 360);
    number("hueSoft", T("Farbton-Weichheit"), def.hueSoft, 0, 180);
    number("satLow", T("Sättigung von"), def.satLow, 0, 100);
    number("satHigh", T("Sättigung bis"), def.satHigh, 0, 100);
    number("satSoft", T("Sättigung-Weichheit"), def.satSoft, 0, 100);
    number("lumLow", T("Helligkeit von"), def.lumLow, 0, 100);
    number("lumHigh", T("Helligkeit bis"), def.lumHigh, 0, 100);
    number("lumSoft", T("Helligkeit-Weichheit"), def.lumSoft, 0, 100);
    {
        EffectParam p;
        p.key = "invert";
        p.label = T("Invertieren");
        p.type = EffectParam::Bool;
        p.defaultValue = false;
        add(p, false);
    }
    for (const EffectParam& g : grade)
        if (GradeWindows::correctionKeys().contains(g.key)) add(g, true);
    return out;
}

bool exists(const Clip& c)
{
    const EffectInstance* e = EffectRegistry::instance(c, kGrade);
    return e && e->params.contains(key("hue"));
}

void add(Clip& c)
{
    EffectInstance* e = EffectRegistry::instance(c, kGrade);
    if (!e || exists(c)) return;
    // Range written explicitly: the file then does not depend on today's defaults
    const Range r;
    const double values[] = {r.hue, r.hueWidth, r.hueSoft, r.satLow, r.satHigh, r.satSoft, r.lumLow, r.lumHigh, r.lumSoft};
    for (int i = 0; i < rangeKeys().size(); ++i) e->params[key(rangeKeys()[i])] = values[i];
    e->params[key("invert")] = r.invert;
}

void remove(Clip& c)
{
    if (EffectInstance* e = EffectRegistry::instance(c, kGrade))
        e->params.removeIf([](const auto& it) { return isKey(it.key()); });
    c.keys.removeIf([](const auto& it) {
        QString effectId, k;
        return Keys::effectParamOf(it.key(), &effectId, &k) && effectId == QLatin1String(kGrade) && isKey(k);
    });
}

Range rangeAt(const Clip& c, double t)
{
    Range r;
    const auto v = [&](const char* name) { return Keys::valueAt(c, param(name), t); };
    r.hue = v("hue");
    r.hueWidth = std::clamp(v("hueWidth"), 0.0, 360.0);
    r.hueSoft = std::max(0.0, v("hueSoft"));
    r.satLow = v("satLow");
    r.satHigh = v("satHigh");
    r.satSoft = std::max(0.0, v("satSoft"));
    r.lumLow = v("lumLow");
    r.lumHigh = v("lumHigh");
    r.lumSoft = std::max(0.0, v("lumSoft"));
    r.invert = EffectRegistry::value(c, kGrade, key("invert")).toBool();
    return r;
}

void measure(float r, float g, float b, double* hue, double* sat, double* lum)
{
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const float chroma = mx - mn;
    double h = 0;
    if (chroma > 1e-6f) {
        if (mx == r) h = std::fmod((g - b) / chroma + 6.0, 6.0);
        else if (mx == g) h = (b - r) / chroma + 2.0;
        else h = (r - g) / chroma + 4.0;
    }
    *hue = h * 60.0;
    *sat = chroma * 100.0;
    *lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) * 100.0;
}

float band(double v, double lo, double hi, double soft)
{
    if (v >= lo && v <= hi) return 1;
    const double d = v < lo ? lo - v : v - hi;
    if (soft <= 0 || d >= soft) return 0;
    const double f = 1 - d / soft;
    return float(f * f * (3 - 2 * f));
}

float hueWeight(const Range& r, double h)
{
    if (r.hueWidth >= 360) return 1;
    double d = std::fmod(std::abs(h - r.hue), 360.0);
    if (d > 180) d = 360 - d;
    const double half = r.hueWidth / 2;
    // Soft beyond the width, but never wider than the rest of the circle (soft edges meeting at the far side)
    return band(d, 0, half, std::min(r.hueSoft, 180 - half));
}

float Range::weight(float r, float g, float b) const
{
    double h, s, l;
    measure(r, g, b, &h, &s, &l);
    float w = band(s, satLow, satHigh, satSoft) * band(l, lumLow, lumHigh, lumSoft);
    if (w > 0) w *= hueWeight(*this, h);
    return invert ? 1 - w : w;
}

Range around(const QColor& color)
{
    double h, s, l;
    measure(float(color.redF()), float(color.greenF()), float(color.blueF()), &h, &s, &l);
    Range r;
    if (s < 6) { // a gray: every hue, low saturation
        r.hue = 0;
        r.hueWidth = 360;
        r.satLow = 0;
        r.satHigh = s + 8;
    } else {
        r.hue = h;
        r.hueWidth = 30;
        r.hueSoft = 20;
        r.satLow = std::max(0.0, s * 0.5);
        r.satHigh = std::min(100.0, s * 1.6 + 10);
    }
    r.satSoft = 8;
    r.lumLow = std::max(0.0, l - 30);
    r.lumHigh = std::min(100.0, l + 30);
    r.lumSoft = 10;
    return r;
}

} // namespace GradeQualifier
