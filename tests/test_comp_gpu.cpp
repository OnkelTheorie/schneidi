// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graphs on the graphics card (0.19 package F, engine/picture/gpu/CompGpu + shaders merge/mask/key/blur)
// against the CPU evaluation (CompEval), synthetic pictures: every merge mode (opaque and transparent backdrop,
// opacity, mask), mask shapes (rect, ellipse, linear, radial; rotated, soft, inverted, combined), chroma and luma key,
// blur (with mask), transform, crop, grade, and recipe-like graphs (green screen over a background with a title band,
// masked blur, key -> grade -> blur -> merge). Within 2 code values (premultiplied colour and alpha). Also the split
// rule: a CPU node after a card node, or a graph without card nodes, stays on the CPU; a polygon mask is uploaded.
// Projective draws (0.22: corner pin by hand and following a quad tracker, a text's box pinned, a transform tilted in
// depth): PSNR of premultiplied RGBA at least 40 dB.
// Skips (77) without a hardware GPU device; SCHNEIDI_GPU=1 runs it on a software device too.
// COMP_GPU_BENCH=n: n frames of the recipes at 1920x1080, CPU (CompEval, all threads) vs card (plan + passes +
// readback), ms per frame.

#include "render_check.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/gpu/CompGpu.h"
#include "engine/picture/gpu/Gpu.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSurfaceFormat>

#include <cmath>
#include <cstring>
#include <vector>

using namespace Picture;
namespace PC = Picture::Comp;

