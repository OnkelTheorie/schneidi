// SPDX-License-Identifier: AGPL-3.0-only
// Compositing node kinds of package A (0.19): clip, media, color, gradient, merge, transform, crop, blur, grade, fx
#include "engine/picture/CompNodes.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "engine/ColorGrade.h"
#include "engine/VideoEffects.h"
#include "engine/video/Blend.h"
#include "engine/video/Compose.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <vector>

namespace Picture::Comp {

namespace {

EffectParam num(const char* key, const QString& label, double def, double lo, double hi, double step = 0.01,
                int decimals = 3, const QString& description = {})
{
    EffectParam p;
    p.key = QString::fromLatin1(key);
    p.label = label;
    p.type = EffectParam::Double;
    p.defaultValue = def;
    p.min = lo;
    p.max = hi;
    p.step = step;
    p.decimals = decimals;
    p.description = description;
    return p;
}

EffectParam other(EffectParam::Type type, const char* key, const QString& label, const QVariant& def,
                  const QStringList& choices = {}, const QStringList& choiceLabels = {})
{
    EffectParam p;
    p.key = QString::fromLatin1(key);
    p.label = label;
    p.type = type;
    p.defaultValue = def;
    p.choices = choices;
    p.choiceLabels = choiceLabels;
    return p;
}

void copyBuf(const Buf& from, Buf& to)
{
    if (to.px.size() == from.px.size()) std::memcpy(to.data(), from.data(), from.px.size());
    to.area = from.area;
}

// A mask input's weight at pixel i, 0..255 (an Rgba picture given as mask: its alpha)
inline uint8_t maskWeight(const Buf& mask, size_t i)
{
    return mask.kind == Buf::Mask ? mask.px[i] : mask.px[i * 4 + 3];
}

// result = src + (result - src) * mask (an effect only where the mask is)
void mixByMask(Buf& result, const Buf& src, const Buf* mask)
{
    if (!mask || mask->w != result.w || mask->h != result.h || src.px.size() != result.px.size()) return;
    result.area = {};
    const int w = result.w;
    Compose::forRows(result.h, 32, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            uint8_t* d = result.data() + size_t(y) * w * 4;
            const uint8_t* s = src.data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, d += 4, s += 4) {
                const int k = maskWeight(*mask, size_t(y) * w + x);
                if (k == 255) continue;
                if (k == 0) {
                    std::memcpy(d, s, 4);
                    continue;
                }
                for (int c = 0; c < 4; ++c) d[c] = uint8_t(int(s[c]) + ((int(d[c]) - int(s[c])) * k) / 255);
            }
        }
    });
}

const QStringList& mergeModes()
{
    static const QStringList m = {"normal", "add", "screen", "multiply", "overlay", "darken", "lighten", "difference"};
    return m;
}

// B(cb, cs) of a separable mode for every pair of bytes, rounded (built once per mode, read-only after)
const uint8_t* blendTable(int mode)
{
    static std::once_flag once[Blend::kModeCount];
    static std::unique_ptr<uint8_t[]> tables[Blend::kModeCount];
    std::call_once(once[mode], [mode] {
        auto t = std::make_unique<uint8_t[]>(256 * 256);
        for (int a = 0; a < 256; ++a)
            for (int b = 0; b < 256; ++b)
                t[size_t(a) * 256 + b] = uint8_t(std::clamp(Blend::channel(mode, a / 255.0f, b / 255.0f), 0.0f, 1.0f) * 255.0f + 0.5f);
        tables[mode] = std::move(t);
    });
    return tables[mode].get();
}

