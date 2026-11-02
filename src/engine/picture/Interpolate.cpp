// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/picture/Interpolate.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

namespace Picture::Interp {

namespace {

// Motion search runs on a luma copy at most this wide (1080p: a third), the in-between picture at full size
constexpr int kWorkWidth = 640;
constexpr int kBlock = 8;

template <typename F>
void parallelRows(int h, int64_t work, F rows)
{
    int n = work < 200'000 ? 1 : std::clamp(int(std::thread::hardware_concurrency()), 1, 8);
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

struct Plane {
    int w = 0, h = 0;
    std::vector<uint8_t> p;
    uint8_t at(int x, int y) const { return p[size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)]; }
    float sample(float x, float y) const // bilinear, clamped to the edge
    {
        x = std::clamp(x, 0.f, float(w - 1));
        y = std::clamp(y, 0.f, float(h - 1));
        const int x0 = int(x), y0 = int(y);
        const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float fx = x - x0, fy = y - y0;
        const uint8_t* r0 = &p[size_t(y0) * w];
        const uint8_t* r1 = &p[size_t(y1) * w];
        const float top = r0[x0] + (r0[x1] - r0[x0]) * fx, bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
        return top + (bottom - top) * fy;
    }
};

Plane luma(const uint8_t* rgba, int w, int h)
{
    Plane l{w, h, std::vector<uint8_t>(size_t(w) * h)};
    parallelRows(h, int64_t(w) * h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint8_t* s = rgba + size_t(y) * w * 4;
            uint8_t* d = &l.p[size_t(y) * w];
            for (int x = 0; x < w; ++x, s += 4) d[x] = uint8_t((s[0] * 54 + s[1] * 183 + s[2] * 19) >> 8);
        }
    });
    return l;
}

// Area average to (w, h)
Plane shrink(const Plane& s, int w, int h)
{
    if (s.w == w && s.h == h) return s;
    Plane d{w, h, std::vector<uint8_t>(size_t(w) * h)};
    for (int y = 0; y < h; ++y) {
        const int sy0 = y * s.h / h, sy1 = std::max(sy0 + 1, (y + 1) * s.h / h);
        for (int x = 0; x < w; ++x) {
            const int sx0 = x * s.w / w, sx1 = std::max(sx0 + 1, (x + 1) * s.w / w);
            int sum = 0;
            for (int yy = sy0; yy < sy1; ++yy) {
                const uint8_t* r = &s.p[size_t(yy) * s.w];
                for (int xx = sx0; xx < sx1; ++xx) sum += r[xx];
            }
            d.p[size_t(y) * w + x] = uint8_t(sum / ((sy1 - sy0) * (sx1 - sx0)));
        }
    }
    return d;
}

struct Vec {
    float x = 0, y = 0;
};

// Block vectors: block (i, j) covers pixels [i * kBlock, (i + 1) * kBlock) of its level
struct Field {
    int gw = 0, gh = 0;
    std::vector<Vec> v;
    std::vector<float> cost; // mean absolute difference per pixel of the best match
    Vec at(int i, int j) const { return v[size_t(std::clamp(j, 0, gh - 1)) * gw + std::clamp(i, 0, gw - 1)]; }
};

// Sum of absolute differences of P's block at (x0, y0) against Q shifted by (dx, dy); stops early above `limit`
int sad(const Plane& P, const Plane& Q, int x0, int y0, int dx, int dy, int limit)
{
    const int bw = std::min(kBlock, P.w - x0), bh = std::min(kBlock, P.h - y0);
    const bool inside = x0 + dx >= 0 && y0 + dy >= 0 && x0 + dx + bw <= Q.w && y0 + dy + bh <= Q.h;
    int sum = 0;
    for (int y = 0; y < bh; ++y) {
        const uint8_t* p = &P.p[size_t(y0 + y) * P.w + x0];
        if (inside) {
            const uint8_t* q = &Q.p[size_t(y0 + y + dy) * Q.w + x0 + dx];
            for (int x = 0; x < bw; ++x) sum += std::abs(int(p[x]) - int(q[x]));
        } else {
            for (int x = 0; x < bw; ++x) sum += std::abs(int(p[x]) - int(Q.at(x0 + x + dx, y0 + y + dy)));
        }
        if (sum > limit) return sum;
    }
    return sum;
}

