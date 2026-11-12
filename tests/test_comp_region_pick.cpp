// SPDX-License-Identifier: AGPL-3.0-only
// Text behind a wall (0.22 block 3): engine/RegionPick, masks following a four-corner tracker, recipe
// text_behind_object, op pick_region, the Comp page's pick mode. Synthetic pictures with known results:
// - pick: a flat slanted wall (antialiased, slight noise) on a high-contrast checkerboard - the polygon lies within
//   2 px of the wall's edges both ways, at most 24 corners; a poster on the wall is filled in; a shaded wall grows
//   further with more tolerance; a point outside is an error; the trackable point lies on structure, not in the wall
// - Masks::mapped: identity map = the same pixels as the rectangle / ellipse itself
// - a polygon mask following a quad tracker lands on the homography of its corners (inside/outside each corner)
// - CLI (ffmpeg): a camera pans over a scene with a pillar; pick_region add:true puts the recipe on and the mask on
//   the pillar, track_node follows the pan, the pillar stays in front of the text at frame 20; combine add/subtract
//   chain masks; a pick after tracking (frame 20) is drawn back to the reference; errors; help, effects, recipes
// - app: CompOverlay::pickAt (Shift = add) one undo step; the Inspector shows the pick row for a mask
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompRecipes.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Masks.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/MaskRaster.h"
#include "engine/RegionPick.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/video/Compose.h"
#include "ui/comp/CompInspector.h"
#include "ui/comp/CompOverlay.h"

#include "render_check.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <cstdio>

using namespace Picture::Comp;

namespace {

constexpr int W = 320, H = 180;

double segDist(QPointF p, QPointF a, QPointF b)
{
    const QPointF d = b - a;
    const double len2 = d.x() * d.x() + d.y() * d.y();
    const double t = std::clamp(len2 > 0 ? ((p.x() - a.x()) * d.x() + (p.y() - a.y()) * d.y()) / len2 : 0.0, 0.0, 1.0);
    const QPointF q = a + t * d - p;
    return std::hypot(q.x(), q.y());
}

double toOutline(QPointF p, const QPolygonF& poly)
{
    double best = 1e9;
    for (int i = 0; i < poly.size(); ++i) best = std::min(best, segDist(p, poly[i], poly[(i + 1) % poly.size()]));
    return best;
}

// Both ways: every corner found near the true outline, every point of the true outline near the one found
double outlineError(const QVector<QPointF>& found01, const QPolygonF& truth)
{
    QPolygonF found;
    for (const QPointF& p : found01) found << QPointF(p.x() * W, p.y() * H);
    double worst = 0;
    for (const QPointF& p : found) worst = std::max(worst, toOutline(p, truth));
    for (int i = 0; i < truth.size(); ++i) {
        const QPointF a = truth[i], b = truth[(i + 1) % truth.size()];
        const int n = int(std::hypot(b.x() - a.x(), b.y() - a.y()) / 2);
        for (int k = 0; k <= n; ++k) worst = std::max(worst, toOutline(a + (b - a) * (double(k) / std::max(1, n)), found));
    }
    return worst;
}

const QPolygonF kWall{QPointF(60, 30), QPointF(250, 20), QPointF(270, 150), QPointF(80, 165)};

void checker(QImage& img, int shift = 0)
{
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const bool on = (((x + shift) / 8) + (y / 8)) & 1;
            img.setPixel(x, y, on ? qRgb(225, 225, 225) : qRgb(25, 25, 25));
        }
}

