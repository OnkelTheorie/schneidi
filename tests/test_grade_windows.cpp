// Test windows of the Color page (core/GradeWindows): mask math (shapes, soft edge, rotation, invert, bounds), pixel
// work (window correction, blur, pixelate, color – only inside), parameters stored sparsely in the grade, editor
// operations with undo, keyframes, copy grade, project file, render through MLT.
#include "check.h"

#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ColorGrade.h"
#include "engine/Snapshot.h"
#include "engine/Tracker.h"
#include "ui/ColorPanel.h"
#include "ui/WindowOverlay.h"

#include <QApplication>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QThread>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

using GradeWindows::Geometry;
using GradeWindows::Mask;
using GradeWindows::Shape;

Geometry geo(Shape s, double x, double y, double w, double h, double rot = 0, double soft = 0, bool inv = false)
{
    Geometry g;
    g.shape = s;
    g.x = x;
    g.y = y;
    g.width = w;
    g.height = h;
    g.rotation = rot;
    g.softness = soft;
    g.invert = inv;
    return g;
}

void testMask()
{
    // 200 x 100 picture, ellipse in the middle, 50 % x 50 % = 100 x 50 pixels
    const Mask m(geo(Shape::Ellipse, 50, 50, 50, 50), 200, 100);
    CHECK_EQ(m.weight(100, 50), 1.0f);
    CHECK_EQ(m.weight(5, 5), 0.0f);
    CHECK_EQ(m.weight(148, 50), 1.0f);  // just inside the right edge (x 150)
    CHECK_EQ(m.weight(152, 50), 0.0f);  // without softness: one-pixel edge
    CHECK_EQ(m.weight(100, 26), 1.0f);  // inside top edge (y 25)
    CHECK_EQ(m.weight(140, 30), 0.0f);  // corner of the bounding box is outside the ellipse
    CHECK(m.bounds().contains(QRect(50, 25, 100, 50)));
    CHECK(m.bounds().width() < 110 && m.bounds().height() < 60);

    // Rectangle includes the corners
    const Mask r(geo(Shape::Rectangle, 50, 50, 50, 50), 200, 100);
    CHECK_EQ(r.weight(55, 30), 1.0f);
    CHECK_EQ(r.weight(45, 50), 0.0f);

    // Soft edge lies outside: inside stays 1, the weight falls off over softness % of the height
    const Mask s(geo(Shape::Rectangle, 50, 50, 50, 50, 0, 20), 200, 100); // 20 px edge
    CHECK_EQ(s.weight(149, 50), 1.0f);
    const float mid = s.weight(160, 50);
    CHECK(mid > 0.3f && mid < 0.7f);
    CHECK(s.weight(155, 50) > mid && s.weight(165, 50) < mid);
    CHECK_EQ(s.weight(171, 50), 0.0f);
    CHECK(s.bounds().right() >= 170);

    // Rotation by 90 degrees swaps width and height: 100 x 50 becomes 50 wide, 100 high (clipped by the picture)
    const Mask rot(geo(Shape::Rectangle, 50, 50, 50, 50, 90), 200, 100);
    CHECK_EQ(rot.weight(100, 5), 1.0f);
    CHECK_EQ(rot.weight(130, 50), 0.0f);
    CHECK_EQ(rot.weight(120, 50), 1.0f);
    // 45 degrees: the center row reaches further than half the height
    const Mask d(geo(Shape::Rectangle, 50, 50, 20, 40, 45), 200, 100); // 40 x 40 px square
    CHECK_EQ(d.weight(100 + 26, 50), 1.0f); // diagonal half = 28 px
    CHECK_EQ(d.weight(100 + 31, 50), 0.0f);

    // Inverted: outside 1, inside 0, the area is the whole picture
    const Mask inv(geo(Shape::Ellipse, 50, 50, 50, 50, 0, 0, true), 200, 100);
    CHECK_EQ(inv.weight(100, 50), 0.0f);
    CHECK_EQ(inv.weight(5, 5), 1.0f);
    CHECK_EQ(inv.bounds(), QRect(0, 0, 200, 100));

    // Window outside the picture: nothing to do
    CHECK(Mask(geo(Shape::Ellipse, 150, 50, 20, 20), 200, 100).bounds().isEmpty());
}

