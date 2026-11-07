// SPDX-License-Identifier: AGPL-3.0-only
// Tracker node of the compositing graph (0.19, package D): engine/picture/CompTrack, Tracker::TwoPointTracker, node
// kind "tracker", Editor::trackCompNode. Synthetic pictures only (1920 x 1080, drawn per frame):
// - one point: a patterned square on a known sub-pixel path -> within 0.5 px every frame
// - two points: a plate with two dots that moves, grows and turns -> center 0.5 px, size 1 %, angle 0.5 degrees
// - occlusion: the square passes behind a bar -> lost there, found again behind it, on the path afterwards
// - outlier: one of the two dots hidden -> the other one leads, size and angle stay
// - faces through a stand-in detector (the real one: test_comp_tracker_face): follows the box, lost and found
// - the node passes its picture through; keyframes n<id>.x/.y/.scale/.angle written, one undo step
// - a portrait clip in a 16:9 project: start points and results in the clip picture's coordinates (no bars)
// Prints the analysis speed at 1080p (frames per second, drawing the pictures not counted).
#include "check.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Tracker.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <cstdio>

using namespace Picture::Comp;

namespace {

constexpr int W = 1920, H = 1080;

QImage canvas()
{
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(QColor(70, 74, 80));
    return img;
}

// Patterned square 80 x 80 centered on c (quadrants: the match has corners to hold on to)
void square(QPainter& p, QPointF c)
{
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 200, 60));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 80, 80));
    p.setBrush(QColor(40, 60, 200));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 40, 40));
    p.drawRect(QRectF(c.x(), c.y(), 40, 40));
}

QPointF squarePath(int t) { return {500 + 7.3 * t, 400 + 3.1 * t}; }

QImage squareFrame(int t)
{
    QImage img = canvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    square(p, squarePath(t));
    return img;
}

// Plate 360 x 360 with a red dot left and a blue dot right of its middle (120 px each), moving, growing, turning
struct Plate {
    QPointF c;
    double scale, angle;
};
Plate platePath(int t) { return {{700 + 4.0 * t, 500 + 2.0 * t}, 1 + 0.004 * t, 0.8 * t}; }

QImage plateFrame(int t, bool hideB = false)
{
    const Plate pl = platePath(t);
    QImage img = canvas();
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(pl.c);
    p.rotate(pl.angle); // clockwise on screen (y down)
    p.scale(pl.scale, pl.scale);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(160, 160, 155));
    p.drawRect(QRectF(-180, -180, 360, 360));
    p.setBrush(QColor(210, 40, 40));
    p.drawEllipse(QPointF(-120, 0), 28, 28);
    p.setBrush(QColor(40, 80, 220));
    p.drawEllipse(QPointF(120, 0), 28, 28);
    if (hideB) { // a sticker over the blue dot
        p.setBrush(QColor(20, 140, 60));
        p.drawRect(QRectF(80, -50, 90, 100));
    }
    return img;
}

QPointF norm(QPointF px) { return {px.x() / W, px.y() / H}; }
double pxErr(const ::Comp::TrackPoint& p, QPointF want) { return std::hypot(p.x * W - want.x(), p.y * H - want.y()); }

