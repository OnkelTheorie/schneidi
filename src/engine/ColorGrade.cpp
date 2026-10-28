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
#include <thread>
#include <vector>

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

inline float sample1d(const Lut& lut, int c, float x)
{
    const int n = lut.size;
    const float span = lut.domainMax[c] - lut.domainMin[c];
    x = shape(lut, c, x);
    float v = span > 0 ? (x - lut.domainMin[c]) / span : x;
    v = std::clamp(v, 0.0f, 1.0f) * float(n - 1);
    const int i0 = std::min(int(v), n - 1), i1 = std::min(i0 + 1, n - 1);
    const float f = v - float(i0);
    return lut.data[i0 * 3 + c] * (1 - f) + lut.data[i1 * 3 + c] * f;
}

inline void sample1d(const Lut& lut, float rgb[3])
{
    for (int c = 0; c < 3; ++c) rgb[c] = sample1d(lut, c, rgb[c]);
}

// 0..1 -> 0..255, rounded (std::lround per channel made a 1080p frame take ~25 ms)
inline uint8_t toByte(float x) { return uint8_t(int((x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x) * 255.0f + 0.5f)); }

bool usable(const Lut* lut)
{
    if (!lut || lut->size < 2) return false;
    const qsizetype n = lut->size;
    return lut->data.size() == (lut->is3d ? n * n * n : n) * 3;
}

// ---- Baked correction ----
// A pixel goes through: curve per channel (exact table per input byte) -> saturation (mixes the channels, fixed
// point) -> LUT. A 1D LUT is per channel again; a 3D LUT keeps its own grid: its nodes in fixed point, three
// channels in one 64-bit word (lanes of 21 bits), so a trilinear step is two multiplications for all three.

constexpr int kLane = 21;
constexpr uint64_t kLaneMask = 0x1FFF | (uint64_t(0x1FFF) << kLane) | (uint64_t(0x1FFF) << 2 * kLane);
constexpr uint64_t kLaneHalf = 128 | (uint64_t(128) << kLane) | (uint64_t(128) << 2 * kLane);
constexpr int kNodeOffset = 2048; // node = (value + 0.5) * 4096: values -0.5..1.5 (LUTs may leave 0..1 a bit)
constexpr int kMaxPacked = 129;   // bigger 3D LUTs (huge Hald CLUTs) stay per pixel
constexpr int kSteps = 4096;      // input resolution of the LUT mapping after the saturation (0..1)

// Lane value (0..8191) -> output byte
const std::array<uint8_t, 8192>& laneByte()
{
    static const auto table = [] {
        std::array<uint8_t, 8192> t{};
        for (int v = 0; v < 8192; ++v) {
            const int x = std::clamp(v - kNodeOffset, 0, kSteps);
            t[v] = uint8_t((x * 255 + kSteps / 2) / kSteps);
        }
        return t;
    }();
    return table;
}

// Interpolate two nodes (all three lanes), f = 0..256
inline uint64_t lerp(uint64_t a, uint64_t b, uint32_t f)
{
    return ((a * (256 - f) + b * f + kLaneHalf) >> 8) & kLaneMask;
}

} // namespace

struct LutTables {
    int n = 0;                   // 3D grid size, 0 = no nodes (too big, per pixel)
    std::vector<uint64_t> nodes; // R | G << 21 | B << 42, padded so the neighbor of the last node can be read
    // Per channel input 0..1 in kSteps steps -> 3D: node offset << 8 | fraction (1/256 of a cell); 1D: output
    // in 1/65536 of 0..1 (+ one entry, interpolated between the steps)
    std::vector<int32_t> coord[3];
    std::vector<int32_t> out1d[3];
};

struct Baked {
    enum Kind { Direct, Mix, MixLut1d, Cube, MixCube, PerPixel } kind = Direct;
    uint8_t direct[3][256];          // Direct: output byte per input byte
    int32_t ys[3][256], vs[3][256];  // Mix: brightness and own part (byte << 16); Mix*: in 1/2^24 of 0..1
    int32_t coord[3][256];           // Cube: node offset << 8 | fraction per input byte
    std::shared_ptr<const LutTables> lut;
    Params params;                   // PerPixel
    const Lut* raw = nullptr;        // PerPixel
};

