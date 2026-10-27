#include "engine/StabilizeMotion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <thread>

namespace StabilizeMotion {

namespace {

// Gray picture as floats; positions are continuous (pixel i covers [i, i + 1), its center is i + 0.5)
struct Plane {
    int w = 0, h = 0;
    std::vector<float> px;
    // bilinear, clamped at the edges
    float sample(float x, float y) const
    {
        x = std::clamp(x - 0.5f, 0.f, float(w - 1));
        y = std::clamp(y - 0.5f, 0.f, float(h - 1));
        const int x0 = std::min(int(x), w - 2), y0 = std::min(int(y), h - 2);
        const float fx = x - float(x0), fy = y - float(y0);
        const float* r = &px[size_t(y0) * w + x0];
        const float top = r[0] + (r[1] - r[0]) * fx, bottom = r[w] + (r[w + 1] - r[w]) * fx;
        return top + (bottom - top) * fy;
    }
};

Plane half(const Plane& in)
{
    Plane out;
    out.w = in.w / 2;
    out.h = in.h / 2;
    out.px.resize(size_t(out.w) * out.h);
    for (int y = 0; y < out.h; ++y) {
        const float* a = &in.px[size_t(2 * y) * in.w];
        const float* b = a + in.w;
        float* o = &out.px[size_t(y) * out.w];
        for (int x = 0; x < out.w; ++x) o[x] = 0.25f * (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1]);
    }
    return out;
}

// Gaussian smoothing with renormalized weights at the ends (sigma = radius / 2)
std::vector<double> gaussian(const std::vector<double>& v, int radius)
{
    if (radius <= 0 || v.size() < 2) return v;
    const double sigma = radius / 2.0;
    std::vector<double> w(size_t(radius) + 1);
    for (int i = 0; i <= radius; ++i) w[size_t(i)] = std::exp(-0.5 * i * i / (sigma * sigma));
    std::vector<double> out(v.size());
    const int n = int(v.size());
    for (int k = 0; k < n; ++k) {
        double s = 0, ws = 0;
        for (int j = std::max(0, k - radius); j <= std::min(n - 1, k + radius); ++j) {
            const double wj = w[size_t(std::abs(j - k))];
            s += wj * v[size_t(j)];
            ws += wj;
        }
        out[size_t(k)] = s / ws;
    }
    return out;
}

// Least squares similarity q = a p + t over the given pairs
Similarity fit(const std::vector<Complex>& p, const std::vector<Complex>& q, const std::vector<int>& idx)
{
    Complex mp = 0, mq = 0;
    for (int i : idx) {
        mp += p[size_t(i)];
        mq += q[size_t(i)];
    }
    mp /= double(idx.size());
    mq /= double(idx.size());
    Complex num = 0;
    double den = 0;
    for (int i : idx) {
        const Complex a = p[size_t(i)] - mp, b = q[size_t(i)] - mq;
        num += std::conj(a) * b;
        den += std::norm(a);
    }
    Similarity s;
    s.a = den > 0 ? num / den : Complex(1);
    s.t = mq - s.a * mp;
    return s;
}

bool plausible(const Similarity& s)
{
    const double scale = std::abs(s.a);
    return scale > 0.8 && scale < 1.25 && std::abs(std::arg(s.a)) < 0.35;
}

} // namespace

struct Estimator::Impl {
    int w = 0, h = 0;
    int levels = 1;
    int searchRadius = 0;   // coarse shift search at the top level (pixels there)
    int cols = 0, rows = 0; // feature grid
    std::vector<Plane> prev, cur;
    std::vector<float> staticScore; // per cell: how often it kept still while the picture moved (0..1)
    uint32_t rng = 0x9e3779b9u;
    bool first = true;

    uint32_t random()
    {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    }

