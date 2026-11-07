// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graph evaluation (0.19 package A, engine/picture/CompEval + CompNodesCore + Renderer::applyComp):
// - merge per blend mode against the W3C compositing formula computed here in doubles (opacity, mask)
// - a linear graph clip -> fx -> out renders bit-identical to the same effect in the clip's effect list (also with
//   the clip's transform, inside a dissolve, a varying effect at other frames)
// - "comp from selection" (Editor::compFrom) renders bit-identical to the picture before, one undo step, survives a
//   project file round trip
// - unknown node kinds are passed through and reported by skippedEffects as "comp:<kind>"
// - static nodes are evaluated once (cache), a cancelled evaluation leaves the clip as it is
// - generators (color, gradient), media node, transform, crop against known pixels
// SCHNEIDI_COMP_BENCH=1: also measures a recipe-like graph at 1920x1080 (ms per frame, printed).

#include "render_check.h"

#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/RenderCache.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/Renderer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstring>
#include <mutex>

using namespace Picture::Comp;

namespace {

QString g_pattern, g_pattern2, g_still;
const ProjectFormat kFormat{320, 180, {25, 1}};

QImage render(const Timeline& tl, int frame) { return Check::renderFrame(tl, kFormat, frame); }

// Largest difference of any channel (RGBA); 999 = sizes differ
int maxDiff(const QImage& a, const QImage& b)
{
    if (a.isNull() || a.size() != b.size()) return 999;
    const QImage x = a.convertToFormat(QImage::Format_RGBA8888), y = b.convertToFormat(QImage::Format_RGBA8888);
    int m = 0;
    for (int r = 0; r < x.height(); ++r) {
        const uint8_t* p = x.constScanLine(r);
        const uint8_t* q = y.constScanLine(r);
        for (int i = 0; i < x.width() * 4; ++i) m = std::max(m, std::abs(int(p[i]) - int(q[i])));
    }
    return m;
}

Clip media(int id, const QString& path, int start, int in, int out)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
}

Timeline oneTrack(const QVector<Clip>& clips)
{
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = clips;
    return tl;
}

EffectInstance effect(const QString& id, const QVariantMap& params)
{
    EffectInstance e;
    e.effectId = id;
    e.params = params;
    return e;
}

// A comp effect from a node list "type" (ids 1..n), links and node values {"n<id>.<key>": v}
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

// clip -> fx(effect id, its values as e_<key>) -> out
EffectInstance fxGraph(const QString& id, const QVariantMap& values)
{
    QVariantMap v{{"n2.effect", id}};
    for (auto it = values.cbegin(); it != values.cend(); ++it) v.insert("n2.e_" + it.key(), it.value());
    return comp({"clip", "fx"}, {{1, 2, "src"}}, 2, v);
}

Buf solid(int w, int h, const QVector<QColor>& colors)
{
    Buf b(Buf::Rgba, w, h);
    for (int i = 0; i < w * h; ++i) {
        const QColor& c = colors[i % colors.size()];
        b.px[size_t(i) * 4] = uint8_t(c.red());
        b.px[size_t(i) * 4 + 1] = uint8_t(c.green());
        b.px[size_t(i) * 4 + 2] = uint8_t(c.blue());
        b.px[size_t(i) * 4 + 3] = uint8_t(c.alpha());
    }
    return b;
}

// ---- merge against the formula ----

double blendRef(const QString& mode, double a, double b)
{
    if (mode == "add") return std::min(1.0, a + b);
    if (mode == "screen") return a + b - a * b;
    if (mode == "multiply") return a * b;
    if (mode == "overlay") return a <= 0.5 ? 2 * a * b : 1 - 2 * (1 - a) * (1 - b);
    if (mode == "darken") return std::min(a, b);
    if (mode == "lighten") return std::max(a, b);
    if (mode == "difference") return std::abs(a - b);
    return b;
}