// Merge: fg over bg with a blend mode (W3C compositing: Cm = (1 - ab) Cs + ab B(Cb, Cs), then source over),
// fg alpha times opacity times mask
bool evalMerge(NodeCtx& ctx, Buf* out)
{
    const Buf* bg = ctx.input("bg");
    const Buf* fg = ctx.input("fg");
    const Buf* mask = ctx.input("mask");
    if (bg && bg->kind != Buf::Rgba) bg = nullptr;
    if (fg && fg->kind != Buf::Rgba) fg = nullptr;
    if (!bg && !fg) return false;
    const double opacity = std::clamp(ctx.num("opacity"), 0.0, 1.0);
    *out = ctx.take(Buf::Rgba);
    if (bg) copyBuf(*bg, *out);
    if (!fg || opacity <= 0) return true;
    // the choice by index (a name or its number, as num reads it) to the shared blend math
    const int mode = Blend::modeIndex(mergeModes().value(int(ctx.num("mode"))));
    const int w = out->w, h = out->h;
    if (fg->w != w || fg->h != h) return true;
    if (mode == Blend::Normal && !mask) { // exactly the timeline's track compositing
        Compose::over(out->data(), fg->data(), w, h, opacity);
        out->area = bg && !bg->area.isEmpty() && !fg->area.isEmpty() ? (bg->area | fg->area) : QRect();
        return true;
    }
    if (mask && (mask->w != w || mask->h != h)) mask = nullptr;
    out->area = {};
    const uint32_t op = uint32_t(std::lround(opacity * 256));
    // Separable modes on an opaque backdrop (the usual case): Cm = B(Cb, Cs), result Cb + (Cm - Cb) * as in integers
    const uint8_t* table = mode < Blend::HslHue && mode != Blend::Saturate ? blendTable(mode) : nullptr;
    Compose::forRows(h, 16, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            uint8_t* d = out->data() + size_t(y) * w * 4;
            const uint8_t* s = fg->data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, d += 4, s += 4) {
                uint32_t sa = s[3];
                if (op != 256) sa = (sa * op + 128) >> 8;
                if (mask) sa = (sa * maskWeight(*mask, size_t(y) * w + x) + 127) / 255;
                if (sa == 0) continue;
                if (table && d[3] == 255) {
                    for (int c = 0; c < 3; ++c) {
                        const int cb = d[c], cm = table[size_t(cb) * 256 + s[c]];
                        d[c] = uint8_t(cb + ((cm - cb) * int(sa) + (cm >= cb ? 127 : -127)) / 255);
                    }
                    continue;
                }
                const float as = sa / 255.0f, ab = d[3] / 255.0f;
                const float cb[3] = {d[0] / 255.0f, d[1] / 255.0f, d[2] / 255.0f};
                const float cs[3] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f};
                float b[3];
                Blend::pixel(mode, cb, cs, b);
                const float ao = as + ab * (1 - as);
                for (int c = 0; c < 3; ++c) {
                    const float cm = (1 - ab) * cs[c] + ab * b[c];
                    const float co = (as * cm + (1 - as) * ab * cb[c]) / ao;
                    d[c] = uint8_t(std::clamp(co, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                d[3] = uint8_t(ao * 255.0f + 0.5f);
            }
        }
    });
    return true;
}

// Where the transform puts the source: anchor (ax, ay of the picture) to (x, y), scaled by size, turned by angle
// (clockwise); with "follow" the tracker carries that placement by the follow rule (core/CompTrack.h): moved by the
// tracker's (x, y) - (rx, ry), scaled by its scale and turned by its angle around (x, y). Not analyzed = unchanged
Compose::Affine transformOf(NodeCtx& ctx, bool* identity)
{
    const double W = ctx.size.width(), H = ctx.size.height();
    double x = ctx.num("x"), y = ctx.num("y"), size = ctx.num("size"), angle = ctx.num("angle");
    const int follow = int(std::lround(ctx.num("follow")));
    if (follow > 0 && ctx.numOf) {
        double ts = ctx.numOf(follow, "scale");
        ts = ts > 0 && std::isfinite(ts) ? ts : 1.0;
        double ta = ctx.numOf(follow, "angle");
        ta = std::isfinite(ta) ? ta : 0.0;
        // The anchor's place mapped like any point of the picture (in pixels: a turn keeps its shape)
        const double tr = ta * std::numbers::pi / 180.0, tc = std::cos(tr), tsn = std::sin(tr);
        const double dx = (x - ctx.numOf(follow, "rx")) * W, dy = (y - ctx.numOf(follow, "ry")) * H;
        x = ctx.numOf(follow, "x") + ts * (tc * dx - tsn * dy) / W;
        y = ctx.numOf(follow, "y") + ts * (tsn * dx + tc * dy) / H;
        size *= ts;
        angle += ta;
    }
    const double ax = ctx.num("ax") * W, ay = ctx.num("ay") * H;
    const double rad = angle * std::numbers::pi / 180.0, cs = std::cos(rad) * size, sn = std::sin(rad) * size;
    Compose::Affine m;
    m.a = cs;
    m.b = -sn;
    m.d = sn;
    m.e = cs;
    m.c = x * W - (cs * ax - sn * ay);
    m.f = y * H - (sn * ax + cs * ay);
    *identity = std::abs(m.a - 1) < 1e-9 && std::abs(m.e - 1) < 1e-9 && std::abs(m.b) < 1e-9 && std::abs(m.d) < 1e-9
                && std::abs(m.c) < 1e-9 && std::abs(m.f) < 1e-9;
    return m;
}