namespace {

// Position of x (after the correction, 0..1) in the 3D LUT: node offset << 8 | fraction, like sample3d
int32_t cubeCoord(const Lut& lut, int c, float x)
{
    const int n = lut.size;
    const float span = lut.domainMax[c] - lut.domainMin[c];
    x = shape(lut, c, x);
    float v = span > 0 ? (x - lut.domainMin[c]) / span : x;
    v = std::clamp(v, 0.0f, 1.0f) * float(n - 1);
    const int q = std::clamp(int(std::lround(v * 256.0f)), 0, (n - 1) * 256);
    const int stride = c == 0 ? 1 : c == 1 ? n : n * n;
    return int32_t(((q >> 8) * stride) << 8 | (q & 255));
}

std::shared_ptr<const LutTables> makeTables(const Lut& lut)
{
    auto t = std::make_shared<LutTables>();
    if (!usable(&lut)) return t;
    const int n = lut.size;
    if (lut.is3d) {
        if (n <= kMaxPacked) {
            t->n = n;
            const size_t count = size_t(n) * n * n;
            t->nodes.assign(count + size_t(n) * n + n + 1, 0);
            for (size_t i = 0; i < count; ++i) {
                uint64_t w = 0;
                for (int c = 0; c < 3; ++c) {
                    const float v = lut.data[qsizetype(i * 3 + c)];
                    const long q = std::lround((std::clamp(double(v), -1.0, 2.0) + 0.5) * kSteps);
                    w |= uint64_t(std::clamp(q, 0L, 8191L)) << (kLane * c);
                }
                t->nodes[i] = w;
            }
            for (int c = 0; c < 3; ++c) {
                t->coord[c].resize(kSteps + 1);
                for (int k = 0; k <= kSteps; ++k) t->coord[c][k] = cubeCoord(lut, c, float(k) / kSteps);
            }
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            t->out1d[c].resize(kSteps + 2);
            for (int k = 0; k <= kSteps; ++k)
                t->out1d[c][k] = int32_t(std::lround(std::clamp(sample1d(lut, c, float(k) / kSteps), 0.0f, 1.0f) * 65536.0f));
            t->out1d[c][kSteps + 1] = t->out1d[c][kSteps];
        }
    }
    return t;
}

// Rows [0, h) in a few threads for big pictures (memory-bound: a few are enough)
template <typename F>
void parallelRows(int w, int h, int threads, F rows)
{
    int n = threads;
    if (n <= 0) n = int64_t(w) * h < 500'000 ? 1 : std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
    n = std::clamp(n, 1, std::max(1, h));
    if (n < 2) {
        rows(0, h);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(n - 1);
    for (int i = 1; i < n; ++i) pool.emplace_back(rows, h * i / n, h * (i + 1) / n);
    rows(0, h / n);
    for (std::thread& t : pool) t.join();
}

// Trilinear in the packed nodes: base = node offset, f* = fractions 0..255; result lanes
inline uint64_t trilinear(const uint64_t* q, int n, uint32_t fr, uint32_t fg, uint32_t fb)
{
    const size_t g = size_t(n), b = size_t(n) * n;
    const uint64_t c00 = lerp(q[0], q[1], fr), c10 = lerp(q[g], q[g + 1], fr);
    const uint64_t c01 = lerp(q[b], q[b + 1], fr), c11 = lerp(q[b + g], q[b + g + 1], fr);
    return lerp(lerp(c00, c10, fg), lerp(c01, c11, fg), fb);
}

// The three lanes as output bytes R, G, B
inline void writeLanes(uint8_t* px, const uint8_t* lanes, uint64_t o)
{
    px[0] = lanes[o & 0x1FFF];
    px[1] = lanes[(o >> kLane) & 0x1FFF];
    px[2] = lanes[(o >> 2 * kLane) & 0x1FFF];
}

// The loops read the tables through local pointers/copies: a store through uint8_t* may alias anything, so
// members would be read again for every pixel
void applyRows(uint8_t* rgba, size_t count, const Baked& b)
{
    switch (b.kind) {
    case Baked::Direct: {
        uint8_t d[3][256];
        std::memcpy(d, b.direct, sizeof d);
        for (size_t i = 0; i < count; ++i, rgba += 4) {
            rgba[0] = d[0][rgba[0]];
            rgba[1] = d[1][rgba[1]];
            rgba[2] = d[2][rgba[2]];
        }
        break;
    }
    case Baked::Mix: {
        int32_t ys[3][256], vs[3][256];
        std::memcpy(ys, b.ys, sizeof ys);
        std::memcpy(vs, b.vs, sizeof vs);
        auto clampByte = [](int v) { return uint8_t(v < 0 ? 0 : v > (255 << 16) ? 255 : v >> 16); };
        for (size_t i = 0; i < count; ++i, rgba += 4) {
            const int r = rgba[0], g = rgba[1], bl = rgba[2];
            const int y = ys[0][r] + ys[1][g] + ys[2][bl];
            rgba[0] = clampByte(y + vs[0][r]);
            rgba[1] = clampByte(y + vs[1][g]);
            rgba[2] = clampByte(y + vs[2][bl]);
        }
        break;
    }
    case Baked::MixLut1d:
    case Baked::MixCube: {
        // Saturation in 1/2^24 of 0..1, then to the kSteps grid of the LUT mapping
        int32_t ys[3][256], vs[3][256];
        std::memcpy(ys, b.ys, sizeof ys);
        std::memcpy(vs, b.vs, sizeof vs);
        auto step = [](int v) { return (std::clamp(v, 0, 1 << 24) + (1 << 11)) >> 12; };
        const LutTables& t = *b.lut;
        if (b.kind == Baked::MixLut1d) {
            // Between the steps linearly (steep curves near black would show the steps)
            const int32_t* o[3] = {t.out1d[0].data(), t.out1d[1].data(), t.out1d[2].data()};
            auto out = [](const int32_t* t, int v) {
                v = std::clamp(v, 0, 1 << 24);
                const int k = v >> 12, f = v & 4095;
                const int x = t[k] + int((int64_t(t[k + 1] - t[k]) * f) >> 12);
                return uint8_t((x * 255 + 32768) >> 16);
            };
            for (size_t i = 0; i < count; ++i, rgba += 4) {
                const int r = rgba[0], g = rgba[1], bl = rgba[2];
                const int y = ys[0][r] + ys[1][g] + ys[2][bl];
                rgba[0] = out(o[0], y + vs[0][r]);
                rgba[1] = out(o[1], y + vs[1][g]);
                rgba[2] = out(o[2], y + vs[2][bl]);
            }
            break;
        }
        const int32_t *c0 = t.coord[0].data(), *c1 = t.coord[1].data(), *c2 = t.coord[2].data();
        const uint64_t* nodes = t.nodes.data();
        const int n = t.n;
        const uint8_t* lanes = laneByte().data();
        for (size_t i = 0; i < count; ++i, rgba += 4) {
            const int r = rgba[0], g = rgba[1], bl = rgba[2];
            const int y = ys[0][r] + ys[1][g] + ys[2][bl];
            const int32_t cr = c0[step(y + vs[0][r])], cg = c1[step(y + vs[1][g])], cb = c2[step(y + vs[2][bl])];
            const uint64_t o = trilinear(nodes + ((cr >> 8) + (cg >> 8) + (cb >> 8)), n, cr & 255, cg & 255, cb & 255);
            writeLanes(rgba, lanes, o);
        }
        break;
    }
    case Baked::Cube: {
        int32_t coord[3][256];
        std::memcpy(coord, b.coord, sizeof coord);
        const uint64_t* nodes = b.lut->nodes.data();
        const int n = b.lut->n;
        const uint8_t* lanes = laneByte().data();
        for (size_t i = 0; i < count; ++i, rgba += 4) {
            const int32_t cr = coord[0][rgba[0]], cg = coord[1][rgba[1]], cb = coord[2][rgba[2]];
            const uint64_t o = trilinear(nodes + ((cr >> 8) + (cg >> 8) + (cb >> 8)), n, cr & 255, cg & 255, cb & 255);
            writeLanes(rgba, lanes, o);
        }
        break;
    }
    case Baked::PerPixel: break; // see apply()
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
    // Baked tables of the last values (keyframes: made again only when the values change)
    mutable std::mutex bakeMutex;
    mutable Params bakedParams;
    mutable std::shared_ptr<const Baked> baked;

    std::shared_ptr<const Baked> tables(const Params& p) const
    {
        {
            std::lock_guard<std::mutex> lock(bakeMutex);
            if (baked && bakedParams == p) return baked;
        }
        std::shared_ptr<const Baked> b = bake(p, lut.get());
        std::lock_guard<std::mutex> lock(bakeMutex);
        bakedParams = p;
        baked = b;
        return b;
    }
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
    // Rows in threads for the preview only (one frame at a time there); the export renders several frames at once
    // (no Before/After switch there), where more threads per frame would only compete for the cores
    if (!p.isNeutral() || d->lut) apply(*image, *width, *height, *d->tables(p), d->bypass ? 0 : 1);
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

// Number of a data line (QByteArray::toDouble: always with a dot, independent of the locale). Only finite
// values that fit a float: toDouble also takes "nan"/"inf", which end up in lround/int casts later (undefined).
double number(const QByteArray& w, bool* ok = nullptr)
{
    bool good = false;
    const double v = w.toDouble(&good);
    good = good && std::isfinite(float(v));
    if (ok) *ok = good;
    return good ? v : 0.0;
}

bool isNumber(const QByteArray& w)
{
    bool ok = false;
    number(w, &ok);
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
                lut->data << float(number(w.at(c), &okc));
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
            for (int c = 0; c < 3 && c + 1 < w.size(); ++c) dst[c] = float(number(w.at(c + 1)));
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") { // Resolve variant
            for (int c = 0; c < 3; ++c) {
                lut->domainMin[c] = float(number(w.value(1)));
                lut->domainMax[c] = float(number(w.value(2)));
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
            raw << float(number(w.at(c), &ok));
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
            nums << number(w, &ok);
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

namespace {

LutPtr parseLutFile(const QString& path, QString* error)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (!EffectFolders::isLutFile(path)) return failLut(error, QStringLiteral("unbekanntes LUT-Format"));
    if (suffix != "cube" && suffix != "3dl" && suffix != "csp") {
        if (!QFileInfo::exists(path)) return failLut(error, QStringLiteral("Datei fehlt"));
        return parseHald(path, error);
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return failLut(error, f.errorString());
    if (f.peek(3) == "\xEF\xBB\xBF") f.skip(3); // UTF-8 BOM (files saved by Windows editors)
    if (suffix == "3dl") return parse3dl(f, error);
    if (suffix == "csp") return parseCsp(f, error);
    return parseCube(f, error);
}

} // namespace

std::shared_ptr<const Lut> parseLut(const QString& path, QString* error)
{
    LutPtr lut = parseLutFile(path, error);
    if (lut) prepare(*lut);
    return lut;
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

void prepare(Lut& lut) { lut.tables = makeTables(lut); }

std::shared_ptr<const Baked> bake(const Params& p, const Lut* lut)
{
    auto b = std::make_shared<Baked>();
    if (!usable(lut)) lut = nullptr;
    const Curve curve(p);
    const double s = std::max(0.0, p.saturation / 50.0);
    const float sf = float(s);
    if (!lut && s == 1.0) {
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) b->direct[c][i] = uint8_t(std::lround(curve(c, i / 255.0) * 255.0));
        return b;
    }
    float table[3][256];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) table[c][i] = float(curve(c, i / 255.0));
    const float w[3] = {0.2126f, 0.7152f, 0.0722f};
    if (!lut) { // same tables as applyReference
        b->kind = Baked::Mix;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) {
                b->ys[c][i] = int(std::lround(table[c][i] * w[c] * (1.0f - sf) * 255.0f * 65536.0f));
                b->vs[c][i] = int(std::lround(table[c][i] * sf * 255.0f * 65536.0f)) + 32768; // + 0.5 for rounding
            }
        return b;
    }
    if (!lut->is3d && sf == 1.0f) { // all per channel
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) b->direct[c][i] = toByte(sample1d(*lut, c, table[c][i]));
        return b;
    }
    b->lut = lut->tables ? lut->tables : makeTables(*lut);
    if (lut->is3d && !b->lut->n) {
        b->kind = Baked::PerPixel;
        b->params = p;
        b->raw = lut;
        return b;
    }
    if (sf == 1.0f) {
        b->kind = Baked::Cube;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) b->coord[c][i] = cubeCoord(*lut, c, table[c][i]);
        return b;
    }
    b->kind = lut->is3d ? Baked::MixCube : Baked::MixLut1d;
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 256; ++i) {
            b->ys[c][i] = int(std::lround(double(table[c][i]) * w[c] * (1.0 - sf) * (1 << 24)));
            b->vs[c][i] = int(std::lround(double(table[c][i]) * sf * (1 << 24)));
        }
    return b;
}

void apply(uint8_t* rgba, int width, int height, const Baked& b, int threads)
{
    if (!rgba || width <= 0 || height <= 0) return;
    parallelRows(width, height, threads, [&](int from, int to) {
        uint8_t* p = rgba + size_t(from) * width * 4;
        if (b.kind == Baked::PerPixel) applyReference(p, width, to - from, b.params, b.raw);
        else applyRows(p, size_t(width) * (to - from), b);
    });
}

void apply(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut)
{
    if (!rgba || width <= 0 || height <= 0) return;
    apply(rgba, width, height, *bake(p, lut));
}

void applyReference(uint8_t* rgba, int width, int height, const Params& p, const Lut* lut)
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
    if (!lut) {
        // Saturation: mix with the brightness (Rec. 709), y + (v - y) * s = y * (1 - s) + v * s. Both parts come
        // from tables in 16-bit fixed point (byte value << 16), so a pixel is a few additions
        int ys[3][256], vs[3][256];
        const float w[3] = {0.2126f, 0.7152f, 0.0722f};
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) {
                ys[c][i] = int(std::lround(table[c][i] * w[c] * (1.0f - sf) * 255.0f * 65536.0f));
                vs[c][i] = int(std::lround(table[c][i] * sf * 255.0f * 65536.0f)) + 32768; // + 0.5 for rounding
            }
        auto clampByte = [](int v) { return uint8_t(v < 0 ? 0 : v > (255 << 16) ? 255 : v >> 16); };
        for (size_t i = 0; i < n; ++i, rgba += 4) {
            const int y = ys[0][rgba[0]] + ys[1][rgba[1]] + ys[2][rgba[2]];
            rgba[0] = clampByte(y + vs[0][rgba[0]]);
            rgba[1] = clampByte(y + vs[1][rgba[1]]);
            rgba[2] = clampByte(y + vs[2][rgba[2]]);
        }
        return;
    }
    for (size_t i = 0; i < n; ++i, rgba += 4) {
        float v[3] = {table[0][rgba[0]], table[1][rgba[1]], table[2][rgba[2]]};
        if (sf != 1.0f) { // saturation: mix with the brightness (Rec. 709)
            const float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
            for (float& x : v) x = std::clamp(y + (x - y) * sf, 0.0f, 1.0f);
        }
        // LUT after the correction (like a LUT on a DaVinci node)
        if (lut->is3d) sample3d(*lut, v);
        else sample1d(*lut, v);
        for (int c = 0; c < 3; ++c) rgba[c] = toByte(v[c]);
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
