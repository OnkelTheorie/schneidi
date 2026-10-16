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
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

namespace ColorGrade {

namespace {

using P = AnimParam;

// Order like Params (wheels each Y/R/G/B, then the sliders)
const QVector<AnimParam> kParams = {
    P::GradeLiftY,    P::GradeLiftR,    P::GradeLiftG,    P::GradeLiftB,    P::GradeGammaY,   P::GradeGammaR,
    P::GradeGammaG,   P::GradeGammaB,   P::GradeGainY,    P::GradeGainR,    P::GradeGainG,    P::GradeGainB,
    P::GradeOffsetY,  P::GradeOffsetR,  P::GradeOffsetG,  P::GradeOffsetB,  P::GradeContrast, P::GradePivot,
    P::GradeSaturation, P::GradeTemp,   P::GradeTint,     P::GradeExposure,
};

inline double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// Tone curve of a channel (c = 0..2 for R/G/B), x = 0..1 (gamma-encoded like the image)
struct Curve {
    double mul[3], add[3], lift[3], gain[3], expo[3];
    double contrast, pivot;

    explicit Curve(const Params& p)
    {
        // White balance (temperature/tint) as channel gain, normalized to equal brightness (Rec. 709)
        const double k = p.temperature / 4000.0 * 0.3, n = p.tint / 100.0 * 0.25;
        double wb[3] = {1.0 + k, 1.0 - n, 1.0 - k};
        const double l = 0.2126 * wb[0] + 0.7152 * wb[1] + 0.0722 * wb[2];
        // Exposure in stops: linear light * 2^ev, in the gamma-encoded image (≈ 2.4) thus 2^(ev/2.4)
        const double e = std::pow(2.0, p.exposure / 2.4);
        for (int c = 0; c < 3; ++c) {
            mul[c] = std::max(0.0, wb[c] / l) * e;
            add[c] = (p.offset[0] - 25.0) / 100.0 + (p.offset[c + 1] - 25.0) / 100.0;
            lift[c] = p.lift[0] + p.lift[c + 1];
            gain[c] = std::max(0.0, p.gain[0] * p.gain[c + 1]);
            // Gamma +: midtones brighter (exponent < 1), 0 = unchanged
            expo[c] = std::pow(2.0, -2.0 * (p.gamma[0] + p.gamma[c + 1]));
        }
        contrast = std::max(0.0, p.contrast);
        pivot = p.pivot;
    }
    double operator()(int c, double x) const
    {
        x = x * mul[c] + add[c];                // white balance, exposure, offset (shifts everything)
        x = (x - pivot) * contrast + pivot;     // Contrast around the pivot
        x = x + lift[c] * (1.0 - x);            // Lift: shift black, white stays
        x = clamp01(x * gain[c]);               // Gain: shift white, black stays
        return expo[c] == 1.0 ? x : std::pow(x, expo[c]); // Gamma: midtones, 0 and 1 stay
    }
};

// Pre-curve (.csp) piecewise linear, held outside
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

// Trilinear interpolation in the 3D LUT (input 0..1 converted to domain)
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

// ---- MLT filter ----

struct State {
    Clip clip;      // copy: keyframes are read in the render thread (read only)
    int a = 0;      // clip frame at the start of the cut
    bool animated = false;
    bool windows = false; // has windows (computed per frame)
    Params fixed;
    std::shared_ptr<const Lut> lut;
    std::shared_ptr<std::atomic<bool>> bypass;
};

// The state can be swapped while the preview renders (transfer(), Inspector changes without a rebuild)
struct FilterData {
    std::atomic<std::shared_ptr<const State>> state;
};

constexpr const char* kDataName = "_schneidi_grade";

void destroyData(void* p) { delete static_cast<FilterData*>(p); }

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    auto* fd = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(filter), kDataName, nullptr));
    const std::shared_ptr<const State> d = fd ? fd->state.load() : nullptr;
    if (!d || (d->bypass && d->bypass->load())) // before/after: pass the image through unchanged
        return mlt_frame_get_image(frame, image, format, width, height, 0);
    *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (err || !*image || *format != mlt_image_rgba) return err;
    const Params p = d->animated ? at(d->clip, d->a + pos) : d->fixed;
    if (!p.isNeutral() || d->lut) apply(*image, *width, *height, p, d->lut.get());
    if (d->windows)
        for (const Window& w : windowsAt(d->clip, d->a + pos)) applyWindow(*image, *width, *height, w);
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

