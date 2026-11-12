// SPDX-License-Identifier: AGPL-3.0-only
// Four-corner tracking and the corner pin (0.22 block 4): Tracker::QuadTracker, tracker mode quad, node corner_pin.
// Synthetic pictures only: a checkerboard on a plane seen through a homography that changes every frame (it moves,
// turns and tilts in perspective), its inner corners known exactly.
// - Compose: homography between two quads, inverse, product, map
// - quad analysis: the corners within 1 px on every frame (640 x 360), also while one corner is hidden behind a flat
//   patch for 10 frames (counted as outliers, never lost); a corner on a flat area is tracked further inside (hint)
//   and still lands within 2.5 px; a crossed quad is refused
// - corner_pin: the picture lands on the corners (inside opaque, outside transparent; a text's box is stretched),
//   follows a quad tracker's corner keys and fades with its seen keys, a point tracker moves its own corners;
//   crossed corners draw nothing
// - follow rule: c keys missing = the start corners, reference point = the quad's middle, numOf(id, "quad")
// - CLI (with ffmpeg): track_node mode quad with corners (one undo step), notes with the corners, node add
//   corner_pin follow, recipe screen_replace, `effects kind:'nodes'`
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/Tracker.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"
#include "engine/video/Compose.h"

#include "render_check.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace Picture::Comp;

namespace {

constexpr int W = 640, H = 360;
constexpr int kFrames = 40;
constexpr int kHideFrom = 12, kHideTo = 21; // corner 3 (index 2) behind a flat patch
// Inner corners of the board on the plane (X junctions), clockwise from the top left
const QPointF kPlanePts[4] = {{200, 100}, {440, 100}, {440, 260}, {200, 260}};

// The plane (W x H) -> picture at frame t: its corners move every frame (shift, turn, perspective)
Compose::Homography planeAt(int t)
{
    const double to[8] = {40 + 2.0 * t,  30 + 1.0 * t,  600 - 1.5 * t, 20 + 1.2 * t,
                          620 - 1.0 * t, 340 - 0.8 * t, 20 + 2.5 * t,  330 - 1.0 * t};
    Compose::Homography h;
    Compose::homography(W, H, to, &h);
    return h;
}

QPointF truth(int t, int i)
{
    double x = 0, y = 0;
    Compose::mapPoint(planeAt(t), kPlanePts[i].x(), kPlanePts[i].y(), &x, &y);
    return {x, y};
}

// Board: squares of 40 between 120..520 x 60..300 of the plane, flat gray around (flatEdge) or checkers everywhere
QImage boardFrame(int t, bool flatEdge, bool hide)
{
    Compose::Homography inv;
    Compose::invert(planeAt(t), &inv);
    QImage img(W, H, QImage::Format_RGB32);
    const QPointF hidden = truth(t, 2);
    for (int y = 0; y < H; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < W; ++x) {
            double sum = 0;
            int n = 0;
            bool inPlane = false;
            for (int sy = 0; sy < 3; ++sy)
                for (int sx = 0; sx < 3; ++sx) { // 3 x 3 samples: soft, exact edges
                    double u = 0, v = 0;
                    if (!Compose::mapPoint(inv, x + (sx + 0.5) / 3, y + (sy + 0.5) / 3, &u, &v) || u < 0 || v < 0 || u >= W || v >= H) {
                        sum += 70;
                        ++n;
                        continue;
                    }
                    inPlane = true;
                    const bool board = u >= 120 && u < 520 && v >= 60 && v < 300;
                    if (flatEdge && !board) sum += 128;
                    else sum += ((int(std::floor((u - 120) / 40)) + int(std::floor((v - 60) / 40))) & 1) ? 220 : 30;
                    ++n;
                }
            (void)inPlane;
            const int g = int(std::lround(sum / n));
            line[x] = qRgb(g, g, g);
        }
    }
    if (hide && t >= kHideFrom && t <= kHideTo) // something flat passes in front of corner 3
        for (int y = int(hidden.y()) - 30; y < int(hidden.y()) + 30; ++y)
            for (int x = int(hidden.x()) - 30; x < int(hidden.x()) + 30; ++x)
                if (x >= 0 && y >= 0 && x < W && y < H) img.setPixel(x, y, qRgb(150, 30, 30));
    return img;
}

