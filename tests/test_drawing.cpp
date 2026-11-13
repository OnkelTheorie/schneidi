// SPDX-License-Identifier: AGPL-3.0-only
// Drawings (0.22, core/Drawing.h): drawing titles rendered with known pixels (a line, a dot, opacity, pressure, a
// stroke crossing itself), the strokes drawing themselves one after another (reveal), project file round trip (and
// what an older version reads), a format of another shape, smoothing, painting in the viewer (one undo step per
// stroke, the eraser, the box that moves and scales, nothing while a source clip shows), following a tracker, the CLI
// op draw (with its errors, info round trip, frames) and `check` (safe areas), the Inspector section.
// Synthetic pictures only. SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "render_check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Drawing.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/Shapes.h"
#include "core/TimelineOps.h"
#include "ui/DrawOverlay.h"
#include "ui/Inspector.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTabletEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QUndoStack>

#include <clocale>
#include <cmath>

namespace {

const ProjectFormat kFormat{320, 180, {25, 1}};
const QColor kRed(255, 0, 0), kBlack(0, 0, 0), kBlue(0, 0, 255);
QString g_blue; // still 320 x 180

QImage render(const Timeline& tl, int frame, const ProjectFormat& fmt = kFormat)
{
    const QImage img = Check::renderFrame(tl, fmt, frame);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull()) {
        static int n = 0;
        img.save(QString("%1/drawing-%2.png").arg(dump).arg(n++, 3, 10, QChar('0')));
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

// A stroke in 0..1 of the frame, `px` pixels wide in the 180 high test frame
DrawStroke stroke(QVector<QPointF> pts, double px, const QColor& color = kRed)
{
    DrawStroke s;
    s.points = std::move(pts);
    s.width = px / 180.0;
    s.color = color;
    return s;
}

Clip drawingClip(int id, QVector<DrawStroke> strokes, int length = 100)
{
    Clip c;
    c.id = id;
    c.kind = ClipKind::Title;
    c.title.kind = TitleKind::Drawing;
    c.title.text.clear();
    c.title.drawing.strokes = std::move(strokes);
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

QString still(const QString& path, const QColor& c)
{
    QImage img(320, 180, QImage::Format_RGB32);
    img.fill(c);
    img.save(path);
    return path;
}

// ---- strokes at known pixels: a 10 px line from x 80 to 240 at y 90, a dot, half opacity, pressure ----
void testRender()
{
    std::printf("render\n");
    const Clip c = drawingClip(1, {stroke({{0.25, 0.5}, {0.75, 0.5}}, 10), stroke({{0.1, 0.1}}, 20, kBlue)});
    const QImage img = render(timelineOf({c}), 0);
    CHECK(at(img, 160, 90, kRed));
    CHECK(at(img, 160, 87, kRed));
    CHECK(at(img, 160, 80, kBlack)); // 5 px half width: 85..95
    CHECK(at(img, 160, 100, kBlack));
    CHECK(at(img, 78, 90, kRed));    // round end: reaches 5 px before its first point
    CHECK(at(img, 72, 90, kBlack));
    CHECK(at(img, 245, 90, kBlack));
    CHECK(at(img, 32, 18, kBlue));   // the dot (20 px wide at 32, 18)
    CHECK(at(img, 32, 4, kBlack));
    // Half opacity over black: half red; crossing itself it is not darker (one stroke, one coverage)
    DrawStroke cross = stroke({{0.2, 0.2}, {0.8, 0.8}, {0.8, 0.2}, {0.2, 0.8}}, 8);
    cross.opacity = 0.5;
    const QImage half = render(timelineOf({drawingClip(1, {cross})}), 0);
    CHECK(std::abs(qRed(half.pixel(160, 90)) - 128) <= 12); // where it crosses
    CHECK(std::abs(qRed(half.pixel(96, 54)) - 128) <= 12);  // on one leg
    // Pressure: full width at the start, a fifth at the end (12 px -> 2.4 px)
    DrawStroke pen = stroke({{0.25, 0.5}, {0.5, 0.5}, {0.75, 0.5}}, 12);
    pen.pressure = {1.0f, 0.6f, 0.2f};
    const QImage p = render(timelineOf({drawingClip(1, {pen})}), 0);
    CHECK(at(p, 82, 95, kRed));    // 5 px off the line near the thick start
    CHECK(at(p, 236, 95, kBlack)); // ... near the thin end: nothing
    CHECK(at(p, 236, 90, kRed));
    // Moved by the title position (y up): 20 px right, 30 px up
    Clip moved = c;
    moved.title.posX = 20;
    moved.title.posY = 30;
    const QImage m = render(timelineOf({moved}), 0);
    CHECK(at(m, 180, 60, kRed));
    CHECK(at(m, 160, 90, kBlack));
}

// ---- reveal: two equally long strokes over 2 s (50 frames): the first by frame 25, the second by 50 ----
void testReveal()
{
    std::printf("reveal\n");
    Clip c = drawingClip(1, {stroke({{0.3, 0.3}, {0.7, 0.3}}, 8), stroke({{0.3, 0.7}, {0.7, 0.7}}, 8)});
    c.title.drawing.reveal = 2;
    const Timeline tl = timelineOf({c});
    CHECK(std::abs(Drawing::strokeDoneAt(c.title.drawing, 0, 320, 180, 25) - 25) < 1e-9);
    CHECK(std::abs(Drawing::strokeDoneAt(c.title.drawing, 1, 320, 180, 25) - 50) < 1e-9);
    const QImage f0 = render(tl, 0);
    CHECK(at(f0, 160, 54, kBlack)); // nothing yet
    const QImage f12 = render(tl, 12); // 24 % of the whole = 48 % of the first stroke: 96 .. 157
    CHECK(at(f12, 110, 54, kRed));
    CHECK(at(f12, 200, 54, kBlack));
    CHECK(at(f12, 160, 126, kBlack));
    const QImage f25 = render(tl, 25); // the first whole, the second not begun
    CHECK(at(f25, 220, 54, kRed));
    CHECK(at(f25, 110, 126, kBlack));
    const QImage f38 = render(tl, 38); // half of the second
    CHECK(at(f38, 110, 126, kRed));
    CHECK(at(f38, 210, 126, kBlack));
    const QImage f60 = render(tl, 60);
    CHECK(at(f60, 220, 126, kRed));
    // Off: all at once
    c.title.drawing.reveal = 0;
    CHECK(at(render(timelineOf({c}), 0), 220, 126, kRed));
}

// ---- project file: round trip, what an older version reads, unknown and broken strokes, another frame shape ----
void testProjectFile(const QString& dir)
{
    std::printf("project file\n");
    Project p;
    ProjectData d;
    d.timeline = timelineOf({});
    d.format = kFormat;
    p.load(d);
    DrawStroke a = stroke({{0.25, 0.5}, {0.5, 0.25}, {0.75, 0.5}}, 9, QColor(10, 200, 30));
    a.pressure = {1.0f, 0.5f, 0.25f};
    a.opacity = 0.5;
    DrawStroke b = stroke({{0.125, 0.875}}, 18, QColor(0, 0, 255, 128));
    Clip c = drawingClip(p.newClipId(), {a, b});
    c.title.drawing.reveal = 1.5;
    c.title.shape.followClip = 9;
    c.title.shape.followNode = 2;
    c.title.posX = 12;
    p.edit("setup", [&](Timeline& tl) { tl.video[0].clips << c; });
    const QString path = dir + "/drawing.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline == p.timeline());
    // An older version: kind "drawing" unknown -> a text title with the stored text, which is empty (draws nothing)
    const QJsonObject t = ProjectFile::clipJson(c).value("title").toObject();
    CHECK_EQ(t.value("kind").toString(), QString("drawing"));
    CHECK(t.value("text").toString().isEmpty());
    const QJsonArray strokes = t.value("drawing").toObject().value("strokes").toArray();
    CHECK(strokes.size() == 2);
    CHECK(strokes[0].toObject().value("points").toArray().size() == 6);   // flat x, y
    CHECK(strokes[0].toObject().contains("pressure"));
    CHECK(!strokes[1].toObject().contains("pressure"));                    // all 1: left out
    CHECK(!strokes[1].toObject().contains("opacity"));
    CHECK(t.value("drawing").toObject().value("followClip").toInt() == 9);
    // Small: a stroke of 200 points is a few kB
    DrawStroke longOne;
    for (int i = 0; i < 200; ++i) longOne.points << QPointF(i / 200.0, 0.5 + 0.3 * std::sin(i / 10.0));
    const QByteArray bytes = QJsonDocument(Drawing::strokeJson(longOne)).toJson(QJsonDocument::Compact);
    CHECK(bytes.size() < 200 * 20);
    // Read leniently: unknown keys stay unread, a stroke without points is dropped, odd points ignored
    const DrawingStyle lenient = Drawing::fromJson(QJsonDocument::fromJson(
        R"({"strokes":[{"points":[0.1,0.2,0.3,0.4],"color":"#ff0000","width":0.01,"glitter":5},{"points":[]},{"points":["x",1]}],"future":1})")
                                                       .object());
    CHECK(lenient.strokes.size() == 1 && lenient.strokes[0].points.size() == 2);
    // A frame of the other shape: the drawing keeps its shape (and pixel width) around the middle
    Timeline turned = p.timeline();
    scaleTimeline(turned, QSize(320, 180), QSize(180, 320));
    const Clip* tc = TimelineOps::findClip(turned, c.id);
    if (CHECK(tc)) {
        const QPointF q = tc->title.drawing.strokes[0].points[1]; // (0.5, 0.25): 45 px above the middle
        CHECK(std::abs(q.x() - 0.5) < 1e-9 && std::abs((0.5 - q.y()) * 320 - 45) < 1e-6);
        const QPointF r = tc->title.drawing.strokes[0].points[0]; // (0.25, 0.5): 80 px left of the middle
        CHECK(std::abs((0.5 - r.x()) * 180 - 80) < 1e-6);
        CHECK(std::abs(tc->title.drawing.strokes[0].width * 320 - 9) < 1e-6);
    }
    // The same shape larger: nothing to change (0..1 of the frame)
    Timeline twice = p.timeline();
    scaleTimeline(twice, QSize(320, 180), QSize(640, 360));
    CHECK(TimelineOps::findClip(twice, c.id)->title.drawing == c.title.drawing);
}

// ---- smoothing: points too close dropped, a zigzag evened out, both ends kept ----
void testSmooth()
{
    std::printf("smooth\n");
    DrawStroke s;
    for (int i = 0; i <= 40; ++i) s.points << QPointF(0.1 + i * 0.02, 0.5 + (i % 2 ? 0.01 : -0.01)); // 6.4 px apart, +-1.8 px
    s.points.insert(5, s.points[5] + QPointF(0.001, 0)); // 0.3 px after its neighbour
    const QPointF first = s.points.first(), last = s.points.last();
    const int n = int(s.points.size());
    Drawing::smooth(s, 320, 180);
    CHECK(s.points.size() == n - 1);
    CHECK(s.points.first() == first && s.points.last() == last);
    double most = 0;
    for (int i = 2; i + 2 < s.points.size(); ++i) most = std::max(most, std::abs(s.points[i].y() - 0.5) * 180);
    CHECK(most < 0.5); // was 1.8 px
}

// ---- painting in the viewer ----
struct Painter {
    Project p;
    Selection sel;
    Editor e{&p, &sel};
    int playhead = 10;
    bool timeline = true;
    DrawOverlay ov{&e, &sel, [this] { return playhead; }, nullptr, [this] { return timeline; }};
    const QRectF frame{0, 0, 320, 180}; // one widget pixel = one project pixel
    Painter()
    {
        ProjectData d;
        d.timeline = timelineOf({});
        d.format = kFormat;
        p.load(d);
    }
    bool drag(const QVector<QPointF>& pts)
    {
        QMouseEvent press(QEvent::MouseButtonPress, pts[0], pts[0], Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        const bool took = ov.mousePress(&press, frame);
        for (int i = 1; i < pts.size(); ++i) {
            QMouseEvent move(QEvent::MouseMove, pts[i], pts[i], Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            ov.mouseMove(&move, frame);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, pts.last(), pts.last(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        ov.mouseRelease(&release, frame);
        return took;
    }
    QVector<QPointF> line(QPointF a, QPointF b, int n = 20)
    {
        QVector<QPointF> out;
        for (int i = 0; i <= n; ++i) out << a + (b - a) * (double(i) / n);
        return out;
    }
    const Clip* drawing() const
    {
        for (const Track& t : p.timeline().video)
            for (const Clip& c : t.clips)
                if (c.isTitle() && c.title.isDrawing()) return &c;
        return nullptr;
    }
};

void testPaint()
{
    std::printf("paint\n");
    DrawBrush::instance().setEraser(false);
    DrawBrush::instance().setColor(QColor(0, 255, 0));
    DrawBrush::instance().setWidth(10 / 180.0);
    DrawBrush::instance().setOpacity(1);
    Painter t;
    t.p.edit("setup", [&](Timeline& tl) { tl.video[0].clips << media(t.p.newClipId(), g_blue, 200); });
    t.ov.setActive(true);
    CHECK(t.ov.target() == 0);
    // The first stroke makes a drawing at the playhead on the free track above the picture, 5 s, selected
    CHECK(t.drag(t.line({80, 60}, {240, 60})));
    const Clip* d = t.drawing();
    if (!CHECK(d)) return;
    TrackRef ref;
    TimelineOps::findClip(t.p.timeline(), d->id, &ref);
    CHECK(ref.index == 1 && d->start == 10 && d->length() == 125);
    CHECK(t.sel.ids() == QSet<int>{d->id} && t.ov.target() == d->id);
    CHECK(d->title.drawing.strokes.size() == 1);
    const DrawStroke& s0 = d->title.drawing.strokes[0];
    CHECK(s0.color == QColor(0, 255, 0) && std::abs(s0.width * 180 - 10) < 1e-6 && s0.pressure.isEmpty());
    CHECK(std::abs(s0.points.first().x() - 0.25) < 1e-6 && std::abs(s0.points.first().y() - 1 / 3.0) < 1e-6);
    CHECK(std::abs(s0.points.last().x() - 0.75) < 1e-6);
    // The second stroke goes into it: one clip, two strokes; it renders where it was painted
    CHECK(t.drag(t.line({80, 120}, {240, 120})));
    d = t.drawing();
    CHECK(d && d->title.drawing.strokes.size() == 2);
    int drawings = 0;
    for (const Track& tr : t.p.timeline().video)
        for (const Clip& c : tr.clips) drawings += c.isTitle() ? 1 : 0;
    CHECK(drawings == 1);
    const QImage img = render(t.p.timeline(), 20);
    CHECK(at(img, 160, 60, QColor(0, 255, 0)));
    CHECK(at(img, 160, 120, QColor(0, 255, 0)));
    CHECK(at(img, 160, 90, kBlue));
    // Undo per stroke: the second goes, then the first with its clip; redo brings them back
    QUndoStack* undo = t.p.undoStack();
    undo->undo();
    CHECK(t.drawing() && t.drawing()->title.drawing.strokes.size() == 1);
    undo->undo();
    CHECK(!t.drawing());
    undo->redo();
    undo->redo();
    CHECK(t.drawing() && t.drawing()->title.drawing.strokes.size() == 2);
    // The eraser removes the whole stroke it touches (the lower one), one undo step for the drag
    t.sel.set({t.drawing()->id});
    DrawBrush::instance().setEraser(true);
    CHECK(t.drag(t.line({150, 110}, {170, 130}, 5)));
    d = t.drawing();
    CHECK(d && d->title.drawing.strokes.size() == 1 && std::abs(d->title.drawing.strokes[0].points[0].y() - 1 / 3.0) < 1e-6);
    undo->undo();
    CHECK(t.drawing() && t.drawing()->title.drawing.strokes.size() == 2);
    // Missing everything: nothing changes, no undo step
    const int steps = undo->count();
    CHECK(t.drag(t.line({10, 170}, {20, 170}, 3)));
    CHECK(undo->count() == steps && t.drawing()->title.drawing.strokes.size() == 2);
    DrawBrush::instance().setEraser(false);
    // A Media Pool source in the viewer: nothing is painted
    t.timeline = false;
    CHECK(!t.drag(t.line({100, 150}, {200, 150})));
    CHECK(t.drawing()->title.drawing.strokes.size() == 2 && undo->count() == steps);
    t.timeline = true;
    // Outside the selected drawing's time: a new drawing there
    t.playhead = 180;
    CHECK(t.ov.target() == 0);
    CHECK(t.drag(t.line({100, 150}, {200, 150})));
    drawings = 0;
    for (const Track& tr : t.p.timeline().video)
        for (const Clip& c : tr.clips) drawings += c.isTitle() ? 1 : 0;
    CHECK(drawings == 2);
}

// ---- the box (tool off): drag inside moves the drawing, a corner scales it around the box's middle ----
void testBox()
{
    std::printf("box\n");
    Painter t;
    t.e.addTitle(0, -1, 50, TitleKind::Drawing, [](Clip& c) {
        c.title.drawing.strokes << stroke({{0.25, 0.25}, {0.5, 0.5}}, 4); // 80,45 .. 160,90
    });
    const int id = *t.sel.ids().begin();
    const QRectF box = t.ov.boxRect(t.frame);
    CHECK(std::abs(box.left() - 77) < 0.01 && std::abs(box.bottom() - 93) < 0.01); // with the half width + 1
    CHECK(t.drag({box.center(), box.center() + QPointF(20, -10)}));
    const Clip* c = TimelineOps::findClip(t.p.timeline(), id);
    CHECK(c && c->title.posX == 20 && c->title.posY == 10);
    const QRectF moved = t.ov.boxRect(t.frame);
    CHECK(std::abs(moved.left() - 97) < 0.01);
    // The bottom right corner twice as far from the middle: zoom 2, the middle stays
    const QPointF mid = moved.center(), corner = moved.bottomRight();
    CHECK(t.drag({corner, mid + (corner - mid) * 2}));
    c = TimelineOps::findClip(t.p.timeline(), id);
    CHECK(c && std::abs(c->transform.zoomX - 2) < 1e-6 && std::abs(c->transform.zoomY - 2) < 1e-6);
    const QRectF scaled = t.ov.boxRect(t.frame);
    CHECK(std::abs(scaled.center().x() - mid.x()) < 0.01 && std::abs(scaled.center().y() - mid.y()) < 0.01);
    CHECK(std::abs(scaled.width() - 2 * moved.width()) < 0.01);
    // ... and the picture engine draws it there: the line (100, 35) - (180, 80) twice as long around its middle
    // (140, 57.5), 8 px wide: now (60, 12.5) - (220, 102.5)
    const QImage img = render(t.p.timeline(), 0);
    CHECK(at(img, 140, 57, kRed));
    CHECK(at(img, 215, 100, kRed));
    CHECK(at(img, 70, 18, kRed));
    CHECK(at(img, 232, 109, kBlack));
    // A stroke painted into the scaled drawing lands under the mouse
    DrawBrush::instance().setEraser(false);
    DrawBrush::instance().setColor(kRed);
    t.ov.setActive(true);
    CHECK(t.drag(t.line({20, 160}, {60, 160}, 4)));
    const QImage after = render(t.p.timeline(), 0);
    CHECK(at(after, 40, 160, kRed));
    t.ov.setActive(false);
    // A Media Pool source: no box
    t.timeline = false;
    CHECK(t.ov.boxRect(t.frame).isEmpty());
    CHECK(!t.drag({mid, mid + QPointF(5, 5)}));
}

// ---- off the picture: painted over its edge (kept within -1..2), into a zoomed drawing, a file round trip; the
// reader keeps any finite point (and so the pressure) ----
void testOffFrame(const QString& dir)
{
    std::printf("off frame\n");
    DrawBrush::instance().setEraser(false);
    DrawBrush::instance().setWidth(6 / 180.0);
    Painter t;
    t.ov.setActive(true);
    CHECK(t.drag(t.line({-80, 90}, {100, 90}, 18))); // starts 80 px left of the picture
    CHECK(t.drag(t.line({-2000, 20}, {40, 20}, 10))); // far off: kept at -1 of the frame
    const Clip* d = t.drawing();
    if (!CHECK(d && d->title.drawing.strokes.size() == 2)) return;
    CHECK(std::abs(d->title.drawing.strokes[0].points.first().x() + 0.25) < 1e-6);
    CHECK(std::abs(d->title.drawing.strokes[1].points.first().x() + 1) < 1e-6);
    // Zoomed 3x around the middle, painted at the picture's edge: far outside the frame in the drawing's own space
    const int id = d->id;
    t.e.modifyClips({id}, "zoom", [](Clip& c) {
        c.transform.zoomX = c.transform.zoomY = 0.25;
    });
    CHECK(t.drag(t.line({0, 170}, {319, 170}, 30)));
    d = t.drawing();
    CHECK(d && d->title.drawing.strokes.size() == 3 && d->title.drawing.strokes[2].points.first().x() < -1 + 1e-9);
    // Pressure on a stroke reaching off the picture survives the file
    t.e.modifyClips({id}, "pressure", [](Clip& c) {
        DrawStroke s = stroke({{-0.6, 0.5}, {0.5, 0.5}, {1.8, 0.5}}, 6);
        s.pressure = {1.0f, 0.5f, 0.25f};
        c.title.drawing.strokes << s;
    });
    const QString path = dir + "/offframe.schneidi";
    QString error;
    CHECK(ProjectFile::save(t.p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    const Clip* b = TimelineOps::findClip(back.timeline, id);
    // the same strokes (painted points are rounded to 1/100000 of the frame in the file)
    const DrawingStyle& was = TimelineOps::findClip(t.p.timeline(), id)->title.drawing;
    bool same = b && b->title.drawing.strokes.size() == was.strokes.size();
    for (int i = 0; same && i < was.strokes.size(); ++i) {
        const DrawStroke &x = was.strokes[i], &y = b->title.drawing.strokes[i];
        same = x.points.size() == y.points.size() && x.pressure == y.pressure && std::abs(x.width - y.width) < 1e-6;
        for (int k = 0; same && k < x.points.size(); ++k)
            same = std::abs(x.points[k].x() - y.points[k].x()) <= 5e-6 && std::abs(x.points[k].y() - y.points[k].y()) <= 5e-6;
    }
    CHECK(same);
    CHECK(b && b->title.drawing.strokes[2].points.first().x() < -1 + 1e-9);
    // A format change may move points past -1..2: a file still keeps them
    const DrawingStyle far = Drawing::fromJson(QJsonDocument::fromJson(
        R"({"strokes":[{"points":[-3.5,0.5,0.5,0.5,4.25,0.5],"pressure":[1,0.5,0.25]}]})").object());
    CHECK(far.strokes.size() == 1 && far.strokes[0].points.size() == 3 && far.strokes[0].pressure.size() == 3
          && far.strokes[0].points[2].x() == 4.25);
}

// ---- reveal through a split and a trimmed start: the clip is a window onto the drawing ----
void testRevealSplit()
{
    std::printf("reveal split\n");
    Project p;
    ProjectData pd;
    pd.timeline = timelineOf({});
    pd.format = kFormat;
    p.load(pd);
    Selection sel;
    Editor e(&p, &sel);
    e.addTitle(0, 0, 100, TitleKind::Drawing, [](Clip& c) {
        c.title.drawing.strokes << stroke({{0.3, 0.3}, {0.7, 0.3}}, 8) << stroke({{0.3, 0.7}, {0.7, 0.7}}, 8);
        c.title.drawing.reveal = 2; // first stroke by frame 25, second by 50
    });
    const int id = *sel.ids().begin();
    const QImage whole = render(p.timeline(), 38);
    e.bladeAt(id, 30);
    int parts = 0;
    for (const Clip& c : p.timeline().video[0].clips) parts += c.isTitle() ? 1 : 0;
    CHECK(parts == 2);
    const QImage split = render(p.timeline(), 38); // in the second part: half of the second stroke, as before
    CHECK(at(split, 110, 126, kRed) && at(split, 210, 126, kBlack));
    CHECK(split == whole);
    // A left trim: clip starting at 25 showing the drawing from its frame 25 on
    Clip trimmed = drawingClip(1, {stroke({{0.3, 0.3}, {0.7, 0.3}}, 8), stroke({{0.3, 0.7}, {0.7, 0.7}}, 8)});
    trimmed.title.drawing.reveal = 2;
    trimmed.in = 25;
    trimmed.out = 99;
    trimmed.start = 25;
    const QImage tr = render(timelineOf({trimmed}), 25); // its first frame: the first stroke whole
    CHECK(at(tr, 220, 54, kRed) && at(tr, 110, 126, kBlack));
}

// ---- a dissolve between the two halves of a split drawing: it stays (dissolving into itself) ----
void testTransition()
{
    std::printf("transition\n");
    Clip a = drawingClip(1, {stroke({{0.25, 0.5}, {0.75, 0.5}}, 12)}, 30);
    Clip b = drawingClip(2, {stroke({{0.25, 0.5}, {0.75, 0.5}}, 12)}, 30);
    b.start = 30;
    b.in = 30;
    b.out = 59;
    a.transOut = b.transIn = 10;
    const Timeline tl = timelineOf({a, b});
    for (int f : {26, 30, 33}) CHECK(at(render(tl, f), 160, 90, kRed, 30));
    // The painted picture is kept while nothing changes: a changed stroke shows at once
    Timeline changed = tl;
    changed.video[0].clips[0].title.drawing.strokes[0].color = kBlue;
    CHECK(at(render(changed, 10), 160, 90, kBlue));
    CHECK(at(render(tl, 10), 160, 90, kRed));
}

// ---- a pen: tablet events carry the pressure into the stroke ----
void testTablet()
{
    std::printf("tablet\n");
    DrawBrush::instance().setEraser(false);
    DrawBrush::instance().setWidth(10 / 180.0);
    Painter t;
    t.ov.setActive(true);
    const QPointingDevice pen("test pen", 4711, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen,
                              QInputDevice::Capability::Position | QInputDevice::Capability::Pressure, 1, 1);
    const auto ev = [&](QEvent::Type type, QPointF at, double pressure, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QTabletEvent e(type, &pen, at, at, pressure, 0, 0, 0, 0, 0, Qt::NoModifier, button, buttons);
        return t.ov.tablet(&e, t.frame);
    };
    CHECK(ev(QEvent::TabletPress, {80, 90}, 1.0, Qt::LeftButton, Qt::LeftButton));
    for (int i = 1; i <= 20; ++i)
        CHECK(ev(QEvent::TabletMove, {80 + i * 8.0, 90}, 1.0 - i * 0.04, Qt::NoButton, Qt::LeftButton));
    CHECK(ev(QEvent::TabletRelease, {240, 90}, 0.2, Qt::LeftButton, Qt::NoButton));
    const Clip* d = t.drawing();
    if (!CHECK(d && d->title.drawing.strokes.size() == 1)) return;
    const DrawStroke& s = d->title.drawing.strokes[0];
    CHECK(s.pressure.size() == s.points.size() && s.pressure.first() > 0.95f && s.pressure.last() < 0.3f);
    // Hovering (no press) is left to the mouse events
    CHECK(!ev(QEvent::TabletMove, {100, 100}, 0, Qt::NoButton, Qt::NoButton));
}

// ---- locked track: a stroke goes into a new drawing, the eraser says so and changes nothing; the eraser is as wide
// on the screen in a zoomed drawing as the brush ----
void testLockedAndZoomedEraser()
{
    std::printf("locked, zoomed eraser\n");
    DrawBrush::instance().setEraser(false);
    DrawBrush::instance().setWidth(10 / 180.0);
    Painter t;
    t.ov.setActive(true);
    CHECK(t.drag(t.line({80, 60}, {240, 60})));
    const int id = t.drawing()->id;
    TrackRef ref;
    TimelineOps::findClip(t.p.timeline(), id, &ref);
    t.e.toggleTrackLock(ref);
    t.sel.set({id});
    CHECK(t.ov.target() == 0);
    CHECK(t.drag(t.line({80, 120}, {240, 120})));
    int drawings = 0;
    for (const Track& tr : t.p.timeline().video)
        for (const Clip& c : tr.clips) drawings += c.isTitle() ? 1 : 0;
    CHECK(drawings == 2 && TimelineOps::findClip(t.p.timeline(), id)->title.drawing.strokes.size() == 1);
    QString said;
    QObject::connect(&t.ov, &DrawOverlay::notice, [&](const QString& s) { said = s; });
    t.sel.set({id});
    DrawBrush::instance().setEraser(true);
    CHECK(t.drag(t.line({150, 50}, {170, 70}, 5)));
    CHECK(!said.isEmpty() && TimelineOps::findClip(t.p.timeline(), id)->title.drawing.strokes.size() == 1);
    t.e.toggleTrackLock(ref);
    // Zoom 3 around the middle: a line at the drawing's y 90; the eraser 30 px wide on the screen (= 5 px of the
    // drawing) passing 10 drawing pixels (30 screen pixels) below it misses, right over it hits
    Painter z;
    z.e.addTitle(0, -1, 50, TitleKind::Drawing, [](Clip& c) {
        c.title.drawing.strokes << stroke({{0.4, 0.5}, {0.6, 0.5}}, 2);
        c.transform.zoomX = c.transform.zoomY = 3;
    });
    z.ov.setActive(true);
    DrawBrush::instance().setWidth(30 / 180.0);
    CHECK(z.drag(z.line({140, 120}, {180, 120}, 4)));
    CHECK(z.drawing()->title.drawing.strokes.size() == 1);
    CHECK(z.drag(z.line({140, 92}, {180, 92}, 4)));
    CHECK(z.drawing()->title.drawing.strokes.isEmpty());
    DrawBrush::instance().setEraser(false);
}

// ---- following a tracker of the clip below: moved with the tracked point (0.5 -> 0.75 of 320 = +80 px) ----
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
    Clip d = drawingClip(2, {stroke({{0.45, 0.5}, {0.55, 0.5}}, 10)}, 25);
    d.title.shape.followClip = 1;
    d.title.shape.followNode = 2;
    const Timeline tl = timelineOf({trackedClip()}, {d});
    const QImage a = render(tl, 0), b = render(tl, 10);
    CHECK(at(a, 160, 90, kRed));
    CHECK(at(b, 240, 90, kRed));
    CHECK(at(b, 160, 90, kBlue));
    // Pasted with the tracked clip: the copy follows the copy
    Timeline copy = tl;
    copy.video[0].clips[0].id = 11;
    copy.video[1].clips[0].id = 12;
    Shapes::remapFollow(copy, {{1, 11}, {2, 12}});
    CHECK(copy.video[1].clips[0].title.shape.followClip == 11);
    // A drawing following a tracker is not painted into (a new drawing is made) and has no box
    Painter t;
    t.p.edit("setup", [&](Timeline& x) {
        x = tl;
    });
    t.sel.set({2});
    t.playhead = 5;
    CHECK(t.ov.target() == 0 && t.ov.boxRect(t.frame).isEmpty());
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
QString errorOf(const QString& ops)
{
    try {
        run("edit", {{"project", g_path}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
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
const Clip* drawingIn(const ProjectData& d)
{
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle() && c.title.isDrawing()) return &c;
    return nullptr;
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
            m.out = 99;
            tl.video[0].clips << m;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
    }
    const int tracked = saved().timeline.video[0].clips[0].id;
    // A line and a dot in pixels, 10 px wide in red, drawing itself over 1 s
    QJsonObject r = edit(R"([{"op":"draw","units":"pixels","color":"#ff0000","width":10,"reveal":"1s","at":0,"duration":50,
                             "strokes":[{"points":[[80,90],[240,90]]},{"points":[32,18],"color":"#00ff00","width":20}]}])");
    CHECK(r.value("changes").toObject().value("added").toArray().size() == 1);
    ProjectData d = saved();
    const Clip* dc = drawingIn(d);
    if (!CHECK(dc)) return;
    const int id = dc->id;
    TrackRef ref;
    TimelineOps::findClip(d.timeline, id, &ref);
    CHECK(ref.index == 1 && dc->length() == 50);
    CHECK(dc->title.drawing.strokes.size() == 2 && std::abs(dc->title.drawing.reveal - 1) < 1e-9);
    CHECK(std::abs(dc->title.drawing.strokes[0].points[1].x() - 0.75) < 1e-9);
    CHECK(std::abs(dc->title.drawing.strokes[0].width * 180 - 10) < 1e-3 && dc->title.drawing.strokes[1].color == QColor(0, 255, 0));
    // frames: the picture engine draws it (frame 40: all drawn; frame 5: only the start of the line)
    const auto frameAt = [&](int frame) {
        const QJsonObject fr = run("frames", {{"source", g_path}, {"out", dir + QString("/f%1").arg(frame)},
                                              {"at", QString::number(frame)}, {"width", 320}});
        return QImage(fr["frames"].toArray().first().toObject()["file"].toString());
    };
    const QImage late = frameAt(40), early = frameAt(5);
    CHECK(!late.isNull() && at(late, 200, 90, kRed, 40) && at(late, 32, 18, QColor(0, 255, 0), 40));
    CHECK(!early.isNull() && at(early, 90, 90, kRed, 40) && at(early, 200, 90, kBlue, 40));
    // info: kind drawing, the strokes in the op's form; they go back into the op unchanged (round trip)
    const QJsonObject info = run("info", {{"project", g_path}});
    QJsonObject listed;
    for (const QJsonValue& t : info.value("timeline").toObject().value("tracks").toArray())
        for (const QJsonValue& c : t.toObject().value("clips").toArray())
            if (c.toObject().value("id").toInt() == id) listed = c.toObject();
    CHECK_EQ(listed.value("kind").toString(), QString("drawing"));
    const QJsonObject dj = listed.value("drawing").toObject();
    CHECK(dj.value("strokes").toArray().size() == 2 && dj.value("strokes").toArray()[0].toObject().value("width").toDouble() == 10);
    CHECK_EQ(dj.value("reveal").toString(), QString("1s"));
    QJsonObject again = dj;
    again["op"] = "draw";
    again["clear"] = true;
    again["clip"] = id;
    edit(QString::fromUtf8(QJsonDocument(QJsonArray{again}).toJson(QJsonDocument::Compact)));
    CHECK(drawingIn(saved())->title.drawing == dc->title.drawing);
    // Add a stroke, remove the first one, reveal off, move it, follow the tracker
    edit(QString(R"([{"op":"draw","clip":%1,"strokes":[[[0.5,0.2],[0.5,0.8]]]}])").arg(id));
    CHECK(drawingIn(saved())->title.drawing.strokes.size() == 3);
    CHECK(std::abs(drawingIn(saved())->title.drawing.strokes[2].width * 180 - 2) < 1e-3); // default 8 px at 1080 -> 2 at 180
    edit(QString(R"([{"op":"draw","clip":%1,"remove":0,"reveal":false,"x":10,"y":-5,"follow":2}])").arg(id));
    d = saved();
    dc = drawingIn(d);
    CHECK(dc && dc->title.drawing.strokes.size() == 2 && dc->title.drawing.reveal == 0);
    CHECK(dc && dc->title.posX == 10 && dc->title.posY == -5);
    CHECK(dc && dc->title.shape.followClip == tracked && dc->title.shape.followNode == 2);
    edit(QString(R"([{"op":"draw","clip":%1,"follow":false}])").arg(id));
    CHECK(drawingIn(saved())->title.shape.followClip == 0);
    // Errors
    CHECK(errorOf(R"([{"op":"draw"}])").startsWith("BAD_ARGUMENT"));                                  // no strokes
    CHECK(errorOf(R"([{"op":"draw","strokes":[{"points":[0.1,0.2,0.3]}]}])").contains("even count"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[{"points":[]}]}])").contains("no points"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[5,5]]]}])").contains("outside"));                     // pixels without units
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[0.1,0.1]]],"units":"cm"}])").startsWith("BAD_ARGUMENT"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[0.1,0.1]]],"width":0}])").startsWith("BAD_ARGUMENT"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[0.1,0.1]]],"color":"brown-ish"}])").startsWith("BAD_ARGUMENT"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[0.1,0.1]]],"reveal":true}])").startsWith("BAD_ARGUMENT"));
    CHECK(errorOf(QString(R"([{"op":"draw","clip":%1,"remove":7}])").arg(id)).contains("does not exist"));
    CHECK(errorOf(QString(R"([{"op":"draw","clip":%1,"strokes":[]}])").arg(tracked)).contains("not a drawing"));
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[0.1,0.1]]],"brush":3}])").startsWith("BAD_ARGUMENT")); // unknown key
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[2.5,0.1]]]}])").contains("outside"));              // past -1..2
    CHECK(errorOf(R"([{"op":"draw","strokes":[[[-0.9,0.1],[1.9,0.9]]]}])") == "none");            // within -1..2
    // op title does not make empty shapes or drawings
    CHECK(errorOf(R"([{"op":"title","kind":"drawing"}])").contains("op: draw"));
    CHECK(errorOf(R"([{"op":"title","kind":"shape"}])").contains("op: shape"));
    // check: a reveal longer than the clip
    edit(QString(R"([{"op":"draw","clip":%1,"reveal":"4s"}])").arg(id));
    bool longReveal = false;
    for (const QJsonValue& v : run("check", {{"project", g_path}}).value("issues").toArray())
        if (v.toObject().value("check").toString() == "drawing_reveal_long") longReveal = true;
    CHECK(longReveal);
    // help
    const QJsonObject help = run("help", {{"command", "edit"}, {"op", "draw"}});
    CHECK(QJsonDocument(help).toJson().contains("reveal"));
}

