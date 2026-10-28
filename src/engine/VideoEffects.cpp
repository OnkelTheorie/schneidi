// SPDX-License-Identifier: AGPL-3.0-only
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
        return chromaDistance((b - y) / kCb, (r - y) / kCr, along);
    }
    float chromaDistance(float cb, float cr, float* along) const
    {
        const float p = (cb * kb + cr * kr) / kk;                          // along the key's hue (1 = as the key)
        const float q2 = std::max(0.0f, (cb * cb + cr * cr) / kk - p * p); // away from it (hue difference)
        const float less = 0.6f * std::max(0.0f, 1.0f - p);               // paler or darker than the key
        *along = p;
        return std::sqrt(q2 + less * less);
    }
};

// ---- Blur (three box blurs in a row approximate a Gaussian) on interleaved 8-bit channels (ch bytes per pixel) ----

// Box blur of radius r along the rows; the edge pixel repeats
void boxRows(const uint8_t* src, uint8_t* dst, int w, int h, int ch, int r)
{
    const int div = 2 * r + 1;
    const uint32_t inv = (65536u + uint32_t(div) / 2) / uint32_t(div);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src + size_t(y) * w * ch;
        uint8_t* d = dst + size_t(y) * w * ch;
        for (int c = 0; c < ch; ++c) {
            uint32_t sum = 0;
            for (int i = -r; i <= r; ++i) sum += s[std::clamp(i, 0, w - 1) * ch + c];
            for (int x = 0; x < w; ++x) {
                d[x * ch + c] = uint8_t(std::min<uint32_t>(255, (sum * inv + 32768) >> 16));
                sum += s[std::min(x + r + 1, w - 1) * ch + c];
                sum -= s[std::max(x - r, 0) * ch + c];
            }
        }
    }
}

// Box blur of radius r along the columns: all columns of a row at once (contiguous memory, vectorizable)
void boxCols(const uint8_t* src, uint8_t* dst, int w, int h, int ch, int r)
{
    const int div = 2 * r + 1;
    const uint32_t inv = (65536u + uint32_t(div) / 2) / uint32_t(div);
    const size_t line = size_t(w) * ch;
    std::vector<uint32_t> sum(line, 0);
    auto row = [&](int y) { return src + size_t(std::clamp(y, 0, h - 1)) * line; };
    for (int k = -r; k <= r; ++k) {
        const uint8_t* s = row(k);
        for (size_t i = 0; i < line; ++i) sum[i] += s[i];
    }
    for (int y = 0; y < h; ++y) {
        uint8_t* d = dst + size_t(y) * line;
        for (size_t i = 0; i < line; ++i) d[i] = uint8_t(std::min<uint32_t>(255, (sum[i] * inv + 32768) >> 16));
        const uint8_t* add = row(y + r + 1);
        const uint8_t* sub = row(y - r);
        for (size_t i = 0; i < line; ++i) sum[i] += uint32_t(add[i]) - sub[i];
    }
}