// One level of the coarse-to-fine search: P(x) ~ Q(x + v). prev = the coarser level's field (empty at the top)
Field searchLevel(const Plane& P, const Plane& Q, const Field* prev, Vec global, int radius, bool subpixel)
{
    Field f;
    f.gw = (P.w + kBlock - 1) / kBlock;
    f.gh = (P.h + kBlock - 1) / kBlock;
    f.v.resize(size_t(f.gw) * f.gh);
    f.cost.resize(f.v.size());
    parallelRows(f.gh, int64_t(P.w) * P.h, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < f.gw; ++i) {
                const int x0 = i * kBlock, y0 = j * kBlock;
                const int pixels = std::min(kBlock, P.w - x0) * std::min(kBlock, P.h - y0);
                // Candidates: the coarser level around this block (doubled), standing still, the dominant motion
                std::array<Vec, 11> cands;
                int n = 0;
                cands[n++] = Vec{};
                cands[n++] = global; // the camera's motion (a fast turn moves everything alike)
                Vec pred;
                if (prev) {
                    const Vec c = prev->at(i / 2, j / 2);
                    pred = {c.x * 2, c.y * 2};
                    cands[n++] = pred;
                    for (auto [di, dj] : {std::pair{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, 1}}) {
                        const Vec o = prev->at(i / 2 + di, j / 2 + dj);
                        cands[n++] = Vec{o.x * 2, o.y * 2};
                    }
                }
                // Cost: difference plus a small pull towards the predicted vector (smooth field in flat areas)
                const auto costOf = [&](int dx, int dy, int limit) {
                    const int s = sad(P, Q, x0, y0, dx, dy, limit);
                    const float away = std::abs(dx - pred.x) + std::abs(dy - pred.y);
                    return float(s) + away * pixels * 0.05f - (dx == 0 && dy == 0 ? pixels * 0.5f : 0.f);
                };
                int bx = 0, by = 0;
                float best = 1e30f;
                for (int k = 0; k < n; ++k) {
                    const int dx = int(std::lround(cands[k].x)), dy = int(std::lround(cands[k].y));
                    const float c = costOf(dx, dy, int(std::min(best, 1e9f)) + pixels);
                    if (c < best) best = c, bx = dx, by = dy;
                }
                const int cx = bx, cy = by;
                for (int dy = -radius; dy <= radius; ++dy)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (!dx && !dy) continue;
                        const float c = costOf(cx + dx, cy + dy, int(std::min(best, 1e9f)) + pixels);
                        if (c < best) best = c, bx = cx + dx, by = cy + dy;
                    }
                Vec v{float(bx), float(by)};
                const int s0 = sad(P, Q, x0, y0, bx, by, 1 << 30);
                if (subpixel) { // parabola through the neighbours' costs
                    const auto fit = [](double m, double c, double p) {
                        const double d = m - 2 * c + p;
                        return d > 0 ? std::clamp((m - p) / (2 * d), -0.5, 0.5) : 0.0;
                    };
                    v.x += float(fit(sad(P, Q, x0, y0, bx - 1, by, 1 << 30), s0, sad(P, Q, x0, y0, bx + 1, by, 1 << 30)));
                    v.y += float(fit(sad(P, Q, x0, y0, bx, by - 1, 1 << 30), s0, sad(P, Q, x0, y0, bx, by + 1, 1 << 30)));
                }
                f.v[size_t(j) * f.gw + i] = v;
                f.cost[size_t(j) * f.gw + i] = float(s0) / pixels;
            }
    });
    return f;
}

// A vector unlike all eight neighbours is noise: the component-wise median of the 3x3 replaces it. Objects of two
// blocks and more keep their own motion (a plain median would erase small fast things at the coarse levels).
void dropOutliers(Field& f)
{
    const std::vector<Vec> in = f.v;
    for (int j = 0; j < f.gh; ++j)
        for (int i = 0; i < f.gw; ++i) {
            const Vec me = in[size_t(j) * f.gw + i];
            std::array<float, 9> xs, ys;
            int n = 0;
            bool alone = true;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int ii = std::clamp(i + di, 0, f.gw - 1), jj = std::clamp(j + dj, 0, f.gh - 1);
                    const Vec o = in[size_t(jj) * f.gw + ii];
                    if ((di || dj) && (ii != i || jj != j) && std::abs(o.x - me.x) + std::abs(o.y - me.y) <= 1.5f)
                        alone = false;
                    xs[n] = o.x;
                    ys[n++] = o.y;
                }
            if (!alone) continue;
            std::nth_element(xs.begin(), xs.begin() + 4, xs.end());
            std::nth_element(ys.begin(), ys.begin() + 4, ys.end());
            f.v[size_t(j) * f.gw + i] = {xs[4], ys[4]};
        }
}

