// SPDX-License-Identifier: AGPL-3.0-only
// Follow rule of the tracker node (0.19, core/CompTrack.h): every follower (mask, transform) is drawn relative to the
// tracker's reference point rx/ry, moves by (x - rx, y - ry) and scales/turns around (x, y). Synthetic pictures only.
// - point, two points and face (stand-in detector): a following mask does not jump when the analysis lands
//   (Editor::trackCompNode writes rx/ry = the start frame's point), and moves with the path afterwards
// - an analysis that adds to a kept path keeps the reference
// - transform with follow: no analysis = the picture unchanged; scale and rotation around the tracked point
#include "render_check.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace Picture::Comp;

namespace {

constexpr int W = 1920, H = 1080;

// Patterned square 80 x 80 on a known path
QPointF squarePath(int t) { return {500 + 7.0 * t, 400 + 3.0 * t}; }
QImage squareFrame(int t)
{
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(QColor(70, 74, 80));
    QPainter p(&img);
    const QPointF c = squarePath(t);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 200, 60));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 80, 80));
    p.setBrush(QColor(40, 60, 200));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 40, 40));
    p.drawRect(QRectF(c.x(), c.y(), 40, 40));
    return img;
}

// Plate with a red and a blue dot 120 px left and right of its middle; moves, grows and turns
QPointF plateCenter(int t) { return {700 + 4.0 * t, 500 + 2.0 * t}; }
QImage plateFrame(int t)
{
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(QColor(70, 74, 80));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(plateCenter(t));
    p.rotate(1.5 * t);
    p.scale(1 + 0.01 * t, 1 + 0.01 * t);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(160, 160, 155));
    p.drawRect(QRectF(-180, -180, 360, 360));
    p.setBrush(QColor(210, 40, 40));
    p.drawEllipse(QPointF(-120, 0), 28, 28);
    p.setBrush(QColor(40, 80, 220));
    p.drawEllipse(QPointF(120, 0), 28, 28);
    return img;
}

// tracker 1, mask 2 (follows 1), black 3, white 4, merge 5 = white where the mask is
EffectInstance maskGraph(const QVariantMap& tracker)
{
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    for (const char* type : {"tracker", "mask", "color", "color", "merge"}) g.nodes << CompNode{int(g.nodes.size()) + 1, type, {}};
    g.links = {{3, 5, "bg"}, {4, 5, "fg"}, {2, 5, "mask"}};
    g.out = 5;
    ::Comp::setGraph(e, g);
    for (auto it = tracker.cbegin(); it != tracker.cend(); ++it) e.params.insert("n1." + it.key(), it.value());
    const QVariantMap rest{{"n2.x", 0.3}, {"n2.y", 0.35}, {"n2.w", 0.12}, {"n2.h", 0.1}, {"n2.follow", 1},
                           {"n3.color", QColor(0, 0, 0)}, {"n4.color", QColor(255, 255, 255)}};
    for (auto it = rest.cbegin(); it != rest.cend(); ++it) e.params.insert(it.key(), it.value());
    return e;
}

Buf evalAt(const Clip& c, double t)
{
    Env env;
    env.size = QSize(192, 108);
    env.t = t;
    env.clip = &c;
    Buf out;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    if (!e || !evaluate(*e, env, nullptr, &out)) return {};
    return out;
}

// Centre of the white pixels (in 0..1 of the picture)
QPointF whiteCenter(const Buf& b)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            if (const double v = b.px[(size_t(y) * b.w + x) * 4]; v > 0) {
                sx += v * (x + 0.5);
                sy += v * (y + 0.5);
                n += v;
            }
    return n > 0 ? QPointF(sx / n / b.w, sy / n / b.h) : QPointF(-1, -1);
}

