#include "engine/VideoEffects.h"

#include <framework/mlt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace VideoEffects {

namespace {

constexpr double kPi = 3.14159265358979323846;
inline uint8_t byte(float v) { return uint8_t(std::clamp(v, 0.0f, 255.0f) + 0.5f); }
inline float smoothstep(float a, float b, float x)
{
    if (b <= a) return x < a ? 0.0f : 1.0f;
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// Rec. 709 luma and chroma (Cb, Cr) in 0..255 units
constexpr float kR = 0.2126f, kG = 0.7152f, kB = 0.0722f;
constexpr float kCb = 1.8556f, kCr = 1.5748f;

// Keyer geometry: chroma of the key, its squared length and the weight of "less saturated than the key"
struct KeyGeometry {
    float kb = 0, kr = 0, kk = 0;
    bool chroma = true; // false: the key is (almost) gray -> distance in RGB instead
    float rgb[3] = {0, 0, 0};
    explicit KeyGeometry(const QColor& c)
    {
        rgb[0] = float(c.red()), rgb[1] = float(c.green()), rgb[2] = float(c.blue());
        const float y = kR * rgb[0] + kG * rgb[1] + kB * rgb[2];
        kb = (rgb[2] - y) / kCb;
        kr = (rgb[0] - y) / kCr;
        kk = kb * kb + kr * kr;
        chroma = kk > 25.0f; // below ~5 levels of chroma there is no hue to key on
    }
    // Distance (see keyDistance) and the share of the key's chroma in the color (p, for the spill)
    float distance(float r, float g, float b, float* along) const
    {
        if (!chroma) {
            *along = 0;
            const float dr = r - rgb[0], dg = g - rgb[1], db = b - rgb[2];
            return std::sqrt(dr * dr + dg * dg + db * db) / 255.0f;
        }
        const float y = kR * r + kG * g + kB * b;
        const float cb = (b - y) / kCb, cr = (r - y) / kCr;
        const float p = (cb * kb + cr * kr) / kk;                          // along the key's hue (1 = as the key)
        const float q2 = std::max(0.0f, (cb * cb + cr * cr) / kk - p * p); // away from it (hue difference)
        const float less = 0.6f * std::max(0.0f, 1.0f - p);               // paler or darker than the key
        *along = p;
        return std::sqrt(q2 + less * less);
    }
};

// Box blur of radius r over the RGB of an RGBA buffer along lines (step = pixel distance of neighbors, n pixels per
// line); the edge pixel repeats
void boxLine(const uint8_t* src, uint8_t* dst, int n, int lines, int step, int lineStep, int r, int channels)
{
    const int div = 2 * r + 1;
    for (int l = 0; l < lines; ++l) {
        const uint8_t* s = src + size_t(l) * lineStep * 4;
        uint8_t* d = dst + size_t(l) * lineStep * 4;
        const auto px = [&](int i) { return s + size_t(std::clamp(i, 0, n - 1)) * step * 4; };
        int sum[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; ++i)
            for (int c = 0; c < channels; ++c) sum[c] += px(i)[c];
        for (int i = 0; i < n; ++i) {
            uint8_t* o = d + size_t(i) * step * 4;
            for (int c = 0; c < channels; ++c) o[c] = uint8_t((sum[c] + div / 2) / div);
            if (channels < 4) o[3] = px(i)[3];
            const uint8_t* add = px(i + r + 1);
            const uint8_t* sub = px(i - r);
            for (int c = 0; c < channels; ++c) sum[c] += add[c] - sub[c];
        }
    }
}

// Gaussian blur approximated by three box blurs (channels 3 = RGB, 4 = RGBA)
void blur(uint8_t* buf, int w, int h, double sigma, int channels = 3)
{
    if (sigma < 0.5 || w <= 0 || h <= 0) return;
    // Radii of three boxes with the same variance as the Gaussian (box of width 2r+1: variance r(r+1)/3)
    const double target = sigma * sigma;
    int r = std::max(1, int(std::floor((std::sqrt(1 + 4 * target) - 1) / 2)));
    int radii[3] = {r, r, r};
    for (int i = 0; i < 3; ++i) { // widen single boxes until the sum of the variances reaches the target
        double sum = 0;
        for (int x : radii) sum += x * (x + 1) / 3.0;
        if (sum >= target) break;
        ++radii[i];
    }
    std::vector<uint8_t> tmp(size_t(w) * h * 4);
    for (int rad : radii) {
        boxLine(buf, tmp.data(), w, h, 1, w, rad, channels);
        boxLine(tmp.data(), buf, h, w, w, 1, rad, channels);
    }
}

// Small fast hash (integer mixing, "lowbias32" constants by C. Wellons, public domain)
inline uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// ---- Blend modes (formulas of the PDF/compositing specification), a = picture, b = gradient, 0..1 ----
float blendChannel(int mode, float a, float b)
{
    switch (mode) {
    case 1: return std::min(1.0f, a + b);                                       // add
    case 3: return a * b;                                                       // multiply
    case 4: return a + b - a * b;                                               // screen
    case 5: return a <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);           // overlay
    case 6: return std::min(a, b);                                              // darken
    case 7: return std::max(a, b);                                              // lighten
    case 8: return a <= 0 ? 0 : b >= 1 ? 1 : std::min(1.0f, a / (1 - b));       // color dodge
    case 9: return a >= 1 ? 1 : b <= 0 ? 0 : 1 - std::min(1.0f, (1 - a) / b);   // color burn
    case 10: return b <= 0.5f ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);          // hard light
    case 11: {                                                                  // soft light
        if (b <= 0.5f) return a - (1 - 2 * b) * a * (1 - a);
        const float d = a <= 0.25f ? ((16 * a - 12) * a + 4) * a : std::sqrt(a);
        return a + (2 * b - 1) * (d - a);
    }
    case 12: return std::abs(a - b);  // difference
    case 13: return a + b - 2 * a * b; // exclusion
    default: return b;                 // normal
    }
}

