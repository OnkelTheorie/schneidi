// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/video/Compose.h"

#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <latch>
#include <numbers>
#include <vector>

namespace Compose {

namespace {

// ---- Rows over threads ----

// Own pool: the jobs never wait for anything, so a frame rendered on any thread (export workers, preview) can hand
// out its rows without the risk of waiting for itself. The calling thread takes the first share.
QThreadPool& pool()
{
    static QThreadPool* p = [] {
        auto* tp = new QThreadPool; // never destroyed: frames may still render while statics are torn down
        tp->setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
        tp->setExpiryTimeout(30000);
        return tp;
    }();
    return *p;
}

} // namespace

void forRows(int rows, int minRows, const std::function<void(int, int)>& fn)
{
    QThreadPool& tp = pool();
    const int jobs = std::clamp(rows / std::max(1, minRows), 1, tp.maxThreadCount() + 1);
    if (jobs <= 1) {
        fn(0, rows);
        return;
    }
    std::latch done(jobs - 1);
    for (int j = 1; j < jobs; ++j)
        tp.start([&fn, &done, rows, jobs, j] {
            fn(int(int64_t(rows) * j / jobs), int(int64_t(rows) * (j + 1) / jobs));
            done.count_down();
        });
    fn(0, rows / jobs);
    done.wait();
}

namespace {

// ---- Pixels ----

// Fixed point of the mix: bilinear weights in 1/128 (four weights sum to 16384), alpha A = alpha * 16384,
// premultiplied colors R = premultiplied value (0..255) * kOne
constexpr uint32_t kOne = 255u * 16384u;

// Source over: one pixel (A, R premultiplied as above, A > 0) onto the straight-alpha destination d
inline void put(uint8_t* d, uint32_t A, const uint32_t R[3])
{
    const uint32_t da = d[3];
    if (da == 255) { // opaque below (a track over another): stays opaque
        const uint32_t rest = kOne - A;
        for (int c = 0; c < 3; ++c) d[c] = uint8_t((R[c] + d[c] * rest + kOne / 2) / kOne);
        return;
    }
    if (da == 0) { // transparent below (the transform's canvas): the sample itself, back to straight alpha
        const uint64_t inv = (uint64_t(1) << 40) / A;
        for (int c = 0; c < 3; ++c)
            d[c] = uint8_t(std::min<uint64_t>(255, (uint64_t(R[c]) * inv + (uint64_t(1) << 39)) >> 40));
        d[3] = uint8_t((A + 8192) >> 14);
        return;
    }
    const uint64_t rest = kOne - A;
    const uint64_t oa = A + da * rest / 255; // alpha * 16384
    for (int c = 0; c < 3; ++c) {
        const uint64_t op = R[c] + uint64_t(d[c]) * da * rest / 255;
        d[c] = uint8_t(std::min<uint64_t>(255, (op + oa / 2) / oa));
    }
    d[3] = uint8_t(std::min<uint64_t>(255, (oa + 8192) >> 14));
}

const uint8_t kClear[4] = {0, 0, 0, 0};

// Range of k (integer) where lo < f0 + k * step < hi, intersected with [from, to)
void narrow(double f0, double step, double lo, double hi, int& from, int& to)
{
    if (std::abs(step) < 1e-12) {
        if (f0 <= lo || f0 >= hi) to = from;
        return;
    }
    double k0 = (lo - f0) / step, k1 = (hi - f0) / step;
    if (k0 > k1) std::swap(k0, k1);
    // one pixel of margin each side: rounding of the fixed point, the per-pixel check decides
    from = std::max(from, int(std::max(-1e9, std::floor(k0))) - 1);
    to = std::min(to, int(std::min(1e9, std::ceil(k1))) + 2);
}

// Prefilter for strong downscales: one mip step, 2x2 (or 2x1 / 1x2) source pixels averaged into one (colors weighted
// by alpha, straight alpha out). An odd last column/row averages with transparent pixels beyond the edge, so the
// result covers exactly twice its size (the soft edge stays where it was).
template <int HX, int HY>
void halve(const uint8_t* src, int sw, int sh, std::vector<uint8_t>& out, int& rw, int& rh)
{
    rw = (sw + HX - 1) / HX;
    rh = (sh + HY - 1) / HY;
    out.resize(size_t(rw) * rh * 4);
    constexpr uint32_t n = HX * HY;
    const size_t stride = size_t(sw) * 4;
    forRows(rh, 16, [&](int r0, int r1) {
        for (int ry = r0; ry < r1; ++ry) {
            const uint8_t* row0 = src + size_t(ry * HY) * stride;
            const uint8_t* row1 = HY == 2 && ry * 2 + 1 < sh ? row0 + stride : nullptr;
            uint8_t* o = out.data() + size_t(ry) * rw * 4;
            for (int x = 0; x < rw; ++x, o += 4) {
                const uint8_t* p[4] = {};
                int k = 0;
                const int sx = x * HX;
                p[k++] = row0 + size_t(sx) * 4;
                if (HX == 2 && sx + 1 < sw) p[k++] = row0 + size_t(sx + 1) * 4;
                if (row1) {
                    p[k++] = row1 + size_t(sx) * 4;
                    if (HX == 2 && sx + 1 < sw) p[k++] = row1 + size_t(sx + 1) * 4;
                }
                uint32_t sa = 0;
                for (int i = 0; i < k; ++i) sa += p[i][3];
                if (sa == 255 * n) { // opaque (most of a video): plain average
                    for (int c = 0; c < 3; ++c) {
                        uint32_t v = 0;
                        for (int i = 0; i < k; ++i) v += p[i][c];
                        o[c] = uint8_t((v + n / 2) / n);
                    }
                    o[3] = 255;
                    continue;
                }
                if (sa == 0) {
                    std::memset(o, 0, 4);
                    continue;
                }
                for (int c = 0; c < 3; ++c) {
                    uint32_t v = 0;
                    for (int i = 0; i < k; ++i) v += uint32_t(p[i][c]) * p[i][3];
                    o[c] = uint8_t((v + sa / 2) / sa);
                }
                o[3] = uint8_t((sa + n / 2) / n);
            }
        }
    });
}

void drawBilinear(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Affine& m, double opacity);

} // namespace

void draw(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Affine& m, double opacity)
{
    if (!dst || !src || dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    // Source pixels per destination pixel along the source's axes. From 2:1 on bilinear alone skips texels (fine
    // patterns flicker, edges stair-step): halve the source first (mip steps), bilinear does the rest (below 2:1).
    const double ax = std::hypot(m.a, m.d), ay = std::hypot(m.b, m.e);
    const int nx = ax > 0 ? int(std::clamp(1.0 / ax + 1e-9, 1.0, double(sw))) : 1;
    const int ny = ay > 0 ? int(std::clamp(1.0 / ay + 1e-9, 1.0, double(sh))) : 1;
    if (nx < 2 && ny < 2) {
        drawBilinear(dst, dw, dh, src, sw, sh, m, opacity);
        return;
    }
    // Halve until less than 2:1 is left (a mip chain, built per call: pictures change every frame)
    std::vector<uint8_t> buf[2];
    const uint8_t* cur = src;
    int cw = sw, ch = sh, fx = 1, fy = 1, i = 0;
    while (nx >= 2 * fx || ny >= 2 * fy) {
        const bool hx = nx >= 2 * fx && cw > 1, hy = ny >= 2 * fy && ch > 1;
        if (!hx && !hy) break;
        int rw = 0, rh = 0;
        std::vector<uint8_t>& out = buf[i];
        if (hx && hy) halve<2, 2>(cur, cw, ch, out, rw, rh);
        else if (hx) halve<2, 1>(cur, cw, ch, out, rw, rh);
        else halve<1, 2>(cur, cw, ch, out, rw, rh);
        fx *= hx ? 2 : 1;
        fy *= hy ? 2 : 1;
        cur = out.data();
        cw = rw;
        ch = rh;
        i ^= 1;
    }
    Affine r = m; // reduced pixel (x, y) = source pixel (x * fx, y * fy)
    r.a *= fx;
    r.d *= fx;
    r.b *= fy;
    r.e *= fy;
    drawBilinear(dst, dw, dh, cur, cw, ch, r, opacity);
}

namespace {

void drawBilinear(uint8_t* dst, int dw, int dh, const uint8_t* src, int sw, int sh, const Affine& m, double opacity)
{
    const uint32_t op = uint32_t(std::lround(std::clamp(opacity, 0.0, 1.0) * 256));
    if (op == 0) return;
    const double det = m.a * m.e - m.b * m.d;
    if (std::abs(det) < 1e-12) return;
    // Inverse: destination -> source
    const double ia = m.e / det, ib = -m.b / det, id = -m.d / det, ie = m.a / det;
    const double ic = -(ia * m.c + ib * m.f), iff = -(id * m.c + ie * m.f);
    // Bounding box of the drawn source (one pixel more for the soft edge)
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (const auto& [sx, sy] : {std::pair{0.0, 0.0}, std::pair{double(sw), 0.0}, std::pair{0.0, double(sh)},
                                 std::pair{double(sw), double(sh)}}) {
        const double x = m.a * sx + m.b * sy + m.c, y = m.d * sx + m.e * sy + m.f;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    const int bx0 = std::max(0, int(std::floor(std::max(-1e9, x0))) - 1);
    const int bx1 = std::min(dw, int(std::ceil(std::min(1e9, x1))) + 1);
    const int by0 = std::max(0, int(std::floor(std::max(-1e9, y0))) - 1);
    const int by1 = std::min(dh, int(std::ceil(std::min(1e9, y1))) + 1);
    if (bx0 >= bx1 || by0 >= by1) return;
    const size_t stride = size_t(sw) * 4;
    forRows(by1 - by0, 32, [&](int r0, int r1) {
        for (int y = by0 + r0; y < by0 + r1; ++y) {
            // Source position of the destination pixel's center, relative to texel centers
            const double u0 = ia * (bx0 + 0.5) + ib * (y + 0.5) + ic - 0.5;
            const double v0 = id * (bx0 + 0.5) + ie * (y + 0.5) + iff - 0.5;
            int from = 0, to = bx1 - bx0;
            narrow(u0, ia, -1.0, double(sw), from, to);
            narrow(v0, id, -1.0, double(sh), from, to);
            from = std::max(from, 0);
            if (from >= to) continue;
            int64_t U = std::llround((u0 + ia * from) * 65536.0), V = std::llround((v0 + id * from) * 65536.0);
            const int64_t dU = std::llround(ia * 65536.0), dV = std::llround(id * 65536.0);
            uint8_t* d = dst + (size_t(y) * dw + bx0 + from) * 4;
            for (int i = from; i < to; ++i, U += dU, V += dV, d += 4) {
                const int xi = int(U >> 16), yi = int(V >> 16);
                if (xi < -1 || xi >= sw || yi < -1 || yi >= sh) continue;
                const uint32_t fx = uint32_t(U >> 9) & 127, fy = uint32_t(V >> 9) & 127;
                const uint8_t *p00, *p10, *p01, *p11;
                if (xi >= 0 && yi >= 0 && xi + 1 < sw && yi + 1 < sh) {
                    p00 = src + size_t(yi) * stride + size_t(xi) * 4;
                    p10 = p00 + 4;
                    p01 = p00 + stride;
                    p11 = p01 + 4;
                } else { // edge: texels outside are transparent
                    auto at = [&](int x, int yy) {
                        return x >= 0 && yy >= 0 && x < sw && yy < sh ? src + size_t(yy) * stride + size_t(x) * 4 : kClear;
                    };
                    p00 = at(xi, yi);
                    p10 = at(xi + 1, yi);
                    p01 = at(xi, yi + 1);
                    p11 = at(xi + 1, yi + 1);
                }
                const uint32_t w00 = (128 - fx) * (128 - fy), w10 = fx * (128 - fy), w01 = (128 - fx) * fy, w11 = fx * fy;
                if (op == 256 && (p00[3] & p10[3] & p01[3] & p11[3]) == 255 && (d[3] == 255 || d[3] == 0)) {
                    // opaque texels (most of a video): plain bilinear color
                    for (int c = 0; c < 3; ++c)
                        d[c] = uint8_t((w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c] + 8192) >> 14);
                    d[3] = 255;
                    continue;
                }
                const uint32_t a00 = w00 * p00[3], a10 = w10 * p10[3], a01 = w01 * p01[3], a11 = w11 * p11[3];
                uint32_t A = a00 + a10 + a01 + a11;
                if (A == 0) continue;
                uint32_t R[3];
                for (int c = 0; c < 3; ++c) R[c] = a00 * p00[c] + a10 * p10[c] + a01 * p01[c] + a11 * p11[c];
                if (op != 256) {
                    A = uint32_t((uint64_t(A) * op) >> 8);
                    for (uint32_t& r : R) r = uint32_t((uint64_t(r) * op) >> 8);
                    if (A == 0) continue;
                }
                put(d, A, R);
            }
        }
    });
}

} // namespace

void over(uint8_t* dst, const uint8_t* src, int w, int h, double opacity)
{
    if (!dst || !src || w <= 0 || h <= 0) return;
    const uint32_t op = uint32_t(std::lround(std::clamp(opacity, 0.0, 1.0) * 256));
    if (op == 0) return;
    forRows(h, 32, [&](int r0, int r1) {
        const uint8_t* s = src + size_t(r0) * w * 4;
        uint8_t* d = dst + size_t(r0) * w * 4;
        for (size_t i = 0, n = size_t(r1 - r0) * w; i < n; ++i, s += 4, d += 4) {
            uint32_t sa = s[3];
            if (op != 256) sa = (sa * op + 128) >> 8;
            if (sa == 0) continue;
            if (sa == 255) {
                std::memcpy(d, s, 3);
                d[3] = 255;
                continue;
            }
            const uint32_t da = d[3];
            if (da == 255) {
                for (int c = 0; c < 3; ++c) d[c] = uint8_t((s[c] * sa + d[c] * (255 - sa) + 127) / 255);
                continue;
            }
            const uint32_t oa = sa * 255 + da * (255 - sa); // alpha * 255
            for (int c = 0; c < 3; ++c) d[c] = uint8_t((s[c] * sa * 255 + d[c] * da * (255 - sa) + oa / 2) / oa);
            d[3] = uint8_t((oa + 127) / 255);
        }
    });
}

void crop(uint8_t* rgba, int w, int h, double x0, double y0, double x1, double y1, const uint8_t color[4])
{
    if (!rgba || w <= 0 || h <= 0) return;
    // Share of each column / row inside the rectangle (0..256)
    auto shares = [](int n, double lo, double hi) {
        std::vector<uint16_t> v(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double inside = std::clamp(std::min(hi, i + 1.0) - std::max(lo, double(i)), 0.0, 1.0);
            v[size_t(i)] = uint16_t(std::lround(inside * 256));
        }
        return v;
    };
    const std::vector<uint16_t> cx = shares(w, x0, x1), cy = shares(h, y0, y1);
    uint32_t fill;
    std::memcpy(&fill, color, 4);
    const uint32_t ca = color[3];
    forRows(h, 32, [&](int r0, int r1) {
        for (int y = r0; y < r1; ++y) {
            uint8_t* row = rgba + size_t(y) * w * 4;
            const uint32_t sy = cy[size_t(y)];
            for (int x = 0; x < w; ++x) {
                const uint32_t k = (sy * cx[size_t(x)] + 128) >> 8; // share inside, 0..256
                if (k >= 256) continue;
                uint8_t* p = row + size_t(x) * 4;
                if (k == 0) {
                    std::memcpy(p, &fill, 4);
                    continue;
                }
                // inside share of the picture plus the rest of the color, mixed by alpha (straight)
                const uint32_t pa = p[3] * k, cw = ca * (256 - k); // alpha * 256
                const uint32_t oa = pa + cw;
                if (oa == 0) {
                    p[3] = 0;
                    continue;
                }
                for (int c = 0; c < 3; ++c) p[c] = uint8_t((p[c] * pa + color[c] * cw + oa / 2) / oa);
                p[3] = uint8_t((oa + 128) >> 8);
            }
        }
    });
}

namespace {

// ---- Placement ----

struct Place {
    double x, y, w, h; // rectangle in pixels of the requested size
    double angle;      // degrees, clockwise
    double opacity;
};

// Source (sw x sh) onto the place: stretched (distort) or fitted keeping its aspect ratio, rotated around the
// rectangle's center (rotateCenter) or its top left
Affine mapping(const Place& pl, int sw, int sh, bool distort, bool rotateCenter)
{
    double x = pl.x, y = pl.y, w = pl.w, h = pl.h;
    if (!distort && sw > 0 && sh > 0) {
        const double s = std::min(w / sw, h / sh);
        x += (w - sw * s) / 2;
        y += (h - sh * s) / 2;
        w = sw * s;
        h = sh * s;
    }
    const double kx = sw > 0 ? w / sw : 1.0, ky = sh > 0 ? h / sh : 1.0;
    const double rad = pl.angle * std::numbers::pi / 180.0, cs = std::cos(rad), sn = std::sin(rad);
    // around (ox, oy): p' = o + R * (S * p + t - o)
    const double ox = rotateCenter ? pl.x + pl.w / 2 : pl.x, oy = rotateCenter ? pl.y + pl.h / 2 : pl.y;
    const double tx = x - ox, ty = y - oy;
    Affine m;
    m.a = cs * kx;
    m.b = -sn * ky;
    m.c = ox + cs * tx - sn * ty;
    m.d = sn * kx;
    m.e = cs * ky;
    m.f = oy + sn * tx + cs * ty;
    return m;
}

} // namespace

Affine placement(double x, double y, double w, double h, double angle, int sw, int sh, bool distort, bool rotateCenter)
{
    return mapping(Place{x, y, w, h, angle, 1.0}, sw, sh, distort, rotateCenter);
}

} // namespace Compose
