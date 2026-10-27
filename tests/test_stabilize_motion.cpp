// Own stabilizer core (engine/StabilizeMotion, no MLT): frames rendered from a textured scene through a known camera
// (random shift, rotation, scale) -> the estimated motion matches it, also with an object moving on its own and a
// still overlay (a game HUD); the smoothed path takes the shake out (residual motion measured against the known
// camera); warps: identity/shift exact, borders as chosen; time per 1080p frame printed.
#include "check.h"

#include "engine/StabilizeMotion.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace StabilizeMotion;

namespace {

constexpr int W = 960, H = 540;
constexpr int SW = 1600, SH = 1100; // scene

struct Rng {
    uint32_t s = 12345;
    double next() // 0..1
    {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / double(1 << 24);
    }
    double between(double a, double b) { return a + (b - a) * next(); }
};

// Textured scene: blurred noise plus rectangles of random gray (corners everywhere)
std::vector<float> makeScene(int w, int h, uint32_t seed)
{
    Rng rng{seed};
    std::vector<float> s(size_t(w) * h);
    for (float& v : s) v = float(rng.between(0, 255));
    for (int pass = 0; pass < 3; ++pass) { // box blur 5x5
        std::vector<float> t = s;
        for (int y = 2; y < h - 2; ++y)
            for (int x = 2; x < w - 2; ++x) {
                float sum = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) sum += t[size_t(y + j) * w + x + i];
                s[size_t(y) * w + x] = sum / 25;
            }
    }
    for (int r = 0; r < 300; ++r) {
        const int x0 = int(rng.between(0, w - 60)), y0 = int(rng.between(0, h - 60));
        const int rw = int(rng.between(8, 60)), rh = int(rng.between(8, 60));
        const float g = float(rng.between(0, 255));
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x) s[size_t(y) * w + x] = 0.5f * s[size_t(y) * w + x] + 0.5f * g;
    }
    return s;
}

double sampleScene(const std::vector<float>& s, int w, int h, double x, double y)
{
    x = std::clamp(x - 0.5, 0.0, double(w - 1.001));
    y = std::clamp(y - 0.5, 0.0, double(h - 1.001));
    const int x0 = int(x), y0 = int(y);
    const double fx = x - x0, fy = y - y0;
    const float* r = &s[size_t(y0) * w + x0];
    return (r[0] * (1 - fx) + r[1] * fx) * (1 - fy) + (r[w] * (1 - fx) + r[w + 1] * fx) * fy;
}

// Camera k: frame point f (normalized) shows scene point g * f + u (normalized, scene center = 0)
struct Camera {
    Complex g = 1, u = 0;
};

// Frame through camera c; extra: optional overlay drawn in frame pixels (value < 0 = none)
std::vector<uint8_t> render(const std::vector<float>& scene, const Camera& c,
                            const std::function<double(int, int)>& overlay = {})
{
    std::vector<uint8_t> f(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double v = overlay ? overlay(x, y) : -1;
            if (v < 0) {
                const Complex p((x + 0.5 - W / 2.0) / W, (y + 0.5 - H / 2.0) / W);
                const Complex q = c.g * p + c.u;
                v = sampleScene(scene, SW, SH, q.real() * W + SW / 2.0, q.imag() * W + SH / 2.0);
            }
            f[size_t(y) * W + x] = uint8_t(std::clamp(std::lround(v), 0L, 255L));
        }
    return f;
}

// Expected motion frame a -> frame b: f_b = (g_a f_a + u_a - u_b) / g_b
Similarity expected(const Camera& a, const Camera& b) { return {a.g / b.g, (a.u - b.u) / b.g}; }

// Error of an estimated motion in pixels (960 wide): largest distance at the frame corners
double errorPx(const Motion& m, const Similarity& e)
{
    const Similarity s{std::polar(m.scale, m.angle), Complex(m.dx, m.dy)};
    double worst = 0;
    for (const Complex c : {Complex(-0.5, -0.28), Complex(0.5, -0.28), Complex(0.5, 0.28), Complex(-0.5, 0.28)})
        worst = std::max(worst, std::abs(s.map(c) - e.map(c)) * W);
    return worst;
}