bool evalTransform(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    bool identity = false;
    const Compose::Affine m = transformOf(ctx, &identity);
    *out = ctx.take(Buf::Rgba);
    if (identity) {
        copyBuf(*src, *out);
        return true;
    }
    Compose::draw(out->data(), out->w, out->h, src->data(), src->w, src->h, m, 1.0);
    return true;
}

bool evalCrop(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    *out = ctx.take(Buf::Rgba);
    copyBuf(*src, *out);
    const int w = out->w, h = out->h;
    const double x0 = std::clamp(ctx.num("left"), 0.0, 1.0) * w, y0 = std::clamp(ctx.num("top"), 0.0, 1.0) * h;
    const double x1 = (1 - std::clamp(ctx.num("right"), 0.0, 1.0)) * w;
    const double y1 = (1 - std::clamp(ctx.num("bottom"), 0.0, 1.0)) * h;
    const double soft = std::max(0.0, ctx.num("soft")) * ctx.scale;
    if (x0 <= 0 && y0 <= 0 && x1 >= w && y1 >= h) return true;
    if (soft < 0.5) {
        const uint8_t none[4] = {0, 0, 0, 0};
        Compose::crop(out->data(), w, h, x0, y0, x1, y1, none);
        return true;
    }
    // Soft edge: alpha ramps from 0 at the crop line to full `soft` pixels inside
    // (only on the cropped sides: the picture's own border stays hard)
    auto ramp = [soft](double d, bool cut) { return cut ? std::clamp(d / soft, 0.0, 1.0) : d < 0 ? 0.0 : 1.0; };
    std::vector<uint16_t> cx(static_cast<size_t>(w)), cy(static_cast<size_t>(h));
    for (int i = 0; i < w; ++i)
        cx[size_t(i)] = uint16_t(std::lround(256 * std::min(ramp(i + 0.5 - x0, x0 > 0), ramp(x1 - i - 0.5, x1 < w))));
    for (int i = 0; i < h; ++i)
        cy[size_t(i)] = uint16_t(std::lround(256 * std::min(ramp(i + 0.5 - y0, y0 > 0), ramp(y1 - i - 0.5, y1 < h))));
    Compose::forRows(h, 32, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            uint8_t* p = out->data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, p += 4) {
                const uint32_t k = (uint32_t(cx[size_t(x)]) * cy[size_t(y)] + 128) >> 8;
                if (k < 256) p[3] = uint8_t((p[3] * k + 128) >> 8);
            }
        }
    });
    out->area = {};
    return true;
}

bool evalBlur(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    *out = ctx.take(Buf::Rgba);
    copyBuf(*src, *out);
    const double sigma = std::max(0.0, ctx.num("radius")) * ctx.scale;
    if (sigma <= 0) return true;
    VideoEffects::gaussian(out->data(), out->w, out->h, sigma);
    out->area = {};
    mixByMask(*out, *src, ctx.input("mask"));
    return true;
}

// Color page values: wheels lift/gamma/gain/offset each y (master), r, g, b; then the sliders
constexpr const char* kWheels[] = {"lift", "gamma", "gain", "offset"};
constexpr const char* kWheelParts[] = {"y", "r", "g", "b"};