inline float lum(const float c[3]) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }
void clipColor(float c[3])
{
    const float l = lum(c), n = std::min({c[0], c[1], c[2]}), x = std::max({c[0], c[1], c[2]});
    for (int i = 0; i < 3; ++i) {
        if (n < 0) c[i] = l + (c[i] - l) * l / std::max(1e-6f, l - n);
        if (x > 1) c[i] = l + (c[i] - l) * (1 - l) / std::max(1e-6f, x - l);
    }
}
void setLum(float c[3], float l)
{
    const float d = l - lum(c);
    for (int i = 0; i < 3; ++i) c[i] += d;
    clipColor(c);
}
inline float sat(const float c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }
void setSat(float c[3], float s)
{
    int mx = 0, mn = 0;
    for (int i = 1; i < 3; ++i) {
        if (c[i] > c[mx]) mx = i;
        if (c[i] < c[mn]) mn = i;
    }
    if (mx == mn) {
        c[0] = c[1] = c[2] = 0;
        return;
    }
    const int mid = 3 - mx - mn;
    c[mid] = (c[mid] - c[mn]) * s / (c[mx] - c[mn]);
    c[mx] = s;
    c[mn] = 0;
}

const char* const kBlendModes[] = {"normal",     "add",       "saturate",  "multiply",   "screen",     "overlay",
                                   "darken",     "lighten",   "colordodge", "colorburn", "hardlight",  "softlight",
                                   "difference", "exclusion", "hslhue",    "hslsaturation", "hslcolor", "hslluminosity"};

} // namespace

// ---- Green Screen ----

double keyDistance(const QColor& key, const QColor& pixel)
{
    float along = 0;
    return KeyGeometry(key).distance(float(pixel.red()), float(pixel.green()), float(pixel.blue()), &along);
}