std::vector<Camera> shakyCameras(int n, double shift, double angleDeg, double zoom, Complex pan, uint32_t seed)
{
    Rng rng{seed};
    std::vector<Camera> cams;
    for (int k = 0; k < n; ++k) {
        Camera c;
        const double a = rng.between(-angleDeg, angleDeg) * std::numbers::pi / 180;
        c.g = std::polar(1.0 + rng.between(-zoom, zoom), a);
        c.u = Complex(rng.between(-shift, shift), rng.between(-shift, shift)) / double(W) + pan * double(k) / double(W);
        cams.push_back(c);
    }
    return cams;
}

struct Stats {
    double mean = 0, max = 0;
    int invalid = 0;
};

Stats run(const std::vector<float>& scene, const std::vector<Camera>& cams, std::vector<Motion>* motions = nullptr,
          const std::function<double(int, int, int)>& overlay = {}, int skip = 0)
{
    Estimator est(W, H, 5, 9);
    Stats st;
    int n = 0;
    for (int k = 0; k < int(cams.size()); ++k) {
        const auto f = render(scene, cams[size_t(k)], overlay ? std::function<double(int, int)>([&](int x, int y) {
            return overlay(k, x, y);
        }) : std::function<double(int, int)>());
        const Motion m = est.push(f.data(), W);
        if (motions) motions->push_back(m);
        if (k == 0 || k < skip) continue;
        if (!m.valid) {
            ++st.invalid;
            continue;
        }
        const double e = errorPx(m, expected(cams[size_t(k) - 1], cams[size_t(k)]));
        st.mean += e;
        st.max = std::max(st.max, e);
        ++n;
    }
    if (n) st.mean /= n;
    return st;
}