void testPoint()
{
    TrackStart st;
    st.a = norm(squarePath(0));
    double draw = 0;
    QElapsedTimer all;
    all.start();
    int calls = 0;
    const TrackRun r = trackFrames(
        [&](int t) {
            QElapsedTimer d;
            d.start();
            QImage img = squareFrame(t);
            draw += d.nsecsElapsed() / 1e6;
            return img;
        },
        st, 0, 60, [&](int done, int total) {
            ++calls;
            CHECK(done <= total && total == 60);
        });
    const double ms = all.nsecsElapsed() / 1e6 - draw;
    std::printf("point: %.1f frames/s at 1080p (%.2f ms per frame without drawing)\n", 61 / ms * 1000, ms / 61);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.points.size(), 61);
    CHECK_EQ(calls, 60);
    CHECK(r.lostFrames.isEmpty());
    double worst = 0;
    for (const ::Comp::TrackPoint& p : r.points) {
        worst = std::max(worst, pxErr(p, squarePath(p.frame)));
        CHECK_EQ(p.scale, 1.0);
        CHECK_EQ(p.angle, 0.0);
    }
    if (!CHECK(worst <= 0.5)) std::printf("       point: worst %.3f px\n", worst);
    if (!r.points.isEmpty()) CHECK_EQ(r.points.first().frame, 0);

    // Backwards from the end finds the start again
    st.a = norm(squarePath(60));
    const TrackRun back = trackFrames(squareFrame, st, 60, 0);
    CHECK_EQ(back.points.size(), 61);
    if (!back.points.isEmpty()) {
        CHECK_EQ(back.points.last().frame, 0);
        CHECK(pxErr(back.points.last(), squarePath(0)) <= 0.5);
    }
    // Cancel right away: only the start
    std::atomic<bool> stop{true};
    const TrackRun none = trackFrames(squareFrame, st, 0, 20, {}, &stop);
    CHECK(none.canceled);
    CHECK_EQ(none.points.size(), 1);
    // No picture: an error
    CHECK(!trackFrames([](int) { return QImage(); }, st, 0, 5).error.isEmpty());
}

void testTwoPoints()
{
    TrackStart st;
    st.mode = TrackMode::Two;
    st.a = norm(platePath(0).c - QPointF(120, 0));
    st.b = norm(platePath(0).c + QPointF(120, 0));
    double draw = 0;
    QElapsedTimer all;
    all.start();
    const TrackRun r = trackFrames(
        [&](int t) {
            QElapsedTimer d;
            d.start();
            QImage img = plateFrame(t);
            draw += d.nsecsElapsed() / 1e6;
            return img;
        },
        st, 0, 60);
    const double ms = all.nsecsElapsed() / 1e6 - draw;
    std::printf("two points: %.1f frames/s at 1080p (%.2f ms per frame without drawing)\n", 61 / ms * 1000, ms / 61);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.points.size(), 61);
    CHECK_EQ(r.outliers, 0);
    double worstPos = 0, worstScale = 0, worstAngle = 0;
    for (const ::Comp::TrackPoint& p : r.points) {
        const Plate want = platePath(p.frame);
        worstPos = std::max(worstPos, pxErr(p, want.c));
        worstScale = std::max(worstScale, std::abs(p.scale / want.scale - 1));
        worstAngle = std::max(worstAngle, std::abs(p.angle - want.angle));
    }
    std::printf("two points: worst %.3f px, %.3f %%, %.3f degrees (48 degrees turned, 24 %% grown)\n", worstPos,
                worstScale * 100, worstAngle);
    CHECK(worstPos <= 0.5);
    CHECK(worstScale <= 0.01);
    CHECK(worstAngle <= 0.5);

    // Two points on top of each other: an error, not a division by zero
    st.b = st.a;
    CHECK(!trackFrames([](int t) { return plateFrame(t); }, st, 0, 3).error.isEmpty());
}

