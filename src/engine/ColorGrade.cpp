#include "engine/ColorGrade.h"

#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"

#include <Mlt.h>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QRgba64>
#include <QList>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace ColorGrade {

namespace {

using P = AnimParam;

// Reihenfolge wie Params (Räder je Y/R/G/B, danach die Regler)
const QVector<AnimParam> kParams = {
    P::GradeLiftY,    P::GradeLiftR,    P::GradeLiftG,    P::GradeLiftB,    P::GradeGammaY,   P::GradeGammaR,
    P::GradeGammaG,   P::GradeGammaB,   P::GradeGainY,    P::GradeGainR,    P::GradeGainG,    P::GradeGainB,
    P::GradeOffsetY,  P::GradeOffsetR,  P::GradeOffsetG,  P::GradeOffsetB,  P::GradeContrast, P::GradePivot,
    P::GradeSaturation, P::GradeTemp,   P::GradeTint,     P::GradeExposure,
};

inline double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// Tonwertkurve eines Kanals (c = 0..2 für R/G/B), x = 0..1 (Gamma-kodiert wie das Bild)
struct Curve {
    double mul[3], add[3], lift[3], gain[3], expo[3];
    double contrast, pivot;

    explicit Curve(const Params& p)
    {
        // Weißabgleich (Temperatur/Tönung) als Kanal-Verstärkung, auf gleiche Helligkeit (Rec. 709) normiert
        const double k = p.temperature / 4000.0 * 0.3, n = p.tint / 100.0 * 0.25;
        double wb[3] = {1.0 + k, 1.0 - n, 1.0 - k};
        const double l = 0.2126 * wb[0] + 0.7152 * wb[1] + 0.0722 * wb[2];
        // Belichtung in Blendenstufen: lineares Licht * 2^ev, im Gamma-kodierten Bild (≈ 2,4) also 2^(ev/2,4)
        const double e = std::pow(2.0, p.exposure / 2.4);
        for (int c = 0; c < 3; ++c) {
            mul[c] = std::max(0.0, wb[c] / l) * e;
            add[c] = (p.offset[0] - 25.0) / 100.0 + (p.offset[c + 1] - 25.0) / 100.0;
            lift[c] = p.lift[0] + p.lift[c + 1];
            gain[c] = std::max(0.0, p.gain[0] * p.gain[c + 1]);
            // Gamma +: Mitten heller (Exponent < 1), 0 = unverändert
            expo[c] = std::pow(2.0, -2.0 * (p.gamma[0] + p.gamma[c + 1]));
        }
        contrast = std::max(0.0, p.contrast);
        pivot = p.pivot;
    }
    double operator()(int c, double x) const
    {
        x = x * mul[c] + add[c];                // Weißabgleich, Belichtung, Offset (verschiebt alles)
        x = (x - pivot) * contrast + pivot;     // Kontrast um den Pivot
        x = x + lift[c] * (1.0 - x);            // Lift: Schwarz verschieben, Weiß bleibt
        x = clamp01(x * gain[c]);               // Gain: Weiß verschieben, Schwarz bleibt
        return expo[c] == 1.0 ? x : std::pow(x, expo[c]); // Gamma: Mitten, 0 und 1 bleiben
    }
};

// Vorab-Kurve (.csp) stückweise linear, außerhalb festgehalten
inline float shape(const Lut& lut, int c, float x)
{
    const QVector<float>& in = lut.shaperIn[c];
    if (in.isEmpty()) return x;
    const QVector<float>& out = lut.shaperOut[c];
    if (x <= in.first()) return out.first();
    if (x >= in.last()) return out.last();
    const int i = int(std::upper_bound(in.cbegin(), in.cend(), x) - in.cbegin()); // in[i-1] <= x < in[i]
    const float span = in[i] - in[i - 1];
    return span > 0 ? out[i - 1] + (out[i] - out[i - 1]) * (x - in[i - 1]) / span : out[i];
}

// Trilineare Interpolation in der 3D-LUT (Eingang 0..1 nach Domain umgerechnet)
inline void sample3d(const Lut& lut, float rgb[3])
{
    const int n = lut.size;
    float f[3];
    int i0[3], i1[3];
    for (int c = 0; c < 3; ++c) {
        const float span = lut.domainMax[c] - lut.domainMin[c];
        const float x = shape(lut, c, rgb[c]);
        float v = span > 0 ? (x - lut.domainMin[c]) / span : x;
        v = std::clamp(v, 0.0f, 1.0f) * float(n - 1);
        i0[c] = std::min(int(v), n - 1);
        i1[c] = std::min(i0[c] + 1, n - 1);
        f[c] = v - float(i0[c]);
    }
    const float* d = lut.data.constData();
    auto at = [&](int r, int g, int b, int c) { return d[(size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3 + c]; };
    for (int c = 0; c < 3; ++c) {
        const float c00 = at(i0[0], i0[1], i0[2], c) * (1 - f[0]) + at(i1[0], i0[1], i0[2], c) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], c) * (1 - f[0]) + at(i1[0], i1[1], i0[2], c) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], c) * (1 - f[0]) + at(i1[0], i0[1], i1[2], c) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], c) * (1 - f[0]) + at(i1[0], i1[1], i1[2], c) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        rgb[c] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