Field motionField(const std::vector<Plane>& P, const std::vector<Plane>& Q)
{
    Field f;
    Vec global;
    for (int l = int(P.size()) - 1; l >= 0; --l) {
        const bool top = l == int(P.size()) - 1;
        f = searchLevel(P[l], Q[l], top ? nullptr : &f, global, top ? 12 : 1, l == 0);
        dropOutliers(f);
        // Dominant motion = the component-wise median of the field, doubled for the finer level
        std::vector<float> xs, ys;
        for (const Vec& v : f.v) xs.push_back(v.x), ys.push_back(v.y);
        std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
        std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
        global = {std::round(xs[xs.size() / 2]) * 2, std::round(ys[ys.size() / 2]) * 2};
    }
    return f;
}

std::vector<Plane> pyramid(const Plane& base)
{
    std::vector<Plane> p{base};
    // Three levels: the top one (a quarter of the work size) searches +-8 px there = +-32 px at work size; coarser
    // levels would swallow small fast objects (a 64 px target is two blocks at the top already)
    while (p.size() < 3 && p.back().w >= 64 && p.back().h >= 40) {
        const Plane& s = p.back();
        p.push_back(shrink(s, s.w / 2, s.h / 2));
    }
    return p;
}

// How differently the colours of a and b are distributed: per channel the share of pixels that would have to change
// their 16-step bin, the largest of the three (0 = same, 1 = nothing in common). A CS2 cut between two maps: 0.33;
// fast camera turns of the same shot: up to 0.09 (a luma histogram alone saw the cut as 0.2)
double colourDistance(const uint8_t* a, const uint8_t* b, int w, int h)
{
    const int step = std::max(1, std::min(w, h) / 180); // a thinned-out picture is enough for histograms
    std::array<std::array<int, 16>, 3> ha{}, hb{};
    int n = 0;
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step, ++n) {
            const uint8_t* p = a + (size_t(y) * w + x) * 4;
            const uint8_t* q = b + (size_t(y) * w + x) * 4;
            for (int c = 0; c < 3; ++c) ++ha[c][p[c] >> 4], ++hb[c][q[c] >> 4];
        }
    double worst = 0;
    for (int c = 0; c < 3; ++c) {
        int d = 0;
        for (int i = 0; i < 16; ++i) d += std::abs(ha[c][i] - hb[c][i]);
        worst = std::max(worst, d / (2.0 * std::max(1, n)));
    }
    return worst;
}

constexpr double kCutDistance = 0.2;

} // namespace

struct Motion {
    int w = 0, h = 0;
    float scale = 1; // full size / work size
    Plane A, B;       // work size luma
    Plane fullA, fullB;
    Field fwd, bwd;   // A -> B, B -> A (work pixels)
    std::vector<uint8_t> okF, okB;
    bool cut = false;
    double bad = 0;
};

void blend(const uint8_t* a, const uint8_t* b, uint8_t* out, int w, int h, double t)
{
    const uint32_t k = uint32_t(std::clamp(std::lround(t * 256), 0L, 256L));
    parallelRows(h, int64_t(w) * h, [&](int y0, int y1) {
        const size_t from = size_t(y0) * w * 4, to = size_t(y1) * w * 4;
        for (size_t i = from; i < to; ++i) out[i] = uint8_t((a[i] * (256 - k) + b[i] * k + 128) >> 8);
    });
}

bool isCut(const uint8_t* a, const uint8_t* b, int w, int h) { return colourDistance(a, b, w, h) > kCutDistance; }