// A clip with the mask graph; the analysis' points land through Editor::trackCompNode. The mask at the start frame
// stays exactly where it was drawn; at the last frame it moved by the path's way from the start
void noJump(const char* name, const QVariantMap& tracker, const TrackRun& run)
{
    std::printf("%s\n", name);
    if (!CHECK(run.error.isEmpty() && run.points.size() > 2)) return;
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    ed.modifyClips({id}, "Komposition", [&](Clip& c) { c.effects << maskGraph(tracker); });
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    const int start = run.points.first().frame, last = run.points.last().frame;
    const Buf before = evalAt(*c, start);
    CHECK(!before.isNull());
    CHECK(ed.trackCompNode(id, 1, run.points));
    c = TimelineOps::findClip(p.timeline(), id);
    const EffectInstance* e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    CHECK(e && e->params.value("n1.rx").toDouble() == run.points.first().x);
    CHECK(e && e->params.value("n1.ry").toDouble() == run.points.first().y);
    const Buf after = evalAt(*c, start);
    if (!CHECK(!after.isNull() && after.px == before.px)) {
        const QPointF a = whiteCenter(before), b = whiteCenter(after);
        std::printf("       jumped from %.4f, %.4f to %.4f, %.4f\n", a.x(), a.y(), b.x(), b.y());
    }
    // Later the mask moved with the tracked thing (its centre by the path's way, turned/scaled around the point)
    const ::Comp::TrackPoint& q = run.points.last();
    const QPointF r(run.points.first().x, run.points.first().y), mid(0.3, 0.35);
    const double rad = q.angle * std::numbers::pi / 180;
    const QPointF d((mid.x() - r.x()) * 192, (mid.y() - r.y()) * 108);
    const QPointF want(q.x + q.scale * (d.x() * std::cos(rad) - d.y() * std::sin(rad)) / 192,
                       q.y + q.scale * (d.x() * std::sin(rad) + d.y() * std::cos(rad)) / 108);
    const QPointF got = whiteCenter(evalAt(*c, last));
    if (!CHECK(std::hypot((got.x() - want.x()) * 192, (got.y() - want.y()) * 108) < 1.0))
        std::printf("       frame %d: mask at %.4f, %.4f (expected %.4f, %.4f)\n", last, got.x(), got.y(), want.x(), want.y());

    // A second analysis that adds to the kept path (restarted at a later frame) keeps the reference point
    QVector<::Comp::TrackPoint> more{{last + 5, q.x + 0.01, q.y, q.scale, q.angle}, {last + 6, q.x + 0.02, q.y, q.scale, q.angle}};
    CHECK(ed.trackCompNode(id, 1, more));
    c = TimelineOps::findClip(p.timeline(), id);
    e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    CHECK(e && e->params.value("n1.rx").toDouble() == run.points.first().x);
    CHECK(evalAt(*c, start).px == before.px);
    // One that replaces the whole path sets it anew
    QVector<::Comp::TrackPoint> fresh{{start, 0.6, 0.6, 1, 0}, {start + 1, 0.61, 0.6, 1, 0}};
    ed.modifyClips({id}, "Keys", [&](Clip& cl) {
        for (const char* k : ::Comp::kTrackKeys)
            for (int t : Keys::keyTimes(cl, {Keys::effectParam(::Comp::kEffectId, QString("n1.") + k)}))
                Keys::removeKey(cl, Keys::effectParam(::Comp::kEffectId, QString("n1.") + k), t);
    });
    CHECK(ed.trackCompNode(id, 1, fresh));
    c = TimelineOps::findClip(p.timeline(), id);
    e = EffectRegistry::instance(*c, ::Comp::kEffectId);
    CHECK(e && e->params.value("n1.rx").toDouble() == 0.6);
    CHECK(evalAt(*c, start).px == before.px);
}