// Loaded LUTs by path (read-only once loaded; shared by every render like the Renderer's own LUT cache)
std::shared_ptr<const ColorGrade::Lut> lutOf(const QString& path)
{
    static std::mutex mutex;
    static std::map<QString, std::shared_ptr<const ColorGrade::Lut>> luts;
    std::lock_guard lock(mutex);
    if (auto it = luts.find(path); it != luts.end()) return it->second;
    auto l = ColorGrade::loadLut(path);
    if (luts.size() > 16) luts.clear();
    luts.emplace(path, l);
    return l;
}

// Baked tables by values + LUT (baking costs more than applying at preview size; a static correction bakes once).
// The entry holds its LUT, so a LUT's address cannot be reused by another while it is here
std::shared_ptr<const ColorGrade::Baked> bakedOf(const ColorGrade::Params& p, const std::shared_ptr<const ColorGrade::Lut>& lut)
{
    struct Entry {
        ColorGrade::Params p;
        std::shared_ptr<const ColorGrade::Lut> lut;
        std::shared_ptr<const ColorGrade::Baked> baked;
    };
    static std::mutex mutex;
    static std::vector<Entry> entries; // newest last
    {
        std::lock_guard lock(mutex);
        for (auto it = entries.rbegin(); it != entries.rend(); ++it)
            if (it->lut == lut && it->p == p) return it->baked;
    }
    auto baked = ColorGrade::bake(p, lut.get());
    std::lock_guard lock(mutex);
    if (entries.size() >= 16) entries.erase(entries.begin());
    entries.push_back({p, lut, baked});
    return baked;
}

bool evalGrade(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    *out = ctx.take(Buf::Rgba);
    copyBuf(*src, *out);
    ColorGrade::Params p;
    for (int i = 0; i < 4; ++i) {
        double* wheel = i == 0 ? p.lift : i == 1 ? p.gamma : i == 2 ? p.gain : p.offset;
        for (int j = 0; j < 4; ++j)
            wheel[j] = ctx.num(QStringLiteral("%1_%2").arg(QLatin1String(kWheels[i]), QLatin1String(kWheelParts[j])));
    }
    p.contrast = ctx.num("contrast");
    p.pivot = ctx.num("pivot");
    p.saturation = ctx.num("saturation");
    p.temperature = ctx.num("temperature");
    p.tint = ctx.num("tint");
    p.exposure = ctx.num("exposure");
    const QString lp = ctx.text("lut");
    const auto lut = lp.isEmpty() ? nullptr : lutOf(lp);
    if (p.isNeutral() && !lut) return true;
    ColorGrade::apply(out->data(), out->w, out->h, *bakedOf(p, lut), 1);
    mixByMask(*out, *src, ctx.input("mask"));
    return true;
}

const QStringList& fxEffects()
{
    static const QStringList ids = {"none",
                                    "frei0r.glow",
                                    "frei0r.colorhalftone",
                                    "frei0r.rgbnoise",
                                    "frei0r.scanline0r",
                                    "frei0r.nosync0r",
                                    "frei0r.cairogradient",
                                    "frei0r.alpha0ps_alpha0ps",
                                    "chromakey"};
    return ids;
}

// Any own picture effect; its values under its own keys prefixed "e_" (missing = the effect's default)
bool evalFx(NodeCtx& ctx, Buf* out)
{
    const Buf* src = ctx.input("src");
    if (!src || src->kind != Buf::Rgba) return false;
    *out = ctx.take(Buf::Rgba);
    copyBuf(*src, *out);
    const QString id = ctx.text("effect");
    const EffectDescriptor* d = id == QLatin1String("none") ? nullptr : EffectRegistry::find(id);
    if (!d || !VideoEffects::isOwn(id)) return true;
    auto find = [d](const char* name) -> const EffectParam* {
        for (const EffectParam& p : d->params)
            if (p.mltProperty == QLatin1String(name)) return &p;
        return nullptr;
    };
    // A value given to the node (text() is empty for a missing one), else the effect's default
    auto given = [&ctx](const EffectParam& p, QString* s) {
        *s = ctx.text(QStringLiteral("e_") + p.key);
        return !s->isEmpty();
    };
    VideoEffects::Params vp;
    vp.has = [find](const char* name) { return find(name) != nullptr; };
    vp.num = [&](const char* name) {
        const EffectParam* p = find(name);
        QString s;
        if (given(*p, &s)) {
            if (p->type == EffectParam::Bool) return s == QLatin1String("true") || s == QLatin1String("1") ? 1.0 : 0.0;
            return ctx.num(QStringLiteral("e_") + p->key);
        }
        const QVariant v = p->defaultValue;
        return p->type == EffectParam::Bool ? (v.toBool() ? 1.0 : 0.0) : v.toDouble();
    };
    vp.color = [&](const char* name) {
        const EffectParam* p = find(name);
        QString s;
        return given(*p, &s) ? QColor(s) : p->defaultValue.value<QColor>();
    };
    vp.text = [&](const char* name) {
        const EffectParam* p = find(name);
        QString s;
        return given(*p, &s) ? s : p->defaultValue.toString();
    };
    VideoEffects::apply(id, out->data(), out->w, out->h, vp, ctx.frameNo);
    out->area = {};
    mixByMask(*out, *src, ctx.input("mask"));
    return true;
}