void testMergeModes()
{
    std::printf("merge modes\n");
    const int w = 7, h = 5;
    const QVector<QColor> bgc = {QColor(200, 40, 90, 255), QColor(10, 250, 128, 255), QColor(128, 128, 128, 180),
                                 QColor(0, 0, 0, 0), QColor(255, 255, 255, 90), QColor(60, 120, 30, 255)};
    const QVector<QColor> fgc = {QColor(30, 200, 255, 255), QColor(240, 10, 128, 128), QColor(90, 90, 200, 255),
                                 QColor(255, 128, 0, 200), QColor(5, 5, 5, 60), QColor(255, 0, 0, 0),
                                 QColor(128, 255, 64, 255)};
    const Buf bg = solid(w, h, bgc), fg = solid(w, h, fgc);
    Buf mask(Buf::Mask, w, h);
    for (int i = 0; i < w * h; ++i) mask.px[size_t(i)] = uint8_t((i * 37) % 256);
    RunOptions opt;
    opt.size = QSize(w, h);
    const QStringList modes = {"normal", "add", "screen", "multiply", "overlay", "darken", "lighten", "difference"};
    for (const QString& mode : modes)
        for (const double opacity : {1.0, 0.5})
            for (const bool withMask : {false, true}) {
                Buf out;
                QHash<QString, Buf> in{{"bg", bg}, {"fg", fg}};
                if (withMask) in.insert("mask", mask);
                const bool ran = runNode("merge", {{"mode", mode}, {"opacity", opacity}}, in, opt, &out);
                int worst = 0;
                for (int i = 0; ran && i < w * h; ++i) {
                    const uint8_t* d = &bg.px[size_t(i) * 4];
                    const uint8_t* s = &fg.px[size_t(i) * 4];
                    const double k = opacity * (withMask ? mask.px[size_t(i)] / 255.0 : 1.0);
                    const double as = s[3] / 255.0 * k, ab = d[3] / 255.0;
                    const double ao = as + ab * (1 - as);
                    int ref[4];
                    for (int c = 0; c < 3; ++c) {
                        const double cb = d[c] / 255.0, cs = s[c] / 255.0;
                        const double cm = (1 - ab) * cs + ab * blendRef(mode, cb, cs);
                        const double co = ao > 0 ? (as * cm + (1 - as) * ab * cb) / ao : 0;
                        ref[c] = int(std::lround(std::clamp(co, 0.0, 1.0) * 255));
                    }
                    ref[3] = int(std::lround(ao * 255));
                    const uint8_t* o = &out.px[size_t(i) * 4];
                    if (ref[3] == 0) { // fully transparent: color does not matter
                        worst = std::max(worst, int(o[3]));
                        continue;
                    }
                    // a nearly transparent result has color only as exact as its alpha allows
                    const int tol = ref[3] < 64 ? 4 : 0;
                    for (int c = 0; c < 4; ++c) worst = std::max(worst, std::max(0, std::abs(int(o[c]) - ref[c]) - tol));
                }
                const QString name = QString("merge %1 opacity %2%3").arg(mode).arg(opacity).arg(withMask ? " mask" : "");
                Check::report(ran && worst <= 2, qPrintable(name), __FILE__, __LINE__,
                              QString("max deviation %1").arg(worst));
            }
    // The mode given as its number (a keyframe-able index) is the same mode as by name
    Buf byName, byNumber;
    runNode("merge", {{"mode", "screen"}}, {{"bg", bg}, {"fg", fg}}, opt, &byName);
    runNode("merge", {{"mode", 2}}, {{"bg", bg}, {"fg", fg}}, opt, &byNumber);
    CHECK(byName.px == byNumber.px);
    // Without fg: bg; without anything: nothing
    Buf only;
    CHECK(runNode("merge", {}, {{"bg", bg}}, opt, &only) && only.px == bg.px);
    CHECK(!runNode("merge", {}, {}, opt, &only));
}

// ---- generators, transform, crop on known pixels ----

void testSimpleNodes()
{
    std::printf("generators / transform / crop\n");
    RunOptions opt;
    opt.size = QSize(40, 20);
    Buf c;
    CHECK(runNode("color", {{"color", QColor(10, 20, 30, 200)}}, {}, opt, &c));
    CHECK(c.px.size() == 40u * 20 * 4 && c.px[0] == 10 && c.px[1] == 20 && c.px[2] == 30 && c.px[3] == 200);
    // Default color node: black, opaque (nothing surprising)
    CHECK(runNode("color", {}, {}, opt, &c) && c.px[0] == 0 && c.px[3] == 255);
    // Linear gradient top (black) to bottom (white)
    Buf g;
    CHECK(runNode("gradient", {}, {}, opt, &g));
    const auto at = [](const Buf& b, int x, int y, int ch) { return int(b.px[(size_t(y) * b.w + x) * 4 + ch]); };
    CHECK(at(g, 5, 0, 0) < 12 && at(g, 5, 19, 0) > 243 && std::abs(at(g, 5, 10, 0) - 134) <= 3);
    // Radial: centre = c0, far = c1
    CHECK(runNode("gradient", {{"shape", "radial"}, {"x0", 0.5}, {"y0", 0.5}, {"x1", 1.0}, {"y1", 0.5},
                               {"c0", QColor(255, 0, 0)}, {"c1", QColor(0, 0, 255)}},
                  {}, opt, &g));
    CHECK(at(g, 20, 10, 0) > 240 && at(g, 20, 10, 2) < 15 && at(g, 0, 0, 2) > 240);

    // Transform: default = unchanged; half size around the centre keeps the centre, the corners empty
    Buf src = solid(40, 20, {QColor(255, 0, 0)});
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) src.px[(size_t(y) * 40 + x) * 4 + 1] = 255; // left half yellow
    Buf t;
    CHECK(runNode("transform", {}, {{"src", src}}, opt, &t) && t.px == src.px);
    CHECK(runNode("transform", {{"size", 0.5}}, {{"src", src}}, opt, &t));
    CHECK(at(t, 0, 0, 3) == 0 && at(t, 39, 19, 3) == 0 && at(t, 20, 10, 3) == 255);
    CHECK(at(t, 12, 10, 1) == 255 && at(t, 28, 10, 1) == 0); // left still left
    // Moved right by a quarter: the left edge of the picture now at x = 10
    CHECK(runNode("transform", {{"x", 0.75}}, {{"src", src}}, opt, &t));
    CHECK(at(t, 9, 10, 3) == 0 && at(t, 11, 10, 3) == 255 && at(t, 11, 10, 1) == 255);
    // Turned 180°: the yellow half on the right
    CHECK(runNode("transform", {{"angle", 180.0}}, {{"src", src}}, opt, &t));
    CHECK(at(t, 35, 10, 1) == 255 && at(t, 5, 10, 1) == 0);

    // Crop: left quarter and bottom half gone (transparent)
    Buf k;
    CHECK(runNode("crop", {{"left", 0.25}, {"bottom", 0.5}}, {{"src", src}}, opt, &k));
    CHECK(at(k, 5, 5, 3) == 0 && at(k, 15, 5, 3) == 255 && at(k, 15, 15, 3) == 0);
    // Soft: alpha rises inside the line
    CHECK(runNode("crop", {{"left", 0.25}, {"soft", 6.0}}, {{"src", src}}, opt, &k));
    CHECK(at(k, 11, 5, 3) > 0 && at(k, 11, 5, 3) < at(k, 14, 5, 3) && at(k, 30, 5, 3) == 255);

    // Blur: radius 0 = unchanged, with a mask only inside it
    Buf b;
    CHECK(runNode("blur", {{"radius", 0.0}}, {{"src", src}}, opt, &b) && b.px == src.px);
    Buf half(Buf::Mask, 40, 20);
    for (int y = 0; y < 20; ++y) std::fill_n(&half.px[size_t(y) * 40 + 20], 20, uint8_t(255)); // right half
    CHECK(runNode("blur", {{"radius", 4.0}}, {{"src", src}, {"mask", half}}, opt, &b));
    CHECK(at(b, 18, 10, 1) == 255 && at(b, 21, 10, 1) > 0 && at(b, 21, 10, 1) < 255);

    // Grade: neutral = unchanged; saturation 0 = gray
    Buf gr;
    CHECK(runNode("grade", {}, {{"src", src}}, opt, &gr) && gr.px == src.px);
    CHECK(runNode("grade", {{"saturation", 0.0}}, {{"src", src}}, opt, &gr));
    CHECK(std::abs(at(gr, 30, 5, 0) - at(gr, 30, 5, 1)) <= 2 && std::abs(at(gr, 30, 5, 1) - at(gr, 30, 5, 2)) <= 2);

    // Media: through the engine service, offset and speed in clip frames
    double asked = -1;
    opt.t = 10;
    opt.mediaPicture = [&](const QString& path, double clipTime) {
        asked = clipTime;
        return path == "x.png" ? solid(40, 20, {QColor(1, 2, 3)}) : Buf();
    };
    Buf m;
    CHECK(runNode("media", {{"path", "x.png"}, {"offset", 5}, {"speed", 2.0}}, {}, opt, &m) && m.px[2] == 3);
    CHECK(asked == 25.0);
    CHECK(!runNode("media", {}, {}, opt, &m)); // no file: nothing
}