// ---- check: a drawing at the bottom of a vertical timeline is in the apps' unsafe area, like a title ----
void testCheck()
{
    std::printf("check\n");
    const ProjectFormat vertical{1080, 1920, {25, 1}};
    Timeline tl = timelineOf({drawingClip(5, {stroke({{0.2, 0.92}, {0.8, 0.95}}, 40)}, 50)});
    bool found = false;
    for (const ExportCheck::Issue& is : ExportCheck::layout(tl, 0, 50, vertical))
        if (is.check == QLatin1String("safe_area") && is.clip == 5) found = true;
    CHECK(found);
    tl.video[0].clips[0].title.drawing.strokes = {stroke({{0.3, 0.45}, {0.7, 0.5}}, 40)}; // the middle is safe
    found = false;
    for (const ExportCheck::Issue& is : ExportCheck::layout(tl, 0, 50, vertical))
        if (is.check == QLatin1String("safe_area")) found = true;
    CHECK(!found);
    // Following a tracker that is gone: named as a drawing
    tl.video[0].clips[0].title.shape.followClip = 77;
    tl.video[0].clips[0].title.shape.followNode = 2;
    found = false;
    for (const ExportCheck::Issue& is : ExportCheck::timeline(tl, 0, 50, vertical))
        if (is.check == QLatin1String("shape_follow_missing") && is.clip == 5)
            found = is.message.contains(T("Zeichnung folgt einem Tracker, den es nicht mehr gibt (Clip %1, Knoten %2) – "
                                          "sie bleibt stehen").arg(77).arg(2));
    CHECK(found);
}

