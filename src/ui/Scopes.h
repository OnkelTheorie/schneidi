#pragma once
#include <QImage>
#include <QPointF>
#include <QVector>
#include <array>

// Video scopes of the Color page (like DaVinci Resolve): Waveform (luma), RGB Parade, Vectorscope, Histogram.
// Pure computation without widgets, safe to run off the UI thread (QImage only, no fonts/painting of text).
// The frame is point-sampled down to at most maxColumns x maxRows pixels before analysis, so a 960x540 preview
// frame costs about a millisecond.
namespace Scopes {

enum class Type { Waveform, Parade, Vectorscope, Histogram };

constexpr int kLevels = 256;     // vertical resolution of waveform/parade, bins of the histogram
constexpr int kVectorSize = 256; // vectorscope grid (kVectorSize x kVectorSize)

// BT.709 luma (full range 0..255) of an 8-bit RGB pixel, rounded
int luma(int r, int g, int b);
// BT.709 colour difference of an RGB colour (components 0..1): cb, cr in -0.5..0.5
void chroma(double r, double g, double b, double* cb, double* cr);
// Vectorscope cell of cb/cr (x = Cb to the right, y = Cr upwards -> row 0 = Cr +0.5)
QPoint vectorCell(double cb, double cr);

struct Data {
    int columns = 0;       // width of waveform/parade (= downsampled frame width)
    int rows = 0;          // sampled rows per column
    int samples = 0;       // analysed pixels (columns * rows)
    QVector<quint32> luma; // waveform: columns * kLevels, index x * kLevels + level
    std::array<QVector<quint32>, 3> rgb;              // parade R, G, B, same layout as luma
    QVector<quint32> vector;                          // kVectorSize^2, index row * kVectorSize + col
    std::array<std::array<quint32, kLevels>, 4> hist{}; // histogram R, G, B, Y

    bool isEmpty() const { return samples == 0; }
    quint32 wave(int x, int level) const { return luma.value(x * kLevels + level); }
    quint32 parade(int channel, int x, int level) const { return rgb[channel].value(x * kLevels + level); }
    quint32 vec(QPoint cell) const { return vector.value(cell.y() * kVectorSize + cell.x()); }
};

Data compute(const QImage& frame, int maxColumns = 480, int maxRows = 270);

// Trace image (transparent background, premultiplied) in data resolution, scaled up by the widget:
// Waveform columns x kLevels, Parade 3*columns x kLevels (R | G | B), Vectorscope kVectorSize^2.
// Level 0 is the bottom row. Histogram has no trace image (drawn as curves) -> null image.
QImage trace(const Data& d, Type type);

// Brightness 0..1 of a cell with count hits out of reference samples (logarithmic, single hits stay visible)
double intensity(quint32 count, double reference);

} // namespace Scopes