std::vector<uint8_t> image(int w, int h, const std::function<int(int, int)>& value)
{
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = uint8_t(value(x, y));
            p[3] = 255;
        }
    return px;
}
int at(const std::vector<uint8_t>& px, int w, int x, int y, int c = 0) { return px[(size_t(y) * w + x) * 4 + c]; }

void testPixels()
{
    const int W = 200, H = 100;
    using GradeWindows::Fill;

    // Correction only inside: gain 0.5 halves the gray inside, outside stays
    {
        ColorGrade::Window w{geo(Shape::Rectangle, 50, 50, 50, 50), {}, {}};
        w.params.gain[0] = 0.5;
        auto px = image(W, H, [](int, int) { return 200; });
        ColorGrade::applyWindow(px.data(), W, H, w);
        CHECK(std::abs(at(px, W, 100, 50) - 100) <= 2);
        CHECK_EQ(at(px, W, 10, 10), 200);
        CHECK_EQ(at(px, W, 190, 90), 200);
        CHECK_EQ(at(px, W, 100, 50, 3), 255); // alpha stays
        // Inverted: the other way round
        w.geometry.invert = true;
        auto inv = image(W, H, [](int, int) { return 200; });
        ColorGrade::applyWindow(inv.data(), W, H, w);
        CHECK_EQ(at(inv, W, 100, 50), 200);
        CHECK(std::abs(at(inv, W, 10, 10) - 100) <= 2);
    }
    // Color fill: inside the color, outside unchanged; soft edge mixes
    {
        ColorGrade::Window w{geo(Shape::Ellipse, 50, 50, 40, 60, 0, 10), {}, {}};
        w.fill.fill = Fill::Color;
        w.fill.color = QColor(255, 0, 0);
        auto px = image(W, H, [](int, int) { return 100; });
        ColorGrade::applyWindow(px.data(), W, H, w);
        CHECK_EQ(at(px, W, 100, 50, 0), 255);
        CHECK_EQ(at(px, W, 100, 50, 1), 0);
        CHECK_EQ(at(px, W, 5, 5, 0), 100);
        const int edge = at(px, W, 145, 50, 1); // 5 px into the 10 px edge
        CHECK(edge > 10 && edge < 90);
    }
    // Blur: checkerboard turns gray inside, outside stays sharp
    {
        ColorGrade::Window w{geo(Shape::Rectangle, 50, 50, 40, 60), {}, {}};
        w.fill.fill = Fill::Blur;
        w.fill.amount = 100; // sigma = 5 px at 100 px height
        auto px = image(W, H, [](int x, int y) { return ((x / 2 + y / 2) % 2) ? 255 : 0; });
        const auto before = px;
        ColorGrade::applyWindow(px.data(), W, H, w);
        CHECK(std::abs(at(px, W, 100, 50) - 128) < 20);
        CHECK(std::abs(at(px, W, 101, 51) - 128) < 20);
        CHECK_EQ(at(px, W, 10, 10), at(before, W, 10, 10));
        CHECK_EQ(at(px, W, 11, 10), at(before, W, 11, 10));
        // amount 0 = no blur
        w.fill.amount = 0;
        auto sharp = before;
        ColorGrade::applyWindow(sharp.data(), W, H, w);
        CHECK(sharp == before);
    }
    // Pixelate: blocks on a grid from the picture corner (100 % = 1/10 of the height = 10 px)
    {
        ColorGrade::Window w{geo(Shape::Rectangle, 50, 50, 40, 60), {}, {}};
        w.fill.fill = Fill::Pixelate;
        w.fill.amount = 100;
        auto px = image(W, H, [](int x, int) { return x; });
        ColorGrade::applyWindow(px.data(), W, H, w);
        CHECK_EQ(at(px, W, 100, 50), at(px, W, 109, 59)); // one block 100..109
        CHECK(std::abs(at(px, W, 100, 50) - 104) <= 1);   // average of 100..109
        CHECK(at(px, W, 110, 50) != at(px, W, 109, 50));
        CHECK_EQ(at(px, W, 10, 10), 10);                  // outside unchanged
    }
    // A window that does nothing changes nothing
    {
        const ColorGrade::Window w{geo(Shape::Rectangle, 50, 50, 40, 60), {}, {}};
        CHECK(w.isNoop());
        auto px = image(W, H, [](int x, int y) { return (x * 7 + y) % 256; });
        const auto before = px;
        ColorGrade::applyWindow(px.data(), W, H, w);
        CHECK(px == before);
    }
}