// ---- engine: linear graph == the effect chain, transform, transition, varying effects ----

void testLinearEqualsChain()
{
    std::printf("linear graph = effect chain\n");
    const QVariantMap glow{{"0", 0.4}, {"strength", 0.8}, {"threshold", 0.2}};
    Clip a = media(1, g_pattern, 0, 0, 49);
    a.effects << effect("frei0r.glow", glow);
    Clip b = media(1, g_pattern, 0, 0, 49);
    b.effects << fxGraph("frei0r.glow", glow);
    const QImage plain = render(oneTrack({media(1, g_pattern, 0, 0, 49)}), 7);
    const QImage chain = render(oneTrack({a}), 7);
    CHECK(maxDiff(plain, chain) > 20); // the effect does something (else the check below proves nothing)
    Check::report(maxDiff(chain, render(oneTrack({b}), 7)) == 0, "glow: graph == effect list", __FILE__, __LINE__,
                  QString("max %1").arg(maxDiff(chain, render(oneTrack({b}), 7))));

    // Noise changes per frame: the node sees the same material frame
    Clip na = media(1, g_pattern, 0, 3, 49), nb = na;
    na.effects << effect("frei0r.rgbnoise", {{"0", 0.5}});
    nb.effects << fxGraph("frei0r.rgbnoise", {{"0", 0.5}});
    for (int f : {2, 9})
        Check::report(maxDiff(render(oneTrack({na}), f), render(oneTrack({nb}), f)) == 0, "noise: graph == list",
                      __FILE__, __LINE__, QString("frame %1").arg(f));

    // With the clip's transform: the graph works before it, like any effect
    a.transform.zoomX = a.transform.zoomY = 0.6;
    a.transform.rotation = 20;
    a.transform.opacity = 80;
    b.transform = a.transform;
    Check::report(maxDiff(render(oneTrack({a}), 7), render(oneTrack({b}), 7)) == 0, "glow + transform", __FILE__,
                  __LINE__);

    // In a dissolve (both sides of the transition)
    Clip la = media(1, g_pattern, 0, 0, 24), lb = la;
    Clip ra = media(2, g_pattern2, 25, 10, 40), rb = ra;
    la.transOut = ra.transIn = lb.transOut = rb.transIn = 10;
    la.effects << effect("frei0r.glow", glow);
    ra.effects << effect("frei0r.glow", glow);
    lb.effects << fxGraph("frei0r.glow", glow);
    rb.effects << fxGraph("frei0r.glow", glow);
    for (int f : {22, 25, 28})
        Check::report(maxDiff(render(oneTrack({la, ra}), f), render(oneTrack({lb, rb}), f)) == 0, "glow in dissolve",
                      __FILE__, __LINE__, QString("frame %1").arg(f));

    // A disabled comp is no comp; an empty graph (out 0) leaves the picture
    Clip off = b;
    off.transform = {};
    off.effects[0].enabled = false;
    CHECK(maxDiff(render(oneTrack({off}), 7), plain) == 0);
    Clip empty = media(1, g_pattern, 0, 0, 49);
    empty.effects << comp({}, {}, 0);
    CHECK(maxDiff(render(oneTrack({empty}), 7), plain) == 0);

    // Render cache: a file the graph reads, replaced under the same name, makes the cached render stale
    {
        QTemporaryDir dir;
        const QString bgFile = dir.filePath("bg.png");
        QImage(8, 8, QImage::Format_RGB32).save(bgFile);
        Clip rc = media(1, g_pattern, 0, 0, 49);
        rc.effects << comp({"clip", "media", "merge"}, {{2, 3, "bg"}, {1, 3, "fg"}}, 3, {{"n2.path", bgFile}});
        const QString k1 = RenderCache::key(rc, kFormat);
        QImage img(16, 16, QImage::Format_RGB32);
        img.fill(Qt::red);
        img.save(bgFile); // other size
        CHECK(RenderCache::key(rc, kFormat) != k1);
    }

    // A recipe: clip made smaller over a media background, blurred where a gradient says so
    Clip r = media(1, g_pattern, 0, 0, 49);
    r.effects << comp({"clip", "transform", "media", "merge", "blur"},
                      {{1, 2, "src"}, {3, 4, "bg"}, {2, 4, "fg"}, {4, 5, "src"}}, 5,
                      {{"n2.size", 0.5}, {"n3.path", g_still}, {"n5.radius", 3.0}});
    const QImage recipe = render(oneTrack({r}), 7);
    // Corner: the still's (blurred) picture; centre: the clip's
    const QImage still = render(oneTrack({media(1, g_still, 0, 0, 49)}), 7);
    CHECK(!recipe.isNull() && maxDiff(recipe.copy(0, 0, 40, 20), still.copy(0, 0, 40, 20)) < 40);
    CHECK(maxDiff(recipe.copy(150, 80, 20, 20), still.copy(150, 80, 20, 20)) > 20);
}

