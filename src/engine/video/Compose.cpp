#include "engine/video/Compose.h"

#include <Mlt.h>
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

std::atomic<bool> g_qtReference{false};

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

// fn(first, last) for row ranges covering [0, rows); minRows per job keeps small pictures on one thread
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

void setQtReference(bool on) { g_qtReference = on; }
bool qtReference() { return g_qtReference; }

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

// ---- MLT services ----

struct Place {
    double x, y, w, h; // rectangle in pixels of the requested size
    double angle;      // degrees, clockwise
    double opacity;
};

// The services' rectangle at pos (profile pixels, scaled to the requested size w x h)
Place placeAt(mlt_properties p, int pos, int len, int pw, int ph, int w, int h)
{
    Place pl{0, 0, double(w), double(h), 0, 1};
    if (mlt_properties_get(p, "rect")) {
        const mlt_rect r = mlt_properties_anim_get_rect(p, "rect", pos, len);
        const double sx = pw > 0 ? double(w) / pw : 1.0, sy = ph > 0 ? double(h) / ph : 1.0;
        if (r.x != DBL_MIN) pl.x = r.x * sx;
        if (r.y != DBL_MIN) pl.y = r.y * sy;
        if (r.w != DBL_MIN) pl.w = r.w * sx;
        if (r.h != DBL_MIN) pl.h = r.h * sy;
        if (r.o != DBL_MIN) pl.opacity = std::clamp(r.o, 0.0, 1.0);
    }
    if (mlt_properties_get(p, "rotation")) pl.angle = mlt_properties_anim_get_double(p, "rotation", pos, len);
    return pl;
}

bool fills(const Place& pl, int w, int h)
{
    constexpr double eps = 1e-3;
    const double turn = std::fmod(pl.angle, 360.0);
    return std::abs(pl.x) < eps && std::abs(pl.y) < eps && std::abs(pl.w - w) < eps && std::abs(pl.h - h) < eps
           && std::abs(turn) < 1e-6 && pl.opacity >= 1.0;
}

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

uint8_t* canvas(int w, int h, int* size)
{
    *size = mlt_image_format_size(mlt_image_rgba, w, h, nullptr);
    auto* out = static_cast<uint8_t*>(mlt_pool_alloc(*size));
    if (out) std::memset(out, 0, size_t(*size));
    return out;
}

// -- transform filter --

int transformGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    const int len = mlt_frame_pop_service_int(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    const mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
    const int pw = profile ? profile->width : *width, ph = profile ? profile->height : *height;
    if (*width <= 0 || *height <= 0) {
        *width = pw;
        *height = ph;
    }
    const int w = *width, h = *height;
    const Place pl = placeAt(fp, pos, len, pw, ph, w, h);
    const bool distort = mlt_properties_get_int(fp, "distort") != 0;
    // The source at its own size (scaled when drawn); a frame without media size (color, titles) at project size:
    // a title's outline is drawn at project size and scaled with the text (qtext did not scale it)
    int sw = mlt_properties_get_int(props, "meta.media.width"), sh = mlt_properties_get_int(props, "meta.media.height");
    const bool media = sw > 0 && sh > 0;
    if (!media) {
        sw = pw;
        sh = ph;
    }
    if (distort) mlt_properties_set_int(props, "distort", 1); // no bars from MLT's resize: the rectangle is the shape
    // The whole frame: the source as it comes, at the requested size and format (a video scales in its decoder's
    // swscale pass; qtblend handed it on at media size in YUV, see attachRequestedSize)
    if (fills(pl, w, h) && (media || (sw == w && sh == h)))
        return mlt_frame_get_image(frame, image, format, width, height, writable);
    mlt_image_format sf = mlt_image_rgba;
    uint8_t* src = nullptr;
    const int error = mlt_frame_get_image(frame, &src, &sf, &sw, &sh, 0);
    if (error || !src || sf != mlt_image_rgba) return error ? error : 1;
    int size = 0;
    uint8_t* out = canvas(w, h, &size);
    if (!out) return 1;
    draw(out, w, h, src, sw, sh, mapping(pl, sw, sh, distort, mlt_properties_get_int(fp, "rotate_center") != 0),
         pl.opacity);
    mlt_frame_set_image(frame, out, size, mlt_pool_release);
    mlt_frame_set_alpha(frame, nullptr, 0, nullptr);
    *image = out;
    *format = mlt_image_rgba;
    *width = w;
    *height = h;
    return 0;
}