Clip videoClip(int id, int start, int len)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.mp4";
    c.start = start;
    c.out = len - 1;
    return c;
}

void testModel(const QString& dir)
{
    // Grade without windows: no window values in the instance (file stays small)
    Clip c = videoClip(1, 0, 100);
    EffectRegistry::add(c, "grade");
    const EffectInstance* g = EffectRegistry::instance(c, "grade");
    CHECK(!g->params.contains("w1.x"));
    CHECK(g->params.contains("liftY"));
    CHECK(GradeWindows::list(c).isEmpty());
    CHECK_EQ(GradeWindows::freeNumber(c), 1);
    CHECK(!ColorGrade::active(c));

    GradeWindows::add(c, 2, Shape::Rectangle);
    CHECK_EQ(GradeWindows::list(c), QVector<int>{2});
    CHECK_EQ(GradeWindows::freeNumber(c), 1);
    // Window without correction/fill: does nothing, the grade stays inactive
    CHECK(!ColorGrade::active(c));
    Keys::setValue(c, GradeWindows::param(2, "exposure"), 0, 1.0);
    CHECK(ColorGrade::active(c));
    CHECK_EQ(ColorGrade::windowAt(c, 2, 0).exposure, 1.0);
    CHECK_EQ(ColorGrade::windowAt(c, 1, 0).exposure, 0.0);
    CHECK_EQ(ColorGrade::windowsAt(c, 0).size(), 1);
    // Keyframes on the position
    Keys::setKey(c, GradeWindows::param(2, "x"), 0, 20);
    Keys::setKey(c, GradeWindows::param(2, "x"), 10, 40);
    CHECK_EQ(GradeWindows::geometryAt(c, 2, 5).x, 30.0);
    CHECK(GradeWindows::geometryAt(c, 2, 5).shape == Shape::Rectangle);
    CHECK(Keys::label(GradeWindows::param(2, "x")).contains("2"));
    // Removing takes values and keyframes along, the grade stays
    GradeWindows::remove(c, 2);
    CHECK(GradeWindows::list(c).isEmpty());
    CHECK(!Keys::hasKeys(c));
    CHECK(EffectRegistry::has(c, "grade"));
    for (auto it = g->params.cbegin(); it != g->params.cend(); ++it) CHECK(GradeWindows::windowOf(it.key()) == 0);

    // Editor with undo
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/a.mp4"}, 100, 0);
    QUndoStack& undo = *p.undoStack();
    const int id1 = p.timeline().video[0].clips[0].id, id2 = p.timeline().video[0].clips[1].id;
    ed.addGradeWindow({id1}, 1, "ellipse");
    const Clip* c1 = TimelineOps::findClip(p.timeline(), id1);
    CHECK(GradeWindows::exists(*c1, 1));
    ed.setGradeValues({id1, id2}, {{GradeWindows::param(1, "saturation"), 0.0}}, 0, "Color");
    CHECK_EQ(ColorGrade::windowAt(*TimelineOps::findClip(p.timeline(), id1), 1, 0).saturation, 0.0);
    // clip 2 has no window 1: no stray values
    const Clip* c2 = TimelineOps::findClip(p.timeline(), id2);
    CHECK(!c2 || !EffectRegistry::has(*c2, "grade") ||
          !EffectRegistry::instance(*c2, "grade")->params.contains("w1.saturation"));
    ed.setGradeWindowOption({id1}, 1, "fill", "blur");
    ed.setGradeWindowOption({id1}, 1, "invert", true);
    c1 = TimelineOps::findClip(p.timeline(), id1);
    CHECK(GradeWindows::fillAt(*c1, 1, 0).fill == GradeWindows::Fill::Blur);
    CHECK(GradeWindows::geometryAt(*c1, 1, 0).invert);
    // Keyframe of the window: its values get keys, the grade's own values not
    ed.setGradeKeyframe({id1}, 20, true, 1);
    c1 = TimelineOps::findClip(p.timeline(), id1);
    CHECK(Keys::keyAt(*c1, GradeWindows::param(1, "x"), 20));
    CHECK(Keys::keyAt(*c1, GradeWindows::param(1, "saturation"), 20));
    CHECK(!Keys::keyAt(*c1, AnimParam::GradeSaturation, 20));
    undo.undo();
    CHECK(!Keys::hasKeys(*TimelineOps::findClip(p.timeline(), id1)));

    // Copy grade includes the windows (like a DaVinci node with its windows)
    CHECK(ed.copyGrade(id1));
    ed.pasteGrade({id2});
    c2 = TimelineOps::findClip(p.timeline(), id2);
    CHECK(GradeWindows::exists(*c2, 1));
    CHECK(GradeWindows::fillAt(*c2, 1, 0).fill == GradeWindows::Fill::Blur);

    // Project file: window values and keyframes survive
    ed.setGradeKeyframe({id1}, 30, true, 1);
    ed.setGradeValues({id1}, {{GradeWindows::param(1, "x"), 70.0}}, 30, "Move");
    const QString path = dir + "/w.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData q;
    CHECK(ProjectFile::load(path, &q, &error));
    const Clip* l1 = TimelineOps::findClip(q.timeline, id1);
    c1 = TimelineOps::findClip(p.timeline(), id1);
    if (CHECK(l1 && c1)) {
        CHECK(*l1 == *c1);
        CHECK(GradeWindows::exists(*l1, 1));
        CHECK_EQ(GradeWindows::geometryAt(*l1, 1, 30).x, 70.0);
    }

    // Remove with undo
    ed.removeGradeWindow({id1}, 1);
    CHECK(!GradeWindows::exists(*TimelineOps::findClip(p.timeline(), id1), 1));
    undo.undo();
    CHECK(GradeWindows::exists(*TimelineOps::findClip(p.timeline(), id1), 1));
}