// ---- unknown kinds, cache, cancel ----

void testEngine()
{
    std::printf("unknown kinds / cache / cancel\n");
    const QVariantMap glow{{"0", 0.4}, {"strength", 0.8}};
    // A kind of a newer version between clip and glow: passed through
    Clip a = media(1, g_pattern, 0, 0, 49);
    a.effects << effect("frei0r.glow", glow);
    Clip b = media(1, g_pattern, 0, 0, 49);
    b.effects << comp({"clip", "hologram", "fx"}, {{1, 2, "src"}, {2, 3, "src"}}, 3,
                      {{"n3.effect", "frei0r.glow"}, {"n3.e_0", 0.4}, {"n3.e_strength", 0.8}, {"n2.depth", 5}});
    CHECK(maxDiff(render(oneTrack({a}), 4), render(oneTrack({b}), 4)) == 0);
    const QVector<Picture::SkippedEffect> skipped = Picture::skippedEffects(oneTrack({b}));
    CHECK(skipped.size() == 1 && skipped.value(0).effect == "comp:hologram" && skipped.value(0).clip == 1);
    CHECK(Picture::skippedEffects(oneTrack({a})).isEmpty());
    CHECK(unknownKinds(b.effects[0]) == QStringList{"hologram"});

    // Static parts: a color and a gradient merged are evaluated once, then come from the cache
    const EffectInstance e = comp({"color", "gradient", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                                  {{"n1.color", QColor(255, 0, 0)}, {"n3.mode", "multiply"}});
    Env env;
    env.size = QSize(64, 32);
    Cache cache;
    Buf first, second;
    Stats s1, s2;
    CHECK(evaluate(e, env, &cache, &first, &s1) && s1.evaluated == 3 && s1.cached == 0);
    env.t = 12; // another frame: nothing changes with time
    CHECK(evaluate(e, env, &cache, &second, &s2) && s2.cached == 1 && s2.evaluated == 0 && first.px == second.px);
    // Another size: evaluated anew
    env.size = QSize(32, 16);
    Stats s3;
    CHECK(evaluate(e, env, &cache, &second, &s3) && s3.evaluated == 3);
    // A keyframed value makes the node and what reads it non-static
    Clip k;
    k.keys[Keys::effectParam(::Comp::kEffectId, "n3.opacity")] = {Keyframe{0, 0.0}, Keyframe{10, 1.0}};
    env.clip = &k;
    Stats s4;
    CHECK(evaluate(e, env, &cache, &second, &s4) && s4.cached == 2 && s4.evaluated == 1);
    env.clip = nullptr;
    // The clip's picture is borrowed, no copy per node; peak buffers bounded by the graph width
    Buf clipPic = solid(32, 16, {QColor(9, 9, 9)});
    env.clipPicture = [&]() -> const Buf* { return &clipPic; };
    const EffectInstance chain = comp({"clip", "blur", "blur", "blur", "blur"},
                                      {{1, 2, "src"}, {2, 3, "src"}, {3, 4, "src"}, {4, 5, "src"}}, 5);
    Stats s5;
    CHECK(evaluate(chain, env, nullptr, &second, &s5) && s5.peakBuffers <= 2);
    // Cancelled: false (the clip's picture stays)
    std::atomic_bool cancel{true};
    env.cancel = &cancel;
    CHECK(!evaluate(chain, env, nullptr, &second));
    // A cycle or a missing out node: nothing
    env.cancel = nullptr;
    CHECK(!evaluate(comp({"blur", "blur"}, {{1, 2, "src"}, {2, 1, "src"}}, 2), env, nullptr, &second));
    CHECK(!evaluate(comp({"blur"}, {}, 7), env, nullptr, &second));

    // A mask following a tracker that was not analyzed yet stays where it is drawn (the tracker reports its start
    // point as x/y, Picture::Comp::trackerValue); an analyzed tracker moves it. Shown as white through the mask on black
    const QStringList kinds{"tracker", "mask", "color", "color", "merge"};
    const QVector<CompLink> links{{3, 5, "bg"}, {4, 5, "fg"}, {2, 5, "mask"}};
    const QVariantMap shape{{"n2.x", 0.3}, {"n2.y", 0.4}, {"n2.w", 0.2}, {"n2.h", 0.2},
                            {"n3.color", QColor(0, 0, 0)}, {"n4.color", QColor(255, 255, 255)}};
    QVariantMap follow = shape;
    follow.insert("n2.follow", 1);
    follow.insert("n1.ax", 0.6);
    follow.insert("n1.ay", 0.7);
    Buf plain, followed, moved;
    CHECK(evaluate(comp(kinds, links, 5, shape), env, nullptr, &plain));
    CHECK(evaluate(comp(kinds, links, 5, follow), env, nullptr, &followed));
    CHECK(!plain.isNull() && plain.px == followed.px);
    follow.insert("n1.x", 0.8);
    follow.insert("n1.y", 0.7);
    CHECK(evaluate(comp(kinds, links, 5, follow), env, nullptr, &moved) && moved.px != plain.px);
}

// A node passing its input through (an unknown kind of a newer version, a tracker) keeps that picture alive for the
// nodes after it, without a copy
void testPassThrough()
{
    std::printf("pass-through keeps its input\n");
    Buf clipPic(Buf::Rgba, 48, 32);
    for (int i = 0; i < 48 * 32; ++i) {
        clipPic.px[size_t(i) * 4] = uint8_t((i % 48) * 5);
        clipPic.px[size_t(i) * 4 + 1] = uint8_t(i % 7 * 30);
        clipPic.px[size_t(i) * 4 + 3] = uint8_t(i % 48 < 24 ? 255 : 120);
    }
    Env env;
    env.size = QSize(48, 32);
    env.clipPicture = [&]() -> const Buf* { return &clipPic; };
    const QVariantMap v{{"n2.radius", 3}, {"n3.color", QColor(10, 200, 30)}};
    Buf direct;
    Stats sDirect;
    // clip -> blur -> merge fg over a color; the same with a pass-through node between blur and merge
    CHECK(evaluate(comp({"clip", "blur", "color", "merge"}, {{1, 2, "src"}, {3, 4, "bg"}, {2, 4, "fg"}}, 4, v), env,
                   nullptr, &direct, &sDirect));
    Buf blurOnly;
    CHECK(evaluate(comp({"clip", "blur"}, {{1, 2, "src"}}, 2, v), env, nullptr, &blurOnly));
    for (const QString& through : {QString("hologram"), QString(::Comp::kTrackerType)}) {
        Stats s1, s2;
        Buf a, b;
        const QStringList kinds{"clip", "blur", "color", "merge", through};
        CHECK(evaluate(comp(kinds, {{1, 2, "src"}, {2, 5, "src"}, {3, 4, "bg"}, {5, 4, "fg"}}, 4, v), env, nullptr, &a,
                       &s1)
              && a.px == direct.px);
        CHECK(evaluate(comp(kinds, {{1, 2, "src"}, {2, 5, "src"}}, 5, v), env, nullptr, &b, &s2) && b.px == blurOnly.px);
        CHECK(s1.peakBuffers == sDirect.peakBuffers && s2.peakBuffers == 1); // no copy
        // Read by two nodes after it: both see the picture
        const QVariantMap two{{"n2.radius", 3}, {"n4.radius", 1}};
        Buf c, ref;
        CHECK(evaluate(comp({"clip", "blur", "merge", "blur"}, {{1, 2, "src"}, {2, 3, "bg"}, {2, 4, "src"}, {4, 3, "fg"}},
                            3, two),
                       env, nullptr, &ref));
        CHECK(evaluate(comp({"clip", "blur", "merge", "blur", through},
                            {{1, 2, "src"}, {2, 5, "src"}, {5, 3, "bg"}, {5, 4, "src"}, {4, 3, "fg"}}, 3, two),
                       env, nullptr, &c)
              && c.px == ref.px);
    }
}

// A compound track is painted straight onto the merge reading it only when that merge is its one reader
void testFusedTrack()
{
    std::printf("compound track fused into a merge\n");
    Env env;
    env.size = QSize(32, 16);
    Buf whole = solid(32, 16, {QColor(1, 2, 3)});
    env.clipPicture = [&]() -> const Buf* { return &whole; };
    int painted = 0;
    env.paintTrack = [&](int track, Buf& onto) {
        ++painted;
        for (size_t i = 0; i < onto.px.size(); i += 4)
            if ((i / 4) % 3 == size_t(track)) onto.px[i] = 250, onto.px[i + 3] = 255;
        return true;
    };
    const QVariantMap v{{"n1.track", 0}, {"n2.color", QColor(0, 0, 90)}, {"n4.radius", 1}};
    // track -> fg of a merge over a color: fused (painted once, onto the color)
    Buf fused;
    CHECK(evaluate(comp({"clip", "color", "merge"}, {{2, 3, "bg"}, {1, 3, "fg"}}, 3, v), env, nullptr, &fused)
          && painted == 1 && fused.px[2] == 90 && fused.px[0] == 250);
    // The same track also read by a blur: a picture of the track alone that both read
    painted = 0;
    Buf shared;
    CHECK(evaluate(comp({"clip", "color", "merge", "blur", "merge"},
                        {{2, 3, "bg"}, {1, 3, "fg"}, {1, 4, "src"}, {3, 5, "bg"}, {4, 5, "fg"}}, 5, v),
                   env, nullptr, &shared)
          && painted == 1 && !shared.isNull());
}

// Scratch pictures of a node go back to the pool: after an evaluation only the result is out of it
void testScratchGivenBack()
{
    std::printf("scratch buffers back to the pool\n");
    Buf clipPic = solid(40, 24, {QColor(200, 30, 30, 255), QColor(20, 200, 20, 0)});
    Env env;
    env.size = QSize(40, 24);
    env.clipPicture = [&]() -> const Buf* { return &clipPic; };
    int taken = 0, given = 0;
    env.takeBytes = [&](size_t n) {
        ++taken;
        return std::vector<uint8_t>(n, 0);
    };
    env.giveBytes = [&](std::vector<uint8_t>&&) { ++given; };
    Buf out;
    CHECK(evaluate(comp({"clip", "matte_tune"}, {{1, 2, "src"}}, 2, {{"n2.soft", 2}, {"n2.black", 0.1}, {"n2.grow", 1}}),
                   env, nullptr, &out));
    CHECK(taken >= 3 && taken - given == 1);
}

// fx changes by itself only with an effect drawing a new pattern each frame (noise): the others are cached
void testFxVarying()
{
    std::printf("fx varying per effect\n");
    Env env;
    env.size = QSize(40, 24);
    Cache cache;
    for (const QString& fx : {QString("frei0r.glow"), QString("frei0r.rgbnoise"), QString("1")}) {
        cache.clear();
        const EffectInstance e = comp({"color", "fx"}, {{1, 2, "src"}}, 2,
                                      {{"n1.color", QColor(120, 90, 60)}, {"n2.effect", fx}, {"n2.e_0", 0.5}});
        Buf a, b;
        Stats s1, s2;
        env.t = 0, env.frameNo = 0;
        CHECK(evaluate(e, env, &cache, &a, &s1) && s1.evaluated == 2);
        env.t = 1, env.frameNo = 1;
        CHECK(evaluate(e, env, &cache, &b, &s2));
        if (fx == "frei0r.rgbnoise") CHECK(s2.cached == 1 && s2.evaluated == 1 && a.px != b.px);
        else CHECK(s2.cached == 1 && s2.evaluated == 0 && a.px == b.px); // "1" = the index of glow
    }
}

// A node following a tracker (numOf) is cached only while the tracker's values are fixed, and keyed by them: an
// analysed tracker (keyframes) moves it per frame, a new analysis or another value is seen with a cache
void testFollowCached()
{
    std::printf("follower with a cache\n");
    for (const QString& follower : {QString("transform"), QString("mask")}) {
        // A white square (transform of a smaller white picture / a mask shape) on black
        const bool isMask = follower == "mask";
        const QStringList kinds = isMask ? QStringList{"tracker", "color", "mask", "color", "merge"}
                                         : QStringList{"tracker", "color", "transform", "color", "merge"};
        const QVector<CompLink> links = isMask ? QVector<CompLink>{{4, 5, "bg"}, {2, 5, "fg"}, {3, 5, "mask"}}
                                               : QVector<CompLink>{{2, 3, "src"}, {4, 5, "bg"}, {3, 5, "fg"}};
        QVariantMap v{{"n2.color", QColor(255, 255, 255)}, {"n4.color", QColor(0, 0, 0)}, {"n3.follow", 1}};
        if (isMask) v.insert("n3.w", 0.2), v.insert("n3.h", 0.2);
        else v.insert("n3.size", 0.2);
        Env env;
        env.size = QSize(64, 32);
        Cache cache;
        Clip k;
        const AnimParam kx = Keys::effectParam(::Comp::kEffectId, "n1.x");
        k.keys[kx] = {Keyframe{0, 0.3}, Keyframe{10, 0.7}};
        env.clip = &k;
        Buf f0, f10, again;
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &f0));
        env.t = 10;
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &f10) && f10.px != f0.px);
        // Analysed anew: other keyframes, the same frame shows the new place
        k.keys[kx] = {Keyframe{0, 0.3}, Keyframe{10, 0.4}};
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &again) && again.px != f10.px);
        // No keyframes: static (cached at another frame), but another tracker value is not taken from the cache
        env.clip = nullptr;
        v.insert("n1.x", 0.3);
        Buf a, b, c;
        Stats s1, s2;
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &a));
        env.t = 3;
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &b, &s1) && s1.cached >= 1 && a.px == b.px);
        v.insert("n1.x", 0.7);
        CHECK(evaluate(comp(kinds, links, 5, v), env, &cache, &c, &s2) && c.px != a.px);
    }
}

