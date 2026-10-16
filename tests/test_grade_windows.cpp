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

#include <QApplication>
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
    return Check::result();
}