bool evalColor(NodeCtx& ctx, Buf* out)
{
    const QColor c = ctx.color("color");
    *out = ctx.take(Buf::Rgba);
    const uint8_t px[4] = {uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue()), uint8_t(c.alpha())};
    uint32_t word;
    std::memcpy(&word, px, 4);
    if (word != 0) std::fill_n(reinterpret_cast<uint32_t*>(out->data()), size_t(out->w) * out->h, word);
    return true;
}

// Linear (from p0 to p1) or radial (centre p0, radius = distance to p1) with 2-4 stops (color c<i> at position s<i>)
bool evalGradient(NodeCtx& ctx, Buf* out)
{
    *out = ctx.take(Buf::Rgba);
    const int w = out->w, h = out->h;
    const bool radial = ctx.text("shape") == QLatin1String("radial");
    const int stops = std::clamp(int(std::lround(ctx.num("stops"))), 2, 4);
    float col[4][4], pos[4];
    for (int i = 0; i < stops; ++i) {
        const QColor c = ctx.color(QStringLiteral("c%1").arg(i));
        col[i][0] = c.redF(), col[i][1] = c.greenF(), col[i][2] = c.blueF(), col[i][3] = c.alphaF();
        pos[i] = float(std::clamp(ctx.num(QStringLiteral("s%1").arg(i)), 0.0, 1.0));
        if (i > 0) pos[i] = std::max(pos[i], pos[i - 1]);
    }
    const float sx = float(ctx.num("x0") * w), sy = float(ctx.num("y0") * h);
    const float dx = float(ctx.num("x1") * w) - sx, dy = float(ctx.num("y1") * h) - sy;
    const float len2 = std::max(1e-6f, dx * dx + dy * dy);
    Compose::forRows(h, 16, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            uint8_t* o = out->data() + size_t(y) * w * 4;
            for (int x = 0; x < w; ++x, o += 4) {
                const float px = x + 0.5f - sx, py = y + 0.5f - sy;
                const float t = radial ? std::sqrt((px * px + py * py) / len2) : (px * dx + py * dy) / len2;
                int i = 0;
                while (i + 1 < stops - 1 && t > pos[i + 1]) ++i;
                const float span = pos[i + 1] - pos[i];
                const float k = span > 1e-6f ? std::clamp((t - pos[i]) / span, 0.0f, 1.0f) : (t >= pos[i + 1] ? 1.0f : 0.0f);
                for (int c = 0; c < 4; ++c)
                    o[c] = uint8_t((col[i][c] + (col[i + 1][c] - col[i][c]) * k) * 255.0f + 0.5f);
            }
        }
    });
    return true;
}

bool evalMedia(NodeCtx& ctx, Buf* out)
{
    const QString path = ctx.text(QString::fromLatin1(::Comp::kPathKey));
    if (path.isEmpty() || !ctx.mediaPicture) return false;
    Buf b = ctx.mediaPicture(path, ctx.num("offset") + ctx.t * ctx.num("speed"));
    if (b.isNull()) return false;
    *out = std::move(b);
    return true;
}