mlt_frame transformProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_length2(filter, frame)));
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, transformGetImage);
    return frame;
}

// -- blend transition --

bool opaque(const uint8_t* alpha, size_t n, size_t step)
{
    for (size_t i = 0; i < n; ++i, alpha += step)
        if (*alpha != 255) return false;
    return true;
}

int blendGetImage(mlt_frame a, uint8_t** image, mlt_image_format* format, int* width, int* height, int writable)
{
    auto transition = static_cast<mlt_transition>(mlt_frame_pop_service(a));
    mlt_frame b = mlt_frame_pop_frame(a);
    const int pos = mlt_frame_pop_service_int(a);
    const int len = mlt_frame_pop_service_int(a);
    mlt_properties tp = MLT_TRANSITION_PROPERTIES(transition);
    const mlt_profile profile = mlt_service_profile(MLT_TRANSITION_SERVICE(transition));
    const int pw = profile ? profile->width : *width, ph = profile ? profile->height : *height;
    if (*width <= 0 || *height <= 0) {
        *width = pw;
        *height = ph;
    }
    const int w = *width, h = *height;
    if (!b || mlt_frame_is_test_card(b)) return mlt_frame_get_image(a, image, format, width, height, writable);
    mlt_properties ap = MLT_FRAME_PROPERTIES(a), bp = MLT_FRAME_PROPERTIES(b);
    // the consumer's wishes (scaler, deinterlacer …) reach the upper track's frame too
    mlt_properties_copy(bp, ap, "consumer.");
    mlt_properties_pass_list(bp, ap, "consumer_aspect_ratio,consumer_deinterlace,deinterlace_method");
    const Place pl = placeAt(tp, pos, len, pw, ph, w, h);
    // RGBA asked (preview), an RGBA source (pictures, titles) or one MLT's resizer will put bars around: b in RGBA
    // right away (bars in YUV 4:2:0 came out with soft, shifted edges). Else b as it comes, no conversion: an opaque
    // one is handed on as it is (export of a plain video: no conversion at all), a transparent one mixed in RGBA.
    const auto bars = [&] {
        const int fw = mlt_properties_get_int(bp, "width"), fh = mlt_properties_get_int(bp, "height");
        double sar = mlt_properties_get_double(bp, "aspect_ratio");
        if (sar <= 0) sar = 1.0;
        if (fw <= 0 || fh <= 0 || !profile) return false;
        const double own = fw * sar / fh, frame = mlt_profile_dar(profile);
        return std::abs(own - frame) > 0.005 * frame;
    };
    mlt_image_format bf = *format == mlt_image_rgba || mlt_properties_get_int(bp, "format") == mlt_image_rgba || bars()
                              ? mlt_image_rgba
                              : mlt_image_none;
    uint8_t* bi = nullptr;
    int bw = w, bh = h;
    int error = mlt_frame_get_image(b, &bi, &bf, &bw, &bh, 0);
    if (error || !bi) return mlt_frame_get_image(a, image, format, width, height, writable);
    if (fills(pl, w, h) && bw == w && bh == h) {
        bool covers = false;
        if (bf == mlt_image_rgba) {
            covers = opaque(bi + 3, size_t(w) * h, 4);
        } else {
            int asize = 0;
            const uint8_t* alpha = mlt_frame_get_alpha_size(b, &asize);
            covers = !alpha || (size_t(asize) >= size_t(w) * h && opaque(alpha, size_t(w) * h, 1));
        }
        if (covers) { // the lower track is never asked for its picture
            // In the format asked for, converted by b with its own standard (an RGBA picture of the transform in
            // YUV export: b knows the source's standard, a does not)
            if (*format != mlt_image_none && bf != *format) {
                bf = *format;
                if (mlt_frame_get_image(b, &bi, &bf, &bw, &bh, 0) || !bi)
                    return mlt_frame_get_image(a, image, format, width, height, writable);
            }
            mlt_properties_inc_ref(bp);
            mlt_properties_set_data(ap, "_schneidi_blend_b", b, 0, reinterpret_cast<mlt_destructor>(mlt_frame_close),
                                    nullptr);
            mlt_frame_set_image(a, bi, 0, nullptr); // lives with b, which a now holds
            mlt_frame_set_alpha(a, nullptr, 0, nullptr);
            mlt_properties_pass_list(ap, bp, "colorspace,full_range,color_trc,color_primaries,progressive,top_field_first");
            *image = bi;
            *format = bf;
            return 0;
        }
    }
    mlt_image_format af = mlt_image_rgba;
    uint8_t* ai = nullptr;
    int aw = w, ah = h;
    error = mlt_frame_get_image(a, &ai, &af, &aw, &ah, 1);
    if (error || !ai || af != mlt_image_rgba) return error ? error : 1;
    if (bf != mlt_image_rgba) {
        bf = mlt_image_rgba;
        error = mlt_frame_get_image(b, &bi, &bf, &bw, &bh, 0);
        if (error || !bi || bf != mlt_image_rgba) bi = nullptr;
    }
    if (bi) {
        const Place on{pl.x * aw / w, pl.y * ah / h, pl.w * aw / w, pl.h * ah / h, pl.angle, pl.opacity};
        if (bw == aw && bh == ah && std::abs(on.x) < 1e-3 && std::abs(on.y) < 1e-3 && std::abs(on.w - aw) < 1e-3
            && std::abs(on.h - ah) < 1e-3 && std::abs(std::fmod(on.angle, 360.0)) < 1e-6)
            over(ai, bi, aw, ah, on.opacity);
        else
            draw(ai, aw, ah, bi, bw, bh,
                 mapping(on, bw, bh, mlt_properties_get_int(tp, "distort") != 0,
                         mlt_properties_get_int(tp, "rotate_center") != 0),
                 on.opacity);
    }
    *image = ai;
    *format = mlt_image_rgba;
    *width = aw;
    *height = ah;
    return 0;
}

