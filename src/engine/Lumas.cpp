#include "engine/Lumas.h"

#include "core/EffectFolders.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Lumas {

namespace {

// Wert-Noise (glatt interpoliert) für „Wolken“, fester Startwert -> immer dasselbe Bild
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

// Rohwert je Pixel (beliebiger Bereich, wird danach auf 0..1 gestreckt). u/v: Mitte = 0, Höhe = -1..1 (rund)
bool rawValues(const QString& id, int w, int h, std::vector<double>* out)
{
    out->resize(size_t(w) * h);
    const double aspect = double(w) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double fx = (x + 0.5) / w, fy = (y + 0.5) / h; // 0..1
            const double u = (fx * 2 - 1) * aspect, v = fy * 2 - 1;
            const double angle = std::atan2(u, -v); // 0 = oben, im Uhrzeigersinn
            double t = 0;
            if (id == "kreis") t = std::hypot(u, v);
            else if (id == "kreis-zu") t = -std::hypot(u, v);
            else if (id == "raute") t = std::abs(u) + std::abs(v);
            else if (id == "rechteck") t = std::max(std::abs(u) / aspect, std::abs(v));
            else if (id == "tuer-h") t = std::abs(fx - 0.5);
            else if (id == "tuer-v") t = std::abs(fy - 0.5);
            else if (id == "diagonal") t = fx * aspect + fy;
            else if (id == "uhr") t = angle < 0 ? angle + 2 * M_PI : angle;
            else if (id == "faecher") t = std::abs(angle);
            else if (id == "spirale") {
                const double turns = 2.0;
                double a = (angle < 0 ? angle + 2 * M_PI : angle) / (2 * M_PI);
                const double r = std::hypot(u, v) / std::hypot(aspect, 1.0); // 0..1
                // Arme der Spirale: Winkel + Radius, Reihenfolge von innen nach außen
                t = std::floor(r * turns - a) + a;
                t = t / turns + r * 1e-3;
            } else if (id == "jalousie-h") t = std::fmod(fy * 8, 1.0) + fy * 1e-3;
            else if (id == "jalousie-v") t = std::fmod(fx * 12, 1.0) + fx * 1e-3;
            else if (id == "schachbrett") {
                const int cx = int(fx * 16), cy = int(fy * 9);
                const double inCell = std::fmod(fx * 16, 1.0);
                t = ((cx + cy) % 2) * 1.0 + inCell; // erst jedes zweite Feld, dann die anderen
            } else if (id == "welle") t = fx * aspect + 0.12 * std::sin(fy * 2 * M_PI * 2.5);
            else if (id == "pixel") {
                const int cx = int(fx * 32), cy = int(fy * 18);
                t = hashNoise(cx * 7 + 3, cy * 13 + 1);
            } else if (id == "wolken") {
                double n = 0, amp = 1, f = 3;
                for (int o = 0; o < 5; ++o, amp *= 0.5, f *= 2) n += amp * valueNoise(u * f + 17, v * f + 5);
                t = n;
            } else
                return false;
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
    QImage img(w, h, QImage::Format_Grayscale16); // 16 Bit: weiche, stufenlose Verläufe
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

QString file(const QString& path, int w, int h, bool invert)
{
    const QFileInfo fi(path);
    // "v1": Version der mitgelieferten Verläufe (bei Änderungen an rawValues erhöhen)
    const QByteArray key = "v1|" + path.toUtf8() + '|' + QByteArray::number(fi.lastModified().toMSecsSinceEpoch()) +
                           '|' + QByteArray::number(fi.size());
    const QString hash = QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(12);
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/lumas";
    const QString out = QString("%1/user_%2_%3x%4%5.png").arg(dir, hash).arg(w).arg(h).arg(invert ? "_inv" : "");
    if (QFile::exists(out)) return out;
    const QImage img = image(path, w, h, invert);
    if (img.isNull()) return {};
    QDir().mkpath(dir);
    // Erst unter anderem Namen schreiben, damit eine zweite Instanz nie ein halbes Bild liest
    const QString tmp = out + ".tmp.png";
    if (!img.save(tmp)) return {};
    QFile::remove(out);
    QFile::rename(tmp, out);
    return out;
}

QImage preview(const QImage& luma, double progress)
{
    const QImage g = luma.convertToFormat(QImage::Format_Grayscale8);
    QImage out(g.size(), QImage::Format_RGB32);
    const QColor a(0x3b, 0x6a, 0xa0), b(0xc8, 0x8a, 0x3c); // wie die anderen Übergangssymbole
    const double soft = 0.08;
    for (int y = 0; y < g.height(); ++y) {
        const uchar* src = g.constScanLine(y);
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < g.width(); ++x) {
            // wie MLT luma: Stellen unter dem Fortschritt zeigen schon B, weicher Rand
            const double t = std::clamp((progress * (1 + soft) - src[x] / 255.0) / soft, 0.0, 1.0);
            dst[x] = qRgb(int(a.red() + (b.red() - a.red()) * t), int(a.green() + (b.green() - a.green()) * t),
                          int(a.blue() + (b.blue() - a.blue()) * t));
        }
    }
    return out;
}

} // namespace Lumas