inline void sample1d(const Lut& lut, float rgb[3])
{
    const int n = lut.size;
    for (int c = 0; c < 3; ++c) {
        const float span = lut.domainMax[c] - lut.domainMin[c];
        const float x = shape(lut, c, rgb[c]);
        float v = span > 0 ? (x - lut.domainMin[c]) / span : x;
        v = std::clamp(v, 0.0f, 1.0f) * float(n - 1);
        const int i0 = std::min(int(v), n - 1), i1 = std::min(i0 + 1, n - 1);
        const float f = v - float(i0);
        rgb[c] = lut.data[i0 * 3 + c] * (1 - f) + lut.data[i1 * 3 + c] * f;
    }
}

// ---- MLT-Filter ----

struct FilterData {
    Clip clip;      // Kopie: Keyframes werden im Render-Thread gelesen (nur lesend)
    int a = 0;      // Clip-Frame am Anfang des Ausschnitts
    bool animated = false;
    Params fixed;
    std::shared_ptr<const Lut> lut;
    std::shared_ptr<std::atomic<bool>> bypass;
};

constexpr const char* kDataName = "_schneidi_grade";

void destroyData(void* p) { delete static_cast<FilterData*>(p); }

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    auto* d = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), kDataName, nullptr));
    if (!d || (d->bypass && d->bypass->load())) // Vorher/Nachher: Bild unverändert durchreichen
        return mlt_frame_get_image(frame, image, format, width, height, 0);
    *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (err || !*image || *format != mlt_image_rgba) return err;
    apply(*image, *width, *height, d->animated ? at(d->clip, d->a + pos) : d->fixed, d->lut.get());
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

} // namespace

bool Params::isNeutral() const
{
    for (int i = 0; i < 4; ++i)
        if (lift[i] != 0 || gamma[i] != 0 || gain[i] != 1 || offset[i] != 25) return false;
    return contrast == 1 && saturation == 50 && temperature == 0 && tint == 0 && exposure == 0;
}

const QVector<AnimParam>& animParams() { return kParams; }

Params at(const Clip& c, double t)
{
    Params p;
    if (!EffectRegistry::has(c, EffectId)) return p;
    auto v = [&](AnimParam a) { return Keys::valueAt(c, a, t); };
    for (int i = 0; i < 4; ++i) {
        p.lift[i] = v(kParams[i]);
        p.gamma[i] = v(kParams[4 + i]);
        p.gain[i] = v(kParams[8 + i]);
        p.offset[i] = v(kParams[12 + i]);
    }
    p.contrast = v(P::GradeContrast);
    p.pivot = v(P::GradePivot);
    p.saturation = v(P::GradeSaturation);
    p.temperature = v(P::GradeTemp);
    p.tint = v(P::GradeTint);
    p.exposure = v(P::GradeExposure);
    return p;
}

QString lutPath(const Clip& c)
{
    return EffectRegistry::has(c, EffectId) ? EffectRegistry::value(c, EffectId, "lut").toString() : QString();
}

bool active(const Clip& c)
{
    const EffectInstance* e = EffectRegistry::instance(c, EffectId);
    if (!e || !e->enabled) return false;
    if (!lutPath(c).isEmpty()) return true;
    if (std::any_of(kParams.begin(), kParams.end(), [&](AnimParam p) { return Keys::animated(c, p); })) return true;
    return !at(c, 0).isNeutral();
}

