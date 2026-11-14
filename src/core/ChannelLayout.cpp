// SPDX-License-Identifier: AGPL-3.0-only
#include "core/ChannelLayout.h"

#include <algorithm>
#include <cmath>

namespace ChannelLayout {

namespace {
constexpr double kHalfPower = 0.70710678118654752440; // -3 dB
constexpr double kPi = 3.14159265358979323846;
} // namespace

QString name(int channels)
{
    switch (valid(channels)) {
    case 6: return QStringLiteral("5.1");
    case 8: return QStringLiteral("7.1");
    default: return QStringLiteral("stereo");
    }
}

int parse(const QString& text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("stereo") || t == QLatin1String("2.0") || t == QLatin1String("2")) return 2;
    if (t == QLatin1String("5.1") || t == QLatin1String("6") || t == QLatin1String("surround")) return 6;
    if (t == QLatin1String("7.1") || t == QLatin1String("8")) return 8;
    return 0;
}

QString label(int channels, int c)
{
    static const char* const kLabels[] = {"L", "R", "C", "LFE", "Ls", "Rs", "Lss", "Rss"};
    if (c < 0 || c >= valid(channels)) return {};
    if (valid(channels) == 8 && (c == BL || c == BR)) return QLatin1String(c == BL ? "Lrs" : "Rrs");
    return QLatin1String(kLabels[c]);
}

const char* ffmpegName(int channels)
{
    switch (valid(channels)) {
    case 6: return "5.1";
    case 8: return "7.1";
    default: return "stereo";
    }
}

double loudnessWeight(int channels, int c)
{
    const int n = valid(channels);
    if (c < 0 || c >= n) return 0.0;
    if (n == 2) return 1.0;
    if (c == LFE) return 0.0;
    return c >= BL ? 1.41 : 1.0;
}

Matrix conversion(int from, int to)
{
    from = valid(from);
    to = valid(to);
    Matrix m{};
    if (from == to) {
        for (int c = 0; c < from; ++c) m[size_t(c)][size_t(c)] = 1.0;
        return m;
    }
    m[FL][FL] = m[FR][FR] = 1.0;
    if (to == 2) { // ITU-R BS.775 downmix
        m[FL][FC] = m[FR][FC] = kHalfPower;
        m[FL][BL] = m[FR][BR] = kHalfPower;
        if (from == 8) m[FL][SL] = m[FR][SR] = kHalfPower;
        return m;
    }
    if (from == 2) return m; // up: front pair only
    m[FC][FC] = m[LFE][LFE] = 1.0;
    if (from == 6) { // 5.1 -> 7.1: the surround pair stays the back pair
        m[BL][BL] = m[BR][BR] = 1.0;
    } else { // 7.1 -> 5.1
        m[BL][BL] = m[BL][SL] = kHalfPower;
        m[BR][BR] = m[BR][SR] = kHalfPower;
    }
    return m;
}

PanGains surroundPan(int channels, double pan, double depth, double center, double lfeDb)
{
    PanGains g;
    const int n = valid(channels);
    const double p = std::clamp(pan / 100.0, -1.0, 1.0);
    const double sideL = p < 0 ? 1.0 : std::max(0.0, 1.0 - p);
    const double sideR = p < 0 ? std::max(0.0, p + 1.0) : 1.0;
    if (n == 2) {
        g.m[FL][0] = sideL;
        g.m[FR][1] = sideR;
        return g;
    }
    const double d = std::clamp(depth / 100.0, 0.0, 1.0);
    const double c = std::clamp(center / 100.0, 0.0, 1.0);
    // Constant power between neighbouring speaker pairs, exact 0/1 at the ends
    const auto cosine = [](double x) { return x <= 0 ? 1.0 : x >= 1 ? 0.0 : std::cos(x * kPi / 2); };
    const auto sine = [](double x) { return x <= 0 ? 0.0 : x >= 1 ? 1.0 : std::sin(x * kPi / 2); };
    double front = 0, side = 0, back = 0;
    if (n == 6) {
        front = cosine(d);
        back = sine(d);
    } else if (d <= 0.5) {
        front = cosine(2 * d);
        side = sine(2 * d);
    } else {
        side = cosine(2 * d - 1);
        back = sine(2 * d - 1);
    }
    g.m[FL][0] = sideL * front * (1 - c);
    g.m[FR][1] = sideR * front * (1 - c);
    g.m[FC][0] = 0.5 * sideL * front * c;
    g.m[FC][1] = 0.5 * sideR * front * c;
    g.m[BL][0] = sideL * back;
    g.m[BR][1] = sideR * back;
    if (n == 8) {
        g.m[SL][0] = sideL * side;
        g.m[SR][1] = sideR * side;
    }
    g.lfe = lfeDb <= kLfeOff ? 0.0 : 0.5 * std::pow(10.0, std::min(lfeDb, 12.0) / 20.0);
    return g;
}

} // namespace ChannelLayout