// Choice values given as an index (a number or digits) mean the same as the choice id, for every kind
void testChoiceValues()
{
    std::printf("choice values by id or index\n");
    RunOptions o;
    o.size = QSize(48, 32);
    auto same = [&](const QString& type, const QVariantMap& byId, const QVariantMap& byIndex,
                    const QHash<QString, Buf>& inputs = {}) {
        Buf a, b, d;
        const bool ok = runNode(type, byId, inputs, o, &a) && runNode(type, byIndex, inputs, o, &b);
        runNode(type, {}, inputs, o, &d); // the default must differ, or the check says nothing
        return ok && a.px == b.px && a.px != d.px;
    };
    const QVariantMap rect{{"x", 0.5}, {"y", 0.5}, {"w", 0.5}, {"h", 0.6}};
    QVariantMap ellipse = rect, ellipseN = rect, ellipseS = rect;
    ellipse["shape"] = "ellipse";
    ellipseN["shape"] = 1;
    ellipseS["shape"] = "1";
    CHECK(same("mask", ellipse, ellipseN));
    CHECK(same("mask", ellipse, ellipseS));
    CHECK(same("gradient", {{"shape", "radial"}}, {{"shape", 1}}));
    Buf half(Buf::Rgba, 48, 32);
    for (size_t i = 0; i < half.px.size(); i += 4) half.px[i] = 200, half.px[i + 3] = 90;
    CHECK(same("matte", {{"channel", "alpha"}}, {{"channel", 1}}, {{"src", half}}));
    // Out of range or unknown: the default
    Buf a, d;
    CHECK(runNode("gradient", {{"shape", 7}}, {}, o, &a) && runNode("gradient", {}, {}, o, &d) && a.px == d.px);
    const NodeType* m = find("mask");
    CHECK(m && choiceValue(*m, {{"shape", "radial"}}, "shape") == 5 && choiceValue(*m, {{"shape", 2.0}}, "shape") == 2
          && choiceValue(*m, {{"shape", -1}}, "shape") == 0 && textValue(*m, {{"shape", 3}}, "shape") == "bezier");
}