std::shared_ptr<const Motion> estimate(const uint8_t* a, const uint8_t* b, int w, int h)
{
    auto m = std::make_shared<Motion>();
    m->w = w;
    m->h = h;
    m->fullA = luma(a, w, h);
    m->fullB = luma(b, w, h);
    const int ww = std::min(w, kWorkWidth), wh = std::max(1, int(std::lround(double(h) * ww / w)));
    m->scale = float(w) / ww;
    m->A = shrink(m->fullA, ww, wh);
    m->B = shrink(m->fullB, ww, wh);
    m->cut = isCut(a, b, w, h);
    if (m->cut) return m;
    const std::vector<Plane> pa = pyramid(m->A), pb = pyramid(m->B);
    m->fwd = motionField(pa, pb);
    m->bwd = motionField(pb, pa);
    // Forward/backward consistency: following v from a block and coming back must land near the start
    const auto check = [](const Field& f, const Field& g, std::vector<uint8_t>& ok) {
        ok.assign(f.v.size(), 0);
        int good = 0;
        for (int j = 0; j < f.gh; ++j)
            for (int i = 0; i < f.gw; ++i) {
                const Vec v = f.v[size_t(j) * f.gw + i];
                const float cx = i * kBlock + kBlock / 2.f + v.x, cy = j * kBlock + kBlock / 2.f + v.y;
                const Vec back = g.at(int(std::floor(cx / kBlock)), int(std::floor(cy / kBlock)));
                const float err = std::hypot(v.x + back.x, v.y + back.y);
                const bool fine = err <= 1.0f + 0.25f * std::hypot(v.x, v.y);
                ok[size_t(j) * f.gw + i] = fine;
                good += fine;
            }
        return 1.0 - double(good) / std::max<size_t>(1, f.v.size());
    };
    m->bad = check(m->fwd, m->bwd, m->okF);
    check(m->bwd, m->fwd, m->okB);
    // A cut the colours did not give away: the motion explains neither the picture nor itself
    double cost = 0;
    for (float c : m->fwd.cost) cost += c;
    cost /= std::max<size_t>(1, m->fwd.cost.size());
    if (m->bad > 0.6 && cost > 25) m->cut = true;
    return m;
}

bool cut(const Motion& m) { return m.cut; }
double unreliable(const Motion& m) { return m.bad; }