void key(uint8_t* rgba, int width, int height, const KeyParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const KeyGeometry g(p.color);
    const float inner = float(std::clamp(p.tolerance, 0.0, 1.0));
    const float outer = inner + float(std::clamp(p.softness, 0.0, 1.0));
    const float spill = float(std::clamp(p.spill, 0.0, 1.0));
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; ++i, rgba += 4) {
        if (rgba[3] == 0) continue;
        const float r = rgba[0], gr = rgba[1], b = rgba[2];
        float along = 0;
        const float d = g.distance(r, gr, b, &along);
        const float a = smoothstep(inner, outer, d);
        if (a <= 0) {
            rgba[3] = 0;
            continue;
        }
        rgba[3] = byte(rgba[3] * a);
        // Spill: take the key's hue out of what stays (brightness kept), as far as the color has it
        if (spill > 0 && g.chroma && along > 0) {
            const float y = kR * r + kG * gr + kB * b;
            const float k = spill * std::min(along, 1.0f);
            const float cb = (b - y) / kCb - k * g.kb, cr = (r - y) / kCr - k * g.kr;
            const float nr = y + kCr * cr, nb = y + kCb * cb;
            const float ng = (y - kR * nr - kB * nb) / kG;
            rgba[0] = byte(nr), rgba[1] = byte(ng), rgba[2] = byte(nb);
        }
    }
}

// ---- frei0r replacements ----

void glow(uint8_t* rgba, int width, int height, double amount)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const size_t n = size_t(width) * height * 4;
    std::vector<uint8_t> soft(rgba, rgba + n);
    blur(soft.data(), width, height, std::clamp(amount, 0.0, 1.0) * height / 20.0);
    for (size_t i = 0; i < n; i += 4)
        for (int c = 0; c < 3; ++c) { // screen
            const int a = rgba[i + c], b = soft[i + c];
            rgba[i + c] = uint8_t(a + b - (a * b + 127) / 255);
        }
}

void halftone(uint8_t* rgba, int width, int height, double dot, double cyan, double magenta, double yellow)
{
    if (!rgba || width <= 0 || height <= 0) return;
    // Cell size relative to the picture height (same look in preview and export): 0.4 = 1/90 of 1080 lines
    const float cell = std::max(2.0f, float(std::clamp(dot, 0.0, 1.0) * height / 36.0) + 1.0f);
    const std::vector<uint8_t> src(rgba, rgba + size_t(width) * height * 4);
    const double angles[3] = {cyan, magenta, yellow};
    for (int ch = 0; ch < 3; ++ch) {
        const double a = angles[ch] * 2 * kPi;
        const float ca = float(std::cos(a)), sa = float(std::sin(a));
        for (int y = 0; y < height; ++y) {
            uint8_t* row = rgba + size_t(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                // position in the rotated grid (cell units)
                const float u = (ca * x + sa * y) / cell, v = (-sa * x + ca * y) / cell;
                const float u0 = std::floor(u - 0.5f), v0 = std::floor(v - 0.5f);
                float ink = 0;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i) { // the four nearest cell centers
                        const float cu = u0 + i + 0.5f, cv = v0 + j + 0.5f;
                        // ink of this channel at the cell center (cyan = 1 - red …)
                        const int sx = std::clamp(int(std::lround((ca * cu - sa * cv) * cell)), 0, width - 1);
                        const int sy = std::clamp(int(std::lround((sa * cu + ca * cv) * cell)), 0, height - 1);
                        const float amount = 1.0f - src[(size_t(sy) * width + sx) * 4 + ch] / 255.0f;
                        const float radius = std::sqrt(amount / float(kPi)) * 1.15f * cell; // dot area ~ ink
                        const float dist = std::hypot(u - cu, v - cv) * cell;
                        ink = std::max(ink, std::clamp(radius - dist + 0.5f, 0.0f, 1.0f));
                    }
                row[x * 4 + ch] = byte(255.0f * (1.0f - ink));
            }
        }
    }
}

void noise(uint8_t* rgba, int width, int height, double amount, uint32_t seed)
{
    if (!rgba || width <= 0 || height <= 0 || amount <= 0) return;
    // Sum of two uniform values per channel (bell shaped, like film grain); 1 = up to ±255
    const int scale = int(std::lround(std::clamp(amount, 0.0, 1.0) * 256));
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; ++i, rgba += 4) {
        const uint32_t h1 = mix(uint32_t(i) * 2654435761U ^ seed), h2 = mix(h1 ^ 0x68e31da4U);
        for (int c = 0; c < 3; ++c) {
            const int u = int((h1 >> (c * 10)) & 1023) + int((h2 >> (c * 10)) & 1023) - 1023; // -1023..1023
            rgba[c] = uint8_t(std::clamp(rgba[c] + u * scale / 2048, 0, 255));
        }
    }
}

