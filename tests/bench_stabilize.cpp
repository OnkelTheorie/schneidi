// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark of the stabilizer (not run by CTest): synthetic shaky clips (a textured scene filmed through a camera
// with known random shift/rotation/zoom, 1920x1080, encoded lossless) are stabilized with the own analysis + filter
// and, when MLT's vid.stab module is installed, with vid.stab for comparison. Reported: residual motion of the output
// (frame to frame, pixels at 1920 wide, measured with the own estimator on the output frames - the same ruler for
// both), mean frame difference, analysis time per frame. Further arguments: real clips (speed, valid frames, mean
// motion; frames before/after as PNG into $BENCH_OUT when set).
//   bench_stabilize [clip.mkv ...]
#include "check.h"

#include "core/EffectRegistry.h"
#include "engine/Profiles.h"
#include "engine/StabilizeMotion.h"
#include "engine/Stabilizer.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <numbers>
#include <vector>

using namespace StabilizeMotion;

namespace {

constexpr int FW = 1920, FH = 1080, SW = 2600, SH = 1700;

struct Rng {
    uint32_t s;
    double next()
    {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / double(1 << 24);
    }
    double between(double a, double b) { return a + (b - a) * next(); }
};

std::vector<float> makeScene(uint32_t seed)
{
    Rng rng{seed};
    std::vector<float> s(size_t(SW) * SH);
    for (float& v : s) v = float(rng.between(0, 255));
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<float> t = s;
        for (int y = 2; y < SH - 2; ++y)
            for (int x = 2; x < SW - 2; ++x) {
                float sum = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) sum += t[size_t(y + j) * SW + x + i];
                s[size_t(y) * SW + x] = sum / 25;
            }
    }
    for (int r = 0; r < 900; ++r) {
        const int x0 = int(rng.between(0, SW - 120)), y0 = int(rng.between(0, SH - 120));
        const int rw = int(rng.between(10, 120)), rh = int(rng.between(10, 120));
        const float g = float(rng.between(0, 255));
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x) s[size_t(y) * SW + x] = 0.4f * s[size_t(y) * SW + x] + 0.6f * g;
    }
    return s;
}

