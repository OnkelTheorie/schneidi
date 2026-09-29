#include "engine/ColorGrade.h"

#include "core/EffectRegistry.h"
#include "core/Keyframes.h"

#include <Mlt.h>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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

// Trilineare Interpolation in der 3D-LUT (Eingang 0..1 nach Domain umgerechnet)
inline void sample3d(const Lut& lut, float rgb[3])
{
    const int n = lut.size;
    float f[3];
    int i0[3], i1[3];
    for (int c = 0; c < 3; ++c) {
        const float span = lut.domainMax[c] - lut.domainMin[c];
        float v = span > 0 ? (rgb[c] - lut.domainMin[c]) / span : rgb[c];
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
        float v = span > 0 ? (rgb[c] - lut.domainMin[c]) / span : rgb[c];
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

std::shared_ptr<const Lut> loadCube(const QString& path, QString* error)
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
    auto fail = [&](const QString& msg) -> std::shared_ptr<const Lut> {
        if (error) *error = msg;
        return nullptr;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());

    auto lut = std::make_shared<Lut>();
    int expected = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const QList<QByteArray> w = line.simplified().split(' ');
        const QByteArray key = w.first().toUpper();
        bool ok = false;
        w.first().toDouble(&ok); // Zahlen (QByteArray::toDouble: immer mit Punkt, unabhängig vom Gebietsschema)
        if (ok) {
            if (w.size() < 3 || !expected) return fail(QStringLiteral("Datenzeile vor der Größenangabe"));
            for (int c = 0; c < 3; ++c) {
                bool okc = false;
                lut->data << float(w.at(c).toDouble(&okc));
                if (!okc) return fail(QStringLiteral("ungültige Zahl"));
            }
            continue;
        }
        if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
            lut->is3d = key == "LUT_3D_SIZE";
            lut->size = w.value(1).toInt();
            if (lut->size < 2 || lut->size > (lut->is3d ? 256 : 65536)) return fail(QStringLiteral("ungültige Größe"));
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
        return fail(QStringLiteral("Anzahl der Werte passt nicht zur Größe"));

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
    if (const QString path = lutPath(c); !path.isEmpty()) d->lut = loadCube(path); // fehlt/kaputt: ohne LUT
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