void testOcclusion()
{
    // Square at (600 + 6 t, 500) behind a bar x 900..1020 (fully hidden while its center is in 940..980: t 57..63)
    const auto path = [](int t) { return QPointF(600 + 6.0 * t, 500); };
    const auto frame = [&](int t) {
        QImage img = canvas();
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        square(p, path(t));
        p.fillRect(QRectF(900, 0, 120, H), QColor(40, 160, 40));
        return img;
    };
    TrackStart st;
    st.a = norm(path(0));
    const TrackRun r = trackFrames(frame, st, 0, 100);
    CHECK(r.error.isEmpty());
    CHECK(!r.gaveUp);
    for (int t = 57; t <= 63; ++t)
        if (!CHECK(r.lostFrames.contains(t))) std::printf("       frame %d (fully hidden) not lost\n", t);
    CHECK(r.lostAt > 40 && r.lostAt <= 57);
    if (!CHECK(r.foundAt > 63 && r.foundAt < 80)) std::printf("       lost at %d, found at %d\n", r.lostAt, r.foundAt);
    // Out from behind the bar it is on its path again
    double worst = 0;
    for (const ::Comp::TrackPoint& p : r.points)
        if (p.frame >= 75) worst = std::max(worst, pxErr(p, path(p.frame)));
    if (!CHECK(worst <= 0.5)) std::printf("       after the bar: worst %.3f px\n", worst);
    if (CHECK(!r.points.isEmpty())) CHECK_EQ(r.points.last().frame, 100);

    // Hidden for good: gives up after maxLost frames
    st.maxLost = 5;
    const TrackRun gone = trackFrames([&](int t) { return t < 3 ? frame(0) : canvas(); }, st, 0, 50);
    CHECK(gone.gaveUp);
    CHECK_EQ(gone.lostFrames.size(), 5);
    CHECK_EQ(gone.lostAt, 3);
    CHECK_EQ(gone.foundAt, -1);
}

void testOutlier()
{
    // The blue dot is covered from frame 10 to 20: the red one leads, size and angle hold
    TrackStart st;
    st.mode = TrackMode::Two;
    st.a = norm(platePath(0).c - QPointF(120, 0));
    st.b = norm(platePath(0).c + QPointF(120, 0));
    const auto frame = [](int t) { return plateFrame(t, t >= 10 && t <= 20); };
    const TrackRun r = trackFrames(frame, st, 0, 30);
    CHECK(r.error.isEmpty());
    CHECK(r.outliers > 0);
    CHECK_EQ(r.points.size(), 31);
    // While covered the plate keeps turning (0.8 degrees a frame) but the result holds the last angle: only after the
    // sticker the angle comes back
    double worstLate = 0;
    for (const ::Comp::TrackPoint& p : r.points)
        if (p.frame >= 25) worstLate = std::max(worstLate, std::abs(p.angle - platePath(p.frame).angle));
    if (!CHECK(worstLate <= 0.5)) std::printf("       after the sticker: angle off by %.3f\n", worstLate);
}

void testFaces()
{
    // A stand-in detector: the frame number is in the first pixel; frames 10..14 have no face, 20 has a second face
    // far away
    const auto frame = [](int t) {
        QImage img(64, 36, QImage::Format_RGB32);
        img.fill(Qt::black);
        img.setPixel(0, 0, qRgb(t, 0, 0));
        return img;
    };
    const auto box = [](int t) { return FaceDetect::Face{0.3 + 0.005 * t, 0.4, 0.1 * (1 + 0.01 * t), 0.12, 0.9}; };
    const FaceFinder finder = [&](const QImage& img, QString*) -> std::optional<std::vector<FaceDetect::Face>> {
        const int t = qRed(img.pixel(0, 0));
        std::vector<FaceDetect::Face> out;
        if (t == 20) out.push_back({0.8, 0.1, 0.1, 0.12, 0.95});
        if (t < 10 || t > 14) out.push_back(box(t));
        return out;
    };
    TrackStart st;
    st.mode = TrackMode::Face;
    st.a = {0.33, 0.45}; // inside the face at frame 0
    const TrackRun r = trackFrames(frame, st, 0, 30, {}, nullptr, finder);
    CHECK(r.error.isEmpty());
    CHECK_EQ(r.lostAt, 10);
    CHECK_EQ(r.foundAt, 15);
    CHECK_EQ(r.lostFrames.size(), 5);
    CHECK_EQ(r.points.size(), 26);
    for (const ::Comp::TrackPoint& p : r.points) {
        const FaceDetect::Face f = box(p.frame);
        CHECK(std::abs(p.x - (f.x + f.w / 2)) < 1e-9 && std::abs(p.y - (f.y + f.h / 2)) < 1e-9);
        CHECK(std::abs(p.scale - (1 + 0.01 * p.frame)) < 1e-9);
    }
    // Start point on no face: an error
    st.a = {0.9, 0.9};
    CHECK(!trackFrames(frame, st, 0, 5, {}, nullptr, finder).error.isEmpty());
    // A failing detector: its error, stop
    const FaceFinder broken = [](const QImage&, QString* e) -> std::optional<std::vector<FaceDetect::Face>> {
        *e = "model failed";
        return std::nullopt;
    };
    CHECK_EQ(trackFrames(frame, st, 0, 5, {}, nullptr, broken).error, QString("model failed"));
    // Without the extension (the test's folders have none): a clear error, no fallback
    if (!FaceDetect::available()) {
        QString err;
        CHECK(!installedFaces(&err));
        CHECK(err.contains("YuNet"));
        st.a = {0.33, 0.45};
        const TrackRun none = trackFrames(frame, st, 0, 5);
        CHECK(none.error.contains("YuNet"));
        CHECK(none.points.isEmpty());
    }
}