    void pyramid(const uint8_t* luma, int stride, std::vector<Plane>& out)
    {
        out.resize(size_t(levels));
        Plane& base = out[0];
        base.w = w;
        base.h = h;
        base.px.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = luma + size_t(y) * stride;
            float* o = &base.px[size_t(y) * w];
            for (int x = 0; x < w; ++x) o[x] = s[x];
        }
        for (int l = 1; l < levels; ++l) out[size_t(l)] = half(out[size_t(l) - 1]);
    }

    // Whole-picture shift prev -> cur at the top level (integer pixels there), mean absolute difference
    Complex coarseShift() const
    {
        const Plane& a = prev.back();
        const Plane& b = cur.back();
        const int r = std::min({searchRadius, a.w / 3, a.h / 3});
        double best = 1e30;
        int bx = 0, by = 0;
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                double s = 0;
                for (int y = r; y < a.h - r; ++y) {
                    const float* pa = &a.px[size_t(y) * a.w];
                    const float* pb = &b.px[size_t(y + dy) * b.w + dx];
                    for (int x = r; x < a.w - r; ++x) s += std::abs(pa[x] - pb[x]);
                }
                s += 1e-3 * (dx * dx + dy * dy); // ties (flat picture): the smaller shift
                if (s < best) {
                    best = s;
                    bx = dx;
                    by = dy;
                }
            }
        return {double(bx), double(by)};
    }

    // Features in prev: per grid cell the position with the largest Shi-Tomasi score (level 1 if there is one).
    // Returns continuous level-0 positions and their cells.
    void features(std::vector<Complex>& pos, std::vector<int>& cell) const
    {
        const int l = levels > 1 ? 1 : 0;
        const Plane& p = prev[size_t(l)];
        const int scale = 1 << l;
        // gradients and their products, summed over 5 x 5
        const int W = p.w, H = p.h;
        std::vector<float> gxx(size_t(W) * H, 0.f), gxy(gxx.size(), 0.f), gyy(gxx.size(), 0.f);
        for (int y = 1; y < H - 1; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const size_t i = size_t(y) * W + x;
                const float ix = 0.5f * (p.px[i + 1] - p.px[i - 1]);
                const float iy = 0.5f * (p.px[i + W] - p.px[i - W]);
                gxx[i] = ix * ix;
                gxy[i] = ix * iy;
                gyy[i] = iy * iy;
            }
        auto box = [&](std::vector<float>& g) {
            std::vector<float> t(g.size(), 0.f);
            for (int y = 0; y < H; ++y)
                for (int x = 2; x < W - 2; ++x) {
                    const float* r = &g[size_t(y) * W + x];
                    t[size_t(y) * W + x] = r[-2] + r[-1] + r[0] + r[1] + r[2];
                }
            for (int y = 2; y < H - 2; ++y)
                for (int x = 0; x < W; ++x) {
                    const size_t i = size_t(y) * W + x;
                    g[i] = t[i - 2 * size_t(W)] + t[i - W] + t[i] + t[i + W] + t[i + 2 * size_t(W)];
                }
        };
        box(gxx);
        box(gxy);
        box(gyy);
        const int margin = std::max(8, W / 25);
        struct Best {
            float score = 0;
            int x = 0, y = 0;
        };
        std::vector<Best> best(size_t(cols) * rows);
        float top = 0;
        for (int y = margin; y < H - margin; ++y)
            for (int x = margin; x < W - margin; ++x) {
                const size_t i = size_t(y) * W + x;
                const float a = gxx[i], b = gxy[i], c = gyy[i];
                const float score = 0.5f * ((a + c) - std::sqrt((a - c) * (a - c) + 4 * b * b));
                const int cx = std::min(cols - 1, (x - margin) * cols / (W - 2 * margin));
                const int cy = std::min(rows - 1, (y - margin) * rows / (H - 2 * margin));
                Best& bc = best[size_t(cy) * cols + cx];
                if (score > bc.score) bc = {score, x, y};
                top = std::max(top, score);
            }
        // weak corners (flat or noise only) are left out; 25 px * (2 levels/px)^2 ~ a soft edge
        const float minScore = std::max(0.01f * top, 25.f * 4.f);
        for (int i = 0; i < int(best.size()); ++i)
            if (best[size_t(i)].score >= minScore) {
                pos.emplace_back((best[size_t(i)].x + 0.5) * scale, (best[size_t(i)].y + 0.5) * scale);
                cell.push_back(i);
            }
    }

    // Pyramidal Lucas-Kanade: displacement of the feature at p (continuous level-0 position); false = lost
    bool track(Complex p, Complex guessTop, Complex* out) const
    {
        constexpr int R = 5; // window 11 x 11
        Complex g = guessTop;
        float tmpl[(2 * R + 1) * (2 * R + 1)], gx[(2 * R + 1) * (2 * R + 1)], gy[(2 * R + 1) * (2 * R + 1)];
        for (int l = levels - 1; l >= 0; --l) {
            const Plane& a = prev[size_t(l)];
            const Plane& b = cur[size_t(l)];
            const double s = 1.0 / (1 << l);
            const float px = float(p.real() * s), py = float(p.imag() * s);
            double sxx = 0, sxy = 0, syy = 0;
            int k = 0;
            for (int j = -R; j <= R; ++j)
                for (int i = -R; i <= R; ++i, ++k) {
                    const float x = px + float(i), y = py + float(j);
                    tmpl[k] = a.sample(x, y);
                    gx[k] = 0.5f * (a.sample(x + 1, y) - a.sample(x - 1, y));
                    gy[k] = 0.5f * (a.sample(x, y + 1) - a.sample(x, y - 1));
                    sxx += double(gx[k]) * gx[k];
                    sxy += double(gx[k]) * gy[k];
                    syy += double(gy[k]) * gy[k];
                }
            const double det = sxx * syy - sxy * sxy;
            Complex v = 0;
            if (det > 1e-3 * (sxx + syy) * (sxx + syy) && det > 1e-6) {
                for (int it = 0; it < 12; ++it) {
                    double bx = 0, by = 0;
                    const float qx = px + float(g.real() + v.real()), qy = py + float(g.imag() + v.imag());
                    k = 0;
                    for (int j = -R; j <= R; ++j)
                        for (int i = -R; i <= R; ++i, ++k) {
                            const float diff = tmpl[k] - b.sample(qx + float(i), qy + float(j));
                            bx += double(diff) * gx[k];
                            by += double(diff) * gy[k];
                        }
                    const Complex dv((syy * bx - sxy * by) / det, (sxx * by - sxy * bx) / det);
                    v += dv;
                    if (std::abs(v) > 2 * R) return false; // ran away
                    if (std::norm(dv) < 1e-4) break;
                }
            } else if (l == 0) {
                return false; // no texture
            }
            g += v;
            if (l > 0) g *= 2.0;
        }
        // residual check: occlusion, flash or a different object
        const Plane& a = prev[0];
        const Plane& b = cur[0];
        const float px = float(p.real()), py = float(p.imag());
        const float qx = float(p.real() + g.real()), qy = float(p.imag() + g.imag());
        if (qx < 1 || qy < 1 || qx > float(w - 1) || qy > float(h - 1)) return false;
        double err = 0;
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) err += std::abs(a.sample(px + float(i), py + float(j)) - b.sample(qx + float(i), qy + float(j)));
        if (err / ((2 * R + 1) * (2 * R + 1)) > 20) return false;
        *out = g;
        return true;
    }


    Motion estimate()
    {
        const Complex guess = coarseShift();
        std::vector<Complex> pos;
        std::vector<int> cell;
        features(pos, cell);
        const Complex center(w / 2.0, h / 2.0);
        std::vector<Complex> p, q;
        std::vector<int> cells;
        for (size_t i = 0; i < pos.size(); ++i) {
            Complex d;
            if (!track(pos[i], guess, &d)) continue;
            p.push_back(pos[i] - center);
            q.push_back(pos[i] + d - center);
            cells.push_back(cell[i]);
        }
        const Motion none{0, 0, 0, 1, false};
        if (p.size() < 6) return none;
        // Cells that kept still while the picture moved (an overlay) do not take part
        std::vector<int> use;
        for (int i = 0; i < int(p.size()); ++i)
            if (staticScore[size_t(cells[size_t(i)])] < 0.6f) use.push_back(i);
        if (use.size() < 6) {
            use.resize(p.size());
            for (int i = 0; i < int(p.size()); ++i) use[size_t(i)] = i;
        }
        // RANSAC: similarity from two features, the one most others agree with
        const double unit = w / 960.0; // tolerances are meant for a picture 960 pixels wide
        const double thr2 = (1.5 * unit) * (1.5 * unit);
        const double minSpan2 = (0.1 * w) * (0.1 * w);
        auto inliers = [&](const Similarity& s, std::vector<int>* out) {
            int n = 0;
            double err = 0;
            for (int i : use) {
                const double e = std::norm(s.map(p[size_t(i)]) - q[size_t(i)]);
                if (e < thr2) {
                    ++n;
                    err += e;
                    if (out) out->push_back(i);
                }
            }
            return std::pair{n, err};
        };
        Similarity best;
        std::pair<int, double> bestScore{0, 0};
        for (int it = 0; it < 200; ++it) {
            const int i = use[random() % use.size()], j = use[random() % use.size()];
            const Complex dp = p[size_t(j)] - p[size_t(i)];
            if (i == j || std::norm(dp) < minSpan2) continue;
            Similarity s;
            s.a = (q[size_t(j)] - q[size_t(i)]) / dp;
            s.t = q[size_t(i)] - s.a * p[size_t(i)];
            if (!plausible(s)) continue;
            const auto score = inliers(s, nullptr);
            if (score.first > bestScore.first || (score.first == bestScore.first && score.second < bestScore.second)) {
                best = s;
                bestScore = score;
            }
        }
        const int minInliers = std::max(6, int(use.size() * 0.15));
        if (bestScore.first < minInliers) return none;
        std::vector<int> in;
        for (int round = 0; round < 2; ++round) { // refine on the inliers, twice
            in.clear();
            inliers(best, &in);
            if (int(in.size()) < minInliers) return none;
            const Similarity s = fit(p, q, in);
            if (!plausible(s)) return none;
            best = s;
        }
        // Still cells: a feature that does not move where the camera motion moves it more than a pixel
        for (int i = 0; i < int(p.size()); ++i) {
            const double model = std::abs(best.map(p[size_t(i)]) - p[size_t(i)]);
            if (model < 1.0 * unit) continue;
            float& sc = staticScore[size_t(cells[size_t(i)])];
            const bool still = std::abs(q[size_t(i)] - p[size_t(i)]) < 0.35 * unit;
            sc = 0.85f * sc + (still ? 0.15f : 0.f);
        }
        Motion m;
        m.dx = best.t.real() / w;
        m.dy = best.t.imag() / w;
        m.angle = std::arg(best.a);
        m.scale = std::abs(best.a);
        return m;
    }
};