// The wall: flat orange (or shaded from left to right) with slight noise, antialiased edges
QImage wallPicture(bool poster, int shade = 0)
{
    QImage img(W, H, QImage::Format_RGB32);
    checker(img);
    QImage wall(W, H, QImage::Format_ARGB32_Premultiplied);
    wall.fill(Qt::transparent);
    {
        QPainter p(&wall);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        QLinearGradient g(60, 0, 270, 0);
        g.setColorAt(0, QColor(std::max(0, 200 - shade), std::max(0, 120 - shade), std::max(0, 40 - shade / 2)));
        g.setColorAt(1, QColor(200, 120, 40));
        p.setBrush(g);
        p.drawPolygon(kWall);
    }
    QRandomGenerator rnd(7);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const QRgb w = wall.pixel(x, y);
            const int a = qAlpha(w);
            if (!a) continue;
            const QRgb b = img.pixel(x, y);
            const int n = a == 255 ? int(rnd.bounded(7)) - 3 : 0;
            const auto mix = [&](int fg, int bg) { return std::clamp((fg * 255 + bg * (255 - a)) / 255 + n, 0, 255); };
            img.setPixel(x, y, qRgb(mix(qRed(w), qRed(b)), mix(qGreen(w), qGreen(b)), mix(qBlue(w), qBlue(b))));
        }
    if (poster)
        for (int y = 70; y < 100; ++y)
            for (int x = 120; x < 150; ++x) img.setPixel(x, y, qRgb(30, 60, 200));
    return img;
}

void testPick()
{
    std::printf("pick\n");
    const QImage flat = wallPicture(false);
    const RegionPick::Result r = RegionPick::pick(flat, QPointF(0.5, 0.5));
    if (!CHECK(r.ok())) std::printf("       %s\n", qPrintable(r.error));
    CHECK(r.polygon.size() <= RegionPick::kMaxPoints);
    const double err = outlineError(r.polygon, kWall);
    std::printf("       wall: outline within %.2f px, %d corners, area %.3f\n", err, int(r.polygon.size()), r.area);
    CHECK(err <= 2.0);
    CHECK(!r.touchesEdge);
    CHECK(r.warning.isEmpty());
    const double area = std::abs(QPolygonF(kWall).boundingRect().width()) > 0 ? r.area : 0;
    CHECK(area > 0.35 && area < 0.5); // the wall covers ~42 % of the picture

    // A poster on the wall: its hole is filled, the outline stays the wall's
    const RegionPick::Result poster = RegionPick::pick(wallPicture(true), QPointF(0.3, 0.4));
    CHECK(poster.ok());
    const double perr = outlineError(poster.polygon, kWall);
    if (!CHECK(perr <= 2.0)) std::printf("       with poster: off by %.2f px\n", perr);

    // Shaded wall: more tolerance reaches further into the dark side
    const QImage shaded = wallPicture(false, 90);
    RegionPick::Options lo, hi;
    lo.tolerance = 0.05;
    hi.tolerance = 0.7;
    const RegionPick::Result rl = RegionPick::pick(shaded, QPointF(0.75, 0.5), lo);
    const RegionPick::Result rh = RegionPick::pick(shaded, QPointF(0.75, 0.5), hi);
    CHECK(rl.ok() && rh.ok());
    if (!CHECK(rh.area > rl.area + 0.05)) std::printf("       low %.3f high %.3f\n", rl.area, rh.area);
    CHECK(rh.area < 0.5); // still not into the checkerboard

    // Errors: outside, no picture
    CHECK(!RegionPick::pick(flat, QPointF(1.5, 0.5)).ok());
    CHECK(!RegionPick::pick(QImage(), QPointF(0.5, 0.5)).ok());

    // The point to track: on structure (checker edge around the wall), not inside the even wall
    const QPointF tp = RegionPick::trackablePoint(flat, r.polygon, 0.05);
    CHECK(toOutline(QPointF(tp.x() * W, tp.y() * H), kWall) <= 3);

    // Combine ids
    RegionPick::Combine c;
    CHECK(RegionPick::combineFromId("subtract", &c) && c == RegionPick::Combine::Subtract);
    CHECK(!RegionPick::combineFromId("xor", &c));
}

