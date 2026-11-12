// SPDX-License-Identifier: AGPL-3.0-only
// Time ranges of compositing nodes on the graphics card (0.22): the renderer hands the card's planner the graph as it
// works at t (Picture::Comp::activeGraph); the card's picture equals the CPU evaluation inside and outside the ranges -
// a color in a merge's fg, the merge itself, a blur, a generator out of range (the added transparent node) and that
// generator as the output. Within 2 code values. Skips (77) without a hardware GPU device; SCHNEIDI_GPU=1 forces one.
#include "render_check.h"

#include "core/CompGraph.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/gpu/CompGpu.h"
#include "engine/picture/gpu/Gpu.h"

#include <QGuiApplication>

#include <cmath>
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

// Stripes, so a blur shows
PC::Buf stripes(int w, int h)
{
    PC::Buf b(PC::Buf::Rgba, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = b.data() + (size_t(y) * w + x) * 4;
            const bool on = (x / 8) % 2;
            p[0] = on ? 230 : 20;
            p[1] = 200;
            p[2] = on ? 40 : 180;
            p[3] = 255;
        }
    return b;
}

// CPU and card at clip frame t (the card gets the active graph, as the renderer does); -1 = not on the card
int worst(const EffectInstance& e, const PC::Buf& clip, double t, bool* onCard)
{
    PC::Env env;
    env.size = QSize(clip.w, clip.h);
    env.fps = 25;
    env.t = t;
    env.clipPicture = [&clip]() { return &clip; };
    PC::Buf cpu;
    if (!PC::evaluate(e, env, nullptr, &cpu)) return 1000;
    EffectInstance active;
    const bool ranged = PC::activeGraph(e, t, 25, nullptr, &active);
    QString why;
    const Gpu::CompPlanPtr plan = Gpu::planComp(ranged ? active : e, env, nullptr, &why);
    *onCard = bool(plan);
    if (!plan) {
        std::printf("       not on the card at %g: %s\n", t, qPrintable(why));
        return 0;
    }
    std::vector<uint8_t> out(cpu.px.size());
    std::vector<Gpu::TexturePtr> keep;
    g_gpu->begin();
    Gpu::TexturePtr tex = Gpu::runComp(*g_gpu, *plan, &keep);
    Gpu::TexturePtr canvas = g_gpu->target(env.size);
    if (tex && canvas) {
        g_gpu->draw(*canvas, {tex.get(), {}, 1.0, nullptr, {}});
        g_gpu->readRgba(*canvas, out.data());
    }
    g_gpu->finish();
    if (!tex) return 1000;
    int w = 0;
    for (size_t i = 0; i < out.size(); i += 4) {
        const int ac = cpu.px[i + 3], ag = out[i + 3];
        w = std::max(w, std::abs(ac - ag));
        for (int c = 0; c < 3; ++c)
            w = std::max(w, std::abs((cpu.px[i + c] * ac + 127) / 255 - (out[i + c] * ag + 127) / 255));
    }
    return w;
}

void same(const char* name, const EffectInstance& e, const PC::Buf& clip, std::initializer_list<double> times)
{
    for (double t : times) {
        bool onCard = false;
        const int w = worst(e, clip, t, &onCard);
        Check::report(w <= 2, name, __FILE__, __LINE__, QString("t %1: max difference %2").arg(t).arg(w));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_range_gpu");
    QString why;
    std::unique_ptr<Gpu::Context> gpu = Gpu::Context::create(&why, qgetenv("SCHNEIDI_GPU") == "1");
    if (!gpu) return Check::skip(qPrintable("no GPU device (" + why + ")"));
    if (!gpu->compSupported()) return Check::skip("the device has no RGBA16F targets / compositing shaders");
    g_gpu = gpu.get();
    std::printf("GPU: %s on %s\n", qPrintable(gpu->info().api), qPrintable(gpu->info().device));
    const PC::Buf pic = stripes(160, 90);

    same("color in a merge's fg, 1..2 s",
         comp({"clip", "color", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
              {{"n2.color", "#80ff0000"}, {"n2.from", 1.0}, {"n2.until", 2.0}}),
         pic, {0, 30, 60});
    same("the merge with the range",
         comp({"clip", "color", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
              {{"n2.color", "#80ff0000"}, {"n3.from", 1.0}, {"n3.until", 2.0}}),
         pic, {0, 30, 60});
    same("blur from 1 s",
         comp({"clip", "blur", "merge", "color"}, {{1, 2, "src"}, {2, 3, "bg"}, {4, 3, "fg"}}, 3,
              {{"n2.radius", 6.0}, {"n2.from", 1.0}, {"n4.color", "#20000000"}}),
         pic, {0, 30});
    same("generator out of range into a merge and as the output",
         comp({"clip", "color"}, {}, 2, {{"n2.color", "#ff3060ff"}, {"n2.until", 1.0}}), pic, {0, 30});
    return Check::result();
}