void testNode()
{
    const NodeType* k = find("tracker");
    if (!CHECK(k)) return;
    CHECK_EQ(k->inputs, QStringList{"src"});
    for (const char* key : {"mode", "ax", "ay", "bx", "by", "size", "x", "y", "scale", "angle"}) CHECK(k->param(key));
    CHECK(!k->varying);
    // Passes its picture through unchanged
    Buf src(Buf::Rgba, 64, 36);
    for (size_t i = 0; i < src.px.size(); ++i) src.px[i] = uint8_t(i * 7);
    src.area = QRect(4, 4, 20, 10);
    Buf out;
    CHECK(runNode("tracker", {}, {{"src", src}}, {}, &out));
    CHECK(out.px == src.px && out.area == src.area && out.kind == Buf::Rgba);
    CHECK(!runNode("tracker", {}, {}, {}, &out)); // nothing in, nothing out
    // Cost per frame at 1080p (a copy of the picture; the interface hands inputs as const)
    {
        Buf big(Buf::Rgba, W, H);
        Buf o;
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 20; ++i) runNode("tracker", {}, {{"src", big}}, {}, &o);
        std::printf("node: %.2f ms per 1080p frame (runNode, copy of the input included)\n", t.nsecsElapsed() / 2e7);
    }
    // Start values from the node's values
    const TrackStart st = startOf({{"mode", "two"}, {"ax", 0.2}, {"by", 0.7}});
    CHECK(st.mode == TrackMode::Two);
    CHECK(st.a == QPointF(0.2, 0.5) && st.b == QPointF(0.6, 0.7));
    CHECK(startOf({}).mode == TrackMode::Point);
    // What followers read: not analyzed = at the start point (no jump of a mask drawn relative to ax/ay)
    const QVariantMap fresh{{"ax", 0.3}, {"ay", 0.8}};
    CHECK_EQ(trackerValue(fresh, "x"), 0.3);
    CHECK_EQ(trackerValue(fresh, "y"), 0.8);
    CHECK_EQ(trackerValue(fresh, "scale"), 1.0);
    CHECK_EQ(trackerValue(fresh, "angle"), 0.0);
    CHECK_EQ(trackerValue({}, "x"), 0.5);
    const QVariantMap done{{"ax", 0.3}, {"x", 0.42}, {"y", 0.1}, {"angle", 12.5}};
    CHECK_EQ(trackerValue(done, "x"), 0.42);
    CHECK_EQ(trackerValue(done, "y"), 0.1);
    CHECK_EQ(trackerValue(done, "angle"), 12.5);
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    const QVector<::Comp::TrackPoint> points{{5, 0.4, 0.5, 1, 0}, {6, 0.41, 0.52, 1.02, 1.5}, {8, 0.43, 0.55, 1.05, 3}};
    CHECK(!ed.trackCompNode(id, 1, points)); // no comp yet
    CompGraph g;
    g.nodes << CompNode{1, "tracker", {}} << CompNode{2, "blur", {}};
    g.out = 1;
    ed.modifyClips({id}, "Comp", [&](Clip& c) {
        EffectInstance e{::Comp::kEffectId, {}, true};
        ::Comp::setGraph(e, g);
        c.effects << e;
    });
    QUndoStack& undo = *p.undoStack();
    const int before = undo.index();
    CHECK(!ed.trackCompNode(id, 2, points)); // not a tracker
    CHECK(!ed.trackCompNode(id, 3, points)); // no such node
    CHECK(!ed.trackCompNode(id, 1, {}));
    CHECK_EQ(undo.index(), before);
    // A keyframe of x inside the range goes, one outside stays
    const AnimParam px = Keys::effectParam(::Comp::kEffectId, "n1.x"), pa = Keys::effectParam(::Comp::kEffectId, "n1.angle");
    ed.modifyClips({id}, "Keys", [&](Clip& c) {
        Keys::setKey(c, px, 7, 0.9);
        Keys::setKey(c, px, 20, 0.1);
    });
    const int keyed = undo.index();
    CHECK(ed.trackCompNode(id, 1, points));
    CHECK_EQ(undo.index(), keyed + 1); // one undo step
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK(!Keys::keyAt(*c, px, 7));
    CHECK(Keys::keyAt(*c, px, 20));
    CHECK_EQ(Keys::keyTimes(*c, {pa}), (QVector<int>{5, 6, 8}));
    CHECK(std::abs(Keys::valueAt(*c, px, 6) - 0.41) < 1e-9);
    CHECK(std::abs(Keys::valueAt(*c, Keys::effectParam(::Comp::kEffectId, "n1.y"), 8) - 0.55) < 1e-9);
    CHECK(std::abs(Keys::valueAt(*c, Keys::effectParam(::Comp::kEffectId, "n1.scale"), 7) - 1.035) < 1e-9);
    CHECK(std::abs(Keys::valueAt(*c, pa, 8) - 3) < 1e-9);
    undo.undo();
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK(Keys::keyAt(*c, px, 7));
    CHECK(Keys::keyTimes(*c, {pa}).isEmpty());

    // The clip analysis: errors for a missing node and a title
    TrackRun r = trackNode(p.format(), *c, 9, 0, 10);
    CHECK(!r.error.isEmpty());
    Clip title = *c;
    title.kind = ClipKind::Title;
    r = trackNode(p.format(), title, 1, 0, 10);
    CHECK(!r.error.isEmpty());
}