// Smooth texture around a moving point (dx, dy): sub-pixel shifts change the picture
Tracker::Gray pattern(int w, int h, double dx, double dy)
{
    Tracker::Gray g;
    g.width = w;
    g.height = h;
    g.px.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double u = x - 60 - dx, v = y - 40 - dy;
            const double blob = std::exp(-(u * u + v * v) / 60.0) + 0.6 * std::exp(-((u - 5) * (u - 5) + (v + 4) * (v + 4)) / 20.0);
            // the fine texture belongs to the object and moves with it (a still background inside the window pulls a
            // template matcher towards "no movement")
            g.px[size_t(y) * w + x] = float(0.2 + 0.5 * blob + 0.05 * std::sin(u * 0.3) * std::cos(v * 0.2));
        }
    return g;
}

void testTrackerMath()
{
    Tracker::PointTracker t(pattern(160, 100, 0, 0), QRectF(48, 28, 24, 24));
    CHECK(t.valid());
    CHECK_EQ(t.center(), QPointF(60, 40));
    // Moves 3.4 px right and 1.7 px up per picture
    for (int i = 1; i <= 8; ++i) {
        CHECK(t.next(pattern(160, 100, 3.4 * i, -1.7 * i)));
        if (!CHECK(std::abs(t.center().x() - (60 + 3.4 * i)) < 0.35 && std::abs(t.center().y() - (40 - 1.7 * i)) < 0.35))
            std::printf("       picture %d: %.3f, %.3f (expected %.3f, %.3f)\n", i, t.center().x(), t.center().y(),
                        60 + 3.4 * i, 40 - 1.7 * i);
    }
    CHECK(t.score() > 0.9);
    // Nothing there any more: lost, position stays
    Tracker::Gray flat;
    flat.width = 160;
    flat.height = 100;
    flat.px.assign(size_t(160) * 100, 0.5f);
    const QPointF before = t.center();
    CHECK(!t.next(flat));
    CHECK_EQ(t.center(), before);
}