namespace {

// Values of the parameters `list` (order like kParams) at clip frame t
Params paramsAt(const Clip& c, const QVector<AnimParam>& list, double t)
{
    Params p;
    if (!EffectRegistry::has(c, EffectId)) return p;
    auto v = [&](int i) { return Keys::valueAt(c, list[i], t); };
    for (int i = 0; i < 4; ++i) {
        p.lift[i] = v(i);
        p.gamma[i] = v(4 + i);
        p.gain[i] = v(8 + i);
        p.offset[i] = v(12 + i);
    }
    p.contrast = v(16);
    p.pivot = v(17);
    p.saturation = v(18);
    p.temperature = v(19);
    p.tint = v(20);
    p.exposure = v(21);
    return p;
}

} // namespace

Params at(const Clip& c, double t) { return paramsAt(c, kParams, t); }

const QVector<AnimParam>& windowParams(int n)
{
    static const auto table = [] {
        std::array<QVector<AnimParam>, GradeWindows::kMax + 1> t;
        for (int w = 1; w <= GradeWindows::kMax; ++w)
            for (const QString& k : GradeWindows::correctionKeys()) t[w] << GradeWindows::param(w, k);
        return t;
    }();
    return table[std::clamp(n, 0, GradeWindows::kMax)];
}

Params windowAt(const Clip& c, int n, double t)
{
    if (n < 1 || n > GradeWindows::kMax) return {};
    return paramsAt(c, windowParams(n), t);
}

QVector<Window> windowsAt(const Clip& c, double t)
{
    QVector<Window> out;
    for (int n : GradeWindows::list(c)) {
        Window w{GradeWindows::geometryAt(c, n, t), windowAt(c, n, t), GradeWindows::fillAt(c, n, t)};
        if (!w.isNoop()) out << w;
    }
    return out;
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
    if (!at(c, 0).isNeutral()) return true;
    for (int n : GradeWindows::list(c)) {
        const QVector<AnimParam>& list = windowParams(n);
        if (std::any_of(list.begin(), list.end(), [&](AnimParam p) { return Keys::animated(c, p); })) return true;
        if (Keys::animated(c, GradeWindows::param(n, "amount"))) return true;
        if (!windowAt(c, n, 0).isNeutral() || GradeWindows::fillAt(c, n, 0).fill != GradeWindows::Fill::None)
            return true;
    }
    return false;
}

namespace {

using LutPtr = std::shared_ptr<Lut>;

LutPtr failLut(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return nullptr;
}

// Number of a data line (QByteArray::toDouble: always with a dot, independent of the locale)
bool isNumber(const QByteArray& w)
{
    bool ok = false;
    w.toDouble(&ok);
    return ok;
}

QList<QByteArray> words(const QByteArray& line) { return line.simplified().split(' '); }

// .cube (Adobe/Resolve): LUT_3D_SIZE/LUT_1D_SIZE, DOMAIN_MIN/MAX resp. LUT_*_INPUT_RANGE, data R fastest index
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
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") { // Resolve variant
            for (int c = 0; c < 3; ++c) {
                lut->domainMin[c] = float(w.value(1).toDouble());
                lut->domainMax[c] = float(w.value(2).toDouble());
            }
        } // skip TITLE and unknown things
    }
    if (!expected || lut->data.size() != expected * 3)
        return failLut(error, QStringLiteral("Anzahl der Werte passt nicht zur Größe"));
    return lut;
}

// .3dl (Lustre/Flame/Nuke): optional line with the input grid (N values), then N^3 integer RGB triples,
// B fastest index. Output bit depth from "Mesh <in> <out>" or from the largest value (10/12/16 bit).
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
            continue; // skip 3DMESH, LUT8, gamma …
        }
        if (w.size() != 3 && raw.isEmpty() && !size) { // input grid: fixes the size
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
    if (!size) size = int(std::lround(std::cbrt(double(count)))); // without grid line
    if (size < 2 || size > 256 || count != size * size * size)
        return failLut(error, QStringLiteral("Anzahl der Werte passt nicht zur Größe"));
    if (outMax <= 0) {
        const float m = raw.isEmpty() ? 0 : *std::max_element(raw.cbegin(), raw.cend());
        outMax = m <= 1.0f ? 1 : m <= 1023 ? 1023 : m <= 4095 ? 4095 : 65535;
    }
    auto lut = std::make_shared<Lut>();
    lut->size = size;
    lut->data.resize(raw.size());
    // B fastest index -> R fastest index
    for (int r = 0; r < size; ++r)
        for (int g = 0; g < size; ++g)
            for (int b = 0; b < size; ++b) {
                const size_t from = (size_t(r) * size * size + size_t(g) * size + size_t(b)) * 3;
                const size_t to = (size_t(b) * size * size + size_t(g) * size + size_t(r)) * 3;
                for (int c = 0; c < 3; ++c) lut->data[to + c] = float(raw[from + c] / outMax);
            }
    return lut;
}

