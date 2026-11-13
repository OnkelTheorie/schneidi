// SPDX-License-Identifier: AGPL-3.0-only
// Shapes (0.22, core/Shapes.h): shape titles drawn with known pixels (each kind at its edge and centre), the stroke and
// its self-drawing reveal, keyframes of size, project file round trip (and what an older version reads), the picture
// below a shape (blur / pixelate inside, unchanged outside, spotlight), a clip cut to a shape (clip_shape), a shape
// following a tracker of the clip below, the CLI ops shape / clip_shape / keyframe and `check` (safe areas).
// Synthetic pictures only. SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "render_check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/Selection.h"
#include "ui/Inspector.h"
#include "ui/ShapeOverlay.h"
#include <QMouseEvent>
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/EffectRegistry.h"
#include "core/Shapes.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFormat{320, 180, {25, 1}};
const QColor kRed(255, 0, 0), kWhite(255, 255, 255), kBlack(0, 0, 0);
QString g_stripes, g_gray, g_red, g_blue; // stills 320 x 180

QImage render(const Timeline& tl, int frame, const ProjectFormat& fmt = kFormat)
{
    const QImage img = Check::renderFrame(tl, fmt, frame);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull()) {
        static int n = 0;
        img.save(QString("%1/shapes-%2.png").arg(dump).arg(n++, 3, 10, QChar('0')));
    }
    return img;
}

bool near(QRgb a, const QColor& b, int tol = 12)
{
    return std::abs(qRed(a) - b.red()) <= tol && std::abs(qGreen(a) - b.green()) <= tol && std::abs(qBlue(a) - b.blue()) <= tol;
}
bool at(const QImage& img, int x, int y, const QColor& c, int tol = 12)
{
    const bool ok = near(img.pixel(x, y), c, tol);
    if (!ok) std::printf("       pixel %d,%d = %s, expected %s\n", x, y, qPrintable(QColor(img.pixel(x, y)).name()), qPrintable(c.name()));
    return ok;
}

Clip shapeClip(int id, ShapeKind kind, double w, double h, int length = 25)
{
    Clip c;
    c.id = id;
    c.kind = ClipKind::Title;
    c.title.kind = TitleKind::Shape;
    c.title.text.clear();
    c.title.color = kRed;
    c.title.shape.kind = kind;
    c.title.shape.width = w;
    c.title.shape.height = h;
    c.out = length - 1;
    return c;
}

Clip media(int id, const QString& path, int length = 25)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.out = length - 1;
    return c;
}

Timeline timelineOf(QVector<Clip> v1, QVector<Clip> v2 = {})
{
    Timeline tl = emptyTimeline();
    TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
    tl.video[0].clips = std::move(v1);
    tl.video[1].clips = std::move(v2);
    return tl;
}

QString still(const QString& path, const std::function<void(QImage&)>& paint)
{
    QImage img(320, 180, QImage::Format_RGB32);
    img.fill(Qt::black);
    paint(img);
    img.save(path);
    return path;
}

// ---- each kind: pixels inside, at and outside its edge (100 x 100 box in the middle: 110..210, 40..140) ----
void testKinds()
{
    std::printf("kinds\n");
    auto one = [](ShapeKind k, double w = 100, double h = 100, const std::function<void(Clip&)>& more = {}) {
        Clip c = shapeClip(1, k, w, h);
        if (more) more(c);
        return render(timelineOf({c}), 0);
    };
    {
        const QImage img = one(ShapeKind::Ellipse);
        CHECK(at(img, 160, 90, kRed));
        CHECK(at(img, 207, 90, kRed));   // inside the right edge (radius 50)
        CHECK(at(img, 213, 90, kBlack)); // outside
        CHECK(at(img, 160, 43, kRed));
        CHECK(at(img, 205, 135, kBlack)); // the box's corner is outside a circle
    }
    {
        const QImage img = one(ShapeKind::Rect);
        CHECK(at(img, 205, 135, kRed)); // corner inside a rectangle
        CHECK(at(img, 213, 90, kBlack));
        const QImage round = one(ShapeKind::Rect, 100, 100, [](Clip& c) { c.title.shape.corner = 40; });
        CHECK(at(round, 207, 90, kRed));
        CHECK(at(round, 207, 137, kBlack)); // rounded off
    }
    {
        const QImage img = one(ShapeKind::Triangle);
        CHECK(at(img, 160, 135, kRed));  // bottom edge middle
        CHECK(at(img, 205, 135, kRed));  // bottom right corner
        CHECK(at(img, 115, 48, kBlack)); // top left of the box: outside
        CHECK(at(img, 160, 60, kRed));   // below the tip
    }
    {
        const QImage img = one(ShapeKind::Arrow, 120, 60);
        CHECK(at(img, 215, 90, kRed));   // near the tip (x 220)
        CHECK(at(img, 110, 90, kRed));   // shaft
        CHECK(at(img, 110, 75, kBlack)); // beside the shaft (half width 12)
        CHECK(at(img, 175, 72, kRed));   // the head is wider than the shaft
        CHECK(at(img, 224, 90, kBlack)); // past the tip
    }
    {
        Clip c = shapeClip(1, ShapeKind::Line, 200, 10);
        const QImage img = render(timelineOf({c}), 0);
        CHECK(at(img, 160, 90, kRed));
        CHECK(at(img, 80, 90, kRed));
        CHECK(at(img, 160, 100, kBlack));
    }
    {
        const QImage img = one(ShapeKind::Star);
        CHECK(at(img, 160, 90, kRed));
        CHECK(at(img, 160, 45, kRed)); // top tip
        // between the two upper tips at the outer radius: outside
        const double a = (-90 + 36) * M_PI / 180;
        CHECK(at(img, int(160 + 46 * std::cos(a)), int(90 + 46 * std::sin(a)), kBlack));
    }
    {
        const QImage img = one(ShapeKind::Polygon, 100, 100, [](Clip& c) { c.title.shape.points = 4; });
        // a diamond: tips on the axes, the box's corner outside
        CHECK(at(img, 160, 44, kRed));
        CHECK(at(img, 206, 90, kRed));
        CHECK(at(img, 200, 50, kBlack));
    }
    { // rotation: a 160 x 20 rectangle turned 90 degrees stands upright
        const QImage img = one(ShapeKind::Rect, 160, 20, [](Clip& c) { c.title.shape.rotation = 90; });
        CHECK(at(img, 160, 20, kRed));
        CHECK(at(img, 230, 90, kBlack));
    }
    { // opacity of the clip
        const QImage img = one(ShapeKind::Rect, 100, 100, [](Clip& c) { c.transform.opacity = 50; });
        CHECK(at(img, 160, 90, QColor(128, 0, 0), 8));
    }
}