mlt_frame blendProcess(mlt_transition transition, mlt_frame a, mlt_frame b)
{
    mlt_frame_push_service_int(a, int(mlt_transition_get_length(transition)));
    mlt_frame_push_service_int(a, int(mlt_transition_get_position(transition, a)));
    mlt_frame_push_frame(a, b);
    mlt_frame_push_service(a, transition);
    mlt_frame_push_get_image(a, blendGetImage);
    return a;
}

// -- crop filter --

int cropGetImage(mlt_frame frame, uint8_t** image, mlt_image_format* format, int* width, int* height, int)
{
    auto filter = static_cast<mlt_filter>(mlt_frame_pop_service(frame));
    const int pos = mlt_frame_pop_service_int(frame);
    const int len = mlt_frame_pop_service_int(frame);
    mlt_properties fp = MLT_FILTER_PROPERTIES(filter);
    if (!mlt_properties_get(fp, "rect")) return mlt_frame_get_image(frame, image, format, width, height, 0);
    *format = mlt_image_rgba;
    const int error = mlt_frame_get_image(frame, image, format, width, height, 1);
    if (error || !*image || *format != mlt_image_rgba) return error;
    const mlt_profile profile = mlt_service_profile(MLT_FILTER_SERVICE(filter));
    const int w = *width, h = *height;
    const double sx = profile && profile->width > 0 ? double(w) / profile->width : 1.0;
    const double sy = profile && profile->height > 0 ? double(h) / profile->height : 1.0;
    const mlt_rect r = mlt_properties_anim_get_rect(fp, "rect", pos, len);
    uint8_t color[4] = {0, 0, 0, 0};
    if (mlt_properties_get(fp, "color")) {
        const mlt_color c = mlt_properties_anim_get_color(fp, "color", pos, len);
        color[0] = c.r;
        color[1] = c.g;
        color[2] = c.b;
        color[3] = c.a;
    }
    crop(*image, w, h, r.x * sx, r.y * sy, (r.x + r.w) * sx, (r.y + r.h) * sy, color);
    mlt_frame_set_alpha(frame, nullptr, 0, nullptr);
    return 0;
}