void drag(WindowOverlay& ov, const QRectF& frame, QPointF from, QPointF to, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, mods);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, mods);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, mods);
    CHECK(ov.mousePress(&press, frame));
    ov.mouseMove(&move, frame);
    ov.mouseRelease(&release, frame);
}

void testOverlayAndPanel()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    ed.addGradeWindow({id}, 1, "rectangle");
    const auto win = [&] { return GradeWindows::geometryAt(*TimelineOps::findClip(p.timeline(), id), 1, 10); };

    // Project 1920 x 1080 shown at half size
    const QRectF frame(10, 20, 960, 540);
    WindowOverlay ov(&ed, [&] { return QVector<int>{id}; });
    ov.setTarget(id, 1, 10);
    CHECK(ov.centerPos(frame).isNull()); // hidden: nothing to grab
    ov.setVisible(true);
    CHECK_EQ(ov.centerPos(frame), QPointF(490, 290));
    CHECK_EQ(ov.handlePos(3, frame), QPointF(490 + 192, 290)); // 40 % of 960 / 2

    // Move: 96 widget pixels = 10 % of the width; one undo step
    const int undoBefore = p.undoStack()->count();
    drag(ov, frame, QPointF(490, 290), QPointF(490 + 96, 290 + 27));
    CHECK(std::abs(win().x - 60) < 1e-6);
    CHECK(std::abs(win().y - 55) < 1e-6);
    CHECK_EQ(p.undoStack()->count(), undoBefore + 1);
    // Right handle: width around the center
    drag(ov, frame, ov.handlePos(3, frame), ov.centerPos(frame) + QPointF(288, 0));
    CHECK(std::abs(win().width - 60) < 1e-6);
    CHECK(std::abs(win().height - 40) < 1e-6);
    // Rotate knob to the right of the center = -90 degrees (counterclockwise positive)
    drag(ov, frame, ov.handlePos(8, frame), ov.centerPos(frame) + QPointF(100, 0));
    CHECK(std::abs(win().rotation + 90) < 1e-6);
    // Clicks outside the window are left to the viewer
    QMouseEvent outside(QEvent::MouseButtonPress, QPointF(15, 25), QPointF(15, 25), Qt::LeftButton, Qt::LeftButton, {});
    CHECK(!ov.mousePress(&outside, frame));
    // Keyframed position: dragging sets a keyframe at the playhead
    ed.setGradeKeyframe({id}, 0, true, 1);
    ov.setTarget(id, 1, 40);
    drag(ov, frame, ov.centerPos(frame), ov.centerPos(frame) + QPointF(-96, 0));
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK(Keys::keyAt(*c, GradeWindows::param(1, "x"), 40));
    CHECK(std::abs(GradeWindows::geometryAt(*c, 1, 40).x - 50) < 1e-6);
    CHECK(std::abs(GradeWindows::geometryAt(*c, 1, 0).x - 60) < 1e-6);

    // Panel: the layer follows the windows; a deleted window falls back to the whole picture
    ColorPanel panel(&ed);
    panel.setPlayhead(10);
    panel.show();
    panel.setLayer(1);
    CHECK_EQ(panel.layer(), 1);
    panel.setLayer(3); // does not exist
    CHECK_EQ(panel.layer(), 0);
    panel.setLayer(1);
    // Keyframe button of the window layer keys the window, not the whole picture
    ed.setGradeKeyframe({id}, 60, true, panel.layer());
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK(Keys::keyAt(*c, GradeWindows::param(1, "saturation"), 60));
    CHECK(!Keys::keyAt(*c, AnimParam::GradeSaturation, 60));
    ed.removeGradeWindow({id}, 1);
    CHECK_EQ(panel.layer(), 0);
}