void testMaskFollows()
{
    // One point: the square's corner pattern
    {
        TrackStart st;
        st.a = QPointF(squarePath(0).x() / W, squarePath(0).y() / H);
        const QVariantMap v{{"mode", "point"}, {"ax", st.a.x()}, {"ay", st.a.y()}};
        noJump("point", v, trackFrames(squareFrame, st, 0, 8));
    }
    // Two points: the reference is the middle of the dots, not dot A
    {
        TrackStart st;
        st.mode = TrackMode::Two;
        st.a = QPointF((plateCenter(0).x() - 120) / W, plateCenter(0).y() / H);
        st.b = QPointF((plateCenter(0).x() + 120) / W, plateCenter(0).y() / H);
        const QVariantMap v{{"mode", "two"}, {"ax", st.a.x()}, {"ay", st.a.y()}, {"bx", st.b.x()}, {"by", st.b.y()}};
        const TrackRun r = trackFrames(plateFrame, st, 0, 10);
        CHECK(!r.points.isEmpty() && std::abs(r.points.last().angle - 15) < 1 && std::abs(r.points.last().scale - 1.1) < 0.02);
        noJump("two points", v, r);
    }
    // Face (stand-in detector): the reference is the face centre found at the start, not the clicked point
    {
        const auto box = [](int t) { return FaceDetect::Face{0.3 + 0.01 * t, 0.4, 0.1 * (1 + 0.02 * t), 0.12, 0.9}; };
        const auto frame = [](int t) {
            QImage img(64, 36, QImage::Format_RGB32);
            img.fill(Qt::black);
            img.setPixel(0, 0, qRgb(t, 0, 0));
            return img;
        };
        const FaceFinder finder = [&](const QImage& img, QString*) -> std::optional<std::vector<FaceDetect::Face>> {
            return std::vector<FaceDetect::Face>{box(qRed(img.pixel(0, 0)))};
        };
        TrackStart st;
        st.mode = TrackMode::Face;
        st.a = {0.32, 0.43}; // inside the face, not its centre (0.35, 0.46)
        const QVariantMap v{{"mode", "face"}, {"ax", st.a.x()}, {"ay", st.a.y()}};
        noJump("face", v, trackFrames(frame, st, 0, 8, {}, nullptr, finder));
    }
    // Not analyzed, no reference written: what the mode implies (two points: the middle of A and B)
    CHECK_EQ(trackerValue({{"mode", "two"}, {"ax", 0.2}, {"bx", 0.6}, {"ay", 0.3}, {"by", 0.5}}, "x"), 0.4);
    CHECK_EQ(trackerValue({{"mode", "two"}, {"ax", 0.2}, {"bx", 0.6}, {"ay", 0.3}, {"by", 0.5}}, "ry"), 0.4);
    CHECK_EQ(trackerValue({{"mode", "face"}, {"ax", 0.2}, {"ay", 0.3}}, "rx"), 0.2);
    CHECK_EQ(trackerValue({{"ax", 0.2}, {"rx", 0.25}}, "x"), 0.25);
    CHECK_EQ(trackerValue({{"rx", 0.25}, {"x", 0.7}}, "x"), 0.7);
}

int maxDiff(const Buf& a, const Buf& b)
{
    if (a.isNull() || a.px.size() != b.px.size()) return 999;
    int m = 0;
    for (size_t i = 0; i < a.px.size(); ++i) m = std::max(m, std::abs(int(a.px[i]) - int(b.px[i])));
    return m;
}

// A picture with a mark in the top left quarter (a turn or move shows)
Buf picture()
{
    Buf b(Buf::Rgba, 160, 90);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            uint8_t* p = b.data() + (size_t(y) * b.w + x) * 4;
            const bool mark = x >= 20 && x < 50 && y >= 15 && y < 35;
            p[0] = mark ? 250 : uint8_t(x);
            p[1] = mark ? 20 : uint8_t(y * 2);
            p[2] = 90;
            p[3] = 255;
        }
    return b;
}