Estimator::Estimator(int width, int height, int shakiness, int accuracy) : d(std::make_unique<Impl>())
{
    d->w = std::max(16, width);
    d->h = std::max(16, height);
    // top level at least ~60 pixels on its shorter side
    while (d->levels < 4 && (std::min(d->w, d->h) >> d->levels) >= 60) ++d->levels;
    const int topW = d->w >> (d->levels - 1);
    d->searchRadius = std::max(2, int(std::ceil(topW * 0.02 * std::clamp(shakiness, 1, 10))));
    d->cols = std::clamp(int(std::lround(4 + 1.6 * std::clamp(accuracy, 1, 15))), 4, 40);
    d->rows = std::max(3, int(std::lround(double(d->cols) * d->h / d->w)));
    d->staticScore.assign(size_t(d->cols) * d->rows, 0.f);
}

Estimator::~Estimator() = default;

Motion Estimator::push(const uint8_t* luma, int stride)
{
    d->pyramid(luma, stride, d->cur);
    Motion m;
    if (!d->first) m = d->estimate();
    d->first = false;
    std::swap(d->prev, d->cur);
    return m;
}

// ---- Camera path ----

namespace {

constexpr double kMaxZoom = 2.0;

// Zoom so that the output corners map inside the input picture (1 = none needed)
double neededZoom(const Similarity& s, double aspect)
{
    const double hx = 0.5, hy = 0.5 / aspect;
    double wmax = 1; // 1 / zoom
    auto limit = [&](double k, double t0, double lim) {
        if (k > 1e-12) wmax = std::min(wmax, (lim - t0) / k);
        else if (k < -1e-12) wmax = std::min(wmax, (lim + t0) / -k);
    };
    for (const Complex c : {Complex(-hx, -hy), Complex(hx, -hy), Complex(hx, hy), Complex(-hx, hy)}) {
        const Complex ac = s.a * c;
        limit(ac.real(), s.t.real(), hx);
        limit(ac.imag(), s.t.imag(), hy);
    }
    return wmax > 1.0 / kMaxZoom ? 1.0 / wmax : kMaxZoom;
}

} // namespace