// ---- comp from selection ----

MediaInfo info(const QString& path)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = 50;
    m.hasVideo = true;
    return m;
}

void testCompFrom()
{
    std::printf("comp from selection\n");
    ProjectData data;
    data.format = kFormat;
    data.media << info(g_pattern) << info(g_pattern2) << info(g_still);
    data.timeline.video.resize(4);
    data.timeline.audio.resize(1);
    data.timeline.video[0].clips << media(1, g_pattern, 0, 0, 39);
    Clip top = media(2, g_pattern2, 5, 0, 29);
    top.transform.zoomX = top.transform.zoomY = 0.5;
    top.transform.posX = 40;
    top.transform.opacity = 70;
    data.timeline.video[1].clips << top;
    Clip logo = media(3, g_still, 0, 0, 19);
    logo.transform.zoomX = logo.transform.zoomY = 0.3;
    logo.transform.rotation = 15;
    data.timeline.video[3].clips << logo; // a gap track (2) in between
    Clip outside = media(4, g_pattern2, 60, 0, 9);
    data.timeline.video[0].clips << outside;

    Project p;
    p.load(data);
    const Timeline loaded = p.timeline();
    Selection sel;
    Editor ed(&p, &sel);
    const QVector<int> frames = {0, 6, 12, 25, 35};
    QVector<QImage> before;
    for (int f : frames) before << render(p.renderTimeline(), f);

    // Sound alone: nothing
    sel.set({});
    CHECK(ed.compFrom() == 0);
    const int steps = p.undoStack()->count();
    sel.set({1, 2, 3});
    const int seq = ed.compFrom();
    CHECK(seq > 0);
    CHECK(p.undoStack()->count() == steps + 1);
    const Timeline& tl = p.timeline();
    const Clip* c = nullptr;
    for (const Track& t : tl.video)
        for (const Clip& x : t.clips)
            if (x.isNested()) c = &x;
    if (!CHECK(c && c->sequenceId == seq)) return;
    const EffectInstance* e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    CompGraph g;
    CHECK(e && ::Comp::graphOf(*e, &g));
    QString err;
    CHECK(g.validate(kindInfo, &err));
    // three tracks with clips -> three clip nodes, two merges
    CHECK(std::count_if(g.nodes.begin(), g.nodes.end(), [](const CompNode& n) { return n.type == "clip"; }) == 3);
    CHECK(std::count_if(g.nodes.begin(), g.nodes.end(), [](const CompNode& n) { return n.type == "merge"; }) == 2);
    for (int i = 0; i < frames.size(); ++i)
        Check::report(maxDiff(before[i], render(p.renderTimeline(), frames[i])) == 0, "comp_from == before",
                      __FILE__, __LINE__, QString("frame %1 max %2").arg(frames[i])
                                              .arg(maxDiff(before[i], render(p.renderTimeline(), frames[i]))));
    // Same as a plain compound clip of the same clips
    {
        Project q;
        q.load(data);
        Selection s2;
        Editor e2(&q, &s2);
        s2.set({1, 2, 3});
        CHECK(e2.createCompoundClip() > 0);
        CHECK(maxDiff(render(q.renderTimeline(), 12), render(p.renderTimeline(), 12)) == 0);
    }
    // Rebuilt: the top track alone (clip track 2) is what the graph shows now
    {
        CompGraph alone;
        alone.nodes << CompNode{1, "clip", {}};
        alone.out = 1;
        EffectInstance x = *e;
        ::Comp::setGraph(x, alone);
        x.params = {{::Comp::kGraphKey, alone.toVariant()}, {"n1.track", 3}};
        Timeline t2 = p.renderTimeline();
        for (Track& t : t2.video)
            for (Clip& y : t.clips)
                if (y.isNested()) y.effects = {x};
        Timeline only = oneTrack({logo});
        // drawn onto a transparent picture first, then onto black: the rotated edge rounds once more
        const int d = maxDiff(render(t2, 6), render(only, 6));
        Check::report(d <= 2, "graph rebuilt: one inner track", __FILE__, __LINE__, QString("max %1").arg(d));
    }

    // Project file round trip keeps the graph; undo is one step back to the picture before
    QTemporaryDir tmp;
    const QString path = tmp.filePath("p.schneidi");
    CHECK(ProjectFile::save(p.data(), path, &err));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    Project r;
    r.load(back);
    CHECK(maxDiff(render(r.renderTimeline(), 12), before[2]) == 0);
    p.undoStack()->undo();
    CHECK(p.timeline() == loaded);
    CHECK(maxDiff(render(p.renderTimeline(), 12), before[2]) == 0);
}