// ---- Inspector: the section "Zeichnung" for a drawing (its stroke count, Leeren), not for a text ----
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
    e.addTitle(0, -1, 50, TitleKind::Drawing, [](Clip& c) {
        c.title.drawing.strokes << stroke({{0.1, 0.1}, {0.2, 0.2}}, 4) << stroke({{0.3, 0.3}}, 4);
    });
    const int id = *sel.ids().begin();
    insp.resize(380, 1600);
    insp.show();
    QCoreApplication::processEvents();
    QPushButton* clear = nullptr;
    for (QPushButton* b : insp.findChildren<QPushButton*>())
        if (b->text() == T("Leeren")) clear = b;
    if (!CHECK(clear && clear->isVisibleTo(&insp))) return;
    bool counted = false;
    for (QLabel* l : insp.findChildren<QLabel*>())
        if (l->text() == T("%1 Striche").arg(2) && l->isVisibleTo(&insp)) counted = true;
    CHECK(counted);
    bool asked = false;
    QObject::connect(&insp, &Inspector::drawToolRequested, [&] { asked = true; });
    for (QPushButton* b : insp.findChildren<QPushButton*>())
        if (b->text() == T("Malen")) b->click();
    CHECK(asked);
    clear->click();
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK(c && c->title.drawing.strokes.isEmpty());
    // A text title: no drawing section
    sel.clear();
    e.addTitle(100, -1, 50, TitleKind::Text);
    QCoreApplication::processEvents();
    CHECK(!clear->isVisibleTo(&insp));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("drawing");
    I18n::setLanguage("en");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    g_blue = still(tmp.filePath("blue.png"), kBlue);
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testRender();
    testReveal();
    testProjectFile(tmp.path());
    testSmooth();
    testPaint();
    testBox();
    testOffFrame(tmp.path());
    testRevealSplit();
    testTransition();
    testTablet();
    testLockedAndZoomedEraser();
    testFollow();
    testCli(tmp.path());
    testCheck();
    testInspector();
    return Check::result();
}