// Gaussian blur with sigma (pixels) on interleaved channels
void blur(uint8_t* buf, int w, int h, int ch, double sigma)
{
    if (sigma < 0.5 || w <= 0 || h <= 0) return;
    // Radii of three boxes with the same variance as the Gaussian (box of width 2r+1: variance r(r+1)/3)
    const double target = sigma * sigma;
    const int r = std::max(1, int(std::floor((std::sqrt(1 + 4 * target) - 1) / 2)));
    int radii[3] = {r, r, r};
    for (int i = 0; i < 3; ++i) { // widen single boxes until the sum of the variances reaches the target
        double sum = 0;
        for (int x : radii) sum += x * (x + 1) / 3.0;
        if (sum >= target) break;
        ++radii[i];
    }
    std::vector<uint8_t> tmp(size_t(w) * h * ch);
    for (int rad : radii) {
        boxRows(buf, tmp.data(), w, h, ch, std::min(rad, w));
        boxCols(tmp.data(), buf, w, h, ch, std::min(rad, h));
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

namespace {

// Integer chroma of an 8-bit color (Rec. 709, 16-bit fixed point, -128..128): the coefficients of each row add up to 0
inline int chromaB(int r, int g, int b) { return (-7509 * r - 25259 * g + 32768 * b + 32768) >> 16; }
inline int chromaR(int r, int g, int b) { return (32768 * r - 29763 * g - 3005 * b + 32768) >> 16; }
constexpr int kChroma = 257; // table side: chroma -128..128

// What the key does to a color, by its chroma alone (the keyer ignores brightness): alpha and the change of the
// color that takes the spill out (the same for every brightness, since only the chroma moves), in steps of 2 levels.
// 4 bytes, so the table (264 KB) stays in the cache
struct KeyEntry {
    uint8_t alpha;
    int8_t dr, dg, db;
};

// Spill weight: full up to the end of the soft edge, falling to none at the distance of a pale gray and beyond
// (a yellow shirt keeps its yellow, a green fringe loses its green)
inline float spillWeight(float d, float outer) { return std::clamp((1.0f - d) / std::max(0.05f, 1.0f - outer), 0.0f, 1.0f); }
inline int8_t halfStep(float v) { return int8_t(std::clamp(std::lround(v / 2), -128L, 127L)); }

struct KeyTable {
    QRgb color = 0;
    float inner = -1, outer = -1, spill = -1;
    std::vector<KeyEntry> entries;
    void build(const KeyGeometry& g, float in, float out, float sp)
    {
        inner = in, outer = out, spill = sp;
        entries.resize(size_t(kChroma) * kChroma);
        for (int i = 0; i < kChroma; ++i)
            for (int j = 0; j < kChroma; ++j) {
                float p = 0;
                const float d = g.chromaDistance(float(i - 128), float(j - 128), &p);
                KeyEntry& e = entries[size_t(i) * kChroma + j];
                e.alpha = byte(255.0f * smoothstep(inner, outer, d));
                e.dr = e.dg = e.db = 0;
                if (spill > 0 && p > 0 && e.alpha > 0) {
                    const float f = spill * spillWeight(d, outer) * std::min(p, 1.0f);
                    const float dR = -kCr * f * g.kr, dB = -kCb * f * g.kb;
                    const float dG = -(kR * dR + kB * dB) / kG;
                    e.dr = halfStep(dR), e.dg = halfStep(dG), e.db = halfStep(dB);
                }
            }
    }
};

inline uint8_t clampByte(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

void key(uint8_t* rgba, int width, int height, const KeyParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const KeyGeometry g(p.color);
    const float inner = float(std::clamp(p.tolerance, 0.0, 1.0));
    const float outer = inner + float(std::clamp(p.softness, 0.0, 1.0));
    const float spill = float(std::clamp(p.spill, 0.0, 1.0));
    const size_t n = size_t(width) * height;
    if (!g.chroma) { // (almost) gray key: distance in RGB, no spill
        for (size_t i = 0; i < n; ++i, rgba += 4) {
            float along = 0;
            const float a = smoothstep(inner, outer, g.distance(rgba[0], rgba[1], rgba[2], &along));
            rgba[3] = byte(rgba[3] * a);
        }
        return;
    }
    // Table over the chroma plane, built once per setting per thread (~0.5 ms); the pixel loop is integer only
    thread_local KeyTable table;
    if (table.color != p.color.rgb() || table.inner != inner || table.outer != outer || table.spill != spill) {
        table.color = p.color.rgb();
        table.build(g, inner, outer, spill);
    }
    const KeyEntry* entries = table.entries.data();
    for (size_t i = 0; i < n; ++i, rgba += 4) { // without branches: same speed for every picture
        const int r = rgba[0], gr = rgba[1], b = rgba[2];
        const KeyEntry e = entries[(chromaB(r, gr, b) + 128) * kChroma + chromaR(r, gr, b) + 128];
        rgba[0] = clampByte(r + 2 * e.dr);
        rgba[1] = clampByte(gr + 2 * e.dg);
        rgba[2] = clampByte(b + 2 * e.db);
        rgba[3] = uint8_t((rgba[3] * e.alpha * 257 + 32896) >> 16); // a * alpha / 255, rounded
    }
}

// ---- frei0r replacements ----

void glow(uint8_t* rgba, int width, int height, double amount)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const double sigma = std::clamp(amount, 0.0, 1.0) * height / 20.0;
    const size_t n = size_t(width) * height;
    auto screen = [](int a, int b) { return uint8_t(a + b - (a * b + 127) / 255); };
    if (sigma < 1) { // no blur: the picture screened with itself
        for (size_t i = 0; i < n * 4; i += 4)
            for (int c = 0; c < 3; ++c) rgba[i + c] = screen(rgba[i + c], rgba[i + c]);
        return;
    }
    // The glow is soft anyway: blur a picture reduced by f (block means), enlarge it again bilinearly
    const int f = std::clamp(int(sigma / 3), 2, 16);
    const int sw = (width + f - 1) / f, sh = (height + f - 1) / f;
    std::vector<uint8_t> small(size_t(sw) * sh * 3);
    {
        std::vector<uint32_t> acc(size_t(sw) * 3);
        for (int by = 0; by < sh; ++by) {
            std::fill(acc.begin(), acc.end(), 0);
            const int y1 = std::min(height, (by + 1) * f);
            for (int y = by * f; y < y1; ++y) {
                const uint8_t* row = rgba + size_t(y) * width * 4;
                for (int x = 0; x < width; ++x)
                    for (int c = 0; c < 3; ++c) acc[size_t(x / f) * 3 + c] += row[x * 4 + c];
            }
            for (int bx = 0; bx < sw; ++bx) {
                const uint32_t count = uint32_t(std::min(width, (bx + 1) * f) - bx * f) * uint32_t(y1 - by * f);
                for (int c = 0; c < 3; ++c) small[(size_t(by) * sw + bx) * 3 + c] = uint8_t((acc[size_t(bx) * 3 + c] + count / 2) / count);
            }
        }
    }
    blur(small.data(), sw, sh, 3, sigma / f);
    // Bilinear: per output column the source column and weight (0..256); two horizontally enlarged source rows
    std::vector<int> x0(width), wx(width);
    for (int x = 0; x < width; ++x) {
        const double sx = std::clamp((x + 0.5) / f - 0.5, 0.0, double(sw - 1));
        x0[x] = std::min(int(sx), std::max(0, sw - 2));
        wx[x] = sw > 1 ? int(std::lround((sx - x0[x]) * 256)) : 0;
    }
    std::vector<uint16_t> rowA(size_t(width) * 3), rowB(size_t(width) * 3); // 8.8 fixed point
    int haveA = -1, haveB = -1;
    auto widen = [&](int sy, std::vector<uint16_t>& out) {
        const uint8_t* s = small.data() + size_t(sy) * sw * 3;
        const int next = sw > 1 ? 3 : 0;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = s + size_t(x0[x]) * 3;
            for (int c = 0; c < 3; ++c) out[size_t(x) * 3 + c] = uint16_t(p[c] * (256 - wx[x]) + p[c + next] * wx[x]);
        }
    };
    for (int y = 0; y < height; ++y) {
        const double sy = std::clamp((y + 0.5) / f - 0.5, 0.0, double(sh - 1));
        const int y0 = std::min(int(sy), std::max(0, sh - 2)), y1 = std::min(y0 + 1, sh - 1);
        const int wy = sh > 1 ? int(std::lround((sy - y0) * 256)) : 0;
        if (haveA != y0) {
            if (haveB == y0) std::swap(rowA, rowB), std::swap(haveA, haveB);
            else widen(y0, rowA), haveA = y0;
        }
        if (haveB != y1) widen(y1, rowB), haveB = y1;
        uint8_t* row = rgba + size_t(y) * width * 4;
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) {
                const size_t i = size_t(x) * 3 + c;
                const int b = int((uint32_t(rowA[i]) * uint32_t(256 - wy) + uint32_t(rowB[i]) * uint32_t(wy) + 32768) >> 16);
                row[x * 4 + c] = screen(row[x * 4 + c], b);
            }
    }
}

void halftone(uint8_t* rgba, int width, int height, double dot, double cyan, double magenta, double yellow)
{
    if (!rgba || width <= 0 || height <= 0) return;
    // Cell size relative to the picture height (same look in preview and export); at 1080 lines as the frei0r plugin
    // (dot 0.4 = 11.3 px, it used fixed pixels)
    const float cell = std::max(2.0f, float(std::clamp(dot, 0.0, 1.0) * height / 38.2));
    const double angles[3] = {cyan, magenta, yellow};
    // Per channel: the rotated grid of cells over the picture and the dot radius of every cell (pixels + 0.5, the
    // antialiased edge), from the ink at the cell's center (cyan = 1 - red …)
    struct Grid {
        float ca, sa;  // rotation
        float u0, v0;  // grid coordinate of the picture corner's cell row/column 0 (cell units)
        int cols, rows;
        std::vector<float> radius;
    } grids[3];
    for (int ch = 0; ch < 3; ++ch) {
        Grid& g = grids[ch];
        const double a = angles[ch] * 2 * kPi;
        g.ca = float(std::cos(a)), g.sa = float(std::sin(a));
        float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f;
        for (const auto& [x, y] : {std::pair{0.0f, 0.0f}, {float(width), 0.0f}, {0.0f, float(height)},
                                   {float(width), float(height)}}) {
            const float u = (g.ca * x + g.sa * y) / cell, v = (-g.sa * x + g.ca * y) / cell;
            umin = std::min(umin, u), umax = std::max(umax, u), vmin = std::min(vmin, v), vmax = std::max(vmax, v);
        }
        g.u0 = std::floor(umin) - 1, g.v0 = std::floor(vmin) - 1;
        g.cols = int(std::ceil(umax) - g.u0) + 3, g.rows = int(std::ceil(vmax) - g.v0) + 3;
        g.radius.resize(size_t(g.cols) * g.rows);
        for (int j = 0; j < g.rows; ++j)
            for (int i = 0; i < g.cols; ++i) {
                const float cu = g.u0 + i + 0.5f, cv = g.v0 + j + 0.5f;
                const int sx = std::clamp(int(std::lround((g.ca * cu - g.sa * cv) * cell)), 0, width - 1);
                const int sy = std::clamp(int(std::lround((g.sa * cu + g.ca * cv) * cell)), 0, height - 1);
                const float ink = 1.0f - rgba[(size_t(sy) * width + sx) * 4 + ch] / 255.0f;
                // dot radius ~ ink: dots touch at half ink, full ink covers the cell
                g.radius[size_t(j) * g.cols + i] = ink * cell + 0.5f;
            }
    }
    // Draw every dot into an ink plane per channel: rows of the circle, the inside filled, the antialiased edge
    // pixels computed (coverage = radius + 0.5 - distance); overlapping dots take the larger ink
    const size_t n = size_t(width) * height;
    std::vector<uint8_t> ink(n * 3, 0);
    for (int ch = 0; ch < 3; ++ch) {
        const Grid& g = grids[ch];
        uint8_t* plane = ink.data() + n * ch;
        for (int j = 0; j < g.rows; ++j)
            for (int i = 0; i < g.cols; ++i) {
                const float R = g.radius[size_t(j) * g.cols + i]; // radius + 0.5
                if (R <= 0.5f) continue;                            // no ink
                const float cu = g.u0 + i + 0.5f, cv = g.v0 + j + 0.5f;
                const float cx = (g.ca * cu - g.sa * cv) * cell, cy = (g.sa * cu + g.ca * cv) * cell;
                const int ya = std::max(0, int(std::ceil(cy - R))), yb = std::min(height - 1, int(std::floor(cy + R)));
                for (int y = ya; y <= yb; ++y) {
                    const float dy = y - cy;
                    const float outer2 = R * R - dy * dy;
                    if (outer2 <= 0) continue;
                    const float xo = std::sqrt(outer2);
                    const float inner2 = R >= 1 ? (R - 1) * (R - 1) - dy * dy : -1;
                    const float xi = inner2 > 0 ? std::sqrt(inner2) : -1;
                    uint8_t* row = plane + size_t(y) * width;
                    const int xa = std::max(0, int(std::ceil(cx - xo))), xb = std::min(width - 1, int(std::floor(cx + xo)));
                    int ca = xb + 1, cb = xb; // full ink between ca and cb
                    if (xi >= 0) {
                        ca = std::max(xa, int(std::ceil(cx - xi)));
                        cb = std::min(xb, int(std::floor(cx + xi)));
                        if (ca <= cb) std::memset(row + ca, 255, size_t(cb - ca + 1));
                    }
                    auto edge = [&](int x) {
                        const float dx = x - cx;
                        const float cov = std::min(1.0f, R - std::sqrt(dx * dx + dy * dy));
                        if (cov > 0) row[x] = std::max(row[x], uint8_t(cov * 255.0f + 0.5f));
                    };
                    if (ca > cb) { // no full part in this row
                        for (int x = xa; x <= xb; ++x) edge(x);
                    } else {
                        for (int x = xa; x < ca; ++x) edge(x);
                        for (int x = cb + 1; x <= xb; ++x) edge(x);
                    }
                }
            }
    }
    for (size_t i = 0; i < n; ++i)
        for (int ch = 0; ch < 3; ++ch) // the dots are black in their channel, between them the picture stays
            rgba[i * 4 + ch] = uint8_t((rgba[i * 4 + ch] * (255 - ink[n * ch + i]) + 127) / 255);
}

void noise(uint8_t* rgba, int width, int height, double amount, uint32_t seed)
{
    if (!rgba || width <= 0 || height <= 0 || amount <= 0) return;
    // Sum of three uniform values per channel (close to a Gaussian, like film grain); standard deviation 125 levels
    // per 1.0 of amount, as the frei0r plugin. u = -3069..3069 (odd steps), up to ±375 * amount levels
    const int k = int(std::lround(std::clamp(amount, 0.0, 1.0) * 375.0 * 65536 / 3069));
    const size_t n = size_t(width) * height;
    for (size_t i = 0; i < n; ++i, rgba += 4) {
        const uint32_t h1 = mix(uint32_t(i) * 2654435761U ^ seed), h2 = mix(h1 ^ 0x68e31da4U), h3 = mix(h2 ^ 0x1b873593U);
        for (int c = 0; c < 3; ++c) {
            const int sh = c * 10;
            const int u = 2 * int(((h1 >> sh) & 1023) + ((h2 >> sh) & 1023) + ((h3 >> sh) & 1023)) - 3069;
            rgba[c] = uint8_t(std::clamp(rgba[c] + ((u * k + 32768) >> 16), 0, 255));
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
    const int shift = int(std::clamp(offset, 0.0, 1.0) * height) % height;
    if (shift == 0) return;
    // The picture rolled up by `shift` lines, its top coming in again from the bottom (like the frei0r plugin)
    const size_t line = size_t(width) * 4;
    std::vector<uint8_t> top(rgba, rgba + size_t(shift) * line);
    std::memmove(rgba, rgba + size_t(shift) * line, size_t(height - shift) * line);
    std::memcpy(rgba + size_t(height - shift) * line, top.data(), top.size());
}

void gradient(uint8_t* rgba, int width, int height, const GradientParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    int mode = int(std::find(std::begin(kBlendModes), std::end(kBlendModes), p.blend) - std::begin(kBlendModes));
    if (mode >= int(std::size(kBlendModes))) mode = 0; // unknown or empty name: normal
    const float sx = float(p.startX * width), sy = float(p.startY * height);
    const float dx = float(p.endX * width) - sx, dy = float(p.endY * height) - sy;
    const float len2 = std::max(1e-6f, dx * dx + dy * dy);
    const float off = float(std::clamp(p.offset, 0.0, 0.999));
    // As in the frei0r plugin (old projects look the same): the end color and opacity sit at the start point
    const float c0[4] = {p.endColor.redF(), p.endColor.greenF(), p.endColor.blueF(), float(p.endOpacity)};
    const float c1[4] = {p.startColor.redF(), p.startColor.greenF(), p.startColor.blueF(), float(p.startOpacity)};
    for (int y = 0; y < height; ++y) {
        uint8_t* row = rgba + size_t(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const float px = x - sx, py = y - sy;
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

namespace {

// Minimum/maximum over 2r+1 neighbors along the rows of a plane, O(1) per pixel (van Herk / Gil-Werman: running
// extremes forward and backward within blocks of 2r+1)
void minMaxRows(uint8_t* plane, int w, int h, int r, bool grow)
{
    const int k = 2 * r + 1;
    const auto pick = [grow](uint8_t a, uint8_t b) { return grow ? std::max(a, b) : std::min(a, b); };
    const uint8_t pad = grow ? 0 : 255; // outside the picture: neutral for the operation
    std::vector<uint8_t> line(size_t(w) + 2 * r + k), fwd(line.size()), bwd(line.size());
    for (int y = 0; y < h; ++y) {
        uint8_t* row = plane + size_t(y) * w;
        std::fill(line.begin(), line.end(), pad);
        std::copy(row, row + w, line.begin() + r);
        const size_t n = line.size();
        for (size_t b0 = 0; b0 < n; b0 += size_t(k)) {
            const size_t b1 = std::min(n, b0 + size_t(k));
            fwd[b0] = line[b0];
            for (size_t i = b0 + 1; i < b1; ++i) fwd[i] = pick(fwd[i - 1], line[i]);
            bwd[b1 - 1] = line[b1 - 1];
            for (size_t i = b1 - 1; i > b0; --i) bwd[i - 1] = pick(bwd[i], line[i - 1]);
        }
        for (int x = 0; x < w; ++x) row[x] = pick(bwd[size_t(x)], fwd[size_t(x) + 2 * r]); // window x-r..x+r
    }
}

void transpose(const uint8_t* src, uint8_t* dst, int w, int h)
{
    constexpr int B = 32; // in tiles (cache)
    for (int y0 = 0; y0 < h; y0 += B)
        for (int x0 = 0; x0 < w; x0 += B)
            for (int y = y0; y < std::min(h, y0 + B); ++y)
                for (int x = x0; x < std::min(w, x0 + B); ++x) dst[size_t(x) * h + y] = src[size_t(y) * w + x];
}

// Square minimum (shrink) or maximum (grow) filter of radius r on a plane
void minMax(std::vector<uint8_t>& plane, int w, int h, int r, bool grow)
{
    std::vector<uint8_t> t(plane.size());
    minMaxRows(plane.data(), w, h, r, grow);
    transpose(plane.data(), t.data(), w, h);
    minMaxRows(t.data(), h, w, r, grow);
    transpose(t.data(), plane.data(), h, w);
}

} // namespace

void alphaOps(uint8_t* rgba, int width, int height, const AlphaParams& p)
{
    if (!rgba || width <= 0 || height <= 0) return;
    const size_t n = size_t(width) * height;
    std::vector<uint8_t> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = rgba[i * 4 + 3];
    std::vector<uint8_t> alpha = in;
    // Operation (ranges of the frei0r plugin's value: below 0.1 none, …); amount relative to the picture height
    const double op = p.operation;
    const int radius = std::max(1, int(std::lround(std::clamp(p.amount, 0.0, 1.0) * height / 72.0)));
    const auto soften = [&](double sigma) { blur(alpha.data(), width, height, 1, sigma); };
    if (op >= 0.1 && op < 0.25) { // shave: remove single pixels and thin hairs at the edge (opening by one pixel)
        minMax(alpha, width, height, 1, false);
        minMax(alpha, width, height, 1, true);
    } else if (op >= 0.25 && op < 0.35) {
        minMax(alpha, width, height, radius, false); // shrink hard
    } else if (op >= 0.35 && op < 0.5) {
        minMax(alpha, width, height, radius, false); // shrink soft
        soften(radius / 2.0);
    } else if (op >= 0.5 && op < 0.65) {
        minMax(alpha, width, height, radius, true); // grow hard
    } else if (op >= 0.65 && op < 0.75) {
        minMax(alpha, width, height, radius, true); // grow soft
        soften(radius / 2.0);
    } else if (op >= 0.75 && op < 0.9) { // threshold
        const int t = int(std::lround(std::clamp(p.threshold, 0.0, 1.0) * 255));
        for (uint8_t& a : alpha) a = a > t ? 255 : 0;
    } else if (op >= 0.9) {
        soften(radius); // blur
    }
    if (p.invert)
        for (uint8_t& a : alpha) a = uint8_t(255 - a);
    const std::vector<uint8_t>& shown = p.inputAlpha ? in : alpha;
    const double d = p.display;
    if (d < 0.1) { // the picture with the new alpha
        for (size_t i = 0; i < n; ++i) rgba[i * 4 + 3] = alpha[i];
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = rgba + i * 4;
        const uint8_t a = shown[i];
        if (d < 0.3) { // alpha as gray
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
        o[3] = 255;
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