void testMapped()
{
    std::printf("mapped\n");
    for (Masks::Shape shape : {Masks::Shape::Rect, Masks::Shape::Ellipse}) {
        Masks::Spec s;
        s.shape = shape;
        s.center = {0.45, 0.5};
        s.width = 0.4;
        s.height = 0.25;
        s.rotation = 20;
        const Masks::Geometry g = Masks::toPixels(s, QSize(W, H));
        const Masks::Geometry m = Masks::mapped(g, [](QPointF p, QPointF* out) {
            *out = p;
            return true;
        });
        CHECK(m.shape == Masks::Shape::Polygon && !m.empty);
        std::vector<uint8_t> a(size_t(W) * H), b(size_t(W) * H);
        MaskRaster::render(g, W, H, a.data());
        MaskRaster::render(m, W, H, b.data());
        int worst = 0;
        long sum = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            worst = std::max(worst, std::abs(a[i] - b[i]));
            sum += std::abs(a[i] - b[i]);
        }
        if (!CHECK(worst <= 128 && double(sum) / a.size() < 0.5)) std::printf("       shape %d: worst %d mean %.3f\n", int(shape), worst, double(sum) / a.size());
    }
    // A map that fails: empty
    Masks::Spec s;
    const Masks::Geometry none = Masks::mapped(Masks::toPixels(s, QSize(W, H)), [](QPointF, QPointF*) { return false; });
    CHECK(none.empty);
}