// ---- stroke, its reveal (keyframes 0 -> 100 %), shadow ----
void testStroke()
{
    std::printf("stroke\n");
    Clip c = shapeClip(1, ShapeKind::Ellipse, 100, 100);
    c.title.shape.fill = ShapeFill::None;
    c.title.outlineOn = true;
    c.title.outlineColor = kWhite;
    c.title.outlineWidth = 6;
    {
        const QImage img = render(timelineOf({c}), 0);
        CHECK(at(img, 210, 90, kWhite));
        CHECK(at(img, 110, 90, kWhite));
        CHECK(at(img, 160, 90, kBlack)); // no fill
    }
    // Draws itself: from the top clockwise, half at frame 10 of 0..20
    Keys::setKey(c, AnimParam::ShapeReveal, 0, 0);
    Keys::setKey(c, AnimParam::ShapeReveal, 20, 100);
    {
        const QImage none = render(timelineOf({c}), 0);
        CHECK(at(none, 210, 90, kBlack));
        CHECK(at(none, 160, 40, kBlack));
        const QImage half = render(timelineOf({c}), 10);
        CHECK(at(half, 210, 90, kWhite)); // the right side (a quarter) is drawn
        CHECK(at(half, 110, 90, kBlack)); // the left (three quarters) is not yet
        const QImage all = render(timelineOf({c}), 20);
        CHECK(at(all, 110, 90, kWhite));
    }
    // Drop shadow: below right of a white square, grey on black
    Clip s = shapeClip(1, ShapeKind::Rect, 60, 60);
    s.title.color = kWhite;
    s.title.look.shadowOn = true;
    s.title.look.shadowX = 20;
    s.title.look.shadowY = 20;
    s.title.look.shadowSoft = 0;
    s.title.look.shadowColor = QColor(0, 0, 255);
    s.title.look.shadowOpacity = 1;
    const QImage img = render(timelineOf({s}), 0);
    CHECK(at(img, 160, 90, kWhite));
    CHECK(at(img, 205, 135, QColor(0, 0, 255))); // shadow only (the square ends at 190)
    CHECK(at(img, 120, 90, kBlack));
}

// ---- keyframes of the size ----
void testKeyframes()
{
    std::printf("keyframes\n");
    Clip c = shapeClip(1, ShapeKind::Rect, 50, 50);
    Keys::setKey(c, AnimParam::ShapeWidth, 0, 50);
    Keys::setKey(c, AnimParam::ShapeWidth, 10, 150);
    Keys::setKey(c, AnimParam::TitlePosY, 0, 0);
    Keys::setKey(c, AnimParam::TitlePosY, 10, 40); // up
    const QImage a = render(timelineOf({c}), 0), b = render(timelineOf({c}), 10);
    CHECK(at(a, 230, 90, kBlack));
    CHECK(at(b, 230, 50, kRed)); // 75 px right of the middle, 40 px higher
    CHECK(at(b, 160, 90, kBlack));
}