double distPx(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

void testCompose()
{
    std::printf("compose\n");
    const double from[8] = {10, 20, 300, 15, 280, 200, 30, 210};
    const double to[8] = {50, 40, 400, 60, 390, 300, 20, 280};
    Compose::Homography h, inv;
    if (!CHECK(Compose::homography(from, to, &h))) return;
    for (int i = 0; i < 4; ++i) {
        double x = 0, y = 0;
        CHECK(Compose::mapPoint(h, from[2 * i], from[2 * i + 1], &x, &y));
        CHECK(std::abs(x - to[2 * i]) < 1e-6 && std::abs(y - to[2 * i + 1]) < 1e-6);
    }
    CHECK(Compose::invert(h, &inv));
    const Compose::Homography id = Compose::multiply(inv, h);
    double x = 0, y = 0;
    Compose::mapPoint(id, 123, 77, &x, &y);
    CHECK(std::abs(x - 123) < 1e-6 && std::abs(y - 77) < 1e-6);
    // Crossed (corners 2 and 3 swapped): no quad
    const double crossed[8] = {50, 40, 400, 60, 20, 280, 390, 300};
    CHECK(!Compose::homography(from, crossed, &h));
    // The quad's middle: where the diagonals cross
    const QPointF m = ::Comp::quadMiddle({0, 0}, {4, 0}, {4, 2}, {0, 2});
    CHECK(std::abs(m.x() - 2) < 1e-9 && std::abs(m.y() - 1) < 1e-9);
}

TrackStart quadStart(const QPointF picturePts[4])
{
    TrackStart st;
    st.mode = TrackMode::Quad;
    st.size = 0.05;
    for (int i = 0; i < 4; ++i) st.corners[i] = QPointF(picturePts[i].x() / W, picturePts[i].y() / H);
    return st;
}

void testQuadAnalysis()
{
    std::printf("quad analysis\n");
    QVector<QImage> frames;
    for (int t = 0; t < kFrames; ++t) frames << boardFrame(t, false, true);
    QPointF start[4];
    for (int i = 0; i < 4; ++i) start[i] = truth(0, i);
    const TrackRun r = trackFrames([&](int f) { return frames.value(f); }, quadStart(start), 0, kFrames - 1);
    if (!CHECK(r.error.isEmpty())) std::printf("       %s\n", qPrintable(r.error));
    // Never lost (three corners are enough), the hidden frames went without corner 3
    CHECK_EQ(r.points.size(), kFrames);
    CHECK(r.lostFrames.isEmpty());
    if (!CHECK(r.outliers >= kHideTo - kHideFrom + 1 && r.outliers <= kHideTo - kHideFrom + 4))
        std::printf("       outliers %d\n", r.outliers);
    if (!CHECK(r.movedIn.isEmpty()))
        for (int i = 0; i < 4; ++i)
            std::printf("       strength %d: %g\n", i, Tracker::cornerStrength(Tracker::toColor(frames[0]), start[i], 32));
    double worst = 0, worstHidden = 0;
    for (const ::Comp::TrackPoint& p : r.points) {
        if (!CHECK(p.quad)) break;
        for (int i = 0; i < 4; ++i) {
            const double d = distPx(QPointF(p.corners[i].x() * W, p.corners[i].y() * H), truth(p.frame, i));
            worst = std::max(worst, d);
            if (i == 2 && p.frame >= kHideFrom && p.frame <= kHideTo) worstHidden = std::max(worstHidden, d);
        }
    }
    std::printf("       corners off by at most %.2f px (hidden corner %.2f px)\n", worst, worstHidden);
    CHECK(worst <= 1.0);
    // Middle, size and turn of the quad: the start's middle mapped, area ratio
    const ::Comp::TrackPoint& last = r.points.last();
    QPointF c[4];
    for (int i = 0; i < 4; ++i) c[i] = truth(last.frame, i);
    const QPointF mid = ::Comp::quadMiddle(c[0], c[1], c[2], c[3]);
    CHECK(distPx(QPointF(last.x * W, last.y * H), mid) <= 1.0);
    CHECK(r.points.first().scale == 1.0 && r.points.first().angle == 0.0);
    CHECK(last.scale < 1.0 && last.scale > 0.5);

    // Backwards from the last frame: the same corners
    const TrackRun back = trackFrames([&](int f) { return frames.value(f); }, [&] {
        QPointF s[4];
        for (int i = 0; i < 4; ++i) s[i] = truth(kFrames - 1, i);
        return quadStart(s);
    }(), kFrames - 1, 0);
    double worstBack = 0;
    for (const ::Comp::TrackPoint& p : back.points)
        for (int i = 0; i < 4; ++i)
            worstBack = std::max(worstBack, distPx(QPointF(p.corners[i].x() * W, p.corners[i].y() * H), truth(p.frame, i)));
    CHECK_EQ(back.points.size(), kFrames);
    if (!CHECK(worstBack <= 1.0)) std::printf("       backwards off by %.2f px\n", worstBack);

    // Corners on a flat area (outside the board, the plane gray there): tracked further inside, still on the corners
    QVector<QImage> flat;
    for (int t = 0; t < 25; ++t) flat << boardFrame(t, true, false);
    const QPointF outer[4] = {{100, 40}, {540, 40}, {540, 320}, {100, 320}}; // plane: 20 px off the board
    QPointF outerPic[4];
    for (int i = 0; i < 4; ++i) {
        double x = 0, y = 0;
        Compose::mapPoint(planeAt(0), outer[i].x(), outer[i].y(), &x, &y);
        outerPic[i] = {x, y};
    }
    const TrackRun moved = trackFrames([&](int f) { return flat.value(f); }, quadStart(outerPic), 0, 24);
    CHECK_EQ(moved.movedIn.size(), 4);
    CHECK(!moved.hint.isEmpty());
    double worstMoved = 0;
    for (const ::Comp::TrackPoint& p : moved.points)
        for (int i = 0; i < 4; ++i) {
            double x = 0, y = 0;
            Compose::mapPoint(planeAt(p.frame), outer[i].x(), outer[i].y(), &x, &y);
            worstMoved = std::max(worstMoved, distPx(QPointF(p.corners[i].x() * W, p.corners[i].y() * H), QPointF(x, y)));
        }
    std::printf("       corners on a flat area off by at most %.2f px\n", worstMoved);
    CHECK_EQ(moved.points.size(), 25);
    CHECK(worstMoved <= 2.5); // extrapolated from points up to 40 % further inside

    // A crossed quad is refused
    QPointF crossed[4] = {start[0], start[1], start[3], start[2]};
    CHECK(!trackFrames([&](int f) { return frames.value(f); }, quadStart(crossed), 0, 5).error.isEmpty());
}

// clip node 1 (or a color), tracker 2, src picture 3, corner_pin 4 (out)
EffectInstance pinGraph(const QString& srcType, const QVariantMap& values)
{
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    g.nodes = {CompNode{1, "color", {}}, CompNode{2, "tracker", {}}, CompNode{3, srcType, {}}, CompNode{4, "corner_pin", {}}};
    g.links = {{1, 2, "src"}, {3, 4, "src"}};
    g.out = 4;
    ::Comp::setGraph(e, g);
    e.params.insert("n3.color", QColor(255, 0, 0));
    for (auto it = values.cbegin(); it != values.cend(); ++it) e.params.insert(it.key(), it.value());
    return e;
}

Buf evalAt(const EffectInstance& e, const Clip* c, double t)
{
    Env env;
    env.size = QSize(W, H);
    env.t = t;
    env.fps = 25;
    env.clip = c;
    Buf out;
    if (!evaluate(e, env, nullptr, &out)) return {};
    return out;
}

int alphaAt(const Buf& b, QPointF p)
{
    const int x = int(std::floor(p.x())), y = int(std::floor(p.y()));
    if (b.isNull() || x < 0 || y < 0 || x >= b.w || y >= b.h) return -1;
    return b.px[(size_t(y) * b.w + x) * 4 + 3];
}

// Opaque just inside every corner, transparent just outside
bool onCorners(const Buf& b, const QPointF c[4], const char* what)
{
    const QPointF mid = ::Comp::quadMiddle(c[0], c[1], c[2], c[3]);
    bool ok = true;
    for (int i = 0; i < 4; ++i) {
        const QPointF d = (mid - c[i]) / distPx(mid, c[i]);
        const int in = alphaAt(b, c[i] + 3 * d), out = alphaAt(b, c[i] - 3 * d);
        if (in < 250 || out > 5) {
            std::printf("       %s: corner %d inside %d outside %d\n", what, i, in, out);
            ok = false;
        }
    }
    return ok;
}

void testPin()
{
    std::printf("corner pin\n");
    const QPointF c[4] = {{100, 50}, {500, 80}, {560, 300}, {60, 330}};
    QVariantMap v;
    for (int i = 0; i < 4; ++i) {
        v[QString("n4.c%1x").arg(i)] = c[i].x() / W;
        v[QString("n4.c%1y").arg(i)] = c[i].y() / H;
    }
    // By hand: the color fills the quad
    CHECK(onCorners(evalAt(pinGraph("color", v), nullptr, 0), c, "by hand"));
    // Opacity
    QVariantMap half = v;
    half["n4.opacity"] = 0.5;
    const int a = alphaAt(evalAt(pinGraph("color", half), nullptr, 0), QPointF(300, 200));
    CHECK(a >= 125 && a <= 130);
    // Crossed corners: nothing
    QVariantMap crossed = v;
    crossed["n4.c2x"] = c[3].x() / W, crossed["n4.c3x"] = c[2].x() / W;
    crossed["n4.c2y"] = c[3].y() / H, crossed["n4.c3y"] = c[2].y() / H;
    const Buf none = evalAt(pinGraph("color", crossed), nullptr, 0);
    CHECK(alphaAt(none, QPointF(300, 200)) == 0);

    // A text: its box is stretched onto the corners (the text's ink reaches near the corners, nothing outside)
    {
        QVariantMap tv = v;
        tv["n3.text"] = "HELLO";
        tv["n3.size"] = 60.0;
        const Buf b = evalAt(pinGraph("text", tv), nullptr, 0);
        int inside = 0, outside = 0;
        QPolygonF poly;
        for (const QPointF& p : c) poly << p;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (b.px[(size_t(y) * W + x) * 4 + 3] > 128) (poly.containsPoint(QPointF(x + 0.5, y + 0.5), Qt::OddEvenFill) ? inside : outside)++;
        CHECK(inside > 2000);
        CHECK(outside < 20);
    }

    // Following a quad tracker: its corner keys at t (not analyzed: its start corners)
    Clip clip;
    clip.id = 1;
    clip.out = 100;
    QVariantMap tv{{"n2.mode", "quad"}, {"n4.follow", 2}};
    for (int i = 0; i < 4; ++i) {
        tv[QString("n2.q%1x").arg(i)] = c[i].x() / W;
        tv[QString("n2.q%1y").arg(i)] = c[i].y() / H;
    }
    const EffectInstance e = pinGraph("color", tv);
    CHECK(onCorners(evalAt(e, &clip, 0), c, "tracker not analyzed"));
    QPointF c10[4];
    for (int i = 0; i < 4; ++i) c10[i] = c[i] + QPointF(20 + 3 * i, -10 + 2 * i);
    for (int i = 0; i < 8; ++i) {
        const AnimParam p = Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(2, ::Comp::kCornerKeys[i]));
        Keys::setKey(clip, p, 0, i % 2 ? c[i / 2].y() / H : c[i / 2].x() / W);
        Keys::setKey(clip, p, 10, i % 2 ? c10[i / 2].y() / H : c10[i / 2].x() / W);
    }
    clip.effects << e;
    CHECK(onCorners(evalAt(e, &clip, 10), c10, "tracker keys at 10"));
    QPointF c5[4];
    for (int i = 0; i < 4; ++i) c5[i] = (c[i] + c10[i]) / 2;
    CHECK(onCorners(evalAt(e, &clip, 5), c5, "tracker keys at 5"));
    // The tracker lost its point on 20..39 (fade): gone in the gap
    const AnimParam seen = Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(2, ::Comp::kSeenKey));
    for (auto [f, s] : {std::pair{0, 1.0}, std::pair{19, 1.0}, std::pair{20, 0.0}, std::pair{39, 0.0}, std::pair{40, 1.0}})
        Keys::setKey(clip, seen, f, s);
    CHECK(alphaAt(evalAt(e, &clip, 30), c5[0] * 0 + QPointF(300, 200)) == 0);
    CHECK(alphaAt(evalAt(e, &clip, 10), QPointF(300, 200)) > 250);

    // Following a point tracker: its own corners move by the follow rule
    {
        Clip pc;
        pc.id = 2;
        pc.out = 100;
        QVariantMap pv = v;
        pv["n2.mode"] = "point";
        pv["n2.ax"] = 0.5, pv["n2.ay"] = 0.5;
        pv["n4.follow"] = 2;
        const EffectInstance pe = pinGraph("color", pv);
        const AnimParam px = Keys::effectParam(::Comp::kEffectId, "n2.x"), py = Keys::effectParam(::Comp::kEffectId, "n2.y");
        Keys::setKey(pc, px, 0, 0.5 + 30.0 / W);
        Keys::setKey(pc, py, 0, 0.5 - 20.0 / H);
        pc.effects << pe;
        QPointF shifted[4];
        for (int i = 0; i < 4; ++i) shifted[i] = c[i] + QPointF(30, -20);
        CHECK(onCorners(evalAt(pe, &pc, 0), shifted, "point tracker"));
    }

    // The follow rule of a quad tracker: reference = the start quad's middle, numOf "quad"
    const NodeType* k = find(::Comp::kTrackerType);
    if (CHECK(k)) {
        QVariantMap vals{{"mode", "quad"}};
        for (int i = 0; i < 4; ++i) {
            vals[QString("q%1x").arg(i)] = c[i].x() / W;
            vals[QString("q%1y").arg(i)] = c[i].y() / H;
        }
        const QPointF m = ::Comp::quadMiddle(c[0], c[1], c[2], c[3]);
        CHECK(std::abs(trackerValue(vals, "rx") * W - m.x()) < 1e-6);
        CHECK(std::abs(trackerValue(vals, "x") * W - m.x()) < 1e-6);
        CHECK(std::abs(trackerValue(vals, "c2y") * H - c[2].y()) < 1e-6);
        CHECK_EQ(trackerValue(vals, "quad"), 1.0);
        vals["mode"] = "two";
        CHECK_EQ(trackerValue(vals, "quad"), 0.0);
    }
    // Registered: inputs, its corners not pixel values (0..1)
    const NodeType* pk = find(kCornerPinType);
    if (CHECK(pk)) {
        CHECK(pk->inputs == QStringList{"src"});
        CHECK(pk->param("c3y") && pk->param("follow") && pk->param("opacity"));
        CHECK(::Comp::pixelKeys(kCornerPinType).isEmpty() && ::Comp::fileKeys(kCornerPinType).isEmpty());
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
QString text(const QJsonValue& v) { return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)); }