std::vector<Similarity> corrections(const std::vector<Motion>& motions, const PathParams& p)
{
    const int n = int(motions.size());
    std::vector<Similarity> out(motions.size());
    if (n == 0) return out;
    // Camera: where a point of frame 0 is in frame k (C_k = motion_k after C_k-1), as angle, log scale, shift
    std::vector<double> th(out.size()), sg(out.size()), tx(out.size()), ty(out.size());
    Complex t = 0;
    double theta = 0, sigma = 0;
    for (int k = 0; k < n; ++k) {
        const Motion& m = motions[size_t(k)];
        if (k > 0 && m.valid) {
            t = std::polar(m.scale, m.angle) * t + Complex(m.dx, m.dy);
            theta += m.angle;
            sigma += std::log(m.scale);
        }
        th[size_t(k)] = theta;
        sg[size_t(k)] = sigma;
        tx[size_t(k)] = t.real();
        ty[size_t(k)] = t.imag();
    }
    // Smoothed camera
    std::vector<double> sth, ssg, stx, sty;
    if (p.smoothing <= 0) { // still camera: its mean position (needs the least zoom)
        auto mean = [n](const std::vector<double>& v) {
            double s = 0;
            for (double x : v) s += x;
            return std::vector<double>(size_t(n), s / n);
        };
        sth = mean(th), ssg = mean(sg), stx = mean(tx), sty = mean(ty);
    } else {
        sth = gaussian(th, p.smoothing), ssg = gaussian(sg, p.smoothing);
        stx = gaussian(tx, p.smoothing), sty = gaussian(ty, p.smoothing);
    }
    // Output -> input: C_k after S_k^-1 (the smooth camera's picture, taken from the real frame)
    std::vector<double> zoom(out.size(), 1.0);
    for (int k = 0; k < n; ++k) {
        const size_t i = size_t(k);
        const Complex a = std::exp(Complex(sg[i] - ssg[i], th[i] - sth[i]));
        Similarity& s = out[i];
        s.a = a;
        s.t = Complex(tx[i], ty[i]) - a * Complex(stx[i], sty[i]);
        zoom[i] = neededZoom(s, p.aspect);
    }
    if (p.border == Border::Auto) {
        const double z = *std::max_element(zoom.begin(), zoom.end());
        std::fill(zoom.begin(), zoom.end(), z);
    } else if (p.border == Border::Adaptive) {
        // widest need around each frame, smoothed (never below the frame's own need)
        const int r = std::max(1, p.smoothing);
        std::vector<double> wide(out.size());
        for (int k = 0; k < n; ++k)
            wide[size_t(k)] = *std::max_element(zoom.begin() + std::max(0, k - r), zoom.begin() + std::min(n, k + r + 1));
        const std::vector<double> smooth = gaussian(wide, r);
        for (int k = 0; k < n; ++k) zoom[size_t(k)] = std::max(zoom[size_t(k)], smooth[size_t(k)]);
    } else {
        std::fill(zoom.begin(), zoom.end(), 1.0);
    }
    const double extra = std::max(0.1, 1.0 + p.zoom / 100.0);
    for (int k = 0; k < n; ++k) out[size_t(k)].a /= zoom[size_t(k)] * extra;
    return out;
}