void scanlines(uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0) return;
    for (int y = 1; y < height; y += 2) {
        uint8_t* row = rgba + size_t(y) * width * 4;
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) row[x * 4 + c] >>= 1;
    }
}

void noSync(uint8_t* rgba, int width, int height, double offset)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const int shift = int(std::lround(std::clamp(offset, 0.0, 1.0) * width)) % width;
    if (shift == 0) return;
    // The picture read as one long line shifted by `shift` pixels: the right end of each line wraps to the start of
    // the next one (like a TV that lost the horizontal sync)
    const size_t n = size_t(width) * height;
    std::vector<uint8_t> src(rgba, rgba + n * 4);
    for (size_t i = 0; i < n; ++i) std::memcpy(rgba + i * 4, src.data() + ((i + n - size_t(shift)) % n) * 4, 4);
}

void gradient(uint8_t* rgba, int width, int height, const GradientParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const int mode = int(std::find(std::begin(kBlendModes), std::end(kBlendModes), p.blend) - std::begin(kBlendModes));
    const float sx = float(p.startX * width), sy = float(p.startY * height);
    const float dx = float(p.endX * width) - sx, dy = float(p.endY * height) - sy;
    const float len2 = std::max(1e-6f, dx * dx + dy * dy);
    const float off = float(std::clamp(p.offset, 0.0, 0.999));
    const float c0[4] = {p.startColor.redF(), p.startColor.greenF(), p.startColor.blueF(), float(p.startOpacity)};
    const float c1[4] = {p.endColor.redF(), p.endColor.greenF(), p.endColor.blueF(), float(p.endOpacity)};
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rgba + size_t(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f - sx, py = y + 0.5f - sy;
            float t = p.radial ? std::sqrt((px * px + py * py) / len2) : (px * dx + py * dy) / len2;
            t = std::clamp((t - off) / (1 - off), 0.0f, 1.0f);
            float g[4];
            for (int c = 0; c < 4; ++c) g[c] = c0[c] + (c1[c] - c0[c]) * t;
            uint8_t* o = row + x * 4;
            const float a[3] = {o[0] / 255.0f, o[1] / 255.0f, o[2] / 255.0f};
            float out[3];
            if (mode == 2) { // saturate: only where the picture is not opaque
                const float share = std::min(g[3], 1.0f - o[3] / 255.0f);
                for (int c = 0; c < 3; ++c) out[c] = a[c] + g[c] * share;
                for (int c = 0; c < 3; ++c) o[c] = byte(out[c] * 255);
                continue;
            }
            if (mode >= 14) { // non-separable (HSL) modes
                float b[3] = {g[0], g[1], g[2]};
                float c[3] = {a[0], a[1], a[2]};
                if (mode == 14) { // hue of the gradient, saturation and brightness of the picture
                    setSat(b, sat(a));
                    setLum(b, lum(a));
                    std::copy(b, b + 3, out);
                } else if (mode == 15) {
                    setSat(c, sat(b));
                    setLum(c, lum(a));
                    std::copy(c, c + 3, out);
                } else if (mode == 16) {
                    setLum(b, lum(a));
                    std::copy(b, b + 3, out);
                } else {
                    setLum(c, lum(b));
                    std::copy(c, c + 3, out);
                }
            } else {
                for (int c = 0; c < 3; ++c) out[c] = blendChannel(mode, a[c], g[c]);
            }
            for (int c = 0; c < 3; ++c) o[c] = byte((a[c] + (out[c] - a[c]) * g[3]) * 255);
        }
    }
}