void testCli(const QString& dir)
{
    std::printf("cli\n");
    if (!Check::haveFfmpeg()) {
        std::printf("       skipped: no ffmpeg\n");
        return;
    }
    QDir().mkpath(dir + "/frames");
    for (int t = 0; t < kFrames; ++t) boardFrame(t, false, true).save(dir + QString("/frames/f%1.png").arg(t, 3, 10, QChar('0')));
    const QString media = Check::makeMedia(dir + "/board.mp4", {"-framerate", "25", "-i", dir + "/frames/f%03d.png", "-c:v",
                                                                "libx264", "-qp", "0", "-pix_fmt", "yuv444p"});
    QImage red(64, 48, QImage::Format_RGB32);
    red.fill(QColor(255, 0, 0));
    const QString picture = dir + "/red.png";
    red.save(picture);
    if (!CHECK(!media.isEmpty())) return;
    g_project = dir + "/pin.schneidi";
    run("new", {{"project", g_project}, {"width", W}, {"height", H}, {"fps", 25}, {"media", QJsonArray{media}}});
    edit({QJsonObject{{"op", "append"}, {"media", media}}});
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id;

    // The recipe: tracker 2 (quad), media 3, corner_pin 4 following 2, merge 5
    const QJsonObject applied = edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", "screen_replace"},
                                                  {"with", QJsonObject{{"picture", picture}}}}});
    if (!CHECK(!applied.isEmpty())) return;
    QJsonArray corners;
    for (int i = 0; i < 4; ++i) corners << QJsonArray{truth(0, i).x() / W, truth(0, i).y() / H};
    // Wrong corners: an error, nothing changed
    QString err;
    edit({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", 2}, {"corners", QJsonArray{corners[0], corners[1]}}}}, &err);
    CHECK(err.contains("four"));
    err.clear();
    edit({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", 2}, {"mode", "cube"}}}, &err);
    CHECK(err.contains("quad"));

    const QJsonObject a = edit({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", 2}, {"corners", corners}}});
    const QJsonObject note = a["notes"].toArray().first().toObject()["track_node"].toObject();
    CHECK_EQ(note["points"].toInt(), kFrames);
    CHECK(note["lost"].toArray().isEmpty());
    const QJsonArray last = note["corners"].toArray();
    if (CHECK_EQ(last.size(), 4)) {
        double worst = 0;
        for (int i = 0; i < 4; ++i)
            worst = std::max(worst, distPx(QPointF(last[i].toArray()[0].toDouble() * W, last[i].toArray()[1].toDouble() * H),
                                           truth(kFrames - 1, i)));
        if (!CHECK(worst <= 1.0)) std::printf("       %s\n", qPrintable(text(note)));
    }
    // The project: mode and start corners set, corner keys written; one undo step for both
    ProjectFile::load(g_project, &d, nullptr);
    const Clip* c = TimelineOps::findClip(d.timeline, clip);
    if (!CHECK(c)) return;
    const EffectInstance* e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    CHECK(e && e->params.value("n2.mode").toString() == "quad");
    CHECK(e && std::abs(e->params.value("n2.q2x").toDouble() - truth(0, 2).x() / W) < 1e-9);
    CHECK(Keys::keyTimes(*c, {Keys::effectParam(::Comp::kEffectId, "n2.c3y")}).size() == kFrames);
    // The picture lies on the board's corners at frame 30 (the merge's output: red inside, the board outside)
    if (e) {
        EffectInstance pinOnly = *e;
        CompGraph g;
        ::Comp::graphOf(pinOnly, &g);
        g.out = 4;
        ::Comp::setGraph(pinOnly, g);
        Env env;
        env.size = QSize(W, H);
        env.t = 30;
        env.clip = c;
        env.mediaPicture = [&](const QString&, double) {
            Buf b(Buf::Rgba, W, H);
            for (size_t i = 0; i < b.px.size(); i += 4) b.px[i] = 255, b.px[i + 3] = 255;
            return b;
        };
        Buf out;
        CHECK(evaluate(pinOnly, env, nullptr, &out));
        QPointF tc[4];
        for (int i = 0; i < 4; ++i) tc[i] = truth(30, i);
        CHECK(onCorners(out, tc, "cli frame 30"));
    }
    // info lists the node kinds' notes; the corner pin is a node kind
    const QJsonObject kinds = run("effects", {{"kind", "nodes"}});
    bool listed = false;
    for (const QJsonValue& v : kinds["nodes"].toArray())
        if (v["type"] == "corner_pin") listed = !v["note"].toString().isEmpty();
    if (!CHECK(listed)) std::printf("       %s\n", qPrintable(QString::fromUtf8(QJsonDocument(kinds).toJson(QJsonDocument::Compact)).left(300)));
    // node add corner_pin with follow
    const QJsonObject added = edit({QJsonObject{{"op", "node"}, {"clip", clip},
                                                {"add", QJsonObject{{"type", "corner_pin"}, {"params", QJsonObject{{"follow", 2}}}}},
                                                {"input", false}}});
    CHECK(added["notes"].toArray().first().toObject()["node"].toObject()["type"].toString() == "corner_pin");
    // help names the mode
    const QJsonObject help = run("help", {{"command", "edit"}, {"op", "track_node"}});
    CHECK(QString::fromUtf8(QJsonDocument(help).toJson()).contains("mode:'quad'"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_corner_pin");
    Check::initEngine();
    QTemporaryDir dir;
    testCompose();
    testQuadAnalysis();
    testPin();
    testCli(dir.path());
    return Check::result();
}
