// SPDX-License-Identifier: AGPL-3.0-only
// The shared camera of a compositing graph (0.22 block 5, engine/picture/CompCamera): 2.5D parallax.
// Synthetic pictures only, results known:
// - the map: at rest every depth is unchanged; a pan by p pixels moves a layer at z = 1 by p and at z = 2 by p/2;
//   push grows near layers more (z / (z - push)); zoom alike for all; roll turns around the centre; a layer behind the
//   camera is not drawn
// - transform, media, text, text3d with z/camera: a marker moves by the formula; z 0, camera 0, a camera at rest or a
//   camera id that is no camera give a picture bit-identical to the same graph without those values
// - camera following a tracker: a layer at z = 1 lands where a transform following the same tracker lands; z = 2 moves
//   half as far
// - drift: the camera moves with the time (not cached as static), a static camera is cached; a head trim keeps the
//   picture (Comp::followHead moves pan/push by the drift)
// - tables: z/camera on exactly the kinds of Comp::takesCamera, no pixel keys (4K change: the motion scales with the
//   picture), the camera has no time range; check comp_camera_missing; unused counts cameras read through `camera`
// - CLI: node add camera, params z/camera, info (layers, hint), effects kind:'nodes' note, help, recipe parallax_photo
//   (frames differ over time, the photo grows)
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompRecipes.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompCamera.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"

#include "render_check.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace Picture::Comp;

