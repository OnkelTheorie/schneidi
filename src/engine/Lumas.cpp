// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Lumas.h"

#include "core/EffectFolders.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Lumas {

namespace {

// Value noise (smoothly interpolated) for "clouds", fixed seed -> always the same image
double hashNoise(int x, int y)
{
    quint32 h = quint32(x) * 374761393u + quint32(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return double((h ^ (h >> 16)) & 0xffff) / 65535.0;
}
double valueNoise(double x, double y)
{
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    const double fx = x - xi, fy = y - yi;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = hashNoise(xi, yi), b = hashNoise(xi + 1, yi);
    const double c = hashNoise(xi, yi + 1), d = hashNoise(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

// Raw value per pixel (any range, stretched to 0..1 afterwards). u/v: center = 0, height = -1..1 (round)
enum class Shape { Kreis, KreisZu, Raute, Rechteck, TuerH, TuerV, Diagonal, Uhr, Faecher, Spirale, JalousieH, JalousieV,
                   Schachbrett, Welle, Pixel, Wolken };
bool rawValues(const QString& id, int w, int h, std::vector<double>* out)
{
    // The id is looked up once, not per pixel (a 1080p iris took 75 ms, most of it comparing strings)
    static const std::pair<const char*, Shape> kIds[] = {
        {"kreis", Shape::Kreis}, {"kreis-zu", Shape::KreisZu}, {"raute", Shape::Raute}, {"rechteck", Shape::Rechteck},
        {"tuer-h", Shape::TuerH}, {"tuer-v", Shape::TuerV}, {"diagonal", Shape::Diagonal}, {"uhr", Shape::Uhr},
        {"faecher", Shape::Faecher}, {"spirale", Shape::Spirale}, {"jalousie-h", Shape::JalousieH},
        {"jalousie-v", Shape::JalousieV}, {"schachbrett", Shape::Schachbrett}, {"welle", Shape::Welle},
        {"pixel", Shape::Pixel}, {"wolken", Shape::Wolken}};
    const auto found = std::find_if(std::begin(kIds), std::end(kIds), [&](const auto& e) { return id == QLatin1String(e.first); });
    if (found == std::end(kIds)) return false;
    const Shape shape = found->second;
    out->resize(size_t(w) * h);
    const double aspect = double(w) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double fx = (x + 0.5) / w, fy = (y + 0.5) / h; // 0..1
            const double u = (fx * 2 - 1) * aspect, v = fy * 2 - 1;
            double t = 0;
            switch (shape) {
            case Shape::Kreis: t = std::hypot(u, v); break;
            case Shape::KreisZu: t = -std::hypot(u, v); break;
            case Shape::Raute: t = std::abs(u) + std::abs(v); break;
            case Shape::Rechteck: t = std::max(std::abs(u) / aspect, std::abs(v)); break;
            case Shape::TuerH: t = std::abs(fx - 0.5); break;
            case Shape::TuerV: t = std::abs(fy - 0.5); break;
            case Shape::Diagonal: t = fx * aspect + fy; break;
            case Shape::Uhr: {
                const double angle = std::atan2(u, -v); // 0 = top, clockwise
                t = angle < 0 ? angle + 2 * M_PI : angle;
                break;
            }
            case Shape::Faecher: t = std::abs(std::atan2(u, -v)); break;
            case Shape::Spirale: {
                const double angle = std::atan2(u, -v);
                const double turns = 2.0;
                double a = (angle < 0 ? angle + 2 * M_PI : angle) / (2 * M_PI);
                const double r = std::hypot(u, v) / std::hypot(aspect, 1.0); // 0..1
                // Arms of the spiral: angle + radius, order from inside to outside
                t = std::floor(r * turns - a) + a;
                t = t / turns + r * 1e-3;
                break;
            }
            case Shape::JalousieH: t = std::fmod(fy * 8, 1.0) + fy * 1e-3; break;
            case Shape::JalousieV: t = std::fmod(fx * 12, 1.0) + fx * 1e-3; break;
            case Shape::Schachbrett: {
                const int cx = int(fx * 16), cy = int(fy * 9);
                const double inCell = std::fmod(fx * 16, 1.0);
                t = ((cx + cy) % 2) * 1.0 + inCell; // first every second cell, then the others
                break;
            }
            case Shape::Welle: t = fx * aspect + 0.12 * std::sin(fy * 2 * M_PI * 2.5); break;
            case Shape::Pixel: {
                const int cx = int(fx * 32), cy = int(fy * 18);
                t = hashNoise(cx * 7 + 3, cy * 13 + 1);
                break;
            }
            case Shape::Wolken: {
                double n = 0, amp = 1, f = 3;
                for (int o = 0; o < 5; ++o, amp *= 0.5, f *= 2) n += amp * valueNoise(u * f + 17, v * f + 5);
                t = n;
                break;
            }
            }
            (*out)[size_t(y) * w + x] = t;
        }
    return true;
}

QImage builtin(const QString& id, int w, int h)
{
    std::vector<double> raw;
    if (!rawValues(id, w, h, &raw)) return {};
    const auto [lo, hi] = std::minmax_element(raw.begin(), raw.end());
    const double span = *hi - *lo > 0 ? *hi - *lo : 1;
    QImage img(w, h, QImage::Format_Grayscale16); // 16 bit: soft, stepless gradients
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<quint16*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = quint16(std::lround((raw[size_t(y) * w + x] - *lo) / span * 65535));
    }
    return img;
}

} // namespace

QImage image(const QString& path, int w, int h, bool invert, QString* error)
{
    if (w <= 0 || h <= 0) return {};
    QImage img;
    if (EffectFolders::isBuiltin(path)) {
        img = builtin(EffectFolders::builtinId(path), w, h);
        if (img.isNull() && error) *error = QStringLiteral("unbekannter Übergang");
    } else {
        img = QImage(path);
        if (img.isNull()) {
            if (error) *error = QFileInfo::exists(path) ? QStringLiteral("Bild lässt sich nicht lesen") : QStringLiteral("Datei fehlt");
            return {};
        }
        img = img.convertToFormat(QImage::Format_Grayscale16)
                  .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (!img.isNull() && invert) img.invertPixels();
    return img;
}

QImage preview(const QImage& luma, double progress)
{
    const QImage g = luma.convertToFormat(QImage::Format_Grayscale8);
    QImage out(g.size(), QImage::Format_RGB32);
    const QColor a(0x3b, 0x6a, 0xa0), b(0xc8, 0x8a, 0x3c); // like the other transition icons
    const double soft = 0.08;
    for (int y = 0; y < g.height(); ++y) {
        const uchar* src = g.constScanLine(y);
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < g.width(); ++x) {
            // spots below the progress already show B, soft edge
            const double t = std::clamp((progress * (1 + soft) - src[x] / 255.0) / soft, 0.0, 1.0);
            dst[x] = qRgb(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                          int(a.blue() + (b.blue() - a.blue()) * t));
        }
    }
    return out;
}

} // namespace Lumas