mlt_frame cropProcess(mlt_filter filter, mlt_frame frame)
{
    mlt_frame_push_service_int(frame, int(mlt_filter_get_length2(filter, frame)));
    mlt_frame_push_service_int(frame, int(mlt_filter_get_position(filter, frame)));
    mlt_frame_push_service(frame, filter);
    mlt_frame_push_get_image(frame, cropGetImage);
    return frame;
}

using Process = mlt_frame (*)(mlt_filter, mlt_frame);

mlt_filter newFilter(mlt_profile profile, Process process, const char* service)
{
    mlt_filter f = mlt_filter_new();
    if (!f) return nullptr;
    f->process = process;
    // read back by mlt_service_profile()
    mlt_properties_set_data(MLT_FILTER_PROPERTIES(f), "_profile", profile, 0, nullptr, nullptr);
    mlt_properties_set(MLT_FILTER_PROPERTIES(f), "mlt_service", service);
    return f;
}

mlt_transition newTransition(mlt_profile profile, const char* service)
{
    mlt_transition t = mlt_transition_new();
    if (!t) return nullptr;
    t->process = blendProcess;
    mlt_properties_set_data(MLT_TRANSITION_PROPERTIES(t), "_profile", profile, 0, nullptr, nullptr);
    mlt_properties_set(MLT_TRANSITION_PROPERTIES(t), "mlt_service", service);
    mlt_properties_set_int(MLT_TRANSITION_PROPERTIES(t), "_transition_type", 1); // video only
    return t;
}

Mlt::Filter ownFilter(Mlt::Profile& profile, Process process, const char* service)
{
    mlt_filter f = newFilter(profile.get_profile(), process, service);
    if (!f) return Mlt::Filter();
    Mlt::Filter filter(f); // holds its own reference
    mlt_filter_close(f);
    return filter;
}

// Factory entries (MLT names the service by the id it was asked for)
void* createTransform(mlt_profile profile, mlt_service_type, const char* id, const void*)
{
    return newFilter(profile, transformProcess, id);
}
void* createCrop(mlt_profile profile, mlt_service_type, const char* id, const void*)
{
    return newFilter(profile, cropProcess, id);
}
void* createBlend(mlt_profile profile, mlt_service_type, const char* id, const void*)
{
    return newTransition(profile, id);
}

} // namespace

void registerServices()
{
    mlt_repository repo = mlt_factory_repository();
    if (!repo) return;
    mlt_repository_register(repo, mlt_service_filter_type, "schneidi_transform", createTransform);
    mlt_repository_register(repo, mlt_service_filter_type, "schneidi_crop", createCrop);
    mlt_repository_register(repo, mlt_service_transition_type, "schneidi_blend", createBlend);
    // Without MLT's Qt module its names lead here (old graphs, Kdenlive/Shotcut presets, tests asking for them)
    if (!mlt_properties_get_data(mlt_repository_filters(repo), "qtblend", nullptr))
        mlt_repository_register(repo, mlt_service_filter_type, "qtblend", createTransform);
    if (!mlt_properties_get_data(mlt_repository_filters(repo), "qtcrop", nullptr))
        mlt_repository_register(repo, mlt_service_filter_type, "qtcrop", createCrop);
    if (!mlt_properties_get_data(mlt_repository_transitions(repo), "qtblend", nullptr))
        mlt_repository_register(repo, mlt_service_transition_type, "qtblend", createBlend);
}

Mlt::Filter cropFilter(Mlt::Profile& profile)
{
    if (g_qtReference) {
        Mlt::Filter qt(profile, "qtcrop");
        if (qt.is_valid()) return qt;
    }
    return ownFilter(profile, cropProcess, "schneidi_crop");
}

Mlt::Filter transformFilter(Mlt::Profile& profile)
{
    if (g_qtReference) {
        Mlt::Filter qt(profile, "qtblend");
        if (qt.is_valid()) return qt;
    }
    return ownFilter(profile, transformProcess, "schneidi_transform");
}

Mlt::Transition blendTransition(Mlt::Profile& profile)
{
    if (g_qtReference) {
        Mlt::Transition qt(profile, "qtblend");
        if (qt.is_valid()) return qt;
    }
    mlt_transition t = newTransition(profile.get_profile(), "schneidi_blend");
    if (!t) return Mlt::Transition();
    Mlt::Transition transition(t);
    mlt_transition_close(t);
    return transition;
}

} // namespace Compose
