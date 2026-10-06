#include "ui/Scopes.h"

#include "app/Theme.h"

#include <algorithm>
#include <cmath>

namespace Scopes {

namespace {
constexpr double kWr = 0.2126, kWg = 0.7152, kWb = 0.0722; // BT.709

// Integer luma weights (sum 65536) for the inner loop
constexpr int kIr = 13933, kIg = 46871, kIb = 4732;
static_assert(kIr + kIg + kIb == 65536);

// Colour difference scale for 8-bit values: cb = (b - y) / 1.8556 / 255 -> cell = 128 + cb * 255
constexpr double kCb = 1.0 / 1.8556, kCr = 1.0 / 1.5748;

inline int lumaFast(int r, int g, int b) { return (kIr * r + kIg * g + kIb * b + 32768) >> 16; }

inline int cell(double v) { return std::clamp(int(std::floor(128.0 + v * 255.0)), 0, kVectorSize - 1); }

// Trace pixel: colour with brightness i (premultiplied ARGB)
inline QRgb tracePixel(const QColor& c, double i)
{
    const int a = std::clamp(int(i * 255 + 0.5), 0, 255);
    return qRgba(c.red() * a / 255, c.green() * a / 255, c.blue() * a / 255, a);
}
} // namespace

int luma(int r, int g, int b) { return lumaFast(r, g, b); }

void chroma(double r, double g, double b, double* cb, double* cr)
{
    const double y = kWr * r + kWg * g + kWb * b;
    *cb = (b - y) * kCb;
    *cr = (r - y) * kCr;
}

QPoint vectorCell(double cb, double cr) { return {cell(cb), kVectorSize - 1 - cell(cr)}; }

double intensity(quint32 count, double reference)
{
    if (count == 0) return 0;
    reference = std::max(reference, 1.0);
    return std::min(1.0, 0.22 + 0.78 * std::log1p(double(count)) / std::log1p(reference));
}

Data compute(const QImage& frame, int maxColumns, int maxRows)
{
    Data d;
    if (frame.isNull() || frame.width() <= 0 || frame.height() <= 0) return d;
    QImage img = frame;
    bool bytesRgba = false; // RGBA8888 byte order, else 0xAARRGGBB words
    switch (img.format()) {
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888_Premultiplied:
        bytesRgba = true;
        break;
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        break;
    default:
        img = img.convertToFormat(QImage::Format_RGB32);
    }
    const int stepX = std::max(1, (img.width() + maxColumns - 1) / std::max(1, maxColumns));
    const int stepY = std::max(1, (img.height() + maxRows - 1) / std::max(1, maxRows));
    d.columns = (img.width() + stepX - 1) / stepX;
    d.rows = (img.height() + stepY - 1) / stepY;
    d.samples = d.columns * d.rows;
    d.luma.fill(0, d.columns * kLevels);
    for (auto& ch : d.rgb) ch.fill(0, d.columns * kLevels);
    d.vector.fill(0, kVectorSize * kVectorSize);

    quint32* wl = d.luma.data();
    quint32* wr = d.rgb[0].data();
    quint32* wg = d.rgb[1].data();
    quint32* wb = d.rgb[2].data();
    quint32* vec = d.vector.data();
    // Vectorscope cells per 8-bit difference (b - y), (r - y): lookup instead of floating point per pixel
    std::array<int, 511> cbCell, crCell;
    for (int i = 0; i < 511; ++i) {
        cbCell[i] = cell((i - 255) / 255.0 * kCb);
        crCell[i] = kVectorSize - 1 - cell((i - 255) / 255.0 * kCr);
    }
    for (int y = 0, row = 0; y < img.height(); y += stepY, ++row) {
        const uchar* line = img.constScanLine(y);
        for (int x = 0, col = 0; x < img.width(); x += stepX, ++col) {
            int r, g, b;
            if (bytesRgba) {
                const uchar* p = line + x * 4;
                r = p[0];
                g = p[1];
                b = p[2];
            } else {
                const QRgb p = reinterpret_cast<const QRgb*>(line)[x];
                r = qRed(p);
                g = qGreen(p);
                b = qBlue(p);
            }
            const int l = lumaFast(r, g, b);
            const int base = col * kLevels;
            ++wl[base + l];
            ++wr[base + r];
            ++wg[base + g];
            ++wb[base + b];
            ++d.hist[0][r];
            ++d.hist[1][g];
            ++d.hist[2][b];
            ++d.hist[3][l];
            ++vec[crCell[r - l + 255] * kVectorSize + cbCell[b - l + 255]];
        }
    }
    return d;
}

QImage trace(const Data& d, Type type)
{
    if (d.isEmpty() || type == Type::Histogram) return {};
    if (type == Type::Vectorscope) {
        QImage img(kVectorSize, kVectorSize, QImage::Format_ARGB32_Premultiplied);
        // A cell "full" at 0.2 % of all samples (a flat colour fills one cell completely)
        const double ref = std::max(4.0, d.samples * 0.002);
        for (int y = 0; y < kVectorSize; ++y) {
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < kVectorSize; ++x)
                line[x] = tracePixel(Theme::scopeTrace, intensity(d.vector[y * kVectorSize + x], ref));
        }
        return img;
    }
    const int panels = type == Type::Parade ? 3 : 1;
    QImage img(d.columns * panels, kLevels, QImage::Format_ARGB32_Premultiplied);
    // A cell "full" when a tenth of the column hits it
    const double ref = std::max(2.0, d.rows * 0.1);
    for (int p = 0; p < panels; ++p) {
        const QVector<quint32>& src = type == Type::Parade ? d.rgb[p] : d.luma;
        const QColor& color = type == Type::Waveform ? Theme::scopeTrace
                              : p == 0              ? Theme::scopeRed
                              : p == 1              ? Theme::scopeGreen
                                                    : Theme::scopeBlue;
        for (int level = 0; level < kLevels; ++level) {
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(kLevels - 1 - level)) + p * d.columns;
            for (int x = 0; x < d.columns; ++x) line[x] = tracePixel(color, intensity(src[x * kLevels + level], ref));
        }
    }
    return img;
}

} // namespace Scopes