// ---- measurement ----

void bench()
{
    if (qEnvironmentVariable("SCHNEIDI_COMP_BENCH").isEmpty()) return;
    std::printf("bench 1920x1080 (graph alone, no decoding)\n");
    const int W = 1920, H = 1080;
    Buf clipPic(Buf::Rgba, W, H), bg(Buf::Rgba, W, H);
    for (size_t i = 0; i < clipPic.px.size(); ++i) {
        clipPic.px[i] = uint8_t(i * 7);
        bg.px[i] = uint8_t(i * 13 + 5);
    }
    for (size_t i = 3; i < bg.px.size(); i += 4) bg.px[i] = clipPic.px[i] = 255;
    const EffectInstance e = comp({"clip", "transform", "media", "merge", "blur"},
                                  {{1, 2, "src"}, {3, 4, "bg"}, {2, 4, "fg"}, {4, 5, "src"}}, 5,
                                  {{"n2.size", 0.6}, {"n2.angle", 5.0}, {"n3.path", "bg"}, {"n5.radius", 4.0}});
    Env env;
    env.size = QSize(W, H);
    env.clipPicture = [&]() -> const Buf* { return &clipPic; };
    env.mediaPicture = [&](const QString&, double) { return bg; };
    // a pool like the renderer's (fresh 8 MB allocations cost page faults the app does not pay)
    std::mutex poolMutex;
    std::vector<std::vector<uint8_t>> pool;
    env.takeBytes = [&](size_t n) {
        std::lock_guard lock(poolMutex);
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].size() == n) {
                std::vector<uint8_t> v = std::move(pool[i]);
                pool.erase(pool.begin() + qsizetype(i));
                std::memset(v.data(), 0, n);
                return v;
            }
        return std::vector<uint8_t>(n, 0);
    };
    env.giveBytes = [&](std::vector<uint8_t>&& v) {
        std::lock_guard lock(poolMutex);
        if (pool.size() < 8) pool.push_back(std::move(v));
    };
    for (const char* name : {"transform", "merge", "blur", "grade", "recipe"}) {
        EffectInstance x = e;
        if (QString(name) == "transform") x = comp({"clip", "transform"}, {{1, 2, "src"}}, 2, {{"n2.size", 0.6}, {"n2.angle", 5.0}});
        if (QString(name) == "merge")
            x = comp({"clip", "media", "merge"}, {{2, 3, "bg"}, {1, 3, "fg"}}, 3, {{"n2.path", "bg"}, {"n3.mode", "screen"}});
        if (QString(name) == "blur") x = comp({"clip", "blur"}, {{1, 2, "src"}}, 2, {{"n2.radius", 4.0}});
        if (QString(name) == "grade")
            x = comp({"clip", "grade"}, {{1, 2, "src"}}, 2, {{"n2.contrast", 1.2}, {"n2.saturation", 60.0}});
        Buf out;
        evaluate(x, env, nullptr, &out);
        QElapsedTimer t;
        t.start();
        const int n = 10;
        for (int i = 0; i < n; ++i) evaluate(x, env, nullptr, &out);
        std::printf("  %-10s %.1f ms/frame\n", name, t.nsecsElapsed() / 1e6 / n);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp_eval");
    std::setlocale(LC_NUMERIC, "C");
    testMergeModes();
    testSimpleNodes();
    testEngine();
    testChoiceValues();
    testFollowCached();
    testPassThrough();
    testFusedTrack();
    testScratchGivenBack();
    testFxVarying();
    bench();
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_pattern = Check::makeMedia(tmp.filePath("pattern.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2",
                                                               "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                               "yuv420p"});
    g_pattern2 = Check::makeMedia(tmp.filePath("pattern2.mp4"), {"-f", "lavfi", "-i", "smptebars=s=320x180:r=25:d=2",
                                                                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                                                 "yuv420p"});
    {
        QImage s(320, 180, QImage::Format_RGBA8888);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) s.setPixel(x, y, qRgba(x * 255 / 319, 40, y * 255 / 179, 255));
        g_still = tmp.filePath("still.png");
        s.save(g_still);
    }
    if (!CHECK(!g_pattern.isEmpty() && !g_pattern2.isEmpty())) return Check::result();
    Check::initEngine();
    testLinearEqualsChain();
    testCompFrom();
    return Check::result();
}