// .csp (cineSpace): "CSPLUTV100", "3D"/"1D", optional METADATA block, one pre-curve per channel
// (count, inputs, outputs), then 3D: "N N N" + N^3 triples (R fastest index) resp. 1D: count + triples
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
    for (int c = 0; c < 3; ++c) { // Pre-curves
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

// Hald CLUT: square image L^3 x L^3, cube size N = L^2, pixels row by row with R as fastest index
LutPtr parseHald(const QString& path, QString* error)
{
    QImage img(path);
    if (img.isNull()) return failLut(error, QStringLiteral("Bild lässt sich nicht lesen"));
    const int w = img.width();
    const int level = int(std::lround(std::cbrt(double(w))));
    if (img.height() != w || level * level * level != w || level < 2 || level > 16)
        return failLut(error, QStringLiteral("kein Hald-CLUT (quadratisch, Kantenlänge L³)"));
    img = img.convertToFormat(QImage::Format_RGBA64); // 16-bit PNGs without loss
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
    if (!lut && s == 1.0) { // fast: channel curves only
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
        if (sf != 1.0f) { // saturation: mix with the brightness (Rec. 709)
            const float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
            for (float& x : v) x = std::clamp(y + (x - y) * sf, 0.0f, 1.0f);
        }
        if (lut) { // LUT after the correction (like a LUT on a DaVinci node)
            if (lut->is3d) sample3d(*lut, v);
            else sample1d(*lut, v);
        }
        for (int c = 0; c < 3; ++c) rgba[c] = uint8_t(std::lround(std::clamp(v[c], 0.0f, 1.0f) * 255.0f));
    }
}

namespace {

// Radii of three box blurs that together approximate a Gaussian blur with sigma
std::array<int, 3> boxRadii(double sigma)
{
    const double ideal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int lo = int(std::floor(ideal));
    if (lo % 2 == 0) --lo;
    const int hi = lo + 2;
    const int m = int(std::lround((12 * sigma * sigma - 3 * lo * lo - 12 * lo - 9) / (-4.0 * lo - 4)));
    std::array<int, 3> r{};
    for (int i = 0; i < 3; ++i) r[i] = ((i < m ? lo : hi) - 1) / 2;
    return r;
}

// Box blur of radius r over RGB of an RGBA buffer along one direction (step = distance of neighbors in pixels,
// n = pixels per line, lines = number of lines, lineStep = distance of the lines); the edge pixel repeats
void boxBlur(const uint8_t* src, uint8_t* dst, int n, int lines, int step, int lineStep, int r)
{
    const int div = 2 * r + 1;
    for (int l = 0; l < lines; ++l) {
        const uint8_t* s = src + size_t(l) * lineStep * 4;
        uint8_t* d = dst + size_t(l) * lineStep * 4;
        const auto px = [&](int i) { return s + size_t(std::clamp(i, 0, n - 1)) * step * 4; };
        int sum[3] = {0, 0, 0};
        for (int i = -r; i <= r; ++i)
            for (int c = 0; c < 3; ++c) sum[c] += px(i)[c];
        for (int i = 0; i < n; ++i) {
            uint8_t* o = d + size_t(i) * step * 4;
            for (int c = 0; c < 3; ++c) o[c] = uint8_t((sum[c] + div / 2) / div);
            o[3] = px(i)[3];
            const uint8_t* add = px(i + r + 1);
            const uint8_t* sub = px(i - r);
            for (int c = 0; c < 3; ++c) sum[c] += add[c] - sub[c];
        }
    }
}

void gaussian(std::vector<uint8_t>& buf, int w, int h, double sigma)
{
    if (sigma < 0.5 || w <= 0 || h <= 0) return;
    std::vector<uint8_t> tmp(buf.size());
    for (int r : boxRadii(sigma)) {
        if (r <= 0) continue;
        boxBlur(buf.data(), tmp.data(), w, h, 1, w, r); // rows
        boxBlur(tmp.data(), buf.data(), h, w, w, 1, r); // columns
    }
}

void pixelate(std::vector<uint8_t>& buf, int w, int h, int block)
{
    for (int by = 0; by < h; by += block)
        for (int bx = 0; bx < w; bx += block) {
            const int x1 = std::min(w, bx + block), y1 = std::min(h, by + block);
            long sum[3] = {0, 0, 0};
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    for (int c = 0; c < 3; ++c) sum[c] += buf[(size_t(y) * w + x) * 4 + c];
            const long n = long(x1 - bx) * (y1 - by);
            for (int y = by; y < y1; ++y)
                for (int x = bx; x < x1; ++x)
                    for (int c = 0; c < 3; ++c) buf[(size_t(y) * w + x) * 4 + c] = uint8_t((sum[c] + n / 2) / n);
        }
}

} // namespace

void applyWindow(uint8_t* rgba, int width, int height, const Window& w)
{
    using GradeWindows::Fill;
    if (!rgba || width <= 0 || height <= 0 || w.isNoop()) return;
    const GradeWindows::Mask mask(w.geometry, width, height);
    const QRect r = mask.bounds();
    if (r.isEmpty()) return;
    // Area worked on: the mask plus what blur/pixelate need around it (pixel blocks on a grid from the picture corner,
    // so they do not swim while the window moves)
    QRect area = r;
    double sigma = 0;
    int block = 0;
    if (w.fill.fill == Fill::Blur) {
        sigma = w.fill.amount * height / 2000.0; // like the blur effect: 100 = 1/20 of the picture height
        const int m = int(std::ceil(sigma * 3)) + 1;
        area = r.adjusted(-m, -m, m, m);
    } else if (w.fill.fill == Fill::Pixelate) {
        block = std::max(2, int(std::lround(w.fill.amount / 100.0 * height / 10.0))); // 100 = 1/10 of the height
        const int x0 = r.left() / block * block, y0 = r.top() / block * block;
        area = QRect(QPoint(x0, y0), QPoint((r.right() / block + 1) * block - 1, (r.bottom() / block + 1) * block - 1));
    }
    area = area.intersected(QRect(0, 0, width, height));
    const int aw = area.width(), ah = area.height();
    std::vector<uint8_t> fx(size_t(aw) * ah * 4);
    for (int y = 0; y < ah; ++y)
        std::memcpy(fx.data() + size_t(y) * aw * 4, rgba + (size_t(area.top() + y) * width + area.left()) * 4,
                    size_t(aw) * 4);
    if (!w.params.isNeutral()) apply(fx.data(), aw, ah, w.params, nullptr);
    switch (w.fill.fill) {
    case Fill::None: break;
    case Fill::Blur: gaussian(fx, aw, ah, sigma); break;
    case Fill::Pixelate: pixelate(fx, aw, ah, block); break;
    case Fill::Color: {
        const QColor& col = w.fill.color;
        const uint8_t rgb[3] = {uint8_t(col.red()), uint8_t(col.green()), uint8_t(col.blue())};
        for (size_t i = 0; i < fx.size(); i += 4) std::memcpy(&fx[i], rgb, 3);
        break;
    }
    }
    for (int y = r.top(); y <= r.bottom(); ++y) {
        uint8_t* dst = rgba + (size_t(y) * width + r.left()) * 4;
        const uint8_t* src = fx.data() + (size_t(y - area.top()) * aw + (r.left() - area.left())) * 4;
        for (int x = r.left(); x <= r.right(); ++x, dst += 4, src += 4) {
            const float m = mask.weight(x, y);
            if (m <= 0) continue;
            if (m >= 1) {
                std::memcpy(dst, src, 3);
                continue;
            }
            for (int c = 0; c < 3; ++c) dst[c] = uint8_t(std::lround(dst[c] + (src[c] - dst[c]) * m));
        }
    }
}

void attach(Mlt::Producer& cut, const Clip& c, int a, const std::shared_ptr<std::atomic<bool>>& bypass)
{
    if (!active(c)) return;
    auto d = std::make_shared<State>();
    d->clip = c;
    d->a = a;
    d->animated = std::any_of(kParams.begin(), kParams.end(), [&](AnimParam p) { return Keys::animated(c, p); });
    d->fixed = at(c, a);
    if (const QString path = lutPath(c); !path.isEmpty()) d->lut = loadLut(path); // missing/broken: no LUT
    d->bypass = bypass;
    d->windows = !GradeWindows::list(c).isEmpty();
    if (!d->animated && d->fixed.isNeutral() && !d->lut && !d->windows) return;
    mlt_filter f = mlt_filter_new();
    if (!f) return;
    f->process = process;
    auto* fd = new FilterData;
    fd->state.store(std::move(d));
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), kDataName, fd, 0, destroyData, nullptr);
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    // Position for the keyframes counts from filter-in (as with the other keyframe filters)
    filter.set_in_and_out(cut.get_in(), cut.get_out());
    cut.attach(filter);
}

bool transfer(mlt_filter live, mlt_filter fresh)
{
    if (live->process != process || fresh->process != process) return false;
    auto* l = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(live), kDataName, nullptr));
    auto* f = static_cast<FilterData*>(mlt_properties_get_data(MLT_FILTER_PROPERTIES(fresh), kDataName, nullptr));
    if (!l || !f) return false;
    l->state.store(f->state.load());
    return true;
}

} // namespace ColorGrade
