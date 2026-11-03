// SPDX-License-Identifier: AGPL-3.0-only
#include "core/Generators.h"

#include "core/I18n.h"

#include <algorithm>
#include <cmath>

namespace Generators {

double ease(MotionEase e, double x)
{
    x = std::clamp(x, 0.0, 1.0);
    switch (e) {
    case MotionEase::Linear: return x;
    case MotionEase::In: return x * x * x;
    case MotionEase::Out: {
        const double u = 1 - x;
        return 1 - u * u * u;
    }
    case MotionEase::InOut: {
        if (x < 0.5) return 4 * x * x * x;
        const double u = -2 * x + 2;
        return 1 - u * u * u / 2;
    }
    }
    return x;
}

double counterValue(const TitleStyle& s, int t, int length)
{
    const double x = length > 1 ? double(t) / (length - 1) : 1.0;
    const double v = s.countFrom + (s.countTo - s.countFrom) * ease(s.countEase, x);
    const double k = std::pow(10.0, std::clamp(s.decimals, 0, 6));
    return std::round(v * k) / k;
}

QString formatNumber(double v, int decimals, bool grouping, QChar point, QChar group)
{
    decimals = std::clamp(decimals, 0, 6);
    QString digits = QString::number(std::abs(v), 'f', decimals);
    if (digits.toDouble() == 0) v = 0; // no "-0"
    QString whole = digits.section('.', 0, 0), frac = decimals > 0 ? digits.section('.', 1, 1) : QString();
    if (grouping)
        for (int i = whole.size() - 3; i > 0; i -= 3) whole.insert(i, group);
    QString out = v < 0 ? QStringLiteral("-") : QString();
    out += whole;
    if (decimals > 0) out += point + frac;
    return out;
}

QString counterText(const TitleStyle& s, int t, int length)
{
    const NumberStyleInfo& n = numberStyleInfo(s.numbers);
    return s.prefix + formatNumber(counterValue(s, t, length), s.decimals, s.grouping, n.point, n.group) + s.suffix;
}

NumberStyle numberStyleOfLanguage()
{
    // The user's UI language, not the translation installed here: schneidi-cli answers in English but must store
    // the same default as the app (a counter made on a file and live in the app looked different)
    const QString lang = I18n::language();
    if (I18n::decimalPointOf(lang) == '.') return NumberStyle::CommaPoint;
    return I18n::groupSeparatorOf(lang) == '.' ? NumberStyle::PointComma : NumberStyle::SpaceComma;
}

QString counterWidest(const TitleStyle& s, const std::function<double(const QString&)>& width)
{
    const NumberStyleInfo& n = numberStyleInfo(s.numbers);
    const QChar point = n.point, group = n.group;
    const QString a = s.prefix + formatNumber(s.countFrom, s.decimals, s.grouping, point, group) + s.suffix;
    const QString b = s.prefix + formatNumber(s.countTo, s.decimals, s.grouping, point, group) + s.suffix;
    return width(a) >= width(b) ? a : b;
}

QString counterWidest(const TitleStyle& s, const QFontMetricsF& fm)
{
    return counterWidest(s, [&](const QString& t) { return fm.horizontalAdvance(t); });
}

double progress(const TitleStyle& s, const Clip& c, int f, int begin, int end)
{
    if (s.span == ProgressSpan::Clip) return c.length() > 1 ? std::clamp(double(f - c.start) / (c.length() - 1), 0.0, 1.0) : 1.0;
    return end - begin > 1 ? std::clamp(double(f - begin) / (end - 1 - begin), 0.0, 1.0) : 1.0;
}

QRectF progressTrack(const TitleStyle& s, double W, double H)
{
    const double h = std::clamp(s.barThickness, 1.0, H);
    const double margin = std::clamp(s.barMargin, 0.0, H - h);
    return QRectF(0, s.barTop ? margin : H - margin - h, W, h);
}

QRectF progressFill(const TitleStyle& s, double W, double H, double fraction)
{
    QRectF r = progressTrack(s, W, H);
    r.setWidth(W * std::clamp(fraction, 0.0, 1.0));
    return r;
}

} // namespace Generators