// A real clip (a red and blue checkered square 60 x 60 at (100 + 4 n, 150 + 2 n) over a moving test picture, 640 x 360; even whole pixels
// so 4:2:0 does not round the truth): the node's
// start values -> trackNode -> trackCompNode; the keyframes follow the square
void testClip(const QString& media)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({media, "moving.mp4", 75, true, false, false});
    ed.addMediaAt({media}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    CompGraph g;
    g.nodes << CompNode{3, "tracker", {}};
    g.out = 3;
    ed.modifyClips({id}, "Comp", [&](Clip& c) {
        EffectInstance e{::Comp::kEffectId, {}, true};
        ::Comp::setGraph(e, g);
        e.params["n3.ax"] = 130 / 640.0;
        e.params["n3.ay"] = 180 / 360.0;
        e.params["n3.size"] = 70 / 640.0;
        c.effects << e;
    });
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    const TrackRun r = trackNode(p.format(), *c, 3, 0, 50);
    CHECK(r.error.isEmpty());
    if (!CHECK(r.lostFrames.isEmpty())) std::printf("       lost at %d, found at %d, %d lost\n", r.lostAt, r.foundAt, int(r.lostFrames.size()));
    if (!CHECK_EQ(r.points.size(), 51)) return;
    const auto near = [](const ::Comp::TrackPoint& q, double px, double py) {
        const bool ok = std::abs(q.x * 640 - px) <= 0.5 && std::abs(q.y * 360 - py) <= 0.5;
        if (!ok) std::printf("       frame %d: %.2f, %.2f (expected %.2f, %.2f)\n", q.frame, q.x * 640, q.y * 360, px, py);
        return ok;
    };
    CHECK(near(r.points[0], 130, 180));
    for (const ::Comp::TrackPoint& q : r.points) CHECK(near(q, 130 + 4 * q.frame, 180 + 2 * q.frame));
    CHECK(ed.trackCompNode(id, 3, r.points));
    c = TimelineOps::findClip(p.timeline(), id);
    const AnimParam px = Keys::effectParam(::Comp::kEffectId, "n3.x");
    CHECK_EQ(Keys::keyTimes(*c, {px}).size(), 51);
    CHECK(std::abs(Keys::valueAt(*c, px, 50) * 640 - 330) <= 0.5);
    // What another node reads: the comp effect's animated value at clip frame 50
    CHECK(std::abs(Keys::valueAt(*c, Keys::effectParam(::Comp::kEffectId, "n3.y"), 50) * 360 - 280) <= 0.5);
}