namespace {

constexpr int W = 320, H = 180;

// A white 8 x 8 square centred on (cx, cy) on transparent
Buf marker(int w, int h, double cx, double cy, int size = 8)
{
    Buf b(Buf::Rgba, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (std::abs(x + 0.5 - cx) < size / 2.0 && std::abs(y + 0.5 - cy) < size / 2.0) {
                uint8_t* p = b.data() + (size_t(y) * w + x) * 4;
                p[0] = p[1] = p[2] = p[3] = 255;
            }
    return b;
}

// Alpha-weighted centre of a picture (-1, -1 = empty)
QPointF centroid(const Buf& b)
{
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const double a = b.px[(size_t(y) * b.w + x) * 4 + 3];
            sx += a * (x + 0.5), sy += a * (y + 0.5), sw += a;
        }
    return sw > 0 ? QPointF(sx / sw, sy / sw) : QPointF(-1, -1);
}

double alphaSum(const Buf& b)
{
    double s = 0;
    for (size_t i = 3; i < b.px.size(); i += 4) s += b.px[i];
    return s / 255.0;
}

bool near(QPointF a, QPointF b, double tol) { return std::hypot(a.x() - b.x(), a.y() - b.y()) <= tol; }

void print(const char* what, QPointF got, QPointF want)
{
    std::printf("       %s: got (%.3f, %.3f), want (%.3f, %.3f)\n", what, got.x(), got.y(), want.x(), want.y());
}

EffectInstance graph(const QVector<CompNode>& nodes, const QVector<CompLink>& links, int out, const QVariantMap& values)
{
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    g.nodes = nodes;
    g.links = links;
    g.out = out;
    ::Comp::setGraph(e, g);
    for (auto it = values.cbegin(); it != values.cend(); ++it) e.params.insert(it.key(), it.value());
    return e;
}

Buf g_clipPic;
Buf g_mediaPic;

Buf evalAt(const EffectInstance& e, double t = 0, QSize size = QSize(W, H), Cache* cache = nullptr, Stats* stats = nullptr,
           const Clip* clip = nullptr)
{
    Env env;
    env.size = size;
    env.t = t;
    env.fps = 25;
    env.clip = clip;
    env.clipPicture = [] { return &g_clipPic; };
    env.mediaPicture = [](const QString&, double) { return g_mediaPic; };
    Buf out;
    if (!evaluate(e, env, cache, &out, stats)) return {};
    return out;
}

bool same(const Buf& a, const Buf& b)
{
    return !a.isNull() && a.w == b.w && a.h == b.h && a.px.size() == b.px.size()
           && std::memcmp(a.data(), b.data(), a.px.size()) == 0;
}

// ---- The map ----
void testMap()
{
    std::printf("map\n");
    const QSize size(W, H);
    const auto at = [&](const Camera& c, double z, QPointF p) {
        Compose::Homography h;
        if (!cameraMap(c, z, size, &h)) return QPointF(-1e9, -1e9);
        double X = 0, Y = 0;
        Compose::mapPoint(h, p.x(), p.y(), &X, &Y);
        return QPointF(X, Y);
    };
    const QPointF centre(W / 2.0, H / 2.0), off(W / 2.0 + 40, H / 2.0 + 20);
    Camera rest;
    for (double z : {0.25, 0.5, 1.0, 2.0, 7.0}) {
        Compose::Homography h;
        CHECK(cameraMap(rest, z, size, &h) && isIdentity(h));
    }
    Camera pan;
    pan.panX = 30; // pixels at the picture plane
    pan.panY = -12;
    CHECK(near(at(pan, 1, off), off - QPointF(30, -12), 1e-6));
    CHECK(near(at(pan, 2, off), off - QPointF(15, -6), 1e-6));
    CHECK(near(at(pan, 0.5, off), off - QPointF(60, -24), 1e-6));
    Camera push;
    push.push = 0.5;
    CHECK(near(at(push, 1, off), centre + (off - centre) * 2.0, 1e-6));
    CHECK(near(at(push, 2, off), centre + (off - centre) * (2.0 / 1.5), 1e-6));
    Camera zoom;
    zoom.zoom = 1.5;
    CHECK(near(at(zoom, 1, off), centre + (off - centre) * 1.5, 1e-6));
    CHECK(near(at(zoom, 3, off), centre + (off - centre) * 1.5, 1e-6));
    Camera roll; // camera clockwise: the picture turns counter-clockwise
    roll.roll = 90;
    CHECK(near(at(roll, 1, centre + QPointF(10, 0)), centre + QPointF(0, -10), 1e-6));
    CHECK(near(at(roll, 4, centre + QPointF(10, 0)), centre + QPointF(0, -10), 1e-6)); // a turn has no parallax
    Camera turn; // turning right: what is straight ahead moves left, the same for every depth
    turn.tiltY = 10;
    const QPointF a1 = at(turn, 1, centre), a3 = at(turn, 3, centre);
    CHECK(a1.x() < centre.x() - 10 && near(a1, a3, 1e-6));
    Camera up;
    up.tiltX = 10;
    CHECK(at(up, 1, centre).y() > centre.y() + 10);
    Camera through; // the camera passed the layer
    through.push = 1.2;
    Compose::Homography h;
    CHECK(!cameraMap(through, 1, size, &h));
    CHECK(cameraMap(through, 2, size, &h));
}

// ---- Layers ----
// Nodes: 1 clip, 2 transform of it, 3 camera; values for the transform and the camera
EffectInstance transformGraph(const QVariantMap& values)
{
    return graph({{1, "clip", {}}, {2, "transform", {}}, {3, "camera", {}}}, {{1, 2, "src"}}, 2, values);
}

void testLayers()
{
    std::printf("layers\n");
    g_clipPic = marker(W, H, 200, 60);
    g_mediaPic = marker(W, H, 100, 120);
    const QVariantMap place{{"n2.size", 0.9}, {"n2.angle", 7.0}};
    const auto with = [](QVariantMap a, const QVariantMap& b) {
        for (auto it = b.cbegin(); it != b.cend(); ++it) a.insert(it.key(), it.value());
        return a;
    };
    const Buf plain = evalAt(transformGraph(place));
    const QPointF c0 = centroid(plain);
    // Bit-identical without the space: z 0, no camera in the graph, a camera at rest, a camera id that is no camera
    const QVariantMap panned{{"n3.pan_x", 20.0 / W}};
    CHECK(same(plain, evalAt(transformGraph(with(place, with(panned, {{"n2.z", 0.0}, {"n2.camera", 3}}))))));
    CHECK(same(plain, evalAt(graph({{1, "clip", {}}, {2, "transform", {}}}, {{1, 2, "src"}}, 2, with(place, {{"n2.z", 2.0}})))));
    CHECK(same(plain, evalAt(transformGraph(with(place, {{"n2.z", 2.0}, {"n2.camera", 3}})))));
    CHECK(same(plain, evalAt(transformGraph(with(place, with(panned, {{"n2.z", 2.0}, {"n2.camera", 1}}))))));
    // camera 0 = the graph's camera (one per graph): the same as naming it
    CHECK(same(evalAt(transformGraph(with(place, with(panned, {{"n2.z", 2.0}, {"n2.camera", 3}})))),
               evalAt(transformGraph(with(place, with(panned, {{"n2.z", 2.0}}))))));
    // A pan by 20 px: z 1 moves 20 px, z 2 moves 10 px (the other way)
    for (double z : {1.0, 2.0, 4.0}) {
        const Buf b = evalAt(transformGraph(with(place, with(panned, {{"n2.z", z}, {"n2.camera", 3}}))));
        const QPointF want = c0 - QPointF(20 / z, 0);
        if (!CHECK(near(centroid(b), want, 0.1))) print("transform pan", centroid(b), want);
    }
    // Tilted in depth too: the camera after the tilt (a pan moves the tilted picture as a whole)
    {
        const QVariantMap tilt = with(place, {{"n2.tilt_y", 30.0}});
        const QPointF t0 = centroid(evalAt(transformGraph(tilt)));
        const QPointF t1 = centroid(evalAt(transformGraph(with(tilt, with(panned, {{"n2.z", 1.0}, {"n2.camera", 3}})))));
        if (!CHECK(near(t1, t0 - QPointF(20, 0), 0.2))) print("tilted pan", t1, t0 - QPointF(20, 0));
    }
    // Behind the camera: nothing
    CHECK(alphaSum(evalAt(transformGraph(with(place, {{"n2.z", 1.0}, {"n2.camera", 3}, {"n3.push", 0.3}})))) > 0);
    {
        const Buf b = evalAt(graph({{1, "clip", {}}, {2, "transform", {}}, {3, "camera", {}}}, {{1, 2, "src"}}, 2,
                                   {{"n2.z", 0.5}, {"n2.camera", 3}, {"n3.push", 0.9}}));
        CHECK(!b.isNull() && alphaSum(b) == 0);
    }

    // Media: the same rule (moved after the node made its picture)
    {
        const auto media = [&](const QVariantMap& v) {
            return graph({{1, "media", {}}, {2, "camera", {}}}, {}, 1, with({{"n1.path", "/x.png"}}, v));
        };
        const Buf m0 = evalAt(media({}));
        CHECK(same(m0, evalAt(media({{"n1.z", 2.0}, {"n1.camera", 2}}))));
        CHECK(same(m0, evalAt(media({{"n1.z", 0.0}, {"n1.camera", 2}, {"n2.pan_y", 0.1}}))));
        const Buf m1 = evalAt(media({{"n1.z", 2.0}, {"n1.camera", 2}, {"n2.pan_y", 0.1}}));
        const QPointF want = centroid(m0) - QPointF(0, 0.1 * H / 2);
        if (!CHECK(near(centroid(m1), want, 0.1))) print("media pan", centroid(m1), want);
        // A push: the marker moves away from the centre by z / (z - push)
        const Buf m2 = evalAt(media({{"n1.z", 2.0}, {"n1.camera", 2}, {"n2.push", 0.5}}));
        const QPointF ctr(W / 2.0, H / 2.0);
        const QPointF want2 = ctr + (centroid(m0) - ctr) * (2.0 / 1.5);
        if (!CHECK(near(centroid(m2), want2, 0.3))) print("media push", centroid(m2), want2);
    }
    // Text and 3D text: their pictures move alike; without the space bit-identical
    for (const char* kind : {"text", "text3d"}) {
        const auto text = [&](const QVariantMap& v) {
            return graph({{1, kind, {}}, {2, "camera", {}}}, {}, 1,
                         with({{"n1.text", "II"}, {"n1.size", 40.0}, {"n1.x", 0.3}, {"n1.y", 0.4}}, v));
        };
        const Buf t0 = evalAt(text({}));
        if (!CHECK(!t0.isNull() && alphaSum(t0) > 10)) continue;
        CHECK(same(t0, evalAt(text({{"n1.z", 0.0}, {"n1.camera", 2}, {"n2.pan_x", 0.1}}))));
        CHECK(same(t0, evalAt(text({{"n1.z", 3.0}, {"n1.camera", 2}}))));
        const Buf t1 = evalAt(text({{"n1.z", 2.0}, {"n1.camera", 2}, {"n2.pan_x", 0.1}}));
        const QPointF want = centroid(t0) - QPointF(0.1 * W / 2, 0);
        if (!CHECK(near(centroid(t1), want, 0.3))) print(kind, centroid(t1), want);
        // The content area went along: the moved text is inside it
        if (!t1.area.isEmpty()) {
            double outside = 0;
            for (int y = 0; y < t1.h; ++y)
                for (int x = 0; x < t1.w; ++x)
                    if (!t1.area.contains(x, y)) outside += t1.px[(size_t(y) * t1.w + x) * 4 + 3];
            CHECK(outside == 0);
        }
    }
}

// ---- Following a tracker ----
void testFollow()
{
    std::printf("follow\n");
    g_clipPic = marker(W, H, 200, 60);
    // Tracker 3 (two points) moved by (30, -10) px from its reference, 1.25 times as big, turned 20 degrees
    const QVariantMap tracker{{"n3.mode", "two"}, {"n3.rx", 0.5}, {"n3.ry", 0.5}, {"n3.x", 0.5 + 30.0 / W},
                              {"n3.y", 0.5 - 10.0 / H}, {"n3.scale", 1.25}, {"n3.angle", 20.0}};
    QVariantMap follower = tracker;
    follower.insert("n2.follow", 3);
    const Buf byTransform = evalAt(graph({{1, "clip", {}}, {2, "transform", {}}, {3, "tracker", {}}}, {{1, 2, "src"}}, 2, follower));
    QVariantMap byCamera = tracker;
    byCamera.insert("n4.follow", 3);
    byCamera.insert("n2.z", 1.0);
    byCamera.insert("n2.camera", 4);
    const auto cameraGraph = [&](const QVariantMap& v) {
        return graph({{1, "clip", {}}, {2, "transform", {}}, {3, "tracker", {}}, {4, "camera", {}}}, {{1, 2, "src"}}, 2, v);
    };
    const Buf z1 = evalAt(cameraGraph(byCamera));
    if (!CHECK(near(centroid(z1), centroid(byTransform), 0.15))) print("camera follows", centroid(z1), centroid(byTransform));
    CHECK(std::abs(alphaSum(z1) - alphaSum(byTransform)) < 0.02 * alphaSum(byTransform));
    // A pure move: z 2 goes half as far
    QVariantMap move{{"n3.mode", "point"}, {"n3.rx", 0.5}, {"n3.ry", 0.5}, {"n3.x", 0.5 + 30.0 / W}, {"n3.y", 0.5 - 10.0 / H},
                     {"n4.follow", 3}, {"n2.z", 2.0}, {"n2.camera", 4}};
    const QPointF base = centroid(g_clipPic);
    const QPointF want = base + QPointF(15, -5);
    const QPointF got = centroid(evalAt(cameraGraph(move)));
    if (!CHECK(near(got, want, 0.1))) print("z 2 follows half", got, want);
    // cameraAt: own values on top of the tracker's (a pan by hand still works while following)
    move.insert("n4.pan_x", 0.1);
    const QPointF got2 = centroid(evalAt(cameraGraph(move)));
    if (!CHECK(near(got2, want - QPointF(0.1 * W / 2, 0), 0.1))) print("follow + own pan", got2, want - QPointF(0.1 * W / 2, 0));
}

// ---- Time: drift, cache, head trim ----
void testTime()
{
    std::printf("time\n");
    g_clipPic = marker(W, H, 160, 90);
    const EffectInstance drift = transformGraph({{"n2.z", 1.0}, {"n2.camera", 3}, {"n3.drift_x", 0.1}});
    Cache cache;
    const Buf a = evalAt(drift, 0, QSize(W, H), &cache);
    const Buf b = evalAt(drift, 25, QSize(W, H), &cache); // one second: 0.1 picture widths
    if (!CHECK(near(centroid(b), centroid(a) - QPointF(0.1 * W, 0), 0.1))) print("drift", centroid(b), centroid(a) - QPointF(0.1 * W, 0));
    // A static camera with a pan: a text layer is cached (the second frame comes from the cache), a changed pan is a
    // new key
    const auto text = [](double pan) {
        return graph({{1, "text", {}}, {2, "camera", {}}}, {}, 1,
                     {{"n1.text", "I"}, {"n1.size", 40.0}, {"n1.z", 1.0}, {"n1.camera", 2}, {"n2.pan_x", pan}});
    };
    Cache tc;
    Stats s1, s2, s3;
    const Buf t1 = evalAt(text(0.05), 0, QSize(W, H), &tc, &s1);
    const Buf t2 = evalAt(text(0.05), 10, QSize(W, H), &tc, &s2);
    const Buf t3 = evalAt(text(0.1), 10, QSize(W, H), &tc, &s3);
    CHECK_EQ(s2.cached, 1);
    CHECK(same(t1, t2));
    CHECK_EQ(s3.cached, 0);
    CHECK(near(centroid(t3), centroid(t1) - QPointF(0.05 * W, 0), 0.3));
    // A drifting camera keeps a text layer from the cache
    Cache dc;
    Stats d1, d2;
    QVariantMap dv{{"n1.text", "I"}, {"n1.size", 40.0}, {"n1.z", 1.0}, {"n1.camera", 2}, {"n2.drift_y", 0.2}};
    const EffectInstance dt = graph({{1, "text", {}}, {2, "camera", {}}}, {}, 1, dv);
    const Buf e1 = evalAt(dt, 0, QSize(W, H), &dc, &d1);
    const Buf e2 = evalAt(dt, 25, QSize(W, H), &dc, &d2);
    CHECK_EQ(d2.cached, 0);
    CHECK(near(centroid(e2), centroid(e1) - QPointF(0, 0.2 * H), 0.3));
    // Keyframes on the camera: the layer follows them (animated camera = not static)
    {
        Clip c;
        c.id = 1;
        c.in = 0;
        c.out = 100;
        Keys::setKey(c, Keys::effectParam(::Comp::kEffectId, "n2.pan_x"), 0, 0.0);
        Keys::setKey(c, Keys::effectParam(::Comp::kEffectId, "n2.pan_x"), 50, 0.2);
        QVariantMap kv = dv;
        kv.remove("n2.drift_y");
        const EffectInstance kt = graph({{1, "text", {}}, {2, "camera", {}}}, {}, 1, kv);
        Cache kc;
        const Buf k0 = evalAt(kt, 0, QSize(W, H), &kc, nullptr, &c);
        const Buf k1 = evalAt(kt, 25, QSize(W, H), &kc, nullptr, &c);
        if (!CHECK(near(centroid(k1), centroid(k0) - QPointF(0.1 * W, 0), 0.3)))
            print("keyed camera", centroid(k1), centroid(k0) - QPointF(0.1 * W, 0));
    }
    // A head trim: the camera's drift is moved into its values, the picture at a timeline frame stays
    {
        Clip c;
        c.id = 1;
        c.in = 0;
        c.out = 200;
        EffectInstance e = drift;
        e.params.insert("n3.drift_push", 0.02);
        c.effects << e;
        const Buf before = evalAt(c.effects[0], 40, QSize(W, H), nullptr, nullptr, &c);
        ::Comp::followHead(c, 25, 25);
        CHECK(std::abs(c.effects[0].params.value("n3.drift_offset").toDouble() - 1.0) < 1e-12);
        CHECK(!c.effects[0].params.contains("n3.pan_x")); // the camera's own values stay
        const Buf after = evalAt(c.effects[0], 15, QSize(W, H), nullptr, nullptr, &c);
        CHECK(near(centroid(after), centroid(before), 1e-3));
        ::Comp::followHead(c, -25, 25); // trimmed back out: the values as they were
        CHECK(std::abs(c.effects[0].params.value("n3.drift_offset").toDouble()) < 1e-12);
        CHECK(same(evalAt(c.effects[0], 40, QSize(W, H), nullptr, nullptr, &c), before));
    }
}

// ---- Long clips: the push eases off, layers stay bounded; partly behind the camera; runNode defaults ----
void testBounds()
{
    std::printf("bounds\n");
    // driftPush: linear at first, never beyond the cap
    CHECK(std::abs(driftPush(0.03, 1) - 0.03) < 0.001);
    CHECK(driftPush(0.03, 1e6) <= kDriftPushCap && driftPush(-0.03, 1e6) >= -kDriftPushCap);
    CHECK(driftPush(0.03, -2) == -0.06); // before the start (trimmed back out): straight on
    // The recipe's camera (push 0.03/s) over 60 s at 25 fps: the person (z 1) grows at most 1 / (1 - cap) and is never
    // hidden
    g_clipPic = marker(W, H, 200, 60);
    const EffectInstance e = transformGraph({{"n2.z", 1.0}, {"n2.camera", 3}, {"n3.drift_push", 0.03}, {"n2.size", 0.3}});
    double last = 0;
    bool grows = true, shown = true;
    for (int s = 0; s <= 60; s += 5) {
        const Buf b = evalAt(e, s * 25.0);
        const double a = alphaSum(b);
        shown = shown && a > 0;
        grows = grows && a >= last - 1e-6;
        last = a;
        Compose::Homography h;
        Camera cam = cameraAt([](int, const QString& k) { return k == "drift_push" ? 0.03 : k == "zoom" ? 1.0 : k == "fov" ? 40.0 : 0.0; },
                              3, QSize(W, H), s);
        CHECK(cameraMap(cam, 1, QSize(W, H), &h) && scaleAt(h, W / 2.0, H / 2.0) <= 1 / (1 - kDriftPushCap) + 1e-9);
    }
    CHECK(shown && grows);
    // A big layer moved aside, the camera pushed in and turned: corners of the placed picture behind the camera ->
    // nothing (never a mirrored ghost); in front -> drawn
    g_clipPic = marker(W, H, 160, 90, 40);
    const Buf behind = evalAt(transformGraph({{"n2.size", 6.0}, {"n2.x", 2.5}, {"n2.z", 1.0},
                                              {"n2.camera", 3}, {"n3.push", 0.6}, {"n3.tilt_y", 30.0}}));
    CHECK(!behind.isNull() && alphaSum(behind) == 0);
    {
        Camera cam;
        cam.push = 0.6;
        cam.tiltY = 30;
        Compose::Homography h;
        CHECK(cameraMap(cam, 1, QSize(W, H), &h)); // the frame itself is in front ...
        const Compose::Affine m = Compose::placement(2.5 * W - 3 * W, 0.5 * H - 3 * H, 6 * W, 6 * H, 0, W, H, true, true);
        CHECK(!inFront(Compose::multiply(h, {{m.a, m.b, m.c, m.d, m.e, m.f, 0, 0, 1}}), W, H)); // ... the layer is not
    }
    // runNode: a camera given with its kind gets its defaults (zoom 1, fov 40): a layer at rest stays as it is
    RunOptions opt;
    opt.size = QSize(W, H);
    opt.otherNodes.insert(3, {{"type", "camera"}});
    opt.graphCamera = 3;
    QHash<QString, Buf> in;
    in.insert("src", g_clipPic);
    Buf a, b;
    CHECK(runNode("transform", {}, in, opt, &a));
    CHECK(runNode("transform", {{"z", 2.0}}, in, opt, &b));
    CHECK(same(a, b));
}

// ---- The camera holds while its tracker lost the point (hold/fade: the tracker's last seen values) ----
void testLostTracker()
{
    std::printf("lost tracker\n");
    g_clipPic = marker(W, H, 160, 90);
    Clip c;
    c.id = 1;
    c.in = 0;
    c.out = 100;
    const auto key = [&](const char* k, int f, double v) { Keys::setKey(c, Keys::effectParam(::Comp::kEffectId, k), f, v); };
    for (int f = 0; f <= 40; ++f) key("n3.x", f, 0.5 + f * 2.0 / W); // 2 px a frame
    key("n3.y", 0, 0.5);
    for (auto [f, v] : {std::pair{0, 1.0}, {9, 1.0}, {10, 0.0}, {25, 0.0}, {26, 1.0}, {40, 1.0}}) key("n3.seen", f, v);
    for (const char* mode : {"hold", "fade"}) {
        const EffectInstance e = graph({{1, "clip", {}}, {2, "transform", {}}, {3, "tracker", {}}, {4, "camera", {}}},
                                       {{1, 2, "src"}}, 2,
                                       {{"n3.mode", "point"}, {"n3.lost", mode}, {"n3.rx", 0.5}, {"n3.ry", 0.5},
                                        {"n4.follow", 3}, {"n2.z", 1.0}, {"n2.camera", 4}});
        const QPointF at9 = centroid(evalAt(e, 9, QSize(W, H), nullptr, nullptr, &c));
        const Buf b20 = evalAt(e, 20, QSize(W, H), nullptr, nullptr, &c);
        if (!CHECK(near(centroid(b20), at9, 0.05))) print(mode, centroid(b20), at9);
        CHECK(alphaSum(b20) > 0.9 * alphaSum(g_clipPic)); // the camera does not fade the layers
        CHECK(near(at9, centroid(g_clipPic) + QPointF(18, 0), 0.1));
    }
}

// ---- Tables, 4K, check ----
void testTables()
{
    std::printf("tables\n");
    for (const NodeType& k : all()) {
        const bool layer = k.param("z") && k.param("camera");
        if (!CHECK_EQ(layer, ::Comp::takesCamera(k.id))) std::printf("       %s\n", qPrintable(k.id));
    }
    for (const char* kind : {"transform", "media", "text", "text3d"}) CHECK(::Comp::takesCamera(kind));
    const NodeType* cam = find("camera");
    if (!CHECK(cam)) return;
    CHECK(!cam->param("from") && !::Comp::hasRange("camera"));
    CHECK(::Comp::pixelKeys("camera").isEmpty());
    for (const char* kind : {"transform", "media", "text", "text3d"}) {
        CHECK(!::Comp::pixelKeys(kind).contains("z"));
        CHECK(!::Comp::pixelKeys(kind).contains("camera"));
    }
    // Text3d keeps its own depth (extrusion, pixels) next to the layer's z
    CHECK(find("text3d")->param("depth") && ::Comp::pixelKeys("text3d").contains("depth"));
    // 4K change: the values are shares of the picture, the motion scales with it
    g_clipPic = marker(W, H, 200, 60);
    const EffectInstance e = transformGraph({{"n2.z", 2.0}, {"n2.camera", 3}, {"n3.pan_x", 0.1}, {"n3.push", 0.2}});
    const QPointF small = centroid(evalAt(e));
    g_clipPic = marker(2 * W, 2 * H, 400, 120, 16);
    const QPointF big = centroid(evalAt(e, 0, QSize(2 * W, 2 * H)));
    if (!CHECK(near(big, small * 2, 0.3))) print("4K", big, small * 2);

    // check: a depth without a camera; a camera read through `camera` is no unused node
    ProjectFormat f;
    f.width = W;
    f.height = H;
    Timeline tl;
    tl.video.resize(1);
    Clip c;
    c.id = 7;
    c.start = 0;
    c.in = 0;
    c.out = 50;
    c.mediaPath = "/x.png";
    c.effects << graph({{1, "clip", {}}, {2, "transform", {}}, {3, "camera", {}}, {4, "text", {}}, {5, "merge", {}}},
                       {{1, 2, "src"}, {2, 5, "bg"}, {4, 5, "fg"}}, 5,
                       {{"n2.z", 1.0}, {"n2.camera", 3}, {"n4.z", 2.0}, {"n4.camera", 9}, {"n4.text", "x"}});
    tl.video[0].clips << c;
    bool missing = false, unused = false;
    for (const ExportCheck::Issue& i : ExportCheck::comp(tl, 0, 50, f)) {
        if (i.check == "comp_camera_missing") missing = i.message.contains("4");
        if (i.check == "comp_unused_input") unused = true;
    }
    CHECK(missing);
    CHECK(!unused);
    CompGraph g;
    ::Comp::graphOf(c.effects[0], &g);
    CHECK_EQ(::Comp::cameraOf(c.effects[0], g, 2), 3);
    CHECK_EQ(::Comp::cameraOf(c.effects[0], g, 4), 0); // names node 9: no camera
    CHECK_EQ(::Comp::graphCamera(g), 3);
    CHECK(::Comp::valueReads(c.effects[0], g, g.evalOrder()).contains(3));
    // A keyframed depth counts (check and the renderer read the same values)
    {
        Timeline t2 = tl;
        Clip& k = t2.video[0].clips[0];
        k.effects[0].params.remove("n2.z");
        k.effects[0].params.remove("n2.camera");
        k.effects[0].params.remove("n4.camera");
        k.effects[0].params.remove("n4.z");
        k.effects[0].params.insert("n2.camera", 9);
        Keys::setKey(k, Keys::effectParam(::Comp::kEffectId, "n2.z"), 0, 0.0);
        Keys::setKey(k, Keys::effectParam(::Comp::kEffectId, "n2.z"), 20, 1.5);
        bool flagged = false;
        for (const ExportCheck::Issue& i : ExportCheck::comp(t2, 0, 50, f))
            if (i.check == "comp_camera_missing") flagged = i.message.contains("2");
        CHECK(flagged);
        CompGraph g2;
        ::Comp::graphOf(k.effects[0], &g2);
        CHECK(::Comp::everPositive(k.effects[0], &k, 2, "z"));
        CHECK_EQ(::Comp::layerCameraOf(k.effects[0], g2, 2, &k), 0);
    }
}

// ---- CLI ----
QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw, QString* error = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (error) {
            *error = e.message;
            return {};
        }
        std::printf("unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        Check::report(false, "cli error", __FILE__, __LINE__);
        return {};
    }
}
QJsonObject edit(const QJsonArray& ops, QString* error = nullptr)
{
    return run("edit", {{"project", g_project}, {"ops", ops}}, error);
}
QString json(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

QImage frameOf(int clip, int frame)
{
    ProjectData d = load();
    for (Track& t : d.timeline.video)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.id != clip; }), t.clips.end());
    return Check::renderFrame(d.timeline, d.format, frame);
}