void testRender(const QString& media)
{
    ProjectFormat fmt;
    Timeline tl;
    tl.video.resize(1);
    Clip c = videoClip(1, 0, 50);
    c.mediaPath = media;
    tl.video[0].clips << c;
    const QImage ref = Snapshot::timeline(fmt, tl, 10, 480);
    if (!CHECK(!ref.isNull())) return;
    // Window on the right half, filled black: right half black, left half as before
    Clip& w = tl.video[0].clips[0];
    EffectRegistry::add(w, "grade");
    GradeWindows::add(w, 1, Shape::Rectangle);
    auto* g = EffectRegistry::instance(w, "grade");
    g->params["w1.x"] = 75.0;
    g->params["w1.width"] = 50.0;
    g->params["w1.height"] = 100.0;
    g->params["w1.softness"] = 0.0;
    g->params["w1.fill"] = "color";
    g->params["w1.color"] = QColor(Qt::black);
    const QImage img = Snapshot::timeline(fmt, tl, 10, 480);
    if (!CHECK(img.size() == ref.size())) return;
    const QPoint right(img.width() * 3 / 4, img.height() / 3), left(img.width() / 8, img.height() / 3);
    CHECK(qGray(img.pixel(right)) < 10);
    CHECK(qGray(ref.pixel(right)) > 40);
    CHECK(std::abs(qGray(img.pixel(left)) - qGray(ref.pixel(left))) <= 2);
    // Grade switched off: windows off too
    g->enabled = false;
    const QImage off = Snapshot::timeline(fmt, tl, 10, 480);
    CHECK(std::abs(qGray(off.pixel(right)) - qGray(ref.pixel(right))) <= 2);
}