void alphaOps(uint8_t* rgba, int width, int height, const AlphaParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const size_t n = size_t(width) * height;
    std::vector<uint8_t> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = rgba[i * 4 + 3];
    std::vector<uint8_t> alpha = in;
    // Operation (ranges of the frei0r plugin's value: below 0.1 none, …)
    const double op = p.operation;
    const int radius = std::max(1, int(std::lround(std::clamp(p.amount, 0.0, 1.0) * height / 72.0)));
    auto minMax = [&](bool grow, int r) { // square min/max filter, separable
        std::vector<uint8_t> tmp(n);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                uint8_t v = alpha[size_t(y) * width + x];
                for (int k = std::max(0, x - r); k <= std::min(width - 1, x + r); ++k)
                    v = grow ? std::max(v, alpha[size_t(y) * width + k]) : std::min(v, alpha[size_t(y) * width + k]);
                tmp[size_t(y) * width + x] = v;
            }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                uint8_t v = tmp[size_t(y) * width + x];
                for (int k = std::max(0, y - r); k <= std::min(height - 1, y + r); ++k)
                    v = grow ? std::max(v, tmp[size_t(k) * width + x]) : std::min(v, tmp[size_t(k) * width + x]);
                alpha[size_t(y) * width + x] = v;
            }
    };
    auto soften = [&](double sigma) {
        std::vector<uint8_t> buf(n * 4, 0);
        for (size_t i = 0; i < n; ++i) buf[i * 4] = alpha[i];
        blur(buf.data(), width, height, sigma, 1 + 0); // only the first channel is used
        for (size_t i = 0; i < n; ++i) alpha[i] = buf[i * 4];
    };
    if (op >= 0.1 && op < 0.25) { // shave: remove single pixels and thin hairs at the edge (opening by one pixel)
        minMax(false, 1);
        minMax(true, 1);
    } else if (op < 0.35 && op >= 0.25) minMax(false, radius);         // shrink hard
    else if (op < 0.5 && op >= 0.35) { minMax(false, radius); soften(radius / 2.0); } // shrink soft
    else if (op < 0.65 && op >= 0.5) minMax(true, radius);             // grow hard
    else if (op < 0.75 && op >= 0.65) { minMax(true, radius); soften(radius / 2.0); }  // grow soft
    else if (op < 0.9 && op >= 0.75) { // threshold
        const int t = int(std::lround(std::clamp(p.threshold, 0.0, 1.0) * 255));
        for (uint8_t& a : alpha) a = a > t ? 255 : 0;
    } else if (op >= 0.9) soften(radius);
    if (p.invert)
        for (uint8_t& a : alpha) a = uint8_t(255 - a);
    const std::vector<uint8_t>& shown = p.inputAlpha ? in : alpha;
    const double d = p.display;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = rgba + i * 4;
        const uint8_t a = shown[i];
        if (d < 0.1) { // image
        } else if (d < 0.3) { // alpha as gray
            o[0] = o[1] = o[2] = a;
        } else if (d < 0.43) { // gray + red: the picture in gray, transparent parts red
            const int y = (o[0] * 54 + o[1] * 183 + o[2] * 19) >> 8;
            o[0] = uint8_t((y * a + 255 * (255 - a)) / 255);
            o[1] = o[2] = uint8_t(y * a / 255);
        } else { // selection on black / gray / white / checkers
            int bg;
            if (d < 0.57) bg = 0;
            else if (d < 0.71) bg = 128;
            else if (d < 0.9) bg = 255;
            else bg = ((i % width) / 16 + (i / width) / 16) % 2 ? 155 : 100;
            for (int c = 0; c < 3; ++c) o[c] = uint8_t((o[c] * a + bg * (255 - a)) / 255);
        }
        if (d >= 0.1) o[3] = 255;
        else o[3] = alpha[i];
    }
}

// ---- MLT filter ----