// Mask polygon following a quad tracker: the corners land through the homography start corners -> corners found
void testQuadFollow()
{
    std::printf("quad follow\n");
    const QPointF q[4] = {{80, 40}, {200, 40}, {200, 140}, {80, 140}};
    const QPointF c[4] = {{90, 50}, {230, 30}, {240, 160}, {70, 150}};
    const QPointF poly[4] = {{100, 60}, {160, 60}, {160, 120}, {100, 120}};
    QVariantMap tracker{{"quad", 1.0}}, mask{{"shape", "polygon"}, {"points", 4}, {"follow", 2}};
    for (int i = 0; i < 4; ++i) {
        tracker[QString("q%1x").arg(i)] = q[i].x() / W;
        tracker[QString("q%1y").arg(i)] = q[i].y() / H;
        tracker[QString("c%1x").arg(i)] = c[i].x() / W;
        tracker[QString("c%1y").arg(i)] = c[i].y() / H;
        mask[QString("p%1x").arg(i)] = poly[i].x() / W;
        mask[QString("p%1y").arg(i)] = poly[i].y() / H;
    }
    RunOptions o;
    o.size = QSize(W, H);
    o.otherNodes[2] = tracker;
    Buf out;
    if (!CHECK(runNode("mask", mask, {}, o, &out))) return;
    double from[8], to[8];
    for (int i = 0; i < 4; ++i) from[2 * i] = q[i].x(), from[2 * i + 1] = q[i].y(), to[2 * i] = c[i].x(), to[2 * i + 1] = c[i].y();
    Compose::Homography h;
    CHECK(Compose::homography(from, to, &h));
    QPointF m[4], mid;
    for (int i = 0; i < 4; ++i) {
        double x = 0, y = 0;
        Compose::mapPoint(h, poly[i].x(), poly[i].y(), &x, &y);
        m[i] = QPointF(x, y);
        mid += m[i] / 4;
    }
    const auto at = [&](QPointF p) {
        const int x = int(std::floor(p.x())), y = int(std::floor(p.y()));
        return x < 0 || y < 0 || x >= W || y >= H ? -1 : int(out.px[size_t(y) * W + x]);
    };
    CHECK(out.kind == Buf::Mask);
    CHECK(at(mid) == 255);
    for (int i = 0; i < 4; ++i) {
        const QPointF d = (mid - m[i]) / std::hypot(mid.x() - m[i].x(), mid.y() - m[i].y());
        const int in = at(m[i] + 3 * d), outside = at(m[i] - 3 * d);
        if (!CHECK(in >= 250 && outside <= 5)) std::printf("       corner %d: inside %d outside %d\n", i, in, outside);
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
QJsonObject edit(const QJsonArray& ops, QString* error = nullptr) { return run("edit", {{"project", g_project}, {"ops", ops}}, error); }
QJsonObject noteOf(const QJsonObject& r, const char* key) { return r["notes"].toArray().first().toObject()[key].toObject(); }
QString text(const QJsonValue& v) { return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

constexpr int kFrames = 30;
constexpr int kPan = 2; // pixels per frame the camera pans right (the scene moves left)
const QPolygonF kPillar{QPointF(140, 8), QPointF(185, 8), QPointF(178, 172), QPointF(146, 172)};
const QRgb kPillarColor = qRgb(60, 90, 200);

// The scene at frame t: colorful blocks and a blue pillar, all moved left by kPan * t
QImage sceneFrame(int t)
{
    QImage world(W + kPan * kFrames + 8, H, QImage::Format_RGB32);
    QRandomGenerator rnd(11);
    for (int by = 0; by < H; by += 10)
        for (int bx = 0; bx < world.width(); bx += 10) {
            const QRgb col = qRgb(int(rnd.bounded(256)) & 0xf0, 140 + int(rnd.bounded(100)), int(rnd.bounded(60)));
            for (int y = by; y < std::min(H, by + 10); ++y)
                for (int x = bx; x < std::min(world.width(), bx + 10); ++x) world.setPixel(x, y, col);
        }
    {
        QPainter p(&world);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(kPillarColor));
        p.drawPolygon(kPillar);
    }
    return world.copy(kPan * t, 0, W, H);
}

QPolygonF pillarAt(int t) { return kPillar.translated(-kPan * t, 0); }

bool nearColor(QRgb a, QRgb b, int tol = 30)
{
    return std::abs(qRed(a) - qRed(b)) <= tol && std::abs(qGreen(a) - qGreen(b)) <= tol && std::abs(qBlue(a) - qBlue(b)) <= tol;
}

// The clip alone rendered at timeline frame `pos`
QImage frameAt(int pos)
{
    const ProjectData d = load();
    return Check::renderFrame(d.timeline, d.format, pos);
}

// Pillar in front: its middle row shows the pillar color, not the white text; white text left and right of it
bool pillarInFront(int t, const char* what)
{
    const QImage img = frameAt(t);
    const QPolygonF p = pillarAt(t);
    const QPointF mid = p.boundingRect().center();
    int pillar = 0, inside = 0, whiteOutside = 0;
    for (int y = 70; y < 110; ++y)
        for (int x = 0; x < W; ++x) {
            const QPointF c(x + 0.5, y + 0.5);
            const bool in = p.containsPoint(c, Qt::OddEvenFill) && toOutline(c, p) > 3;
            const QRgb px = img.pixel(x, y);
            if (in) {
                ++inside;
                pillar += nearColor(px, kPillarColor);
            } else if (!p.containsPoint(c, Qt::OddEvenFill) && toOutline(c, p) > 3 && nearColor(px, qRgb(255, 255, 255), 20)) {
                ++whiteOutside;
            }
        }
    const bool ok = inside > 0 && pillar == inside && whiteOutside > 200;
    if (!ok) {
        std::printf("       %s: pillar pixels %d of %d, white outside %d (mid %.0f,%.0f)\n", what, pillar, inside, whiteOutside, mid.x(), mid.y());
        img.save(QDir::temp().filePath(QString("region_pick_%1.png").arg(t)));
    }
    return ok;
}

void testCli(const QString& dir)
{
    std::printf("cli\n");
    if (!Check::haveFfmpeg()) {
        std::printf("       skipped: no ffmpeg\n");
        return;
    }
    QDir().mkpath(dir + "/frames");
    for (int t = 0; t < kFrames; ++t) sceneFrame(t).save(dir + QString("/frames/f%1.png").arg(t, 3, 10, QChar('0')));
    const QString media = Check::makeMedia(dir + "/scene.mp4", {"-framerate", "25", "-i", dir + "/frames/f%03d.png", "-c:v",
                                                                "libx264", "-qp", "0", "-pix_fmt", "yuv444p"});
    if (!CHECK(!media.isEmpty())) return;
    g_project = dir + "/behind.schneidi";
    run("new", {{"project", g_project}, {"width", W}, {"height", H}, {"fps", 25}, {"media", QJsonArray{media}}});
    edit({QJsonObject{{"op", "append"}, {"media", media}}});
    ProjectData d = load();
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id;

    // Errors: no point, outside, at outside the clip; nothing changed
    QString err;
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}}}, &err);
    CHECK(err.contains("point"));
    err.clear();
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"point", QJsonArray{1.2, 0.5}}}}, &err);
    CHECK(err.contains("0..1"));
    err.clear();
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"point", QJsonArray{0.5, 0.5}}, {"at", 500}}}, &err);
    CHECK(err.contains("inside clip"));
    err.clear();
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"point", QJsonArray{0.5, 0.5}}, {"combine", "xor"}}}, &err);
    CHECK(err.contains("combine"));

    // Only looking: the polygon, nothing changed
    const QPointF click(162.0 / W, 0.5);
    {
        const QJsonObject r = edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"point", QJsonArray{click.x(), click.y()}}}});
        const QJsonObject n = noteOf(r, "pick_region");
        QVector<QPointF> poly;
        for (const QJsonValue& v : n["polygon"].toArray()) poly << QPointF(v.toArray()[0].toDouble(), v.toArray()[1].toDouble());
        const double e = outlineError(poly, pillarAt(0));
        std::printf("       pillar: outline within %.2f px, %d corners\n", e, n["points"].toInt());
        if (!CHECK(e <= 2.0)) std::printf("       %s\n", qPrintable(text(n)));
        CHECK(n["points"].toInt() <= 24);
        CHECK(!n.contains("node"));
        d = load();
        CHECK(!EffectRegistry::instance(*TimelineOps::findClip(d.timeline, clip), ::Comp::kEffectId));
    }

    // add: the recipe goes on, its mask 6 is the pillar, the fresh tracker's point moves onto structure
    const QJsonObject added = edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"point", QJsonArray{click.x(), click.y()}},
                                                {"add", true}}});
    const QJsonObject an = noteOf(added, "pick_region");
    CHECK_EQ(an["node"].toInt(), 6);
    CHECK(an["recipe"].toString() == "text_behind_object");
    CHECK_EQ(an["follow"].toInt(), 2);
    CHECK(an["next"].toString().contains("track_node"));
    if (CHECK(an.contains("tracker_point"))) {
        const QPointF tp(an["tracker_point"].toArray()[0].toDouble() * W, an["tracker_point"].toArray()[1].toDouble() * H);
        CHECK(toOutline(tp, pillarAt(0)) <= 3);
    }
    // Words over the middle (white, big), then the pillar in front of them where the clip starts
    edit({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 3},
                      {"params", QJsonObject{{"text", "MMMMMMMM"}, {"color", "#ffffff"}, {"size", 70}}}}});
    CHECK(pillarInFront(0, "frame 0"));

    // Tracked over the pan: at frame 20 the pillar moved 40 px left and is still in front
    const QJsonObject tracked = edit({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", 2}}});
    const QJsonObject tn = noteOf(tracked, "track_node");
    CHECK_EQ(tn["points"].toInt(), kFrames);
    CHECK(pillarInFront(20, "frame 20"));

    // Picked again at frame 20 (the tracker moved): drawn back to the reference, so it sits on the pillar there and
    // at frame 25
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"node", 6}, {"at", 20},
                      {"point", QJsonArray{click.x() - 40.0 / W, click.y()}}}});
    CHECK(pillarInFront(20, "picked at 20"));
    CHECK(pillarInFront(25, "picked at 20, frame 25"));
    d = load();
    const Clip* c = TimelineOps::findClip(d.timeline, clip);
    const EffectInstance* e = c ? EffectRegistry::instance(*c, ::Comp::kEffectId) : nullptr;
    if (CHECK(e)) CHECK(std::abs(e->params.value("n6.p0x").toDouble() * W - 140) < 8); // the reference at frame 0

    // combine add / subtract: masks chained behind 6, the merge reads the last one
    const QJsonObject plus = edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"combine", "add"},
                                               {"point", QJsonArray{0.05, 0.05}}, {"tolerance", 0.1}}});
    const int addNode = noteOf(plus, "pick_region")["node"].toInt();
    CHECK(addNode > 7);
    const QJsonObject minus = edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"combine", "subtract"},
                                                {"point", QJsonArray{0.95, 0.95}}, {"tolerance", 0.1}}});
    const int subNode = noteOf(minus, "pick_region")["node"].toInt();
    CHECK(subNode > addNode);
    d = load();
    c = TimelineOps::findClip(d.timeline, clip);
    e = c ? EffectRegistry::instance(*c, ::Comp::kEffectId) : nullptr;
    if (CHECK(e)) {
        CompGraph g;
        ::Comp::graphOf(*e, &g);
        CHECK(g.links.contains(CompLink{6, addNode, "mask"}));
        CHECK(g.links.contains(CompLink{addNode, subNode, "mask"}));
        CHECK(g.links.contains(CompLink{subNode, 7, "mask"}));
        CHECK(e->params.value(CompGraph::paramKey(addNode, "combine")).toString() == "add");
        CHECK(e->params.value(CompGraph::paramKey(subNode, "combine")).toString() == "subtract");
        CHECK_EQ(e->params.value(CompGraph::paramKey(subNode, "follow")).toInt(), 2);
        CHECK_EQ(RegionPick::chainEnd(*c, 6), subNode);
    }
    // A node that is no mask; a graph without a mask in a merge's mask input
    err.clear();
    edit({QJsonObject{{"op", "pick_region"}, {"clip", clip}, {"node", 3}, {"point", QJsonArray{0.5, 0.5}}}}, &err);
    CHECK(err.contains("not a mask"));

    // help, node kinds, recipes
    const QJsonObject help = run("help", {{"command", "edit"}, {"op", "pick_region"}});
    CHECK(QString::fromUtf8(QJsonDocument(help).toJson()).contains("tolerance"));
    bool noted = false;
    for (const QJsonValue& v : run("effects", {{"kind", "nodes"}})["nodes"].toArray())
        if (v["type"] == "mask") noted = v["note"].toString().contains("pick_region");
    CHECK(noted);
    QStringList recipes;
    for (const QJsonValue& v : run("effects", {{"kind", "recipes"}})["recipes"].toArray()) recipes << v["recipe"].toString();
    CHECK(recipes.contains("text_behind_object") && recipes.contains("sticker_behind_object"));
}