namespace {

using LutPtr = std::shared_ptr<Lut>;

LutPtr failLut(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return nullptr;
}

// Zahl einer Datenzeile (QByteArray::toDouble: immer mit Punkt, unabhängig vom Gebietsschema)
bool isNumber(const QByteArray& w)
{
    bool ok = false;
    w.toDouble(&ok);
    return ok;
}

QList<QByteArray> words(const QByteArray& line) { return line.simplified().split(' '); }

// .cube (Adobe/Resolve): LUT_3D_SIZE/LUT_1D_SIZE, DOMAIN_MIN/MAX bzw. LUT_*_INPUT_RANGE, Daten R schnellster Index
LutPtr parseCube(QIODevice& f, QString* error)
{
    auto lut = std::make_shared<Lut>();
    int expected = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> w = words(line);
        const QByteArray key = w.first().toUpper();
        if (isNumber(w.first())) {
            if (w.size() < 3 || !expected) return failLut(error, QStringLiteral("Datenzeile vor der Größenangabe"));
            for (int c = 0; c < 3; ++c) {
                bool okc = false;
                lut->data << float(w.at(c).toDouble(&okc));
                if (!okc) return failLut(error, QStringLiteral("ungültige Zahl"));
            }
            continue;
        }
        if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
            lut->is3d = key == "LUT_3D_SIZE";
            lut->size = w.value(1).toInt();
            if (lut->size < 2 || lut->size > (lut->is3d ? 256 : 65536))
                return failLut(error, QStringLiteral("ungültige Größe"));
            expected = lut->is3d ? lut->size * lut->size * lut->size : lut->size;
            lut->data.reserve(expected * 3);
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            float* dst = key == "DOMAIN_MIN" ? lut->domainMin : lut->domainMax;
            for (int c = 0; c < 3 && c + 1 < w.size(); ++c) dst[c] = float(w.at(c + 1).toDouble());
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") { // Resolve-Variante
            for (int c = 0; c < 3; ++c) {
                lut->domainMin[c] = float(w.value(1).toDouble());
                lut->domainMax[c] = float(w.value(2).toDouble());
            }
        } // TITLE und Unbekanntes überspringen
    }
    if (!expected || lut->data.size() != expected * 3)
        return failLut(error, QStringLiteral("Anzahl der Werte passt nicht zur Größe"));
    return lut;
}

// .3dl (Lustre/Flame/Nuke): optionale Zeile mit dem Eingangsgitter (N Werte), danach N^3 ganzzahlige RGB-Tripel,
// B schnellster Index. Ausgangs-Bittiefe aus „Mesh <ein> <aus>“ oder aus dem größten Wert (10/12/16 Bit).
LutPtr parse3dl(QIODevice& f, QString* error)
{
    int size = 0;
    double outMax = 0;
    QVector<float> raw;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> w = words(line);
        if (!isNumber(w.first())) {
            if (w.first().compare("Mesh", Qt::CaseInsensitive) == 0 && w.size() >= 3)
                outMax = std::pow(2.0, w.at(2).toInt()) - 1;
            continue; // 3DMESH, LUT8, gamma … überspringen
        }
        if (w.size() != 3 && raw.isEmpty() && !size) { // Eingangsgitter: legt die Größe fest
            size = int(w.size());
            continue;
        }
        if (w.size() < 3) return failLut(error, QStringLiteral("ungültige Datenzeile"));
        for (int c = 0; c < 3; ++c) {
            bool ok = false;
            raw << float(w.at(c).toDouble(&ok));
            if (!ok) return failLut(error, QStringLiteral("ungültige Zahl"));
        }
    }
    const int count = int(raw.size() / 3);
    if (!size) size = int(std::lround(std::cbrt(double(count)))); // ohne Gitterzeile
    if (size < 2 || size > 256 || count != size * size * size)
        return failLut(error, QStringLiteral("Anzahl der Werte passt nicht zur Größe"));
    if (outMax <= 0) {
        const float m = raw.isEmpty() ? 0 : *std::max_element(raw.cbegin(), raw.cend());
        outMax = m <= 1.0f ? 1 : m <= 1023 ? 1023 : m <= 4095 ? 4095 : 65535;
    }
    auto lut = std::make_shared<Lut>();
    lut->size = size;
    lut->data.resize(raw.size());
    // B schnellster Index -> R schnellster Index
    for (int r = 0; r < size; ++r)
        for (int g = 0; g < size; ++g)
            for (int b = 0; b < size; ++b) {
                const size_t from = (size_t(r) * size * size + size_t(g) * size + size_t(b)) * 3;
                const size_t to = (size_t(b) * size * size + size_t(g) * size + size_t(r)) * 3;
                for (int c = 0; c < 3; ++c) lut->data[to + c] = float(raw[from + c] / outMax);
            }
    return lut;
}