// A portrait clip (360 x 640, the square at (100 + 4 n, 150 + 2 n) as above) in a 16:9 project: the graph works on
// the clip's own picture without the bars, so start points and results are 0..1 of that picture, not of the frame
void testPortraitClip(const QString& media)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    CHECK_EQ(p.format().width * 9, p.format().height * 16);
    p.addMedia({media, "portrait.mp4", 75, true, false, false});
    ed.addMediaAt({media}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    CompGraph g;
    g.nodes << CompNode{1, "tracker", {}};
    g.out = 1;
    ed.modifyClips({id}, "Comp", [&](Clip& c) {
        EffectInstance e{::Comp::kEffectId, {}, true};
        ::Comp::setGraph(e, g);
        e.params["n1.ax"] = 130 / 360.0;
        e.params["n1.ay"] = 180 / 640.0;
        e.params["n1.size"] = 70 / 360.0;
        c.effects << e;
    });
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    const TrackRun r = trackNode(p.format(), *c, 1, 0, 40);
    CHECK(r.error.isEmpty());
    CHECK(r.lostFrames.isEmpty());
    if (!CHECK_EQ(r.points.size(), 41)) return;
    double worst = 0;
    for (const ::Comp::TrackPoint& q : r.points)
        worst = std::max(worst, std::hypot(q.x * 360 - (130 + 4 * q.frame), q.y * 640 - (180 + 2 * q.frame)));
    std::printf("portrait clip in a 16:9 project: worst %.2f px of the clip picture\n", worst);
    // Tracked on a working picture of 247 px for the clip's 360 (the template of 70 px brought to 48): within about
    // one working pixel (the frame is rendered and the picture cut out of it, two resamplings). Off the frame's
    // coordinates it would be hundreds of pixels
    CHECK(worst <= 1.5);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    testNode();
    testPoint();
    testTwoPoints();
    testOcclusion();
    testOutlier();
    testFaces();
    testEditor();
    QTemporaryDir tmp;
    const QString moving = Check::makeMedia(
        tmp.filePath("moving.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-f", "lavfi", "-i",
         "color=c=red:s=60x60:d=3:r=25", "-filter_complex",
         "[1]drawbox=x=0:y=0:w=30:h=30:color=blue:t=fill,drawbox=x=30:y=30:w=30:h=30:color=blue:t=fill[s];"
         "[0][s]overlay=x='100+n*4':y='150+n*2'", "-c:v",
         "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "1"});
    if (CHECK(!moving.isEmpty())) {
        Check::initEngine();
        testClip(moving);
    }
    const QString portrait = Check::makeMedia(
        tmp.filePath("portrait.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=size=360x640:rate=25:duration=2", "-f", "lavfi", "-i",
         "color=c=red:s=60x60:d=2:r=25", "-filter_complex",
         "[1]drawbox=x=0:y=0:w=30:h=30:color=blue:t=fill,drawbox=x=30:y=30:w=30:h=30:color=blue:t=fill[s];"
         "[0][s]overlay=x='100+n*4':y='150+n*2'", "-c:v",
         "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "1"});
    if (CHECK(!portrait.isEmpty())) {
        Check::initEngine();
        testPortraitClip(portrait);
    }
    return Check::result();
}