// Residual camera motion after stabilizing: output pixel q of frame k shows scene point g_k * M_k(q) + u_k; frame
// to frame change of that at the corners (pixels, 960 wide), RMS over the clip
double residual(const std::vector<Camera>& cams, const std::vector<Similarity>& corr)
{
    double sum = 0;
    int n = 0;
    auto at = [&](int k, Complex q) {
        const Complex p = corr.empty() ? q : corr[size_t(k)].map(q);
        return cams[size_t(k)].g * p + cams[size_t(k)].u;
    };
    for (int k = 1; k < int(cams.size()); ++k)
        for (const Complex c : {Complex(-0.4, -0.2), Complex(0.4, -0.2), Complex(0.4, 0.2), Complex(-0.4, 0.2)}) {
            sum += std::norm((at(k, c) - at(k - 1, c)) * double(W));
            ++n;
        }
    return std::sqrt(sum / n);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<float> scene = makeScene(SW, SH, 7);

    // --- Known shake (±12 px, ±1.5°, ±1 % zoom) recovered
    {
        const auto cams = shakyCameras(40, 12, 1.5, 0.01, 0, 1);
        const Stats s = run(scene, cams);
        std::printf("shake: error mean %.3f px, max %.3f px (960 wide), invalid %d\n", s.mean, s.max, s.invalid);
        CHECK(s.invalid == 0);
        CHECK(s.mean < 0.15);
        CHECK(s.max < 0.6);
    }
    // --- Large shift (±40 px per frame) still found (coarse search)
    {
        const auto cams = shakyCameras(12, 40, 0.5, 0, 0, 2);
        const Stats s = run(scene, cams);
        std::printf("large shake: error mean %.3f px, max %.3f px, invalid %d\n", s.mean, s.max, s.invalid);
        CHECK(s.invalid == 0 && s.mean < 0.3);
    }
    // --- An object moving on its own (a quarter of the picture) does not count
    {
        const auto cams = shakyCameras(30, 8, 1.0, 0, 0, 3);
        const std::vector<float> object = makeScene(260, 260, 99);
        const Stats s = run(scene, cams, nullptr, [&](int k, int x, int y) {
            const int ox = 150 + 9 * k, oy = 120 + 3 * k;
            if (x < ox || y < oy || x >= ox + 260 || y >= oy + 260) return -1.0;
            return double(object[size_t(y - oy) * 260 + (x - ox)]);
        });
        std::printf("moving object: error mean %.3f px, max %.3f px, invalid %d\n", s.mean, s.max, s.invalid);
        CHECK(s.invalid == 0 && s.mean < 0.2 && s.max < 1.0);
    }
    // --- A still overlay (HUD bar at the bottom, box at the top left) while the camera pans with shake
    {
        const auto cams = shakyCameras(60, 6, 0.5, 0, Complex(4, 1), 4);
        const std::vector<float> hud = makeScene(W, 120, 5);
        const Stats s = run(
            scene, cams, nullptr,
            [&](int, int x, int y) {
                if (y >= H - 110) return double(hud[size_t(y - (H - 110)) * W + x]);
                if (x < 220 && y < 220) return double(hud[size_t(y % 120) * W + x]);
                return -1.0;
            },
            8);
        std::printf("still overlay: error mean %.3f px, max %.3f px, invalid %d\n", s.mean, s.max, s.invalid);
        CHECK(s.invalid == 0 && s.mean < 0.2 && s.max < 1.0);
    }
    // --- Flat picture: not measurable (no motion assumed)
    {
        Estimator est(W, H, 5, 9);
        std::vector<uint8_t> flat(size_t(W) * H, 90);
        est.push(flat.data(), W);
        CHECK(!est.push(flat.data(), W).valid);
    }

    // --- Smoothed path: shake taken out, a pan kept
    {
        const auto cams = shakyCameras(100, 10, 1.0, 0.005, Complex(3, 0), 6);
        std::vector<Motion> motions;
        run(scene, cams, &motions);
        PathParams p;
        p.aspect = double(W) / H;
        const auto corr = corrections(motions, p);
        CHECK_EQ(int(corr.size()), 100);
        const double before = residual(cams, {}), after = residual(cams, corr);
        std::printf("residual motion: %.2f px -> %.3f px per frame (pan 3 px/frame kept)\n", before, after);
        CHECK(after < 3.6 && after > 2.4); // the pan (3 px/frame) stays, the shake goes
        // Without the pan: nearly still
        const auto still = shakyCameras(100, 10, 1.0, 0.005, 0, 6);
        motions.clear();
        run(scene, still, &motions);
        const auto corr2 = corrections(motions, p);
        const double b2 = residual(still, {}), a2 = residual(still, corr2);
        std::printf("residual motion (no pan): %.2f px -> %.3f px per frame\n", b2, a2);
        CHECK(a2 < b2 * 0.12);
        // Auto zoom: every output corner inside the input picture
        bool inside = true;
        for (const Similarity& s : corr2)
            for (const Complex c : {Complex(-0.5, -0.28125), Complex(0.5, 0.28125), Complex(0.5, -0.28125), Complex(-0.5, 0.28125)}) {
                const Complex q = s.map(c);
                inside = inside && std::abs(q.real()) <= 0.5 + 1e-9 && std::abs(q.imag()) <= 0.28125 + 1e-9;
            }
        CHECK(inside);
        CHECK(std::abs(corr2[0].a) < 0.99); // zoomed in
        // Black border: no zoom; still camera (smoothing 0): constant scene position
        p.border = Border::Black;
        CHECK(std::abs(std::abs(corrections(motions, p)[50].a) - std::abs(Complex(1) * motions[50].scale)) < 0.02);
        p.smoothing = 0;
        const double tripod = residual(still, corrections(motions, p));
        std::printf("still camera: %.3f px per frame\n", tripod);
        CHECK(tripod < b2 * 0.1);
    }

    // --- A fast turn (30 px per frame for 10 frames) amid shake: followed instead of zooming the whole clip
    {
        auto cams = shakyCameras(80, 8, 0.5, 0, 0, 9);
        for (int k = 0; k < 80; ++k) cams[size_t(k)].u += Complex(std::clamp(k - 35, 0, 10) * 30.0 / W, 0);
        std::vector<Motion> motions;
        run(scene, cams, &motions);
        PathParams p;
        p.aspect = double(W) / H;
        const auto corr = corrections(motions, p);
        const double zoom = 1 / std::abs(corr[0].a);
        // frames well before it (the smooth path starts to turn one smoothing radius early)
        const std::vector<Camera> calm(cams.begin(), cams.begin() + 18);
        const std::vector<Similarity> calmCorr(corr.begin(), corr.begin() + 18);
        const double before = residual(calm, {}), after = residual(calm, calmCorr);
        std::printf("fast turn: zoom %.3f, shake before it %.2f -> %.3f px\n", zoom, before, after);
        CHECK(zoom < 1.21);
        CHECK(after < before * 0.1);
    }

    // --- Warps
    {
        std::vector<uint8_t> src(size_t(64) * 48), dst(src.size(), 0);
        for (size_t i = 0; i < src.size(); ++i) src[i] = uint8_t((i * 37) % 251);
        double m[6];
        pixelMap(Similarity{}, 64, 48, 64.0 / 48, m);
        CHECK(std::abs(m[0] - 1) < 1e-12 && std::abs(m[2]) < 1e-9 && std::abs(m[4] - 1) < 1e-12 && std::abs(m[5]) < 1e-9);
        warpPlane(src.data(), 64, dst.data(), 64, 64, 48, m, Outside::Fill, 7);
        CHECK(dst == src);
        // shift by 3 px right (output x takes input x - 3): left 3 columns filled
        pixelMap(Similarity{1, Complex(-3.0 / 64, 0)}, 64, 48, 64.0 / 48, m);
        warpPlane(src.data(), 64, dst.data(), 64, 64, 48, m, Outside::Fill, 7);
        bool ok = true;
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                ok = ok && dst[size_t(y) * 64 + x] == (x < 3 ? 7 : src[size_t(y) * 64 + x - 3]);
        CHECK(ok);
        // Keep: outside left untouched; Clamp: edge pixels
        std::fill(dst.begin(), dst.end(), 200);
        warpPlane(src.data(), 64, dst.data(), 64, 64, 48, m, Outside::Keep);
        CHECK(dst[0] == 200 && dst[3] == src[0]);
        warpPlane(src.data(), 64, dst.data(), 64, 64, 48, m, Outside::Clamp);
        CHECK(dst[0] == src[0] && dst[64 * 10 + 1] == src[64 * 10]);
        // RGBA: same as the plane warp per channel (within rounding)
        std::vector<uint8_t> rgba(size_t(64) * 48 * 4), out(rgba.size());
        for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t((i * 13) % 253);
        pixelMap(Similarity{std::polar(0.9, 0.1), Complex(0.01, -0.02)}, 64, 48, 64.0 / 48, m);
        warpRgba(rgba.data(), 64 * 4, out.data(), 64 * 4, 64, 48, m, Outside::Clamp);
        std::vector<uint8_t> ch(size_t(64) * 48), chOut(ch.size());
        int worst = 0;
        for (int c = 0; c < 4; ++c) {
            for (size_t i = 0; i < ch.size(); ++i) ch[i] = rgba[i * 4 + size_t(c)];
            warpPlane(ch.data(), 64, chOut.data(), 64, 64, 48, m, Outside::Clamp);
            for (size_t i = 0; i < ch.size(); ++i) worst = std::max(worst, std::abs(int(chOut[i]) - out[i * 4 + size_t(c)]));
        }
        if (!CHECK(worst <= 2)) std::printf("       rgba vs plane: %d\n", worst);

        // Speed per 1080p frame (it runs during playback)
        const int fw = 1920, fh = 1080;
        std::vector<uint8_t> big(size_t(fw) * fh * 4, 0), bigOut(big.size());
        for (size_t i = 0; i < big.size(); ++i) big[i] = uint8_t(i * 7);
        pixelMap(Similarity{std::polar(0.95, 0.02), Complex(0.003, 0.002)}, fw, fh, 16.0 / 9, m);
        QElapsedTimer t;
        const int reps = 10;
        t.start();
        for (int r = 0; r < reps; ++r) warpRgba(big.data(), fw * 4, bigOut.data(), fw * 4, fw, fh, m, Outside::Clamp);
        const double rgbaMs = t.nsecsElapsed() / 1e6 / reps;
        t.restart();
        for (int r = 0; r < reps; ++r) {
            warpPlane(big.data(), fw, bigOut.data(), fw, fw, fh, m, Outside::Clamp);
            double mc[6] = {m[0], m[1], m[2] / 2, m[3], m[4], m[5] / 2};
            warpPlane(big.data(), fw / 2, bigOut.data(), fw / 2, fw / 2, fh / 2, mc, Outside::Clamp);
            warpPlane(big.data(), fw / 2, bigOut.data(), fw / 2, fw / 2, fh / 2, mc, Outside::Clamp);
        }
        const double yuvMs = t.nsecsElapsed() / 1e6 / reps;
        std::printf("warp 1920x1080: rgba %.2f ms, yuv420p %.2f ms\n", rgbaMs, yuvMs);

        // Analysis speed per frame (960x540)
        Estimator est(W, H, 5, 9);
        const auto cams = shakyCameras(11, 10, 1.0, 0.005, 0, 8);
        std::vector<std::vector<uint8_t>> frames;
        for (const Camera& c : cams) frames.push_back(render(scene, c));
        t.restart();
        for (const auto& f : frames) est.push(f.data(), W);
        std::printf("analysis 960x540: %.2f ms per frame\n", t.nsecsElapsed() / 1e6 / double(frames.size()));
    }
    return Check::result();
}