void testTransformFollows()
{
    std::printf("transform\n");
    const Buf src = picture();
    // Through the engine: a transform following a tracker that was not analyzed leaves the picture as it is
    for (const char* mode : {"point", "two", "face"}) {
        EffectInstance e{::Comp::kEffectId, {}, true};
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "tracker", {}} << CompNode{3, "transform", {}};
        g.links = {{1, 2, "src"}, {2, 3, "src"}};
        g.out = 3;
        e.params = {{"n2.mode", mode}, {"n2.ax", 0.7}, {"n2.ay", 0.2}, {"n2.bx", 0.9}, {"n2.by", 0.3}, {"n3.follow", 2}};
        ::Comp::setGraph(e, g);
        Env env;
        env.size = QSize(src.w, src.h);
        env.clipPicture = [&]() -> const Buf* { return &src; };
        Buf out;
        if (!CHECK(evaluate(e, env, nullptr, &out) && out.px == src.px)) std::printf("       mode %s moved it\n", mode);
        // Its own placement stays its own: moved by its x only
        e.params["n3.x"] = 0.6;
        Buf own, placed;
        CHECK(evaluate(e, env, nullptr, &own));
        CHECK(runNode("transform", {{"x", 0.6}}, {{"src", src}}, RunOptions{QSize(src.w, src.h)}, &placed));
        CHECK(maxDiff(own, placed) == 0);
    }
    const auto follow = [&](QVariantMap tracker, const QVariantMap& own) {
        RunOptions o;
        o.size = QSize(src.w, src.h);
        o.otherNodes[7] = tracker;
        QVariantMap p = own;
        p["follow"] = 7;
        Buf b;
        CHECK(runNode("transform", p, {{"src", src}}, o, &b));
        return b;
    };
    const auto plain = [&](const QVariantMap& own) {
        Buf b;
        CHECK(runNode("transform", own, {{"src", src}}, RunOptions{QSize(src.w, src.h)}, &b));
        return b;
    };
    // Moved by the way from the reference
    CHECK(maxDiff(follow({{"rx", 0.3}, {"ry", 0.4}, {"x", 0.4}, {"y", 0.45}, {"scale", 1.0}, {"angle", 0.0}}, {}),
                  plain({{"x", 0.6}, {"y", 0.55}})) == 0);
    // Turned around the tracked point (0.25, 0.5): the picture centre a quarter width right of it goes to its left
    CHECK(maxDiff(follow({{"rx", 0.25}, {"ry", 0.5}, {"x", 0.25}, {"y", 0.5}, {"scale", 1.0}, {"angle", 180.0}}, {}),
                  plain({{"x", 0.0}, {"angle", 180.0}})) <= 1);
    // Scaled around it: half the size, half the way to the centre
    CHECK(maxDiff(follow({{"rx", 0.25}, {"ry", 0.5}, {"x", 0.25}, {"y", 0.5}, {"scale", 0.5}, {"angle", 0.0}}, {}),
                  plain({{"x", 0.375}, {"size", 0.5}})) <= 1);
    // A turn of 90 degrees in a 16:9 picture keeps its shape (pixels, not fractions): point (0.75, 0.5) around the
    // centre goes to below it, 0.25 * W pixels down
    CHECK(maxDiff(follow({{"rx", 0.5}, {"ry", 0.5}, {"x", 0.5}, {"y", 0.5}, {"scale", 1.0}, {"angle", 90.0}},
                         {{"x", 0.75}, {"size", 0.3}}),
                  plain({{"x", 0.5}, {"y", 0.5 + 0.25 * 160 / 90}, {"size", 0.3}, {"angle", 90.0}})) <= 1);
}

} // namespace

// One rounding of the follow id for every kind (mask truncated, transform rounded before): 0.6 names node 1
void testFollowId()
{
    CHECK_EQ(::Comp::followId(0.6), 1);
    CHECK_EQ(::Comp::followId(1.4), 1);
    CHECK_EQ(::Comp::followId(0.3), 0);
    CHECK_EQ(::Comp::followId(-2), 0);
    const QVariantMap tracker{{"x", 0.6}, {"y", 0.55}, {"rx", 0.5}, {"ry", 0.5}};
    Clip whole, part;
    whole.out = part.out = 10;
    whole.effects << maskGraph(tracker);
    part.effects << maskGraph(tracker);
    part.effects[0].params["n2.follow"] = 0.6;
    Clip still = whole;
    still.effects[0].params["n2.follow"] = 0;
    const Buf a = evalAt(whole, 0), b = evalAt(part, 0), c = evalAt(still, 0);
    CHECK(!a.isNull() && a.px == b.px);
    CHECK(!c.isNull() && a.px != c.px); // following moved it
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    testFollowId();
    testMaskFollows();
    testTransformFollows();
    return Check::result();
}