// .csp (cineSpace): „CSPLUTV100“, „3D“/„1D“, optional METADATA-Block, je Kanal eine Vorab-Kurve
// (Anzahl, Eingänge, Ausgänge), dann 3D: „N N N“ + N^3 Tripel (R schnellster Index) bzw. 1D: Anzahl + Tripel
LutPtr parseCsp(QIODevice& f, QString* error)
{
    if (!f.readLine().trimmed().startsWith("CSPLUTV100")) return failLut(error, QStringLiteral("kein cineSpace-LUT"));
    const QByteArray kind = f.readLine().trimmed().toUpper();
    if (kind != "3D" && kind != "1D") return failLut(error, QStringLiteral("unbekannte LUT-Art"));
    QVector<double> nums;
    bool meta = false;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.toUpper().startsWith("BEGIN METADATA")) meta = true;
        if (meta) {
            if (line.toUpper().startsWith("END METADATA")) meta = false;
            continue;
        }
        for (const QByteArray& w : words(line)) {
            bool ok = false;
            nums << w.toDouble(&ok);
            if (!ok) return failLut(error, QStringLiteral("ungültige Zahl"));
        }
    }
    auto lut = std::make_shared<Lut>();
    lut->is3d = kind == "3D";
    qsizetype i = 0;
    auto next = [&](double* v) {
        if (i >= nums.size()) return false;
        *v = nums[i++];
        return true;
    };
    for (int c = 0; c < 3; ++c) { // Vorab-Kurven
        double n = 0;
        if (!next(&n) || n < 2 || n > 65536) return failLut(error, QStringLiteral("ungültige Vorab-Kurve"));
        QVector<float> in, out;
        for (int k = 0; k < int(n); ++k) {
            double v;
            if (!next(&v)) return failLut(error, QStringLiteral("Vorab-Kurve zu kurz"));
            in << float(v);
        }
        for (int k = 0; k < int(n); ++k) {
            double v;
            if (!next(&v)) return failLut(error, QStringLiteral("Vorab-Kurve zu kurz"));
            out << float(v);
        }
        if (!std::is_sorted(in.cbegin(), in.cend())) return failLut(error, QStringLiteral("ungültige Vorab-Kurve"));
        const bool identity = n == 2 && in[0] == 0 && in[1] == 1 && out[0] == 0 && out[1] == 1;
        if (!identity) {
            lut->shaperIn[c] = in;
            lut->shaperOut[c] = out;
        }
    }
    double n = 0;
    if (!next(&n)) return failLut(error, QStringLiteral("Größenangabe fehlt"));
    if (lut->is3d) {
        double n2 = 0, n3 = 0;
        if (!next(&n2) || !next(&n3) || n != n2 || n != n3)
            return failLut(error, QStringLiteral("nur gleich große Achsen unterstützt"));
    }
    lut->size = int(n);
    if (lut->size < 2 || lut->size > (lut->is3d ? 256 : 65536)) return failLut(error, QStringLiteral("ungültige Größe"));
    const qsizetype expected = (lut->is3d ? qsizetype(lut->size) * lut->size * lut->size : lut->size) * 3;
    if (nums.size() - i != expected) return failLut(error, QStringLiteral("Anzahl der Werte passt nicht zur Größe"));
    lut->data.reserve(expected);
    for (; i < nums.size(); ++i) lut->data << float(nums[i]);
    return lut;
}

// Hald-CLUT: quadratisches Bild L^3 x L^3, Würfelgröße N = L^2, Pixel zeilenweise mit R schnellstem Index
LutPtr parseHald(const QString& path, QString* error)
{
    QImage img(path);
    if (img.isNull()) return failLut(error, QStringLiteral("Bild lässt sich nicht lesen"));
    const int w = img.width();
    const int level = int(std::lround(std::cbrt(double(w))));
    if (img.height() != w || level * level * level != w || level < 2 || level > 16)
        return failLut(error, QStringLiteral("kein Hald-CLUT (quadratisch, Kantenlänge L³)"));
    img = img.convertToFormat(QImage::Format_RGBA64); // 16-Bit-PNGs ohne Verlust
    auto lut = std::make_shared<Lut>();
    lut->size = level * level;
    lut->data.reserve(qsizetype(w) * w * 3);
    for (int y = 0; y < w; ++y) {
        const auto* px = reinterpret_cast<const QRgba64*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x)
            lut->data << px[x].red() / 65535.0f << px[x].green() / 65535.0f << px[x].blue() / 65535.0f;
    }
    return lut;
}

} // namespace