// ---- project file: every field round trip, an older version reads an empty text title ----
void testProjectFile(const QString& dir)
{
    std::printf("project file\n");
    Project p;
    ProjectData d;
    d.timeline = timelineOf({});
    d.format = kFormat;
    p.load(d);
    Clip c = shapeClip(p.newClipId(), ShapeKind::Star, 120, 80);
    ShapeStyle& s = c.title.shape;
    s.rotation = 12;
    s.corner = 4;
    s.points = 7;
    s.inner = 0.3;
    s.fill = ShapeFill::Pixelate;
    s.amount = 33;
    s.feather = 5;
    s.spotlight = 40;
    s.reveal = 70;
    s.followClip = 9;
    s.followNode = 2;
    c.title.outlineOn = true;
    c.title.look.shadowOn = true;
    Keys::setKey(c, AnimParam::ShapeRotation, 0, 0);
    Keys::setKey(c, AnimParam::ShapeRotation, 5, 90);
    Keys::setKey(c, AnimParam::ShapeReveal, 3, 10);
    Clip m = media(p.newClipId(), g_red);
    EffectRegistry::add(m, Shapes::kClipShapeEffect);
    EffectRegistry::instance(m, Shapes::kClipShapeEffect)->params["shape"] = "star";
    EffectRegistry::instance(m, Shapes::kClipShapeEffect)->params["size"] = 70.0;
    EffectRegistry::instance(m, Shapes::kClipShapeEffect)->params["stroke_width"] = 6.0;
    p.edit("setup", [&](Timeline& tl) {
        tl.video[0].clips << m;
        tl.video[1].clips << c;
    });
    const QString path = dir + "/shapes.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline == p.timeline());
    // An older version: kind "shape" unknown -> a text title with the stored text, which is empty (draws nothing)
    const QJsonObject t = ProjectFile::clipJson(c).value("title").toObject();
    CHECK_EQ(t.value("kind").toString(), QString("shape"));
    CHECK(t.value("text").toString().isEmpty());
    CHECK(t.value("shape").toObject().value("points").toInt() == 7);
    // A format change scales the shape with the picture
    Timeline twice = p.timeline();
    scaleTimeline(twice, QSize(320, 180), QSize(640, 360));
    const Clip* big = TimelineOps::findClip(twice, c.id);
    CHECK(big && std::abs(big->title.shape.width - 240) < 1e-9 && std::abs(big->title.shape.height - 160) < 1e-9);
    // clip_shape's pixel values with it (stroke, shadow softness; percent values stay)
    const Clip* bm = TimelineOps::findClip(twice, m.id);
    CHECK(bm && std::abs(EffectRegistry::value(*bm, Shapes::kClipShapeEffect, "stroke_width").toDouble() - 12) < 1e-9);
    CHECK(bm && std::abs(EffectRegistry::value(*bm, Shapes::kClipShapeEffect, "shadow_soft").toDouble() - 24) < 1e-9);
    CHECK(bm && std::abs(EffectRegistry::value(*bm, Shapes::kClipShapeEffect, "size").toDouble() - 70) < 1e-9);
}

// Mean of one channel over a rectangle
double mean(const QImage& img, QRect r, int channel)
{
    double s = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb p = img.pixel(x, y);
            s += channel == 0 ? qRed(p) : channel == 1 ? qGreen(p) : qBlue(p);
        }
    return s / (r.width() * r.height());
}
// Largest difference of neighbouring pixels in a row range (stripes: 255, blurred: small)
int contrast(const QImage& img, QRect r)
{
    int most = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x < r.right(); ++x) most = std::max(most, std::abs(qRed(img.pixel(x, y)) - qRed(img.pixel(x + 1, y))));
    return most;
}

// ---- the picture below: blur / pixelate inside, untouched outside; spotlight ----
void testBelow()
{
    std::printf("below\n");
    const QImage plain = render(timelineOf({media(1, g_stripes)}), 0);
    CHECK(contrast(plain, QRect(140, 70, 40, 40)) > 200);
    Clip blur = shapeClip(2, ShapeKind::Rect, 100, 100);
    blur.title.shape.fill = ShapeFill::Blur;
    blur.title.shape.amount = 60;
    const QImage b = render(timelineOf({media(1, g_stripes)}, {blur}), 0);
    CHECK(contrast(b, QRect(140, 70, 40, 40)) < 30);             // blurred inside
    CHECK(std::abs(mean(b, QRect(140, 70, 40, 40), 0) - 127) < 20); // to grey
    bool same = true;                                            // untouched outside
    for (QRect r : {QRect(0, 0, 100, 180), QRect(220, 0, 100, 180), QRect(100, 0, 120, 30), QRect(100, 150, 120, 30)})
        for (int y = r.top(); y <= r.bottom(); ++y)
            for (int x = r.left(); x <= r.right(); ++x) same = same && b.pixel(x, y) == plain.pixel(x, y);
    CHECK(same);
    // Not on the GPU path: the CPU frame is what the card would refuse - the shape still has its fill none
    Clip pix = blur;
    pix.title.shape.fill = ShapeFill::Pixelate;
    pix.title.shape.amount = 50; // blocks of 9 px at 180 high
    const QImage px = render(timelineOf({media(1, g_stripes)}, {pix}), 0);
    // inside one block: one colour
    const int bx = 18 * 9, by = 10 * 9;
    bool flat = true;
    for (int y = by; y < by + 9; ++y)
        for (int x = bx; x < bx + 9; ++x) flat = flat && px.pixel(x, y) == px.pixel(bx, by);
    CHECK(flat);
    CHECK(px.pixel(20, 20) == plain.pixel(20, 20));
    // Spotlight: grey 200 picture, a circle in the middle, outside half as bright
    Clip spot = shapeClip(2, ShapeKind::Ellipse, 100, 100);
    spot.title.shape.fill = ShapeFill::None;
    spot.title.shape.spotlight = 50;
    const QImage sp = render(timelineOf({media(1, g_gray)}, {spot}), 0);
    CHECK(at(sp, 160, 90, QColor(200, 200, 200), 3));
    CHECK(at(sp, 20, 20, QColor(100, 100, 100), 3));
    // A fade halves it: opacity 50 % = a quarter darker outside
    spot.transform.opacity = 50;
    const QImage half = render(timelineOf({media(1, g_gray)}, {spot}), 0);
    CHECK(at(half, 20, 20, QColor(150, 150, 150), 3));
}