// Centre of the dark pixels of a picture (away from its edges: a pan uncovers black there)
QPointF darkCentre(const QImage& img)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 20; y < img.height() - 20; ++y)
        for (int x = 30; x < img.width() - 30; ++x)
            if (qGray(img.pixel(x, y)) < 60) sx += x + 0.5, sy += y + 0.5, ++n;
    return n > 0 ? QPointF(sx / n, sy / n) : QPointF(-1, -1);
}

void testCli(const QString& dir)
{
    std::printf("cli\n");
    // A photo: light gray with a dark square right of the centre
    QImage photo(W, H, QImage::Format_RGB32);
    photo.fill(QColor(200, 200, 200));
    for (int y = 70; y < 100; ++y)
        for (int x = 220; x < 250; ++x) photo.setPixel(x, y, qRgb(0, 0, 0));
    const QString file = dir + "/photo.png";
    photo.save(file);
    g_project = dir + "/camera.schneidi";
    run("new", {{"project", g_project}, {"width", W}, {"height", H}, {"fps", 25}});
    edit({QJsonObject{{"op", "append"}, {"media", file}, {"duration", "4s"}}});
    ProjectData d = load();
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id;

    // node add camera: only added (no input), the answer hints at the layers
    const QJsonObject added = edit({QJsonObject{{"op", "node"}, {"clip", clip},
                                                {"add", QJsonObject{{"type", "camera"}, {"params", QJsonObject{{"pan_x", 0.05}}}}}}});
    const QJsonObject node = added["notes"].toArray().first().toObject()["node"].toObject();
    const int cam = node["id"].toInt();
    CHECK(node["type"] == "camera" && cam > 0);
    {
        bool hinted = false;
        for (const QJsonValue& v : node["graph"].toObject()["nodes"].toArray())
            if (v["id"].toInt() == cam) hinted = v["hint"].toString().contains("z:1");
        if (!CHECK(hinted)) std::printf("       %s\n", qPrintable(json(node)));
        CHECK(node["graph"].toObject()["out"].toInt() != cam); // never the output
    }
    // A transform layer in its space
    const QJsonObject tr = edit({QJsonObject{{"op", "node"}, {"clip", clip}, {"add", QJsonObject{{"type", "transform"}}}}});
    const int layer = tr["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
    const QPointF before = darkCentre(frameOf(clip, 0));
    edit({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", layer}, {"params", QJsonObject{{"z", 1}}}}}); // the graph's camera
    const QPointF after = darkCentre(frameOf(clip, 0));
    if (!CHECK(near(after, before - QPointF(0.05 * W, 0), 1.0))) print("cli pan", after, before - QPointF(0.05 * W, 0));
    // info: the camera's layers, no unused node
    const QJsonObject info = run("info", {{"project", g_project}});
    const QString infoText = json(info);
    CHECK(infoText.contains(QString("\"layers\":[%1]").arg(layer)));
    CHECK(!infoText.contains("\"unused\""));
    // A wrong value: z out of range is an error
    QString err;
    edit({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", layer}, {"params", QJsonObject{{"z", -1}}}}}, &err);
    CHECK(!err.isEmpty());
    // effects kind:'nodes': the camera with its note; the layers have z and camera
    const QJsonObject kinds = run("effects", {{"kind", "nodes"}});
    bool listed = false, layerKeys = false;
    for (const QJsonValue& v : kinds["nodes"].toArray()) {
        if (v["type"] == "camera") listed = v["note"].toString().contains("parallax_photo");
        if (v["type"] == "media") {
            int found = 0;
            for (const QJsonValue& p : v["params"].toArray()) found += p["key"] == "z" || p["key"] == "camera";
            layerKeys = found == 2;
        }
    }
    CHECK(listed && layerKeys);
    // A camera's drift has no keyframes (steady)
    err.clear();
    edit({QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{clip}}, {"param", QString("comp.n%1.drift_x").arg(cam)},
                      {"at", 0}, {"value", 0.1}}}, &err);
    CHECK(err.contains("steady"));
    // help of op node names the camera
    CHECK(json(run("help", {{"command", "edit"}, {"op", "node"}})).contains("type:'camera'"));

    // The recipe: the photo in two layers, the camera drifting forward and right - the frames differ over time, the
    // dark square (right of the centre) moves by the formula, the edges never show
    edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", "parallax_photo"}}});
    d = load();
    const Clip* c = TimelineOps::findClip(d.timeline, clip);
    const EffectInstance* e = c ? EffectRegistry::instance(*c, ::Comp::kEffectId) : nullptr;
    if (!CHECK(e)) return;
    CHECK(e->params.value("n2.camera").toInt() == 6 && e->params.value("n4.camera").toInt() == 6);
    const QImage f0 = frameOf(clip, 0), f1 = frameOf(clip, 75);
    CHECK(f0 != f1);
    const QPointF p0 = darkCentre(f0), p1 = darkCentre(f1);
    // bg at z 1.5 after 3 s: push 0.09, pan 0.018 W - the square grows away from the centre; overscan 1.08 at rest
    const QPointF ctr(W / 2.0, H / 2.0), sq(235, 85);
    const QPointF want0 = ctr + (sq - ctr) * 1.08;
    if (!CHECK(near(p0, want0, 1.0))) print("recipe at rest", p0, want0);
    const double k = 1.5 / (1.5 - 0.09);
    const QPointF want1 = ctr + (want0 - ctr) * k - QPointF(0.018 * W / (1.5 - 0.09), 0);
    if (!CHECK(near(p1, want1, 1.5))) print("recipe at 3 s", p1, want1);
    // Edges never show: the corners stay covered
    for (const QImage& f : {f0, f1})
        for (const QPoint q : {QPoint(1, 1), QPoint(W - 2, 1), QPoint(1, H - 2), QPoint(W - 2, H - 2)})
            CHECK(qGray(f.pixel(q)) > 150);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_camera");
    Check::initEngine();
    QTemporaryDir dir;
    testMap();
    testLayers();
    testFollow();
    testTime();
    testBounds();
    testLostTracker();
    testTables();
    testCli(dir.path());
    return Check::result();
}