// A red square moves over a test picture: the window follows it, forwards and backwards
void testTrack(const QString& media)
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({media, "moving.mp4", 75, true, false, false});
    ed.addMediaAt({media}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    ed.addGradeWindow({id}, 1, "rectangle");
    // Square 60 x 60 at (100 + 80 t, 150 + 20 t) in 640 x 360 -> center at t = 0: 130/640, 180/360
    const auto pct = [](double px, double size) { return px / size * 100; };
    ed.setGradeValues({id}, {{GradeWindows::param(1, "x"), pct(130, 640)}, {GradeWindows::param(1, "y"), pct(180, 360)},
                             {GradeWindows::param(1, "width"), pct(70, 640)}, {GradeWindows::param(1, "height"), pct(70, 360)}},
                      0, "Place");
    const Clip c = *TimelineOps::findClip(p.timeline(), id);
    int calls = 0;
    const Tracker::Result r = Tracker::trackWindow(p.format(), c, 1, 0, 50, [&](int done, int total) {
        ++calls;
        CHECK(done <= total && total == 50);
    });
    CHECK(r.error.isEmpty());
    CHECK(!r.lost);
    CHECK_EQ(r.points.size(), 50);
    CHECK_EQ(calls, 50);
    if (r.points.size() == 50) {
        // Frame 50 = 2 s: center at 290/640, 220/360 (project 1920 x 1080 shows the same picture scaled)
        CHECK(std::abs(r.points.last().x - pct(290, 640)) < 0.6);
        CHECK(std::abs(r.points.last().y - pct(220, 360)) < 0.8);
        CHECK(std::abs(r.points[24].x - pct(130 + 80 * 25 / 25.0, 640)) < 0.6);
    }
    ed.setGradeWindowPath(id, 1, 0, r.points);
    const Clip* t = TimelineOps::findClip(p.timeline(), id);
    CHECK(Keys::keyAt(*t, GradeWindows::param(1, "x"), 0));
    CHECK(Keys::keyAt(*t, GradeWindows::param(1, "x"), 50));
    CHECK(std::abs(GradeWindows::geometryAt(*t, 1, 50).x - pct(290, 640)) < 0.6);
    // Backwards from frame 50 finds the start again; the keyframes there are replaced
    const Tracker::Result back = Tracker::trackWindow(p.format(), *t, 1, 50, 10);
    CHECK_EQ(back.points.size(), 40);
    if (!back.points.isEmpty()) {
        CHECK_EQ(back.points.last().frame, 10);
        if (!CHECK(std::abs(back.points.last().x - pct(130 + 80 * 10 / 25.0, 640)) < 0.8))
            std::printf("       frame 10: x %.3f %% (expected %.3f %%), y %.3f %%\n", back.points.last().x,
                        pct(130 + 80 * 10 / 25.0, 640), back.points.last().y);
    }
    ed.setGradeWindowPath(id, 1, 50, back.points);
    t = TimelineOps::findClip(p.timeline(), id);
    CHECK_EQ(Keys::keyTimes(*t, {GradeWindows::param(1, "x")}).size(), 51);
    // Cancel right away: nothing tracked
    std::atomic<bool> stop{true};
    const Tracker::Result none = Tracker::trackWindow(p.format(), *t, 1, 0, 20, {}, &stop);
    CHECK(none.canceled);
    CHECK(none.points.isEmpty());
    // No window 2
    CHECK(!Tracker::trackWindow(p.format(), *t, 2, 0, 20).error.isEmpty());

    // Color page: tracking runs in the background and sets the keyframes when done
    ColorPanel panel(&ed);
    panel.setPlayhead(0);
    panel.show();
    panel.setLayer(1);
    QSignalSpy spy(&panel, &ColorPanel::trackingFinished);
    panel.startTracking(true);
    CHECK(panel.tracking());
    CHECK(spy.wait(120000));
    CHECK(!panel.tracking());
    t = TimelineOps::findClip(p.timeline(), id);
    CHECK(Keys::keyAt(*t, GradeWindows::param(1, "x"), 74));
    // Another project opened meanwhile: the run is dropped (its clip id may name another clip there)
    p.edit("keys", [&](Timeline& tl) {
        Clip* c = TimelineOps::findClip(tl, id);
        for (AnimParam k : {GradeWindows::param(1, "x"), GradeWindows::param(1, "y")})
            for (int frame : Keys::keyTimes(*c, {k}))
                if (frame > 0) Keys::removeKey(*c, k, frame);
    });
    panel.startTracking(true);
    // Let it finish first: its whole result is waiting. QThread::wait, not a QSignalSpy on finished() polled from here:
    // the spy is filled in the worker thread itself (thread sanitizer: data race)
    if (QThread* worker = panel.findChild<QThread*>()) worker->wait(120000);
    panel.discardTracking();
    CHECK(spy.wait(120000));
    CHECK(!panel.tracking());
    t = TimelineOps::findClip(p.timeline(), id);
    CHECK_EQ(Keys::keyTimes(*t, {GradeWindows::param(1, "x")}).size(), 1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("grade-windows");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    testMask();
    testPixels();
    testModel(tmp.path());
    testOverlayAndPanel();
    testTrackerMath();

    if (!Check::haveFfmpeg()) {
        std::printf("Hinweis: ffmpeg fehlt, Render-Test übersprungen\n");
        return Check::result();
    }
    const QString media = Check::makeMedia(tmp.filePath("bars.mp4"),
                                           {"-f", "lavfi", "-i", "smptebars=size=640x360:rate=25:duration=3", "-c:v",
                                            "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testRender(media);
    const QString moving = Check::makeMedia(
        tmp.filePath("moving.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=3", "-f", "lavfi", "-i",
         "color=c=red:s=60x60:d=3:r=25", "-filter_complex", "[0][1]overlay=x='100+t*80':y='150+t*20'", "-c:v",
         "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "1"});
    if (CHECK(!moving.isEmpty())) testTrack(moving);
    return Check::result();
}