// ---- clip in a shape ----
void testClipShape()
{
    std::printf("clip shape\n");
    Clip m = media(1, g_red);
    EffectRegistry::add(m, Shapes::kClipShapeEffect);
    EffectInstance* e = EffectRegistry::instance(m, Shapes::kClipShapeEffect);
    e->params["size"] = 80.0; // a circle 144 px high (radius 72)
    e->params["stroke_width"] = 6.0;
    e->params["stroke_color"] = QColor(255, 255, 255);
    const QImage img = render(timelineOf({m}), 0);
    CHECK(at(img, 160, 90, kRed));
    CHECK(at(img, 10, 10, kBlack));   // outside the circle: transparent (the black background)
    CHECK(at(img, 300, 90, kBlack));  // a circle, not the picture's width
    CHECK(at(img, 232, 90, kWhite));  // the stroke on the outline
    CHECK(at(img, 160, 20, kWhite));
    // rect with the clip's transform: moved right by 60 px with the picture
    e->params["shape"] = "rect";
    e->params["corner"] = 0.0;
    e->params["stroke_width"] = 0.0;
    m.transform.posX = 60;
    const QImage moved = render(timelineOf({m}), 0);
    CHECK(at(moved, 220 + 70, 90 + 70, kRed)); // corner of the square (half side 72) at 60 px right
    CHECK(at(moved, 220 - 76, 90, kBlack));
    // Shadow on: darker than the background never, but present below right of the shape on a white background
    Clip bg = media(1, g_gray);
    Clip fg = media(2, g_red);
    EffectRegistry::add(fg, Shapes::kClipShapeEffect);
    EffectRegistry::instance(fg, Shapes::kClipShapeEffect)->params["size"] = 50.0;
    EffectRegistry::instance(fg, Shapes::kClipShapeEffect)->params["shadow"] = true;
    EffectRegistry::instance(fg, Shapes::kClipShapeEffect)->params["shadow_soft"] = 12.0;
    EffectRegistry::instance(fg, Shapes::kClipShapeEffect)->params["shadow_opacity"] = 1.0;
    const QImage shadow = render(timelineOf({bg}, {fg}), 0);
    CHECK(qRed(shadow.pixel(160 + 47, 90 + 2)) < 190); // just outside the circle (radius 45), the shadow
    CHECK(at(shadow, 10, 10, QColor(200, 200, 200), 3));
}

// Clip 1: the blue picture with a tracker (node 2) whose point moves from the middle (frame 0) to 3/4 of the width
// (frame 10); shape 2 on V2 follows it
Clip trackedClip()
{
    Clip m = media(1, g_blue, 25);
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    g.nodes << CompNode{1, "clip", {}} << CompNode{2, "tracker", {}};
    g.links = {{1, 2, "src"}};
    g.out = 2;
    ::Comp::setGraph(e, g);
    e.params.insert("n2.mode", "point");
    e.params.insert("n2.ax", 0.5);
    e.params.insert("n2.ay", 0.5);
    e.params.insert("n2.rx", 0.5);
    e.params.insert("n2.ry", 0.5);
    m.effects << e;
    const AnimParam x = Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(2, "x"));
    const AnimParam y = Keys::effectParam(::Comp::kEffectId, CompGraph::paramKey(2, "y"));
    Keys::setKey(m, x, 0, 0.5);
    Keys::setKey(m, x, 10, 0.75);
    Keys::setKey(m, y, 0, 0.5);
    Keys::setKey(m, y, 10, 0.5);
    return m;
}