// The app: the viewer's pick mode (CompOverlay) and the Inspector's row
void testApp()
{
    std::printf("app\n");
    if (g_project.isEmpty()) return; // no ffmpeg
    Project p;
    p.load(load());
    Selection sel;
    Editor ed(&p, &sel);
    const int clip = p.timeline().video[0].clips.first().id;
    CompOverlay overlay(&ed);
    overlay.setTarget(clip, 6, 0);
    overlay.setVisible(true);
    overlay.setPicking(true);
    CHECK(overlay.picking());
    const int steps = p.undoStack()->index();
    const int id = overlay.pickAt(QPointF(0.95, 0.05), Qt::ShiftModifier);
    CHECK(id > 0);
    CHECK_EQ(p.undoStack()->index(), steps + 1);
    CHECK(!overlay.pickStatus().isEmpty());
    // The tolerance picks the last click again: one more step, the same node
    overlay.setPickTolerance(0.2);
    CHECK_EQ(p.undoStack()->index(), steps + 2);
    p.undoStack()->undo();
    p.undoStack()->undo();
    CompGraph g;
    ::Comp::graphOf(*EffectRegistry::instance(*TimelineOps::findClip(p.timeline(), clip), ::Comp::kEffectId), &g);
    CHECK(!g.node(id));
    // Another node: picking ends
    overlay.setTarget(clip, 2, 0);
    CHECK(!overlay.picking());

    CompInspector inspector(&ed);
    inspector.setTarget(clip, 6);
    CHECK(inspector.findChild<QPushButton*>("comp_pick_region"));
    inspector.setTarget(clip, 2);
    CHECK(!inspector.findChild<QPushButton*>("comp_pick_region"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp_region_pick");
    Check::initEngine();
    QTemporaryDir dir;
    testPick();
    testMapped();
    testQuadFollow();
    testCli(dir.path());
    testApp();
    return Check::result();
}