namespace {

Gpu::Context* g_gpu = nullptr;

EffectInstance comp(const QStringList& types, const QVector<CompLink>& links, int out, const QVariantMap& values = {})
{
    EffectInstance e;
    e.effectId = ::Comp::kEffectId;
    CompGraph g;
    for (int i = 0; i < types.size(); ++i) g.nodes << CompNode{i + 1, types[i], QPointF()};
    g.links = links;
    g.out = out;
    ::Comp::setGraph(e, g);
    for (auto it = values.cbegin(); it != values.cend(); ++it) e.params.insert(it.key(), it.value());
    return e;
}

// A figure on a green screen: green background, a shaded disc (skin-ish to blue), a gray bar, a green-tinted fringe
PC::Buf greenScreen(int w, int h)
{
    PC::Buf b(PC::Buf::Rgba, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = b.data() + (size_t(y) * w + x) * 4;
            const double dx = (x - w * 0.5) / (h * 0.3), dy = (y - h * 0.5) / (h * 0.3);
            const double r = std::sqrt(dx * dx + dy * dy);
            int c[3] = {30 + (x * 7) % 40, 190 + (y * 3) % 30, 40};
            if (r < 1.0) {
                c[0] = int(220 - 120 * dx * dx);
                c[1] = int(160 - 60 * r);
                c[2] = int(110 + 120 * std::max(0.0, dy));
            } else if (r < 1.08) { // fringe: half green
                c[0] = 120, c[1] = 200, c[2] = 90;
            }
            if (y > h * 0.8 && x > w * 0.1 && x < w * 0.4) c[0] = c[1] = c[2] = 40 + x % 180;
            p[0] = uint8_t(std::clamp(c[0], 0, 255));
            p[1] = uint8_t(std::clamp(c[1], 0, 255));
            p[2] = uint8_t(std::clamp(c[2], 0, 255));
            p[3] = 255;
        }
    return b;
}

struct Result {
    bool cpu = false, gpu = false;
    int maxColor = 0, maxAlpha = 0;
    double off = 0; // percent of pixels off by more than 1
    double psnr = 0; // premultiplied RGBA, dB (inf = identical)
};

std::vector<uint8_t> runCard(const Gpu::CompPlan& plan, QSize size, bool read = true)
{
    std::vector<uint8_t> out(size_t(size.width()) * size.height() * 4);
    std::vector<Gpu::TexturePtr> keep;
    g_gpu->begin();
    Gpu::TexturePtr t = Gpu::runComp(*g_gpu, plan, &keep);
    Gpu::TexturePtr canvas = g_gpu->target(size);
    if (t && canvas) {
        g_gpu->draw(*canvas, {t.get(), {}, 1.0, nullptr, {}});
        if (read) g_gpu->readRgba(*canvas, out.data());
    }
    g_gpu->finish();
    if (!t) out.clear();
    return out;
}

PC::Env envFor(const PC::Buf& clip)
{
    PC::Env env;
    env.size = QSize(clip.w, clip.h);
    env.clipPicture = [&clip]() { return &clip; };
    return env;
}

Result compare(const EffectInstance& e, const PC::Buf& clip)
{
    Result r;
    const PC::Env env = envFor(clip);
    PC::Buf cpu;
    r.cpu = PC::evaluate(e, env, nullptr, &cpu);
    QString why;
    const Gpu::CompPlanPtr plan = Gpu::planComp(e, env, nullptr, &why);
    if (!plan) {
        std::printf("       not on the card: %s\n", qPrintable(why));
        return r;
    }
    const std::vector<uint8_t> gpu = runCard(*plan, env.size);
    r.gpu = !gpu.empty();
    if (!r.cpu || !r.gpu || cpu.px.size() != gpu.size()) return r;
    size_t off = 0;
    double sq = 0;
    for (size_t i = 0; i < gpu.size(); i += 4) {
        const int ac = cpu.px[i + 3], ag = gpu[i + 3];
        int worst = std::abs(ac - ag);
        sq += double(ac - ag) * (ac - ag);
        for (int c = 0; c < 3; ++c) {
            const int d = (cpu.px[i + c] * ac + 127) / 255 - (gpu[i + c] * ag + 127) / 255;
            sq += double(d) * d;
        }
        r.maxAlpha = std::max(r.maxAlpha, worst);
        for (int c = 0; c < 3; ++c) { // premultiplied: what is seen over any background
            const int pc = (cpu.px[i + c] * ac + 127) / 255, pg = (gpu[i + c] * ag + 127) / 255;
            r.maxColor = std::max(r.maxColor, std::abs(pc - pg));
            worst = std::max(worst, std::abs(pc - pg));
        }
        off += worst > 1;
    }
    r.off = 100.0 * double(off) / double(gpu.size() / 4);
    const double mse = sq / double(gpu.size());
    r.psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 1e9;
    return r;
}

// Projective draws: the card's float map against the CPU's fixed point differs by a little at edges
void close(const char* name, const EffectInstance& e, const PC::Buf& clip, double minPsnr = 40)
{
    const Result r = compare(e, clip);
    const bool ok = r.cpu && r.gpu && r.psnr >= minPsnr;
    if (qEnvironmentVariableIsSet("COMP_GPU_VERBOSE"))
        std::printf("%-60s PSNR %.1f dB, max colour %d alpha %d, %.2f %% off by > 1\n", name, r.psnr, r.maxColor,
                    r.maxAlpha, r.off);
    Check::report(ok, name, __FILE__, __LINE__,
                  QString("cpu %1 gpu %2, PSNR %3 dB, max colour %4, alpha %5").arg(r.cpu).arg(r.gpu)
                      .arg(r.psnr, 0, 'f', 1).arg(r.maxColor).arg(r.maxAlpha));
}

void same(const char* name, const EffectInstance& e, const PC::Buf& clip, int tolerance = 2)
{
    const Result r = compare(e, clip);
    const bool ok = r.cpu && r.gpu && r.maxColor <= tolerance && r.maxAlpha <= tolerance;
    if (qEnvironmentVariableIsSet("COMP_GPU_VERBOSE"))
        std::printf("%-60s max colour %d alpha %d, %.2f %% off by > 1\n", name, r.maxColor, r.maxAlpha, r.off);
    Check::report(ok, name, __FILE__, __LINE__,
                  QString("cpu %1 gpu %2, max colour %3, alpha %4, %5 % of pixels off by > 1")
                      .arg(r.cpu).arg(r.gpu).arg(r.maxColor).arg(r.maxAlpha).arg(r.off, 0, 'f', 2));
}

// A title-like band: a gradient from transparent to an opaque colour (with alpha in between)
QVariantMap band(int id, double y0, double y1, const char* c0, const char* c1)
{
    const QString n = QStringLiteral("n%1.").arg(id);
    return {{n + "x0", 0.2}, {n + "y0", y0}, {n + "x1", 0.8}, {n + "y1", y1}, {n + "c0", QString(c0)}, {n + "c1", QString(c1)}};
}

QVariantMap merged(QVariantMap a, const QVariantMap& b)
{
    for (auto it = b.cbegin(); it != b.cend(); ++it) a.insert(it.key(), it.value());
    return a;
}

// Recipes (clip node 1 is the picture on the green screen)
EffectInstance recipeGreenScreen()
{
    // clip -> key -> merge(bg = gradient background) -> merge(fg = title band) -> out
    return comp({"clip", "key", "gradient", "merge", "gradient", "merge"},
                {{1, 2, "src"}, {3, 4, "bg"}, {2, 4, "fg"}, {4, 6, "bg"}, {5, 6, "fg"}}, 6,
                merged(merged(band(3, 0, 1, "#ff203060", "#ffe0a040"), band(5, 0.75, 0.95, "#00ffffff", "#e0ffffff")),
                       {{"n2.color", "#ff00c828"}, {"n2.tolerance", 0.25}, {"n2.softness", 0.2}, {"n2.spill", 1.0}}));
}

EffectInstance recipeMaskedBlur()
{
    // clip -> blur(mask = soft ellipse) -> out
    return comp({"clip", "mask", "blur"}, {{1, 3, "src"}, {2, 3, "mask"}}, 3,
                {{"n2.shape", "ellipse"}, {"n2.w", 0.3}, {"n2.h", 0.3}, {"n2.soft", 12.0}, {"n3.radius", 10.0}});
}

EffectInstance recipeLook()
{
    // clip -> key -> grade -> transform -> merge over a colour, the background blurred: color -> merge(bg) ...
    return comp({"clip", "key", "grade", "transform", "color", "blur", "merge"},
                {{1, 2, "src"}, {2, 3, "src"}, {3, 4, "src"}, {1, 6, "src"}, {6, 7, "bg"}, {4, 7, "fg"}}, 7,
                {{"n2.color", "#ff00c828"}, {"n3.saturation", 70.0}, {"n3.contrast", 1.15}, {"n4.x", 0.45},
                 {"n4.size", 0.85}, {"n4.angle", 8.0}, {"n6.radius", 6.0}});
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    // A headless machine's card through EGL (SCHNEIDI_TEST_QPA=eglfs): desktop OpenGL, as on a desktop
    if (qEnvironmentVariable("SCHNEIDI_TEST_QPA") == QLatin1String("eglfs")) {
        QSurfaceFormat f = QSurfaceFormat::defaultFormat();
        f.setRenderableType(QSurfaceFormat::OpenGL);
        QSurfaceFormat::setDefaultFormat(f);
    }
    QGuiApplication app(argc, argv);
    Check::initApp("comp_gpu");

    QString why;
    const bool force = qgetenv("SCHNEIDI_GPU") == "1";
    std::unique_ptr<Gpu::Context> gpu = Gpu::Context::create(&why, force);
    if (!gpu) return Check::skip(qPrintable("no GPU device (" + why + ")"));
    if (!gpu->compSupported()) return Check::skip("the device has no RGBA16F targets / compositing shaders");
    g_gpu = gpu.get();
    std::printf("GPU: %s on %s\n", qPrintable(gpu->info().api), qPrintable(gpu->info().device));

    const PC::Buf pic = greenScreen(320, 180);

    // 1. Merge: every mode, over the opaque picture and over a transparent band, with opacity, with a mask
    const char* modes[] = {"normal", "add", "screen", "multiply", "overlay", "darken", "lighten", "difference"};
    for (const char* mode : modes) {
        const QVariantMap fg = band(2, 0, 1, "#40ff3010", "#f010a0ff");
        same(qPrintable(QString("merge %1 over the picture").arg(mode)),
             comp({"clip", "gradient", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3, merged(fg, {{"n3.mode", mode}})), pic);
        same(qPrintable(QString("merge %1 opacity 0.6 over a transparent band").arg(mode)),
             comp({"gradient", "clip", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                  merged(band(1, 0, 1, "#00000000", "#c0ffffff"), {{"n3.mode", mode}, {"n3.opacity", 0.6}})),
             pic);
        same(qPrintable(QString("merge %1 with a mask").arg(mode)),
             comp({"clip", "gradient", "mask", "merge"}, {{1, 4, "bg"}, {2, 4, "fg"}, {3, 4, "mask"}}, 4,
                  merged(fg, {{"n4.mode", mode}, {"n3.shape", "ellipse"}, {"n3.soft", 20.0}})),
             pic);
    }
    same("merge without fg = bg", comp({"clip", "merge"}, {{1, 2, "bg"}}, 2), pic);
    same("merge without bg", comp({"clip", "merge"}, {{1, 2, "fg"}}, 2, {{"n2.opacity", 0.5}}), pic);

    // 2. Masks (shown through merge white over black)
    struct MaskCase {
        const char* name;
        QVariantMap v;
    };
    const MaskCase masks[] = {
        {"rect", {{"n3.shape", "rect"}, {"n3.w", 0.5}, {"n3.h", 0.3}}},
        {"rect rotated soft", {{"n3.shape", "rect"}, {"n3.rotate", 23.0}, {"n3.soft", 15.0}, {"n3.x", 0.4}}},
        {"ellipse soft", {{"n3.shape", "ellipse"}, {"n3.w", 0.6}, {"n3.h", 0.25}, {"n3.soft", 30.0}, {"n3.rotate", -40.0}}},
        {"ellipse inverted", {{"n3.shape", "ellipse"}, {"n3.invert", true}, {"n3.soft", 5.0}}},
        {"linear", {{"n3.shape", "linear"}, {"n3.p0x", 0.2}, {"n3.p0y", 0.3}, {"n3.p1x", 0.7}, {"n3.p1y", 0.9}}},
        {"radial", {{"n3.shape", "radial"}, {"n3.p0x", 0.5}, {"n3.p0y", 0.5}, {"n3.p1x", 0.8}, {"n3.p1y", 0.5}}},
        {"polygon (CPU, uploaded)", {{"n3.shape", "polygon"}, {"n3.soft", 4.0}}},
    };
    for (const MaskCase& m : masks)
        same(qPrintable(QString("mask %1").arg(m.name)),
             comp({"color", "color", "mask", "merge"}, {{1, 4, "bg"}, {2, 4, "fg"}, {3, 4, "mask"}}, 4,
                  merged(m.v, {{"n1.color", "#ff000000"}, {"n2.color", "#ffffffff"}})),
             pic);
    for (const char* combine : {"add", "subtract", "intersect"})
        same(qPrintable(QString("mask combine %1").arg(combine)),
             comp({"color", "color", "mask", "merge", "mask"}, {{1, 4, "bg"}, {2, 4, "fg"}, {5, 3, "mask"}, {3, 4, "mask"}}, 4,
                  {{"n1.color", "#ff000000"}, {"n2.color", "#ffffffff"}, {"n3.shape", "ellipse"}, {"n3.soft", 10.0},
                   {"n3.combine", combine}, {"n5.shape", "rect"}, {"n5.x", 0.65}, {"n5.w", 0.3}, {"n5.h", 0.4}}),
             pic);

    // A polygon mask (drawn on the CPU, uploaded) following a tracker that was not analyzed yet: the follow rule
    // (Comp::followValue) as CompEval - two points, x/y = the reference (middle of a and b), nothing moves
    same("polygon mask following an unanalyzed two-point tracker",
         comp({"color", "color", "mask", "merge", "tracker"}, {{1, 4, "bg"}, {2, 4, "fg"}, {3, 4, "mask"}}, 4,
              {{"n1.color", "#ff000000"}, {"n2.color", "#ffffffff"}, {"n3.shape", "polygon"}, {"n3.follow", 5.0},
               {"n5.mode", "two"}, {"n5.ax", 0.2}, {"n5.ay", 0.3}, {"n5.bx", 0.8}, {"n5.by", 0.7}}),
         pic);

    // 3. Keys
    same("chroma key", comp({"clip", "key"}, {{1, 2, "src"}}, 2, {{"n2.color", "#ff00c828"}}), pic);
    same("chroma key soft, half spill",
         comp({"clip", "key"}, {{1, 2, "src"}}, 2,
              {{"n2.color", "#ff10d040"}, {"n2.tolerance", 0.15}, {"n2.softness", 0.4}, {"n2.spill", 0.5}}),
         pic);
    same("luma key", comp({"clip", "key"}, {{1, 2, "src"}}, 2, {{"n2.mode", "luma"}, {"n2.low", 0.3}, {"n2.high", 0.8}, {"n2.soft", 0.15}}), pic);
    same("luma key inverted", comp({"clip", "key"}, {{1, 2, "src"}}, 2, {{"n2.mode", "luma"}, {"n2.low", 0.5}, {"n2.invert", true}}), pic);

    // 4. Blur, transform, crop, grade
    same("blur 4", comp({"clip", "blur"}, {{1, 2, "src"}}, 2, {{"n2.radius", 4.0}}), pic);
    same("blur 12 of a keyed picture (transparency)", comp({"clip", "key", "blur"}, {{1, 2, "src"}, {2, 3, "src"}}, 3,
                                                          {{"n2.color", "#ff00c828"}, {"n3.radius", 12.0}}), pic);
    same("blur in a mask", recipeMaskedBlur(), pic);
    same("transform", comp({"clip", "transform"}, {{1, 2, "src"}}, 2, {{"n2.x", 0.4}, {"n2.size", 0.8}, {"n2.angle", 12.0}}), pic);
    same("transform enlarged", comp({"clip", "transform"}, {{1, 2, "src"}}, 2, {{"n2.size", 1.7}, {"n2.ay", 0.3}}), pic);
    same("crop", comp({"clip", "crop"}, {{1, 2, "src"}}, 2, {{"n2.left", 0.1}, {"n2.top", 0.237}, {"n2.right", 0.3}}), pic);
    same("grade", comp({"clip", "grade"}, {{1, 2, "src"}}, 2, {{"n2.saturation", 80.0}, {"n2.contrast", 1.1}}), pic);
    same("grade in a mask", comp({"clip", "grade", "mask"}, {{1, 2, "src"}, {3, 2, "mask"}}, 2,
                                 {{"n2.saturation", 20.0}, {"n3.shape", "rect"}, {"n3.soft", 8.0}}), pic);

    // 4b. Projective (0.22): a transform tilted in depth, the corner pin
    close("transform tilted in depth", comp({"clip", "transform"}, {{1, 2, "src"}}, 2,
                                            {{"n2.size", 0.8}, {"n2.tilt_x", 25.0}, {"n2.tilt_y", -15.0}}), pic);
    close("transform tilted, turned, moved", comp({"clip", "transform"}, {{1, 2, "src"}}, 2,
                                                  {{"n2.x", 0.45}, {"n2.angle", 10.0}, {"n2.tilt_y", 40.0}, {"n2.fov", 70.0}}), pic);
    const QVariantMap pin{{"n2.c0x", 0.15}, {"n2.c0y", 0.12}, {"n2.c1x", 0.82}, {"n2.c1y", 0.2},
                          {"n2.c2x", 0.9}, {"n2.c2y", 0.85}, {"n2.c3x", 0.1}, {"n2.c3y", 0.92}};
    close("corner pin by hand", comp({"clip", "corner_pin"}, {{1, 2, "src"}}, 2, pin), pic);
    close("corner pin shrinking (small quad)", comp({"clip", "corner_pin"}, {{1, 2, "src"}}, 2,
                                                   {{"n2.c0x", 0.4}, {"n2.c0y", 0.4}, {"n2.c1x", 0.62}, {"n2.c1y", 0.42},
                                                    {"n2.c2x", 0.6}, {"n2.c2y", 0.6}, {"n2.c3x", 0.41}, {"n2.c3y", 0.58}}), pic);
    close("corner pin over the clip with opacity",
          comp({"clip", "gradient", "corner_pin", "merge"}, {{2, 3, "src"}, {1, 4, "bg"}, {3, 4, "fg"}}, 4,
               merged(merged(band(2, 0, 1, "#ff2040ff", "#ffffe020"), {{"n3.opacity", 0.7}}),
                      {{"n3.c0x", 0.3}, {"n3.c0y", 0.2}, {"n3.c1x", 0.75}, {"n3.c1y", 0.3}, {"n3.c2x", 0.7},
                       {"n3.c2y", 0.7}, {"n3.c3x", 0.25}, {"n3.c3y", 0.8}})), pic);
    close("corner pin following a quad tracker (start corners)",
          comp({"clip", "tracker", "gradient", "corner_pin", "merge"}, {{1, 2, "src"}, {3, 4, "src"}, {2, 5, "bg"}, {4, 5, "fg"}}, 5,
               merged(band(3, 0, 1, "#ff2040ff", "#ffffe020"),
                      {{"n2.mode", "quad"}, {"n2.q0x", 0.2}, {"n2.q0y", 0.25}, {"n2.q1x", 0.7}, {"n2.q1y", 0.2},
                       {"n2.q2x", 0.75}, {"n2.q2y", 0.8}, {"n2.q3x", 0.25}, {"n2.q3y", 0.7}, {"n4.follow", 2.0}})), pic);
    close("corner pin of a text (its box, uploaded)",
          comp({"clip", "text", "corner_pin", "merge"}, {{2, 3, "src"}, {1, 4, "bg"}, {3, 4, "fg"}}, 4,
               merged(pin, {{"n2.text", "PIN"}, {"n2.size", 40.0}, {"n3.c0x", 0.15}, {"n3.c0y", 0.12}, {"n3.c1x", 0.82},
                            {"n3.c1y", 0.2}, {"n3.c2x", 0.9}, {"n3.c2y", 0.85}, {"n3.c3x", 0.1}, {"n3.c3y", 0.92}})), pic);
    {
        const PC::Env env = envFor(pic);
        QString reason;
        CHECK(Gpu::planComp(comp({"clip", "transform"}, {{1, 2, "src"}}, 2, {{"n2.tilt_x", 25.0}}), env, nullptr, &reason));
        CHECK(Gpu::planComp(comp({"clip", "corner_pin"}, {{1, 2, "src"}}, 2, pin), env, nullptr, &reason));
        // A source with a content area (a text's box, a letterboxed media) through a grade on the card: the card does
        // not carry the area, so the graph stays on the CPU (GPU = CPU: the box is stretched, not the whole frame)
        const EffectInstance graded =
            comp({"clip", "text", "grade", "corner_pin", "merge"}, {{2, 3, "src"}, {3, 4, "src"}, {1, 5, "bg"}, {4, 5, "fg"}}, 5,
                 {{"n2.text", "PIN"}, {"n2.size", 40.0}, {"n3.saturation", 60.0}, {"n4.c0x", 0.15}, {"n4.c0y", 0.12},
                  {"n4.c1x", 0.82}, {"n4.c1y", 0.2}, {"n4.c2x", 0.9}, {"n4.c2y", 0.85}, {"n4.c3x", 0.1}, {"n4.c3y", 0.92}});
        CHECK(!Gpu::planComp(graded, env, nullptr, &reason));
        CHECK(reason.contains("content area"));
        // The same pin without the grade (on the card, the text uploaded with its box): the letters land on the same
        // pixels as the graded one on the CPU (a grade keeps alpha)
        EffectInstance plain = graded;
        CompGraph pg;
        ::Comp::graphOf(plain, &pg);
        pg.links = {{2, 4, "src"}, {1, 5, "bg"}, {4, 5, "fg"}};
        pg.out = 4;
        ::Comp::setGraph(plain, pg);
        EffectInstance gradedPin = graded;
        CompGraph gg;
        ::Comp::graphOf(gradedPin, &gg);
        gg.out = 4;
        ::Comp::setGraph(gradedPin, gg);
        PC::Buf b;
        const Gpu::CompPlanPtr plainPlan = Gpu::planComp(plain, env, nullptr, &reason);
        if (CHECK(plainPlan) && CHECK(PC::evaluate(gradedPin, env, nullptr, &b))) {
            const std::vector<uint8_t> card = runCard(*plainPlan, env.size);
            int worst = 0;
            if (CHECK(card.size() == b.px.size()))
                for (size_t i = 3; i < card.size(); i += 4) worst = std::max(worst, std::abs(int(card[i]) - int(b.px[i])));
            if (!CHECK(worst <= 3)) std::printf("       letterboxed pin: alpha off by %d\n", worst);
        }
    }

    // 5. Recipes
    same("recipe green screen over a background with a title band", recipeGreenScreen(), pic);
    same("recipe key -> grade -> transform over a blurred background", recipeLook(), pic);

    // 6. Split rule
    {
        const PC::Env env = envFor(pic);
        QString reason;
        CHECK(!Gpu::planComp(comp({"clip", "key", "fx"}, {{1, 2, "src"}, {2, 3, "src"}}, 3), env, nullptr, &reason));
        CHECK(reason.contains("after a node on the card"));
        CHECK(!Gpu::planComp(comp({"clip", "fx"}, {{1, 2, "src"}}, 2), env, nullptr, &reason));
        CHECK(!Gpu::planComp(comp({"clip", "blur"}, {{1, 2, "src"}}, 2, {{"n2.radius", 400.0}}), env, nullptr, &reason));
        CHECK(Gpu::planComp(recipeGreenScreen(), env, nullptr, &reason));
        // Decided before any work: a graph that ends up on the CPU did not fetch (evaluate, upload) its sources first
        int fetched = 0;
        PC::Env counting = env;
        counting.clipPicture = [&]() {
            ++fetched;
            return &pic;
        };
        CHECK(!Gpu::planComp(comp({"clip", "key", "fx"}, {{1, 2, "src"}, {2, 3, "src"}}, 3), counting, nullptr, &reason));
        CHECK_EQ(fetched, 0);
        CHECK(Gpu::planComp(comp({"clip", "key"}, {{1, 2, "src"}}, 2), counting, nullptr, &reason));
        CHECK_EQ(fetched, 1);
    }

    // Timing at 1920x1080
    if (const int n = qEnvironmentVariableIntValue("COMP_GPU_BENCH"); n > 0) {
        const PC::Buf big = greenScreen(1920, 1080);
        const PC::Env env = envFor(big);
        struct Bench {
            const char* name;
            EffectInstance e;
        };
        const Bench benches[] = {
            {"green screen + background + title", recipeGreenScreen()},
            {"masked blur", recipeMaskedBlur()},
            {"key -> grade -> transform over blur", recipeLook()},
            {"blur 8", comp({"clip", "blur"}, {{1, 2, "src"}}, 2, {{"n2.radius", 8.0}})},
            {"merge multiply", comp({"clip", "gradient", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                                    merged(band(2, 0, 1, "#40ff3010", "#f010a0ff"), {{"n3.mode", "multiply"}}))},
        };
        for (const Bench& b : benches) {
            PC::Cache cache;
            PC::Buf out;
            PC::evaluate(b.e, env, &cache, &out); // warm up (static nodes cached, as in playback)
            QElapsedTimer t;
            t.start();
            for (int i = 0; i < n; ++i) PC::evaluate(b.e, env, &cache, &out);
            const double cpu = double(t.nsecsElapsed()) / 1e6 / n;
            Gpu::CompPlanPtr plan = Gpu::planComp(b.e, env, &cache);
            if (!plan) {
                std::printf("bench %-40s CPU %6.1f ms   card: not planned\n", b.name, cpu);
                continue;
            }
            runCard(*plan, env.size);
            double planMs = 0, cardMs = 0, cardNoRead = 0;
            for (int i = 0; i < n; ++i) {
                t.restart();
                plan = Gpu::planComp(b.e, env, &cache);
                planMs += double(t.nsecsElapsed()) / 1e6;
                t.restart();
                runCard(*plan, env.size);
                cardMs += double(t.nsecsElapsed()) / 1e6;
                t.restart();
                runCard(*plan, env.size, false);
                cardNoRead += double(t.nsecsElapsed()) / 1e6;
            }
            std::printf("bench %-40s CPU %6.1f ms   card: plan %5.1f + passes/readback %5.1f ms (without readback %5.1f)\n",
                        b.name, cpu, planMs / n, cardMs / n, cardNoRead / n);
        }
    }
    return Check::result();
}