void testFollow()
{
    std::printf("follow\n");
    Clip shape = shapeClip(2, ShapeKind::Ellipse, 40, 40);
    shape.title.shape.followClip = 1;
    shape.title.shape.followNode = 2;
    const Timeline tl = timelineOf({trackedClip()}, {shape});
    const QImage a = render(tl, 0), b = render(tl, 10);
    CHECK(at(a, 160, 90, kRed));
    CHECK(at(b, 240, 90, kRed));
    CHECK(at(b, 160, 90, QColor(0, 0, 255))); // left behind: the blue picture
    // A blur following it: blurred where the point is
    Clip blur = shapeClip(2, ShapeKind::Rect, 60, 60);
    blur.title.shape.fill = ShapeFill::Blur;
    blur.title.shape.followClip = 1;
    blur.title.shape.followNode = 2;
    Clip striped = trackedClip();
    striped.mediaPath = g_stripes;
    const QImage c = render(timelineOf({striped}, {blur}), 10);
    CHECK(contrast(c, QRect(230, 80, 20, 20)) < 40);
    CHECK(contrast(c, QRect(150, 80, 20, 20)) > 200);
}

// ---- CLI ----
QString g_path;
QJsonObject run(const QString& name, QJsonObject raw)
{
    if (name == "edit") raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}
QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}
QJsonObject edit(const QString& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); }
ProjectData saved()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    return d;
}