std::shared_ptr<const Lut> parseLut(const QString& path, QString* error)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (!EffectFolders::isLutFile(path)) return failLut(error, QStringLiteral("unbekanntes LUT-Format"));
    if (suffix != "cube" && suffix != "3dl" && suffix != "csp") {
        if (!QFileInfo::exists(path)) return failLut(error, QStringLiteral("Datei fehlt"));
        return parseHald(path, error);
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return failLut(error, f.errorString());
    if (suffix == "3dl") return parse3dl(f, error);
    if (suffix == "csp") return parseCsp(f, error);
    return parseCube(f, error);
}

std::shared_ptr<const Lut> loadLut(const QString& path, QString* error)
{
    struct Entry {
        QDateTime mtime;
        qint64 size = 0;
        std::shared_ptr<const Lut> lut;
    };
    static std::mutex mutex;
    static QHash<QString, Entry> cache;
    const QFileInfo fi(path);
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.constFind(path);
        if (it != cache.cend() && it->mtime == fi.lastModified() && it->size == fi.size()) return it->lut;
    }
    std::shared_ptr<const Lut> lut = parseLut(path, error);
    if (!lut) return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    cache.insert(path, Entry{fi.lastModified(), fi.size(), lut});
    return lut;
}

void apply(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const Curve curve(p);
    const size_t n = size_t(width) * size_t(height);
    const double s = std::max(0.0, p.saturation / 50.0);
    if (!lut && s == 1.0) { // schnell: nur Kanalkurven
        uint8_t table[3][256];
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) table[c][i] = uint8_t(std::lround(curve(c, i / 255.0) * 255.0));
        for (size_t i = 0; i < n; ++i, rgba += 4) {
            rgba[0] = table[0][rgba[0]];
            rgba[1] = table[1][rgba[1]];
            rgba[2] = table[2][rgba[2]];
        }
        return;
    }
    float table[3][256];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) table[c][i] = float(curve(c, i / 255.0));
    const float sf = float(s);
    for (size_t i = 0; i < n; ++i, rgba += 4) {
        float v[3] = {table[0][rgba[0]], table[1][rgba[1]], table[2][rgba[2]]};
        if (sf != 1.0f) { // Sättigung: Mischung mit der Helligkeit (Rec. 709)
            const float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
            for (float& x : v) x = std::clamp(y + (x - y) * sf, 0.0f, 1.0f);
        }
        if (lut) { // LUT nach der Korrektur (wie eine LUT an einem DaVinci-Node)
            if (lut->is3d) sample3d(*lut, v);
            else sample1d(*lut, v);
        }
        for (int c = 0; c < 3; ++c) rgba[c] = uint8_t(std::lround(std::clamp(v[c], 0.0f, 1.0f) * 255.0f));
    }
}

void attach(Mlt::Producer& cut, const Clip& c, int a, const std::shared_ptr<std::atomic<bool>>& bypass)
{
    if (!active(c)) return;
    auto* d = new FilterData;
    d->clip = c;
    d->a = a;
    d->animated = std::any_of(kParams.begin(), kParams.end(), [&](AnimParam p) { return Keys::animated(c, p); });
    d->fixed = at(c, a);
    if (const QString path = lutPath(c); !path.isEmpty()) d->lut = loadLut(path); // fehlt/kaputt: ohne LUT
    d->bypass = bypass;
    if (!d->animated && d->fixed.isNeutral() && !d->lut) {
        delete d;
        return;
    }
    mlt_filter f = mlt_filter_new();
    if (!f) {
        delete d;
        return;
    }
    f->process = process;
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), kDataName, d, 0, destroyData, nullptr);
    Mlt::Filter filter(f); // hält eine eigene Referenz
    mlt_filter_close(f);
    // Position für die Keyframes zählt ab Filter-In (wie bei den anderen Keyframe-Filtern)
    filter.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(filter);
}

} // namespace ColorGrade