namespace {

enum class Kind { Key, Glow, Halftone, Noise, Scanlines, NoSync, Gradient, Alpha };
struct Entry {
    const char* id;
    const char* service; // mlt_service of the own filter (LiveUpdate tells filters apart by it)
    Kind kind;
};
constexpr Entry kEntries[] = {
    {"chromakey", "schneidi.key", Kind::Key},
    {"frei0r.glow", "schneidi.glow", Kind::Glow},
    {"frei0r.colorhalftone", "schneidi.halftone", Kind::Halftone},
    {"frei0r.rgbnoise", "schneidi.noise", Kind::Noise},
    {"frei0r.scanline0r", "schneidi.scanlines", Kind::Scanlines},
    {"frei0r.nosync0r", "schneidi.nosync", Kind::NoSync},
    {"frei0r.cairogradient", "schneidi.gradient", Kind::Gradient},
    {"frei0r.alpha0ps_alpha0ps", "schneidi.alpha", Kind::Alpha},
};

const Entry* entry(const QString& id)
{
    for (const Entry& e : kEntries)
        if (id == QLatin1String(e.id)) return &e;
    return nullptr;
}

struct Ctx {
    mlt_properties props;
    int pos, len;
    double num(const char* name, double def) const
    {
        if (!mlt_properties_get(props, name)) return def;
        return mlt_properties_anim_get_double(props, name, pos, len);
    }
    bool flag(const char* name, bool def) const { return mlt_properties_get(props, name) ? num(name, 0) > 0.5 : def; }
    QColor color(const char* name, const QColor& def) const
    {
        if (!mlt_properties_get(props, name)) return def;
        const mlt_color c = mlt_properties_get_color(props, name);
        return QColor(c.r, c.g, c.b);
    }
    QString text(const char* name, const char* def) const
    {
        const char* v = mlt_properties_get(props, name);
        return QString::fromUtf8(v ? v : def);
    }
};

int getImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    const int len = mlt_frame_pop_service_int(frame);
    const Kind kind = Kind(mlt_frame_pop_service_int(frame));
    *format = mlt_image_rgba;
    const int err = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (err || !*image || *format != mlt_image_rgba) return err;
    const Ctx c{MLT_FILTER_PROPERTIES(filter), pos, len};
    uint8_t* img = *image;
    const int w = *width, h = *height;
    switch (kind) {
    case Kind::Key: {
        KeyParams p;
        p.color = c.color("0", p.color);
        p.tolerance = c.num("1", p.tolerance);
        p.softness = c.num("softness", p.softness);
        p.spill = c.num("spill", p.spill);
        key(img, w, h, p);
        break;
    }
    case Kind::Glow: glow(img, w, h, c.num("0", 0)); break;
    case Kind::Halftone: halftone(img, w, h, c.num("0", 0.4), c.num("1", 0.3), c.num("2", 0.45), c.num("3", 0.25)); break;
    case Kind::Noise: {
        // a new pattern every frame, the same one for the same frame (preview = export)
        const int64_t frameNo = mlt_frame_get_position(frame);
        noise(img, w, h, c.num("0", 0.2), mix(uint32_t(frameNo) * 0x9e3779b9U + 1));
        break;
    }
    case Kind::Scanlines: scanlines(img, w, h); break;
    case Kind::NoSync: noSync(img, w, h, c.num("0", 0)); break;
    case Kind::Gradient: {
        GradientParams p;
        p.radial = c.text("0", "gradient_linear") == QLatin1String("gradient_radial");
        p.startColor = c.color("1", p.startColor);
        p.startOpacity = c.num("2", p.startOpacity);
        p.endColor = c.color("3", p.endColor);
        p.endOpacity = c.num("4", p.endOpacity);
        p.startX = c.num("5", p.startX);
        p.startY = c.num("6", p.startY);
        p.endX = c.num("7", p.endX);
        p.endY = c.num("8", p.endY);
        p.offset = c.num("9", p.offset);
        p.blend = c.text("10", "normal");
        gradient(img, w, h, p);
        break;
    }
    case Kind::Alpha: {
        AlphaParams p;
        p.display = c.num("0", p.display);
        p.inputAlpha = c.flag("1", p.inputAlpha);
        p.operation = c.num("2", p.operation);
        p.threshold = c.num("3", p.threshold);
        p.amount = c.num("4", p.amount);
        p.invert = c.flag("5", p.invert);
        alphaOps(img, w, h, p);
        break;
    }
    }
    return 0;
}

mlt_frame process(mlt_filter filter, mlt_frame frame)
{
    const auto kind = mlt_properties_get_int(MLT_FILTER_PROPERTIES(filter), "_kind");
    mlt_frame_push_service_int(frame, kind);
    mlt_frame_push_service_int(frame, int(mlt_filter_get_length2(filter, frame)));
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, getImage);
    return frame;
}

} // namespace

bool isOwn(const QString& effectId) { return entry(effectId) != nullptr; }

mlt_filter create(const QString& effectId)
{
    const Entry* e = entry(effectId);
    if (!e) return nullptr;
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = process;
    mlt_properties p = MLT_FILTER_PROPERTIES(f);
    mlt_properties_set(p, "mlt_service", e->service);
    mlt_properties_set_int(p, "_kind", int(e->kind));
    return f;
}

} // namespace VideoEffects