// Lossless clip of n frames: frame k shows scene point g_k * f + u_k for frame point f (normalized)
QString makeClip(const QString& path, int n, double shift, double angleDeg, double zoom, uint32_t seed)
{
    const std::vector<float> scene = makeScene(seed);
    QProcess ff;
    ff.start("ffmpeg", {"-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray", "-s", "1920x1080", "-r", "25", "-i", "-",
                        "-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "yuv420p", path});
    if (!ff.waitForStarted()) return {};
    Rng rng{seed * 7 + 1};
    std::vector<uint8_t> f(size_t(FW) * FH);
    for (int k = 0; k < n; ++k) {
        const Complex g = std::polar(1.0 + rng.between(-zoom, zoom), rng.between(-angleDeg, angleDeg) * std::numbers::pi / 180);
        const Complex u = Complex(rng.between(-shift, shift), rng.between(-shift, shift)) / double(FW);
        for (int y = 0; y < FH; ++y)
            for (int x = 0; x < FW; ++x) {
                const Complex q = g * Complex((x + 0.5 - FW / 2.0) / FW, (y + 0.5 - FH / 2.0) / FW) + u;
                double sx = std::clamp(q.real() * FW + SW / 2.0 - 0.5, 0.0, SW - 1.001);
                double sy = std::clamp(q.imag() * FW + SH / 2.0 - 0.5, 0.0, SH - 1.001);
                const int x0 = int(sx), y0 = int(sy);
                const double fx = sx - x0, fy = sy - y0;
                const float* r = &scene[size_t(y0) * SW + x0];
                const double v = (r[0] * (1 - fx) + r[1] * fx) * (1 - fy) + (r[SW] * (1 - fx) + r[SW + 1] * fx) * fy;
                f[size_t(y) * FW + x] = uint8_t(std::lround(v));
            }
        ff.write(reinterpret_cast<const char*>(f.data()), qint64(f.size()));
        ff.waitForBytesWritten(-1);
    }
    ff.closeWriteChannel();
    ff.waitForFinished(-1);
    return ff.exitCode() == 0 ? path : QString();
}

struct Measure {
    double residual = 0; // RMS frame-to-frame motion at the corners, px at 1920 wide
    double diff = 0;     // mean absolute luma difference of consecutive frames (middle half)
};

// Luma frames (w x h) -> measure; skip = frames at the start left out (smoothing edge)
Measure measure(const std::vector<std::vector<uint8_t>>& frames, int w, int h, int skip)
{
    Estimator est(w, h, 5, 12);
    Measure m;
    int n = 0;
    for (size_t k = 0; k < frames.size(); ++k) {
        const Motion mo = est.push(frames[k].data(), w);
        if (int(k) <= skip || int(k) >= int(frames.size()) - skip) continue;
        const Similarity s{std::polar(mo.scale, mo.angle), Complex(mo.dx, mo.dy)};
        double e = 0;
        for (const Complex c : {Complex(-0.4, -0.2), Complex(0.4, -0.2), Complex(0.4, 0.2), Complex(-0.4, 0.2)})
            e += std::norm((s.map(c) - c) * double(FW));
        m.residual += e / 4;
        double d = 0;
        int cnt = 0;
        for (int y = h / 4; y < 3 * h / 4; ++y)
            for (int x = w / 4; x < 3 * w / 4; ++x, ++cnt) d += std::abs(int(frames[k][size_t(y) * w + x]) - frames[k - 1][size_t(y) * w + x]);
        m.diff += d / cnt;
        ++n;
    }
    m.residual = std::sqrt(m.residual / std::max(1, n));
    m.diff /= std::max(1, n);
    return m;
}

// Luma plane at w x h (a bare producer may answer in its own size: 2 x 2 average or nearest pixel)
std::vector<uint8_t> luma(Mlt::Frame& f, int w, int h)
{
    mlt_image_format fmt = mlt_image_yuv420p;
    int ww = w, hh = h;
    const uint8_t* d = f.get_image(fmt, ww, hh);
    std::vector<uint8_t> out(size_t(w) * h, 0);
    if (!d || ww < 1 || hh < 1) return out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (ww == w && hh == h) out[size_t(y) * w + x] = d[size_t(y) * ww + x];
            else if (ww == 2 * w && hh == 2 * h) {
                const uint8_t* r = d + size_t(2 * y) * ww + 2 * x;
                out[size_t(y) * w + x] = uint8_t((r[0] + r[1] + r[ww] + r[ww + 1] + 2) / 4);
            } else out[size_t(y) * w + x] = d[size_t(y * hh / h) * ww + x * ww / w];
        }
    return out;
}

// Own: analysis (timed) + timeline render at w x h
std::vector<std::vector<uint8_t>> ownFrames(const QString& file, int n, int w, int h, double* analysisMs, double* applyMs)
{
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.in = 0;
    c.out = n - 1;
    EffectRegistry::add(c, "stabilize");
    const auto r = Stabilizer::request(c, ProjectFormat{});
    QElapsedTimer t;
    t.start();
    QString error;
    if (!r || !Stabilizer::analyze(*r, &error)) {
        std::printf("own analysis failed: %s\n", qPrintable(error));
        return {};
    }
    *analysisMs = t.nsecsElapsed() / 1e6 / n;
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << c;
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    std::vector<std::vector<uint8_t>> out;
    t.restart();
    for (int k = 0; k < n; ++k) {
        tr->seek(k);
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        out.push_back(luma(*f, w, h));
    }
    *applyMs = t.nsecsElapsed() / 1e6 / n;
    return out;
}

std::vector<std::vector<uint8_t>> plainFrames(const QString& file, int n, int w, int h, double* ms)
{
    ProjectFormat fmt;
    auto prof = makeProfile(fmt);
    ProducerFactory factory(*prof);
    auto p = factory.open(file);
    std::vector<std::vector<uint8_t>> out;
    QElapsedTimer t;
    t.start();
    for (int k = 0; k < n; ++k) {
        p->seek(k);
        std::unique_ptr<Mlt::Frame> f(p->get_frame());
        out.push_back(luma(*f, w, h));
    }
    *ms = t.nsecsElapsed() / 1e6 / n;
    return out;
}

// vid.stab through MLT (only for the comparison): analysis at 1280 wide like schneidi 0.13, then apply
std::vector<std::vector<uint8_t>> vidstabFrames(const QString& file, int n, int w, int h, const QString& trf, double* analysisMs)
{
    std::vector<std::vector<uint8_t>> out;
    {
        ProjectFormat fmt;
        fmt.width = 1280;
        fmt.height = 720;
        auto prof = makeProfile(fmt);
        ProducerFactory factory(*prof);
        auto p = factory.open(file);
        Mlt::Filter f(*prof, "vidstab");
        if (!f.is_valid()) return {};
        f.set("filename", trf.toUtf8().constData());
        f.set("shakiness", 5);
        f.set("accuracy", 9);
        f.set_in_and_out(0, n - 1);
        p->attach(f);
        QElapsedTimer t;
        t.start();
        for (int k = 0; k < n; ++k) {
            p->seek(k);
            std::unique_ptr<Mlt::Frame> fr(p->get_frame());
            mlt_image_format fmtI = mlt_image_yuv420p;
            int ww = 1280, hh = 720;
            fr->get_image(fmtI, ww, hh);
        }
        *analysisMs = t.nsecsElapsed() / 1e6 / n;
        p->detach(f);
    }
    auto prof = makeProfile(ProjectFormat{});
    ProducerFactory factory(*prof);
    auto p = factory.open(file);
    Mlt::Filter f(*prof, "vidstab");
    f.set("results", trf.toUtf8().constData());
    f.set("smoothing", 15);
    f.set("optzoom", 1);
    f.set_in_and_out(0, n - 1);
    p->attach(f);
    for (int k = 0; k < n; ++k) {
        p->seek(k);
        std::unique_ptr<Mlt::Frame> fr(p->get_frame());
        out.push_back(luma(*fr, w, h));
    }
    return out;
}

void savePng(const std::vector<uint8_t>& y, int w, int h, const QString& path)
{
    QImage img(y.data(), w, h, w, QImage::Format_Grayscale8);
    img.copy().save(path);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-stabilize");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    QDir(Stabilizer::cacheDir()).removeRecursively();
    QTemporaryDir tmp;
    const int w = 960, h = 540, n = 150;
    bool haveVidstab = false;
    {
        Mlt::Profile p;
        Mlt::Filter f(p, "vidstab");
        haveVidstab = f.is_valid();
    }
    std::printf("synthetic: %d frames 1920x1080, measured at %dx%d; vid.stab %s\n", n, w, h,
                haveVidstab ? "available" : "not installed");
    struct Case {
        const char* name;
        double shift, angle, zoom;
    };
    const Case cases[] = {{"shift +-12 px", 12, 0, 0}, {"shift +-20 px, rot +-1.5 deg", 20, 1.5, 0},
                          {"shift +-30 px, rot +-2 deg, zoom +-1 %", 30, 2, 0.01}};
    uint32_t seed = 3;
    for (const Case& cs : cases) {
        const QString file = makeClip(tmp.filePath(QString("shaky%1.mp4").arg(seed)), n, cs.shift, cs.angle, cs.zoom, seed);
        ++seed;
        if (file.isEmpty()) {
            std::printf("ffmpeg failed\n");
            return 1;
        }
        double decodeMs = 0, ownAnalysis = 0, ownApply = 0, vsAnalysis = 0;
        const Measure raw = measure(plainFrames(file, n, w, h, &decodeMs), w, h, 16);
        const Measure own = measure(ownFrames(file, n, w, h, &ownAnalysis, &ownApply), w, h, 16);
        std::printf("%-40s raw: residual %6.2f px diff %5.2f | own: residual %5.2f px diff %5.2f, analysis %5.1f ms/frame",
                    cs.name, raw.residual, raw.diff, own.residual, own.diff, ownAnalysis);
        if (haveVidstab) {
            const auto vs = vidstabFrames(file, n, w, h, tmp.filePath(QString("v%1.trf").arg(seed)), &vsAnalysis);
            const Measure v = measure(vs, w, h, 16);
            std::printf(" | vid.stab: residual %5.2f px diff %5.2f, analysis %5.1f ms/frame", v.residual, v.diff, vsAnalysis);
        }
        std::printf("\n");
    }

    // Real clips: speed and what the analysis sees
    const QString outDir = QString::fromLocal8Bit(qgetenv("BENCH_OUT"));
    for (int i = 1; i < argc; ++i) {
        const QString file = QString::fromLocal8Bit(argv[i]);
        Clip c;
        c.id = 1;
        c.mediaPath = file;
        auto prof = makeProfile(ProjectFormat{});
        ProducerFactory factory(*prof);
        auto p = factory.open(file);
        if (!p || !p->is_valid()) continue;
        c.in = 0;
        c.out = std::min(p->get_length() - 1, 25 * 60);
        EffectRegistry::add(c, "stabilize");
        const auto r = Stabilizer::request(c, ProjectFormat{});
        QElapsedTimer t;
        t.start();
        QString error;
        const bool ok = r && Stabilizer::analyze(*r, &error);
        const double ms = t.nsecsElapsed() / 1e6 / (c.out + 1);
        double decode = 0;
        const int probeN = std::min(c.out + 1, 200);
        plainFrames(file, probeN, 960, 540, &decode);
        std::printf("%s: %d frames, analysis %s %.1f ms/frame (decode+scale alone %.1f ms/frame)\n",
                    qPrintable(QFileInfo(file).fileName()), c.out + 1, ok ? "ok" : qPrintable(error), ms, decode);
        if (!ok) continue;
        double analysisMs = 0, applyMs = 0, plainMs = 0;
        // apply cost at full size: timeline frames with and without the filter
        const auto stab = ownFrames(file, probeN, 1920, 1080, &analysisMs, &applyMs);
        const auto plain = plainFrames(file, probeN, 1920, 1080, &plainMs);
        std::printf("  playback 1920x1080: %.1f ms/frame stabilized, %.1f ms/frame plain\n", applyMs, plainMs);
        const Measure before = measure(plain, 1920, 1080, 16), after = measure(stab, 1920, 1080, 16);
        std::printf("  motion (own ruler): %.2f px -> %.2f px per frame; frame difference %.2f -> %.2f\n",
                    before.residual, after.residual, before.diff, after.diff);
        if (!outDir.isEmpty())
            for (int k : {0, probeN / 2, probeN - 1}) {
                savePng(plain[size_t(k)], 1920, 1080, QDir(outDir).filePath(QString("real%1-%2-plain.png").arg(i).arg(k)));
                savePng(stab[size_t(k)], 1920, 1080, QDir(outDir).filePath(QString("real%1-%2-stab.png").arg(i).arg(k)));
            }
    }
    return 0;
}