// The engine hands the clip's picture on without a copy; this is the single-node runner's way
bool evalClip(NodeCtx& ctx, Buf* out)
{
    const Buf* c = ctx.clipPicture ? ctx.clipPicture() : nullptr;
    if (!c || c->isNull()) return false;
    *out = ctx.take(Buf::Rgba);
    copyBuf(*c, *out);
    return true;
}

} // namespace

void registerCoreNodes(QVector<NodeType>& out)
{
    const QColor black(0, 0, 0), white(255, 255, 255);
    {
        NodeType k;
        k.id = "clip";
        k.name = T("Clip");
        k.description = T("Das Bild des Clips mit seinen Effekten (bei einem Compound auch eine einzelne Spur)");
        k.params = {num("track", T("Spur"), -1, -1, 99, 1, 0,
                        T("Nur diese Spur der inneren Sequenz (0 = unterste); -1 = das ganze Bild"))};
        k.varying = true;
        k.eval = evalClip;
        out << k;
    }
    {
        NodeType k;
        k.id = ::Comp::kMediaType;
        k.name = T("Medien");
        k.description = T("Eine Datei aus dem Medienpool (Hintergrund, Logo, Textur)");
        k.params = {other(EffectParam::Path, ::Comp::kPathKey, T("Datei"), QString()),
                    num("offset", T("Versatz"), 0, -1e6, 1e6, 1, 0, T("Bilder, um die die Datei später beginnt")),
                    num("speed", T("Tempo"), 1, -16, 16, 0.01, 2)};
        k.varying = true;
        k.eval = evalMedia;
        out << k;
    }
    {
        NodeType k;
        k.id = "color";
        k.name = T("Farbfläche");
        k.description = T("Eine Fläche in einer Farbe");
        k.params = {other(EffectParam::Color, "color", T("Farbe"), black)};
        k.eval = evalColor;
        out << k;
    }
    {
        NodeType k;
        k.id = "gradient";
        k.name = T("Verlauf");
        k.description = T("Linearer oder runder Farbverlauf mit 2 bis 4 Farben");
        k.params = {other(EffectParam::Choice, "shape", T("Form"), QString("linear"), {"linear", "radial"},
                          {T("Linear"), T("Radial")}),
                    num("stops", T("Farben"), 2, 2, 4, 1, 0),
                    num("x0", T("Start X"), 0.5, -1, 2), num("y0", T("Start Y"), 0, -1, 2),
                    num("x1", T("Ende X"), 0.5, -1, 2), num("y1", T("Ende Y"), 1, -1, 2),
                    other(EffectParam::Color, "c0", T("Farbe 1"), black), num("s0", T("Stelle 1"), 0, 0, 1),
                    other(EffectParam::Color, "c1", T("Farbe 2"), white), num("s1", T("Stelle 2"), 1, 0, 1),
                    other(EffectParam::Color, "c2", T("Farbe 3"), white), num("s2", T("Stelle 3"), 1, 0, 1),
                    other(EffectParam::Color, "c3", T("Farbe 4"), white), num("s3", T("Stelle 4"), 1, 0, 1)};
        k.eval = evalGradient;
        out << k;
    }
    {
        NodeType k;
        k.id = "merge";
        k.name = T("Mischen");
        k.description = T("Legt den Vordergrund mit einem Mischmodus auf den Hintergrund");
        k.inputs = {"bg", "fg", "mask"};
        k.params = {other(EffectParam::Choice, "mode", T("Mischmodus"), QString("normal"), mergeModes(),
                          {T("Normal"), T("Addieren"), T("Negativ multiplizieren"), T("Multiplizieren"), T("Überlagern"),
                           T("Abdunkeln"), T("Aufhellen"), T("Differenz")}),
                    num("opacity", T("Deckkraft"), 1, 0, 1)};
        k.eval = evalMerge;
        out << k;
    }
    {
        NodeType k;
        k.id = "transform";
        k.name = T("Transformieren");
        k.description = T("Verschieben, vergrößern und drehen (folgt auf Wunsch einem Tracker)");
        k.inputs = {"src"};
        k.params = {num("x", T("Position X"), 0.5, -2, 3), num("y", T("Position Y"), 0.5, -2, 3),
                    num("size", T("Größe"), 1, 0, 20), num("angle", T("Drehung"), 0, -3600, 3600, 0.1, 2),
                    num("ax", T("Ankerpunkt X"), 0.5, -1, 2), num("ay", T("Ankerpunkt Y"), 0.5, -1, 2),
                    num("follow", T("Folgt Tracker"), 0, 0, 1e6, 1, 0,
                        T("Id eines Tracker-Knotens (0 = keiner); die Position zählt dann als Versatz"))};
        k.eval = evalTransform;
        out << k;
    }
    {
        NodeType k;
        k.id = "crop";
        k.name = T("Zuschneiden");
        k.description = T("Schneidet die Ränder ab (durchsichtig), auf Wunsch mit weicher Kante");
        k.inputs = {"src"};
        k.params = {num("left", T("Links"), 0, 0, 1), num("top", T("Oben"), 0, 0, 1), num("right", T("Rechts"), 0, 0, 1),
                    num("bottom", T("Unten"), 0, 0, 1), num("soft", T("Weiche Kante"), 0, 0, 500, 1, 1)};
        k.eval = evalCrop;
        out << k;
    }
    {
        NodeType k;
        k.id = "blur";
        k.name = T("Unschärfe");
        k.description = T("Gaußsche Unschärfe, auf Wunsch nur in einer Maske");
        k.inputs = {"src", "mask"};
        k.params = {num("radius", T("Radius"), 8, 0, 500, 0.1, 1)};
        k.eval = evalBlur;
        out << k;
    }
    {
        NodeType k;
        k.id = "grade";
        k.name = T("Farbkorrektur");
        k.description = T("Die Werte der Color-Seite und eine LUT, auf Wunsch nur in einer Maske");
        k.inputs = {"src", "mask"};
        const ColorGrade::Params neutral;
        for (int i = 0; i < 4; ++i) {
            const double* wheel = i == 0 ? neutral.lift : i == 1 ? neutral.gamma : i == 2 ? neutral.gain : neutral.offset;
            for (int j = 0; j < 4; ++j) {
                EffectParam p = num("", QStringLiteral("%1 %2").arg(QLatin1String(kWheels[i]),
                                                                       QString::fromLatin1(kWheelParts[j]).toUpper()),
                                    wheel[j], -100, 100);
                p.key = QStringLiteral("%1_%2").arg(QLatin1String(kWheels[i]), QLatin1String(kWheelParts[j]));
                k.params << p;
            }
        }
        k.params << num("contrast", T("Kontrast"), neutral.contrast, 0, 2) << num("pivot", T("Pivot"), neutral.pivot, 0, 1)
                 << num("saturation", T("Sättigung"), neutral.saturation, 0, 100, 0.1, 1)
                 << num("temperature", T("Temperatur"), neutral.temperature, -4000, 4000, 1, 0)
                 << num("tint", T("Tönung"), neutral.tint, -100, 100, 0.1, 1)
                 << num("exposure", T("Belichtung"), neutral.exposure, -5, 5)
                 << other(EffectParam::Path, "lut", T("LUT"), QString());
        k.eval = evalGrade;
        out << k;
    }
    {
        NodeType k;
        k.id = "fx";
        k.name = T("Effekt");
        k.description = T("Ein eigener Bild-Effekt (Glühen, Rasterdruck, Rauschen …); Werte unter e_<Schlüssel>");
        k.inputs = {"src", "mask"};
        QStringList labels;
        for (const QString& id : fxEffects()) {
            const EffectDescriptor* d = EffectRegistry::find(id);
            labels << (d ? d->name : T("Keiner"));
        }
        k.params = {other(EffectParam::Choice, "effect", T("Effekt"), QString("none"), fxEffects(), labels)};
        // Only noise draws a new pattern each frame (VideoEffects::apply's frameNo); the others are cached when static
        k.isVarying = [](const NodeType& kind, const QVariantMap& values) {
            return textValue(kind, values, QStringLiteral("effect")) == QLatin1String("frei0r.rgbnoise");
        };
        k.eval = evalFx;
        out << k;
    }
}

} // namespace Picture::Comp