void pixelMap(const Similarity& s, int w, int h, double aspect, double m[6])
{
    // normalized u = (X - w/2) / w, v = (Y - h/2) / ky (ky = display height in the unit of the width)
    const double ky = h * aspect > 0 ? h * aspect : w;
    const double ar = s.a.real(), ai = s.a.imag();
    m[0] = ar;
    m[1] = -ai * w / ky;
    m[2] = -ar * w / 2 + ai * w * h / (2 * ky) + w * s.t.real() + w / 2.0;
    m[3] = ai * ky / w;
    m[4] = ar;
    m[5] = -ai * ky / 2 - ar * h / 2.0 + ky * s.t.imag() + h / 2.0;
}

// ---- Warps ----

namespace {

using Fixed = int64_t; // 32.32 fixed point positions in source pixel indices (pixel i's center = i)
constexpr int kShift = 32;

struct Row {
    Fixed x, y, dx, dy;
};

Row rowStart(const double m[6], int y)
{
    const double one = double(Fixed(1) << kShift);
    const double cy = y + 0.5;
    return {Fixed(std::llround((m[0] * 0.5 + m[1] * cy + m[2] - 0.5) * one)),
            Fixed(std::llround((m[3] * 0.5 + m[4] * cy + m[5] - 0.5) * one)), Fixed(std::llround(m[0] * one)),
            Fixed(std::llround(m[3] * one))};
}

// Position inside the picture's area (pixel edges included); clamps it to the outer pixel centers.
// Returns false when it lies outside the picture.
inline bool clampPos(Fixed& fx, Fixed& fy, int w, int h)
{
    const Fixed half = Fixed(1) << (kShift - 1);
    const bool inside = fx >= -half && fy >= -half && fx <= (Fixed(w - 1) << kShift) + half
                        && fy <= (Fixed(h - 1) << kShift) + half;
    fx = std::clamp(fx, Fixed(0), Fixed(w - 1) << kShift);
    fy = std::clamp(fy, Fixed(0), Fixed(h - 1) << kShift);
    return inside;
}

// Integer part (clamped to the last pair) and 0..256 weight
inline void split(Fixed f, int size, int* i, int* weight)
{
    *i = std::min(int(f >> kShift), size - 2);
    *weight = int((f - (Fixed(*i) << kShift)) >> (kShift - 8));
}

inline uint8_t bilinear(const uint8_t* s, int stride, int ax, int ay)
{
    const int top = s[0] * 256 + (s[1] - s[0]) * ax;
    const int bottom = s[stride] * 256 + (s[stride + 1] - s[stride]) * ax;
    return uint8_t((top * 256 + (bottom - top) * ay + 32768) >> 16);
}

inline uint32_t load4(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// 4 bytes -> four 16-bit lanes of a 64-bit number (byte order 0, 2, 1, 3) and back
inline uint64_t expand(uint32_t c) { return (c & 0x00FF00FFu) | (uint64_t((c >> 8) & 0x00FF00FFu) << 32); }
inline uint32_t pack(uint64_t e) { return uint32_t(e & 0x00FF00FFu) | ((uint32_t(e >> 32) & 0x00FF00FFu) << 8); }

// Bilinear of the 2 x 2 pixels at s (4 bytes each), weights 0..256: all four bytes at once. The weights add up to
// exactly 256, so a lane never exceeds 255 * 256 (+ rounding).
inline uint32_t bilinear4(const uint8_t* s, int stride, int ax, int ay)
{
    const uint64_t w11 = uint64_t((ax * ay + 128) >> 8);
    const uint64_t w10 = uint64_t(ay) - w11, w01 = uint64_t(ax) - w11, w00 = 256 - uint64_t(ax) - uint64_t(ay) + w11;
    const uint64_t sum = expand(load4(s)) * w00 + expand(load4(s + 4)) * w01 + expand(load4(s + stride)) * w10
                         + expand(load4(s + stride + 4)) * w11;
    return pack(((sum + 0x0080008000800080ull) >> 8) & 0x00FF00FF00FF00FFull);
}

// Rows [0, h) in a few threads for big pictures (the warp runs during playback, ~1 ms per thread and megapixel)
template <typename F>
void parallelRows(int w, int h, F rows)
{
    const int n = std::clamp(int(std::thread::hardware_concurrency()) / 2, 1, 4);
    if (n < 2 || int64_t(w) * h < 600'000) {
        rows(0, h);
        return;
    }
    std::thread threads[3];
    for (int i = 1; i < n; ++i) threads[i - 1] = std::thread(rows, h * i / n, h * (i + 1) / n);
    rows(0, h / n);
    for (int i = 1; i < n; ++i) threads[i - 1].join();
}

void planeRows(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside,
               uint8_t fill, int rowFrom, int rowTo)
{
    for (int y = rowFrom; y < rowTo; ++y) {
        Row r = rowStart(m, y);
        uint8_t* d = dst + size_t(y) * ds;
        for (int x = 0; x < w; ++x, r.x += r.dx, r.y += r.dy) {
            const int x0 = int(r.x >> kShift), y0 = int(r.y >> kShift);
            if (unsigned(x0) < unsigned(w - 1) && unsigned(y0) < unsigned(h - 1)) {
                const int ax = int(r.x >> (kShift - 8)) & 255, ay = int(r.y >> (kShift - 8)) & 255;
                d[x] = bilinear(src + size_t(y0) * ss + x0, ss, ax, ay);
                continue;
            }
            Fixed fx = r.x, fy = r.y;
            if (!clampPos(fx, fy, w, h) && outside != Outside::Clamp) {
                if (outside == Outside::Fill) d[x] = fill;
                continue;
            }
            int ix, iy, ax, ay;
            split(fx, w, &ix, &ax);
            split(fy, h, &iy, &ay);
            d[x] = bilinear(src + size_t(iy) * ss + ix, ss, ax, ay);
        }
    }
}

void rgbaRows(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside,
              uint32_t fill, int rowFrom, int rowTo)
{
    auto sample = [&](int ix, int iy, int ax, int ay) {
        return bilinear4(src + size_t(iy) * ss + size_t(ix) * 4, ss, ax, ay);
    };
    for (int y = rowFrom; y < rowTo; ++y) {
        Row r = rowStart(m, y);
        uint8_t* d = dst + size_t(y) * ds;
        for (int x = 0; x < w; ++x, r.x += r.dx, r.y += r.dy) {
            const int x0 = int(r.x >> kShift), y0 = int(r.y >> kShift);
            uint32_t v;
            if (unsigned(x0) < unsigned(w - 1) && unsigned(y0) < unsigned(h - 1)) {
                v = sample(x0, y0, int(r.x >> (kShift - 8)) & 255, int(r.y >> (kShift - 8)) & 255);
            } else {
                Fixed fx = r.x, fy = r.y;
                if (!clampPos(fx, fy, w, h) && outside != Outside::Clamp) {
                    if (outside == Outside::Fill) std::memcpy(d + size_t(x) * 4, &fill, 4);
                    continue;
                }
                int ix, iy, ax, ay;
                split(fx, w, &ix, &ax);
                split(fy, h, &iy, &ay);
                v = sample(ix, iy, ax, ay);
            }
            std::memcpy(d + size_t(x) * 4, &v, 4);
        }
    }
}

void rgbRows(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside,
             int rowFrom, int rowTo)
{
    for (int y = rowFrom; y < rowTo; ++y) {
        Row r = rowStart(m, y);
        uint8_t* d = dst + size_t(y) * ds;
        for (int x = 0; x < w; ++x, r.x += r.dx, r.y += r.dy) {
            Fixed fx = r.x, fy = r.y;
            if (!clampPos(fx, fy, w, h) && outside != Outside::Clamp) {
                if (outside == Outside::Fill) std::memset(d + size_t(x) * 3, 0, 3);
                continue;
            }
            int ix, iy, ax, ay;
            split(fx, w, &ix, &ax);
            split(fy, h, &iy, &ay);
            const uint8_t* s = src + size_t(iy) * ss + size_t(ix) * 3;
            for (int c = 0; c < 3; ++c) {
                const int top = s[c] * 256 + (s[c + 3] - s[c]) * ax;
                const int bottom = s[ss + c] * 256 + (s[ss + c + 3] - s[ss + c]) * ax;
                d[size_t(x) * 3 + size_t(c)] = uint8_t((top * 256 + (bottom - top) * ay + 32768) >> 16);
            }
        }
    }
}

} // namespace

void warpPlane(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside,
               uint8_t fill)
{
    if (w < 2 || h < 2) {
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * ds, src + size_t(y) * ss, size_t(w));
        return;
    }
    parallelRows(w, h, [&](int y0, int y1) { planeRows(src, ss, dst, ds, w, h, m, outside, fill, y0, y1); });
}

void warpRgba(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside,
              uint32_t fill)
{
    if (w < 2 || h < 2) {
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * ds, src + size_t(y) * ss, size_t(w) * 4);
        return;
    }
    // 4 bytes per pixel: twice the work of a plane per pixel -> threads from half the size
    parallelRows(w * 2, h, [&](int y0, int y1) { rgbaRows(src, ss, dst, ds, w, h, m, outside, fill, y0, y1); });
}

void warpRgb(const uint8_t* src, int ss, uint8_t* dst, int ds, int w, int h, const double m[6], Outside outside)
{
    if (w < 2 || h < 2) {
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * ds, src + size_t(y) * ss, size_t(w) * 3);
        return;
    }
    parallelRows(w * 2, h, [&](int y0, int y1) { rgbRows(src, ss, dst, ds, w, h, m, outside, y0, y1); });
}

} // namespace StabilizeMotion