void testCli(const QString& dir)
{
    std::printf("cli\n");
    g_path = dir + "/cli.schneidi";
    {
        Project p;
        ProjectData d;
        d.timeline = timelineOf({});
        d.format = kFormat;
        p.load(d);
        p.addMedia({g_blue, "blue.png", 25, false, false, true});
        p.edit("setup", [&](Timeline& tl) {
            Clip m = trackedClip();
            m.id = p.newClipId();
            tl.video[0].clips << m;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
    }
    const int tracked = saved().timeline.video[0].clips[0].id;
    // A star with a stroke drawing itself over 0.4 s, on a free track above
    QJsonObject r = edit(R"([{"op":"shape","kind":"star","size":80,"points":6,"color":"#00ff00","stroke":"#ffffff","stroke_width":4,"draw_on":"0.4s","x":-50}])");
    CHECK(r.value("changes").toObject().value("added").toArray().size() == 1);
    ProjectData d = saved();
    const Clip* star = nullptr;
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle()) star = &c;
    if (!CHECK(star)) return;
    CHECK(star->title.isShape() && star->title.shape.kind == ShapeKind::Star && star->title.shape.points == 6);
    CHECK(star->title.outlineOn && star->title.outlineWidth == 4 && star->title.posX == -50);
    CHECK(star->keys.value(AnimParam::ShapeReveal).size() == 2 && star->keys.value(AnimParam::ShapeReveal).last().frame == 10);
    const int starId = star->id;
    // info: kind shape with its fields; keyframes by name
    const QJsonObject info = run("info", {{"project", g_path}});
    bool listed = false;
    for (const QJsonValue& t : info.value("timeline").toObject().value("tracks").toArray())
        for (const QJsonValue& c : t.toObject().value("clips").toArray())
            if (c.toObject().value("id").toInt() == starId) {
                listed = c.toObject().value("kind").toString() == "shape"
                         && c.toObject().value("shape").toObject().value("kind").toString() == "star"
                         && c.toObject().value("keyframes").toObject().contains("shape_reveal");
            }
    CHECK(listed);
    // Change it: blur fill following the tracker (node only: the clip under it), put onto the tracked point
    r = edit(QString(R"([{"op":"shape","clip":%1,"kind":"ellipse","fill":"blur","amount":40,"follow":2}])").arg(starId));
    d = saved();
    const Clip* blur = TimelineOps::findClip(d.timeline, starId);
    CHECK(blur && blur->title.shape.fill == ShapeFill::Blur && blur->title.shape.followClip == tracked
          && blur->title.shape.followNode == 2);
    CHECK(blur && std::abs(blur->title.posX) < 1e-6 && std::abs(blur->title.posY) < 1e-6); // the tracker's start point
    // keyframe op with the new names
    edit(QString(R"([{"op":"keyframe","clips":[%1],"param":"shape_width","keys":[{"at":0,"value":20},{"at":5,"value":60}]}])").arg(starId));
    {
        const ProjectData k = saved();
        const Clip* keyed = TimelineOps::findClip(k.timeline, starId);
        CHECK(keyed && keyed->keys.value(AnimParam::ShapeWidth).size() == 2);
    }
    // info's stroke object goes back into the op (round trip)
    edit(QString(R"([{"op":"shape","clip":%1,"stroke":{"color":"#ff00ff","width":7}}])").arg(starId));
    {
        const Clip* st = TimelineOps::findClip(saved().timeline, starId);
        CHECK(st && st->title.outlineOn && st->title.outlineWidth == 7 && st->title.outlineColor == QColor(255, 0, 255));
    }
    // A line and back: the next kind is a box again, not a sliver of the line's thickness
    edit(QString(R"([{"op":"shape","clip":%1,"kind":"line"}])").arg(starId));
    edit(QString(R"([{"op":"shape","clip":%1,"kind":"polygon"}])").arg(starId));
    {
        const Clip* poly = TimelineOps::findClip(saved().timeline, starId);
        CHECK(poly && poly->title.shape.kind == ShapeKind::Polygon && poly->title.shape.height > 20
              && poly->title.shape.height == poly->title.shape.width);
    }
    // Bad values are errors
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"shape","kind":"hexagon"}])"}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"shape","fill":"glitter"}])"}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"shape","follow":7}])"}}), QString("BAD_ARGUMENT"));
    // clip_shape: a circle with a stroke; none removes it
    edit(QString(R"([{"op":"clip_shape","clips":[%1],"shape":"ellipse","size":70,"stroke_color":"#ffffff","shadow":true}])").arg(tracked));
    d = saved();
    const EffectInstance* e = EffectRegistry::instance(*TimelineOps::findClip(d.timeline, tracked), Shapes::kClipShapeEffect);
    CHECK(e && e->params.value("size").toDouble() == 70 && e->params.value("shadow").toBool()
          && e->params.value("stroke_width").toDouble() > 0);
    edit(QString(R"([{"op":"clip_shape","clips":[%1],"shape":"none"}])").arg(tracked));
    CHECK(!EffectRegistry::has(*TimelineOps::findClip(saved().timeline, tracked), Shapes::kClipShapeEffect));
    // help texts exist for both ops
    const QJsonObject help = run("help", {{"command", "edit"}, {"op", "shape"}});
    CHECK(QJsonDocument(help).toJson().contains("draw_on"));
    const QJsonObject help2 = run("help", {{"command", "edit"}, {"op", "clip_shape"}});
    CHECK(QJsonDocument(help2).toJson().contains("face cam"));
}

// ---- check: a shape at the bottom of a vertical timeline is in the apps' unsafe area, like a title ----
void testCheck()
{
    std::printf("check\n");
    Timeline tl = timelineOf({});
    Clip c = shapeClip(5, ShapeKind::Rect, 400, 200, 50);
    c.title.posY = -800; // near the bottom of 1080 x 1920
    tl.video[0].clips << c;
    const ProjectFormat vertical{1080, 1920, {25, 1}};
    bool found = false;
    for (const ExportCheck::Issue& is : ExportCheck::layout(tl, 0, 50, vertical))
        if (is.check == QLatin1String("safe_area") && is.clip == 5) {
            found = is.message.contains(T("Form „%1“").arg(T("Rechteck"))); // named as a shape
            if (!found) std::printf("       %s\n", qPrintable(is.message));
        }
    CHECK(found);
    c.title.posY = 0; // the middle is safe
    tl.video[0].clips[0] = c;
    found = false;
    for (const ExportCheck::Issue& is : ExportCheck::layout(tl, 0, 50, vertical))
        if (is.check == QLatin1String("safe_area")) found = true;
    CHECK(!found);
}

// ---- Inspector: the section "Form" for a shape (its kind combo changes the clip), the text field hidden ----
void testInspector()
{
    std::printf("inspector\n");
    Project p;
    ProjectData d;
    d.timeline = timelineOf({});
    d.format = kFormat;
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Inspector insp(&e);
    e.addTitle(0, -1, 50, TitleKind::Shape);
    const int id = *sel.ids().begin();
    insp.resize(380, 1600);
    insp.show();
    QCoreApplication::processEvents();
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK(c && c->title.isShape() && c->title.text.isEmpty() && c->title.shape.width > 0);
    QComboBox* kinds = nullptr;
    for (QComboBox* b : insp.findChildren<QComboBox*>())
        if (b->count() == int(std::size(kShapeKinds)) && b->itemData(0).toInt() == int(ShapeKind::Ellipse)
            && b->itemText(0) == T(kShapeKinds[0].name) && b->itemText(1) == T(kShapeKinds[1].name))
            kinds = b;
    if (!CHECK(kinds && kinds->isVisibleTo(&insp))) return;
    for (QPlainTextEdit* t : insp.findChildren<QPlainTextEdit*>()) CHECK(!t->isVisibleTo(&insp));
    // No "Text" section for a shape: its colour and position are in "Form", which comes first
    for (QToolButton* b : insp.findChildren<QToolButton*>())
        if (b->text() == T("Text")) CHECK(!b->isVisibleTo(&insp));
    auto choose = [&](ShapeKind k) {
        kinds->setCurrentIndex(kinds->findData(int(k)));
        emit kinds->activated(kinds->currentIndex());
        QCoreApplication::processEvents();
        return TimelineOps::findClip(p.timeline(), id);
    };
    const double h0 = c->title.shape.height;
    c = choose(ShapeKind::Star);
    CHECK(c && c->title.shape.kind == ShapeKind::Star);
    // A line is thin; the next kind gets its height back (before: a flat star/polygon sliver of 8 px)
    c = choose(ShapeKind::Line);
    CHECK(c && c->title.shape.height == Shapes::lineThickness(kFormat.height));
    c = choose(ShapeKind::Star);
    CHECK(c && c->title.shape.height == h0);
    // Without a remembered height (a line from the start): as high as wide, an arrow half of it
    ShapeStyle line;
    line.kind = ShapeKind::Line;
    line.width = 200;
    line.height = 4;
    CHECK(Shapes::sizeForKind(line, ShapeKind::Polygon, 4) == QSizeF(200, 200));
    CHECK(Shapes::sizeForKind(line, ShapeKind::Arrow, 4) == QSizeF(200, 100));
    CHECK(Shapes::sizeForKind(line, ShapeKind::Line, 4) == QSizeF(200, 4));
    // Fill none and no stroke: a hint that it is invisible
    auto* invisible = insp.findChild<QLabel*>("shapeInvisibleHint");
    if (CHECK(invisible)) {
        CHECK(!invisible->isVisibleTo(&insp));
        e.modifyClips({id}, "fill", [](Clip& x) {
            x.title.shape.fill = ShapeFill::None;
            x.title.outlineOn = false;
        });
        QCoreApplication::processEvents();
        CHECK(invisible->isVisibleTo(&insp));
    }
    // Keyframes of the shape's values: at their times (the timeline's key lane and markers)
    e.modifyClips({id}, "keys", [](Clip& x) {
        Keys::setKey(x, AnimParam::ShapeReveal, 3, 0);
        Keys::setKey(x, AnimParam::ShapeWidth, 12, 80);
    });
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK(c && Keys::keyTimes(*c) == (QVector<int>{3, 12}));
    // A text title: no shape section
    sel.clear();
    e.addTitle(100, -1, 50, TitleKind::Text);
    QCoreApplication::processEvents();
    CHECK(!kinds->isVisibleTo(&insp));
}

// ---- the box over the viewer: drag inside moves, a side handle sizes around the centre, the knob turns ----
void testOverlay()
{
    std::printf("overlay\n");
    Project p;
    ProjectData d;
    d.timeline = timelineOf({});
    d.format = kFormat;
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    e.addTitle(0, -1, 50, TitleKind::Shape);
    const int id = *sel.ids().begin();
    int playhead = 10;
    ShapeOverlay ov(&e, &sel, [&] { return playhead; });
    const QRectF frame(0, 0, 320, 180); // one widget pixel = one project pixel
    const QPointF c = ov.centerPos(frame);
    CHECK(std::abs(c.x() - 160) < 0.01 && std::abs(c.y() - 90) < 0.01);
    auto drag = [&](QPointF from, QPointF to) {
        QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        const bool took = ov.mousePress(&press, frame);
        ov.mouseMove(&move, frame);
        ov.mouseRelease(&release, frame);
        return took;
    };
    CHECK(drag(c, c + QPointF(20, -10)));
    const Clip* s = TimelineOps::findClip(p.timeline(), id);
    CHECK(s && s->title.posX == 20 && s->title.posY == 10);
    const double w = s->title.shape.width;
    const QPointF right = ov.handlePos(3, frame);
    CHECK(drag(right, right + QPointF(15, 0)));
    s = TimelineOps::findClip(p.timeline(), id);
    CHECK(s && std::abs(s->title.shape.width - (w + 30)) < 1.01);
    const QPointF knob = ov.handlePos(8, frame), centre = ov.centerPos(frame);
    CHECK(drag(knob, centre + QPointF(100, 0))); // to the right of the centre: a quarter turn clockwise
    s = TimelineOps::findClip(p.timeline(), id);
    CHECK(s && std::abs(s->title.shape.rotation - 90) < 0.5);
    // A Media Pool source in the viewer: the box is not the timeline's
    bool timeline = false;
    ShapeOverlay source(&e, &sel, [&] { return playhead; }, nullptr, [&] { return timeline; });
    {
        const QPointF at = ov.centerPos(frame);
        QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        CHECK(!source.mousePress(&press, frame));
        CHECK(source.centerPos(frame).isNull());
        timeline = true;
        CHECK(source.mousePress(&press, frame));
        QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        source.mouseRelease(&release, frame);
    }
    // Outside the clip's time: nothing to grab
    playhead = 200;
    CHECK(!drag(c, c + QPointF(5, 5)));
}

// ---- a blur shape keeps hiding through its transitions: fading in, and dissolving into its other half ----
void testBelowTransitions()
{
    std::printf("below in transitions\n");
    Clip blur = shapeClip(2, ShapeKind::Rect, 100, 100, 25);
    blur.title.shape.fill = ShapeFill::Blur;
    blur.title.shape.amount = 60;
    blur.transIn = 10;
    const QImage in = render(timelineOf({media(1, g_stripes, 50)}, {blur}), 3);
    CHECK(contrast(in, QRect(140, 70, 40, 40)) < 30);
    // Split in two at 25 with a dissolve over the cut
    Clip a = shapeClip(2, ShapeKind::Rect, 100, 100, 25);
    a.title.shape.fill = ShapeFill::Blur;
    a.title.shape.amount = 60;
    Clip b = a;
    b.id = 3;
    b.start = 25;
    b.in = 25;
    b.out = 49;
    a.transOut = 10;
    b.transIn = 10;
    const Timeline tl = timelineOf({media(1, g_stripes, 50)}, {a, b});
    CHECK(!TimelineOps::transitions(tl.video[1], [](const Clip&) { return 0; }).isEmpty());
    for (int f : {21, 25, 29}) {
        const QImage img = render(tl, f);
        CHECK(contrast(img, QRect(140, 70, 40, 40)) < 30);
        CHECK(contrast(img, QRect(10, 10, 40, 40)) > 200); // outside stays sharp
    }
}

// ---- the tracked clip split: the shape follows the part after the cut (it has a new id) ----
void testFollowSplit()
{
    std::printf("follow split\n");
    Clip left = trackedClip();
    left.out = 4;
    Clip right = trackedClip();
    right.id = 3;
    right.start = 5;
    right.in = 5;
    right.out = 24;
    Clip shape = shapeClip(2, ShapeKind::Ellipse, 40, 40);
    shape.title.shape.followClip = 1;
    shape.title.shape.followNode = 2;
    const QImage img = render(timelineOf({left, right}, {shape}), 10);
    CHECK(at(img, 240, 90, kRed));
    CHECK(at(img, 160, 90, QColor(0, 0, 255)));
}

// ---- a stroked line drawn through the bounds path (clip transform) as far as straight onto the frame ----
void testLineBounds()
{
    std::printf("line bounds\n");
    Clip direct = shapeClip(1, ShapeKind::Line, 200, 10);
    direct.title.outlineOn = true;
    direct.title.outlineColor = kWhite;
    direct.title.outlineWidth = 6;
    direct.title.posX = 4;
    Clip placed = direct;
    placed.title.posX = 0;
    placed.transform.posX = 4; // the same place through the clip's transform: drawn only as large as its bounds
    const QImage a = render(timelineOf({direct}), 0), b = render(timelineOf({placed}), 0);
    CHECK(at(a, 164 + 100 + 8, 90, kWhite)); // the outline's round cap past the line's end
    CHECK(at(b, 164 + 100 + 8, 90, kWhite));
    CHECK(at(b, 164, 90 + 9, kWhite));
}

// ---- paste: a shape copied with the clip it follows follows the copy ----
void testPasteFollow()
{
    std::printf("paste follow\n");
    Project p;
    ProjectData d;
    d.timeline = timelineOf({});
    d.format = kFormat;
    p.load(d);
    Selection sel;
    Editor e(&p, &sel);
    Clip m = trackedClip();
    m.id = p.newClipId();
    Clip shape = shapeClip(p.newClipId(), ShapeKind::Ellipse, 40, 40);
    shape.title.shape.followClip = m.id;
    shape.title.shape.followNode = 2;
    p.edit("setup", [&](Timeline& tl) {
        tl.video[0].clips << m;
        tl.video[1].clips << shape;
    });
    sel.set({m.id, shape.id});
    e.copySelection();
    e.paste(100);
    int newTracked = 0, followed = -1;
    for (const Track& t : p.timeline().video)
        for (const Clip& c : t.clips) {
            if (c.start < 100) continue;
            if (c.isTitle()) followed = c.title.shape.followClip;
            else newTracked = c.id;
        }
    CHECK(newTracked > 0 && followed == newTracked);
    // The shape alone: it keeps following the original
    sel.set({shape.id});
    e.copySelection();
    e.paste(200);
    for (const Track& t : p.timeline().video)
        for (const Clip& c : t.clips)
            if (c.start >= 200 && c.isTitle()) CHECK_EQ(c.title.shape.followClip, m.id);
}

// ---- check: a shape following a tracker that is gone ----
void testFollowCheck()
{
    std::printf("follow check\n");
    Clip shape = shapeClip(2, ShapeKind::Ellipse, 40, 40);
    shape.title.shape.followClip = 1;
    shape.title.shape.followNode = 2;
    const auto missing = [](const Timeline& tl) {
        for (const ExportCheck::Issue& is : ExportCheck::timeline(tl, 0, 25, kFormat))
            if (is.check == QLatin1String("shape_follow_missing") && is.clip == 2) return true;
        return false;
    };
    CHECK(!missing(timelineOf({trackedClip()}, {shape})));
    CHECK(missing(timelineOf({media(1, g_blue)}, {shape})));       // no graph
    CHECK(missing(timelineOf({}, {shape})));                       // no clip
    shape.title.shape.followNode = 7;
    CHECK(missing(timelineOf({trackedClip()}, {shape})));          // no such node
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("shapes");
    I18n::setLanguage("en");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_stripes = still(tmp.filePath("stripes.png"), [](QImage& img) {
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) img.setPixel(x, y, (x / 2) % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
    });
    g_gray = still(tmp.filePath("gray.png"), [](QImage& img) { img.fill(QColor(200, 200, 200)); });
    g_red = still(tmp.filePath("red.png"), [](QImage& img) { img.fill(kRed); });
    g_blue = still(tmp.filePath("blue.png"), [](QImage& img) { img.fill(QColor(0, 0, 255)); });
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testKinds();
    testStroke();
    testKeyframes();
    testProjectFile(tmp.path());
    testBelow();
    testClipShape();
    testFollow();
    testCli(tmp.path());
    testCheck();
    testInspector();
    testOverlay();
    testBelowTransitions();
    testFollowSplit();
    testLineBounds();
    testPasteFollow();
    testFollowCheck();
    return Check::result();
}