void flow(const Motion& m, const uint8_t* a, const uint8_t* b, uint8_t* out, int w, int h, double tt)
{
    if (m.cut || w != m.w || h != m.h) { // no in-between across a cut: the nearer frame
        std::copy_n(tt < 0.5 ? a : b, size_t(w) * h * 4, out);
        return;
    }
    if (m.bad > 0.5) { // the motion is mostly guesswork: plain blending looks calmer than wrong warps
        blend(a, b, out, w, h, tt);
        return;
    }
    const float t = float(tt);
    const Field& F = m.fwd;
    const int gw = F.gw, gh = F.gh;
    // 1. Candidates per in-between block: every block of a and b lands at p + t * v (resp. p + (1 - t) * w from b)
    constexpr int kMax = 8;
    struct Cands {
        std::array<Vec, kMax> v;
        std::array<uint8_t, kMax> ok;
        int n = 0;
        void add(Vec x, bool good)
        {
            for (int k = 0; k < n; ++k)
                if (std::abs(v[k].x - x.x) < 0.25f && std::abs(v[k].y - x.y) < 0.25f) {
                    ok[k] |= good;
                    return;
                }
            if (n < kMax) v[n] = x, ok[n++] = good;
        }
    };
    std::vector<Cands> splat(size_t(gw) * gh);
    const auto land = [&](float cx, float cy, Vec v, bool good) {
        // The block's footprint touches up to four blocks of the in-between grid
        const float x0 = cx - kBlock / 2.f, y0 = cy - kBlock / 2.f;
        const int i0 = int(std::floor(x0 / kBlock)), j0 = int(std::floor(y0 / kBlock));
        for (int j = j0; j <= j0 + 1; ++j)
            for (int i = i0; i <= i0 + 1; ++i)
                if (i >= 0 && j >= 0 && i < gw && j < gh) splat[size_t(j) * gw + i].add(v, good);
    };
    for (int j = 0; j < gh; ++j)
        for (int i = 0; i < gw; ++i) {
            const float cx = i * kBlock + kBlock / 2.f, cy = j * kBlock + kBlock / 2.f;
            const Vec v = F.v[size_t(j) * gw + i];
            land(cx + t * v.x, cy + t * v.y, v, m.okF[size_t(j) * gw + i]);
            const Vec u = m.bwd.v[size_t(j) * gw + i];
            land(cx + (1 - t) * u.x, cy + (1 - t) * u.y, Vec{-u.x, -u.y}, m.okB[size_t(j) * gw + i]);
        }
    // 2. Each in-between block takes the candidate whose ends in a and b agree best
    std::vector<Vec> mid(size_t(gw) * gh);
    parallelRows(gh, int64_t(m.A.w) * m.A.h * 4, [&](int j0, int j1) {
        for (int j = j0; j < j1; ++j)
            for (int i = 0; i < gw; ++i) {
                Cands c = splat[size_t(j) * gw + i];
                const int splatted = c.n;
                const Vec own = F.v[size_t(j) * gw + i], back = m.bwd.v[size_t(j) * gw + i];
                c.add(own, false);
                c.add(Vec{-back.x, -back.y}, false);
                c.add(Vec{}, false);
                const int x0 = i * kBlock, y0 = j * kBlock;
                const int bw = std::min(kBlock, m.A.w - x0), bh = std::min(kBlock, m.A.h - y0);
                float best = 1e30f;
                Vec pick;
                for (int k = 0; k < c.n; ++k) {
                    const Vec v = c.v[k];
                    float cost = 0;
                    for (int y = 0; y < bh; ++y)
                        for (int x = 0; x < bw; ++x) {
                            const float px = x0 + x + 0.f, py = y0 + y + 0.f;
                            cost += std::abs(m.A.sample(px - t * v.x, py - t * v.y) -
                                             m.B.sample(px + (1 - t) * v.x, py + (1 - t) * v.y));
                        }
                    cost /= bw * bh;
                    // Projected vectors know who is where; the others only fill holes
                    cost += k < splatted ? (c.ok[k] ? 0.f : 1.f) : 2.f;
                    if (cost < best) best = cost, pick = v;
                }
                mid[size_t(j) * gw + i] = pick;
            }
    });
    // 3. Per output pixel: the vector field around it or standing still, by the 3x3 match at full size
    const float s = m.scale;
    const Plane &LA = m.fullA, &LB = m.fullB;
    const auto costAt = [&](int x, int y, float vx, float vy) {
        const auto round = [](float f) { return int(std::floor(f + 0.5f)); };
        const int ax = round(x - t * vx), ay = round(y - t * vy);
        const int bx = round(x + (1 - t) * vx), by = round(y + (1 - t) * vy);
        int sum = 0;
        if (std::min({ax, ay, bx, by}) >= 1 && std::max(ax, bx) < w - 1 && std::max(ay, by) < h - 1) {
            for (int dy = -1; dy <= 1; ++dy) {
                const uint8_t* p = &LA.p[size_t(ay + dy) * w + ax - 1];
                const uint8_t* q = &LB.p[size_t(by + dy) * w + bx - 1];
                sum += std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]);
            }
            return sum;
        }
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) sum += std::abs(int(LA.at(ax + dx, ay + dy)) - int(LB.at(bx + dx, by + dy)));
        return sum;
    };
    const auto rgba = [](const uint8_t* img, int w, int h, float x, float y, float* o) {
        x = std::clamp(x, 0.f, float(w - 1));
        y = std::clamp(y, 0.f, float(h - 1));
        const int x0 = int(x), y0 = int(y);
        const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
        const float fx = x - x0, fy = y - y0;
        const uint8_t* p00 = img + (size_t(y0) * w + x0) * 4;
        const uint8_t* p10 = img + (size_t(y0) * w + x1) * 4;
        const uint8_t* p01 = img + (size_t(y1) * w + x0) * 4;
        const uint8_t* p11 = img + (size_t(y1) * w + x1) * 4;
        for (int c = 0; c < 4; ++c) {
            const float top = p00[c] + (p10[c] - p00[c]) * fx, bottom = p01[c] + (p11[c] - p01[c]) * fx;
            o[c] = top + (bottom - top) * fy;
        }
    };
    constexpr float kSoft = 9 * 5;     // cost difference that weighs a candidate down to 1/e
    constexpr int kStatic = 9 * 4;     // 3x3 difference of unchanged detail (compression noise)
    constexpr int kDetail = 48;        // contrast in the 3x3 that counts as detail
    parallelRows(h, int64_t(w) * h * 8, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            uint8_t* o = out + size_t(y) * w * 4;
            const float gy = ((y + 0.5f) / s - 0.5f - (kBlock - 1) / 2.f) / kBlock; // block centres
            const int j0 = int(std::floor(gy));
            const float fy = gy - j0;
            for (int x = 0; x < w; ++x, o += 4) {
                const float gx = ((x + 0.5f) / s - 0.5f - (kBlock - 1) / 2.f) / kBlock;
                const int i0 = int(std::floor(gx));
                const float fx = gx - i0;
                const auto at = [&](int i, int j) {
                    return mid[size_t(std::clamp(j, 0, gh - 1)) * gw + std::clamp(i, 0, gw - 1)];
                };
                const Vec q[4] = {at(i0, j0), at(i0 + 1, j0), at(i0, j0 + 1), at(i0 + 1, j0 + 1)};
                Vec base{(q[0].x * (1 - fx) + q[1].x * fx) * (1 - fy) + (q[2].x * (1 - fx) + q[3].x * fx) * fy,
                         (q[0].y * (1 - fx) + q[1].y * fx) * (1 - fy) + (q[2].y * (1 - fx) + q[3].y * fx) * fy};
                // Candidates: the field's vector here, near a motion edge each of the four around (their blend fits
                // neither side), standing still
                struct Cand {
                    float vx, vy;
                    int cost;
                };
                std::array<Cand, 6> cand;
                int n = 0;
                const auto add = [&](float vx, float vy) {
                    for (int k = 0; k < n; ++k)
                        if (std::abs(cand[k].vx - vx) + std::abs(cand[k].vy - vy) < 0.25f) return;
                    cand[n++] = {vx, vy, costAt(x, y, vx, vy)};
                };
                add(base.x * s, base.y * s);
                bool edge = false;
                for (const Vec& v : q) edge |= std::abs(v.x - base.x) + std::abs(v.y - base.y) > 0.5f;
                if (edge)
                    for (const Vec& v : q) add(v.x * s, v.y * s);
                int best = cand[0].cost;
                for (int k = 1; k < n; ++k) best = std::min(best, cand[k].cost);
                const int still = costAt(x, y, 0, 0);
                // Standing still for sure: a sub-pixel motion that matches no better, or detail that is exactly the
                // same in both frames (HUD text, a crosshair) even where the picture behind it moves - the motion
                // would match there too (it samples the background beside it)
                bool fixed = (n == 1 && std::abs(cand[0].vx) + std::abs(cand[0].vy) < 0.75f && still <= best) ||
                             (still <= kStatic && best - still >= 4 * kSoft);
                if (!fixed && still <= kStatic) {
                    int lo = 255, hi = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int v = LA.at(x + dx, y + dy);
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                    fixed = hi - lo >= kDetail;
                }
                const size_t here = (size_t(y) * w + x) * 4;
                if (fixed) { // standing still: exactly the blend (an unchanged HUD stays identical)
                    for (int c = 0; c < 4; ++c) o[c] = uint8_t(a[here + c] + (b[here + c] - a[here + c]) * t + 0.5f);
                    continue;
                }
                if (n < int(cand.size())) cand[n++] = {0, 0, still};
                best = std::min(best, still);
                // Soft choice: every candidate by how well it matches (hard switching makes patches with edges)
                float sum[4] = {0, 0, 0, 0}, weights = 0;
                for (int k = 0; k < n; ++k) {
                    const float d = float(cand[k].cost - best) / kSoft;
                    if (d > 4) continue;
                    const float wk = std::exp(-d);
                    const float vx = cand[k].vx, vy = cand[k].vy;
                    float pa[4], pb[4];
                    if (vx == 0 && vy == 0) {
                        for (int c = 0; c < 4; ++c) pa[c] = a[here + c], pb[c] = b[here + c];
                    } else {
                        rgba(a, w, h, x - t * vx, y - t * vy, pa);
                        rgba(b, w, h, x + (1 - t) * vx, y + (1 - t) * vy, pb);
                    }
                    // A spot one side does not have (it comes in over the frame edge): only the other side
                    const float ax = x - t * vx, ay = y - t * vy, bx = x + (1 - t) * vx, by = y + (1 - t) * vy;
                    const bool aOut = ax < 0 || ay < 0 || ax > w - 1 || ay > h - 1;
                    const bool bOut = bx < 0 || by < 0 || bx > w - 1 || by > h - 1;
                    const float wb = aOut && !bOut ? 1.f : bOut && !aOut ? 0.f : t;
                    for (int c = 0; c < 4; ++c) sum[c] += wk * (pa[c] + (pb[c] - pa[c]) * wb);
                    weights += wk;
                }
                for (int c = 0; c < 4; ++c) o[c] = uint8_t(std::clamp(sum[c] / weights + 0.5f, 0.f, 255.f));
            }
        }
    });
}

std::shared_ptr<const Motion> cachedMotion(const QString& key, const uint8_t* a, const uint8_t* b, int w, int h)
{
    // At 25 % one pair serves three in-between frames; a few pairs cover playback going back and forth
    static std::mutex lock;
    static std::list<std::pair<QString, std::shared_ptr<const Motion>>> cache;
    {
        const std::lock_guard<std::mutex> g(lock);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) {
                cache.splice(cache.begin(), cache, it);
                return it->second;
            }
    }
    auto m = estimate(a, b, w, h);
    const std::lock_guard<std::mutex> g(lock);
    cache.emplace_front(key, m);
    while (cache.size() > 6) cache.pop_back();
    return m;
}

} // namespace Picture::Interp
