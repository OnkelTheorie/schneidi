// SPDX-License-Identifier: AGPL-3.0-only
// Real 3D text (0.19, engine/text3d + node "text3d"): the mesh of "O" has its hole, the front cap's area equals the
// outline's, a 90° turn shows the side (deeper = wider), the face toward the light is brighter, pixels are
// deterministic, edges anti-aliased, a static node comes from the cache (keyframes make it render again), the graph
// survives a project round trip, Text::draw (layout taken out for Text::letters) unchanged bit for bit.
// TEXT3D_BENCH=1: ms per frame of a 3-word title at 1080p with an animated rotation.
#include "check.h"
#include "render_check.h"

#include "core/CompGraph.h"
#include "core/Keyframes.h"
#include "core/ProjectFile.h"
#include "core/TitleLookJson.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/text3d/Text3d.h"
#include "engine/video/Text.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Picture::Comp;

namespace {

Buf run(const QVariantMap& p, QSize size = {320, 180})
{
    RunOptions o;
    o.size = size;
    Buf b;
    if (!runNode("text3d", p, {}, o, &b)) return {};
    return b;
}

const uint8_t* px(const Buf& b, int x, int y) { return b.px.data() + (size_t(y) * b.w + x) * 4; }

// Opaque width of the picture: columns with any alpha >= 128
int opaqueWidth(const Buf& b)
{
    int lo = b.w, hi = -1;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            if (px(b, x, y)[3] >= 128) lo = std::min(lo, x), hi = std::max(hi, x);
    return hi < lo ? 0 : hi - lo + 1;
}

QPainterPath lettersOf(const QString& text, int pixelSize)
{
    Text::Params p;
    p.text = text;
    QFont f(QStringLiteral("Sans"));
    f.setPixelSize(pixelSize);
    f.setBold(true);
    p.font = f;
    p.area = QRectF(0, 0, 1000, 400);
    p.halign = 1;
    p.valign = 1;
    return Text::letters(p);
}

void testMesh()
{
    std::printf("mesh\n");
    // "O": no front triangle covers the middle of the hole, the ring is covered
    const QPainterPath o = lettersOf("O", 200);
    const QRectF r = o.boundingRect();
    const Text3d::Mesh m = Text3d::extrude(o, 30, 0);
    CHECK(!m.empty());
    auto covered = [&](QPointF p) {
        for (int t = 0; t < m.triangles(); ++t) {
            if (m.part[size_t(t)] != Text3d::Mesh::Front) continue;
            const float* v = m.pos.data() + size_t(t) * 9;
            auto side = [&](int a, int b) {
                return (v[b * 3] - v[a * 3]) * (p.y() - v[a * 3 + 1]) - (v[b * 3 + 1] - v[a * 3 + 1]) * (p.x() - v[a * 3]);
            };
            const double s0 = side(0, 1), s1 = side(1, 2), s2 = side(2, 0);
            if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0)) return true;
        }
        return false;
    };
    CHECK(!covered(r.center()));
    CHECK(covered(QPointF(r.left() + r.width() * 0.04, r.center().y())));
    CHECK(o.contains(QPointF(r.left() + r.width() * 0.04, r.center().y())));
    // Front cap area = outline area: "O" by its two contours (outer minus inner), any text against a fine raster
    double outline = 0;
    {
        QVector<double> areas;
        for (const QPolygonF& poly : o.toSubpathPolygons()) {
            double a = 0;
            for (int i = 0; i + 1 < poly.size(); ++i) a += poly[i].x() * poly[i + 1].y() - poly[i + 1].x() * poly[i].y();
            areas << std::abs(a) / 2;
        }
        CHECK(areas.size() == 2);
        std::sort(areas.begin(), areas.end());
        outline = areas.value(1) - areas.value(0);
    }
    const double front = Text3d::area(m, Text3d::Mesh::Front);
    CHECK(std::abs(front - outline) < outline * 1e-6);
    CHECK(std::abs(Text3d::area(m, Text3d::Mesh::Back) - outline) < outline * 1e-6);
    // Overlapping contours, several letters with holes ("A", "B", "8", "%", "&")
    const QPainterPath t = lettersOf("AB8%&@ og", 120);
    const Text3d::Mesh mt = Text3d::extrude(t, 10, 0);
    QImage ref(1000, 400, QImage::Format_ARGB32_Premultiplied);
    ref.fill(0);
    {
        QPainter p(&ref);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(t, Qt::white);
    }
    double raster = 0;
    for (int y = 0; y < ref.height(); ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(ref.constScanLine(y));
        for (int x = 0; x < ref.width(); ++x) raster += (row[x] >> 24) / 255.0;
    }
    const double capArea = Text3d::area(mt, Text3d::Mesh::Front);
    CHECK(std::abs(capArea - raster) < raster * 0.01);
    std::printf("  O: cap %.1f outline %.1f; text: cap %.1f raster %.1f, %d triangles\n", front, outline, capArea, raster,
                mt.triangles());
    // A bevel insets the caps (smaller area) and adds side triangles
    const Text3d::Mesh mb = Text3d::extrude(o, 30, 4);
    CHECK(Text3d::area(mb, Text3d::Mesh::Front) < front * 0.97);
    CHECK(mb.triangles() > m.triangles());
    // depth 0: one flat cap
    const Text3d::Mesh flat = Text3d::extrude(o, 0, 3);
    CHECK(Text3d::area(flat, Text3d::Mesh::Back) == 0 && std::abs(Text3d::area(flat, Text3d::Mesh::Front) - outline) < 1e-3 * outline);
}

void testNode()
{
    std::printf("node\n");
    const NodeType* k = find("text3d");
    CHECK(k && !k->varying && k->inputs.isEmpty());
    CHECK(k && !varies(*k, {}));
    // Front view of "O": the hole is see-through, the ring opaque, edges anti-aliased
    const QVariantMap front{{"text", "O"}, {"size", 120.0}, {"rot_x", 0.0}, {"rot_y", 0.0}, {"bevel", 0.0}};
    const Buf a = run(front);
    CHECK(!a.isNull());
    if (a.isNull()) return;
    CHECK(px(a, 160, 90)[3] == 0);
    CHECK(a.area.contains(160, 90) && a.area.width() > 40 && a.area.width() < 150);
    int partial = 0, opaque = 0;
    for (size_t i = 3; i < a.px.size(); i += 4) partial += a.px[i] > 0 && a.px[i] < 255, opaque += a.px[i] == 255;
    CHECK(partial > 20 && opaque > 500);
    // Without supersampling no partial pixels
    QVariantMap hard = front;
    hard["aa"] = 1.0;
    const Buf h = run(hard);
    int partialHard = 0;
    for (size_t i = 3; i < h.px.size(); i += 4) partialHard += h.px[i] > 0 && h.px[i] < 255;
    CHECK(partialHard == 0);
    // Deterministic
    CHECK(run(front).px == a.px);
    QVariantMap turned{{"text", "Hi"}, {"size", 100.0}, {"rot_x", 20.0}, {"rot_y", -35.0}, {"rot_z", 5.0}};
    CHECK(run(turned).px == run(turned).px);
    // Turned 90° about y: only the side shows, as wide as the depth (perspective almost off)
    auto sideWidth = [&](double depth) {
        return opaqueWidth(run({{"text", "I"}, {"size", 100.0}, {"rot_x", 0.0}, {"rot_y", 90.0}, {"depth", depth},
                                {"bevel", 0.0}, {"fov", 2.0}}));
    };
    const int w20 = sideWidth(20), w60 = sideWidth(60);
    std::printf("  side at 90°: depth 20 -> %d px, depth 60 -> %d px\n", w20, w60);
    CHECK(w20 >= 17 && w20 <= 23);
    CHECK(w60 >= 57 && w60 <= 63);
    // Light: the front facing the light (from the camera) is brighter than under grazing light
    auto lum = [](const uint8_t* p) { return p[0] + p[1] + p[2]; };
    QVariantMap lit = front;
    lit["text"] = "I";
    lit["light_height"] = 85.0;
    lit["shine"] = 0.0;
    const Buf bright = run(lit);
    lit["light_height"] = 5.0;
    const Buf dark = run(lit);
    CHECK(lum(px(bright, 160, 90)) > lum(px(dark, 160, 90)) + 200);
    // The side the light comes from is brighter: "I" turned so its left wall shows, light from the left vs the right
    QVariantMap wall{{"text", "I"}, {"size", 100.0}, {"rot_x", 0.0}, {"rot_y", 60.0}, {"depth", 60.0}, {"bevel", 0.0},
                     {"side_color", QColor(200, 200, 200)}, {"shine", 0.0}, {"light_height", 10.0}};
    auto wallLum = [&](double lightAngle) {
        wall["light_angle"] = lightAngle;
        const Buf b = run(wall);
        // The wall is the leftmost opaque column's neighbourhood
        for (int x = 0; x < b.w; ++x)
            if (px(b, x, 90)[3] == 255) return lum(px(b, x + 2, 90));
        return -1;
    };
    const int fromLeft = wallLum(180), fromRight = wallLum(0);
    std::printf("  wall: light from the left %d, from the right %d\n", fromLeft, fromRight);
    CHECK(fromLeft > fromRight + 60);
    // Empty text: nothing
    CHECK(run({{"text", "  "}}).isNull());
}

void testCacheAndProject()
{
    std::printf("cache / project\n");
    EffectInstance e;
    e.effectId = ::Comp::kEffectId;
    CompGraph g;
    g.nodes << CompNode{1, "text3d", QPointF()};
    g.out = 1;
    ::Comp::setGraph(e, g);
    e.params.insert("n1.text", "Hey");
    e.params.insert("n1.rot_y", -30.0);
    Env env;
    env.size = QSize(160, 90);
    Cache cache;
    Buf first, second;
    Stats s1, s2;
    CHECK(evaluate(e, env, &cache, &first, &s1) && s1.evaluated == 1);
    env.t = 7;
    CHECK(evaluate(e, env, &cache, &second, &s2) && s2.cached == 1 && s2.evaluated == 0 && first.px == second.px);
    Clip k;
    k.keys[Keys::effectParam(::Comp::kEffectId, "n1.rot_y")] = {Keyframe{0, -30.0}, Keyframe{10, 30.0}};
    env.clip = &k;
    Stats s3;
    Buf third;
    CHECK(evaluate(e, env, &cache, &third, &s3) && s3.evaluated == 1 && third.px != first.px);
    // Project round trip of the node and its keyframes
    QTemporaryDir dir;
    ProjectData data;
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Title;
    c.start = 0;
    c.in = 0;
    c.out = 49;
    c.effects << e;
    c.keys = k.keys;
    data.timeline.video.resize(1);
    data.timeline.video[0].clips << c;
    const QString path = dir.path() + "/t.schneidi";
    QString error;
    CHECK(ProjectFile::save(data, path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline.video.value(0).clips.size() == 1);
    if (back.timeline.video.value(0).clips.size() == 1) {
        const Clip& b = back.timeline.video[0].clips[0];
        CHECK(b.effects.size() == 1 && b.effects[0].params.value("n1.text").toString() == "Hey");
        CHECK(std::abs(b.effects[0].params.value("n1.rot_y").toDouble() + 30) < 1e-9);
        CHECK(b.keys == c.keys);
    }
}

// Text::draw as it was before the layout was taken out (0.19 package E): a plain title must stay bit-identical
void oldDraw(uint8_t* rgba, int w, int h, const Text::Params& tp)
{
    const QStringList lines = tp.text.split('\n');
    const QFontMetricsF fm(tp.font);
    double blockW = 0;
    for (const QString& line : lines) blockW = std::max(blockW, fm.horizontalAdvance(line));
    const double blockH = fm.lineSpacing() * (lines.size() - 1) + fm.height();
    const double ext = tp.pad + tp.room / 2;
    const double cw = blockW + 2 * ext, ch = blockH + 2 * ext;
    double k = 1.0;
    if (cw > 0 && tp.area.width() > 0) k = std::min(k, tp.area.width() / cw);
    if (ch > 0 && tp.area.height() > 0) k = std::min(k, tp.area.height() / ch);
    const QRectF& area = tp.area;
    const double ox = tp.halign == 0 ? area.left() : tp.halign == 1 ? area.left() + (area.width() - cw * k) / 2
                                                                    : area.right() - cw * k;
    const double oy = tp.valign == 0 ? area.top() : tp.valign == 1 ? area.top() + (area.height() - ch * k) / 2
                                                                   : area.bottom() - ch * k;
    QPainterPath path;
    for (int i = 0; i < lines.size(); ++i) {
        const double adv = fm.horizontalAdvance(lines[i]);
        const double x = ext + (tp.halign == 0 ? 0.0 : tp.halign == 1 ? (blockW - adv) / 2 : blockW - adv);
        path.addText(QPointF(x, ext + fm.ascent() + i * fm.lineSpacing()), tp.font, lines[i]);
    }
    QImage img(rgba, w, h, w * 4, QImage::Format_RGBA8888);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(ox, oy);
    p.scale(k, k);
    if (tp.bg.alpha() > 0) {
        const QRectF ink = path.boundingRect();
        const double x0 = ink.isEmpty() ? 0.0 : ink.center().x() - cw / 2;
        p.fillRect(QRectF(x0, 0, cw, ch), tp.bg);
    }
    if (tp.outline > 0 && tp.outlineColor.alpha() > 0)
        p.strokePath(path, QPen(tp.outlineColor, tp.outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, tp.fg);
    p.end();
}

void testOldTitle()
{
    std::printf("plain title unchanged\n");
    for (int variant = 0; variant < 4; ++variant) {
        Text::Params p;
        p.text = variant % 2 ? "Zwei\nZeilen hier" : "Titel";
        QFont f(QStringLiteral("Sans"));
        f.setPixelSize(40 + variant * 7);
        f.setBold(variant >= 2);
        p.font = f;
        p.area = QRectF(10, 5, variant == 3 ? 120 : 280, 150);
        p.halign = variant % 3;
        p.valign = (variant + 1) % 3;
        p.fg = QColor(250, 200, 10);
        p.bg = variant >= 1 ? QColor(0, 0, 0, 150) : QColor(Qt::transparent);
        p.pad = variant * 3;
        p.outline = variant == 2 ? 6 : 0;
        p.room = p.outline;
        std::vector<uint8_t> a(300 * 160 * 4, 0), b(300 * 160 * 4, 0);
        Text::draw(a.data(), 300, 160, p);
        oldDraw(b.data(), 300, 160, p);
        CHECK(a == b);
        // Text::letters is where draw puts the letters
        const QRectF ink = Text::letters(p).boundingRect();
        CHECK(ink.width() > 10 && p.area.adjusted(-2, -2, 2, 2).contains(ink));
    }
}

// The title option (TitleLook solidOn): drawn in 3D by the renderer, in the project file only when set, read from the
// CLI forms
void testTitle()
{
    std::printf("title option\n");
    const ProjectFormat format{640, 360, {25, 1}};
    auto render = [&](const TitleStyle& st, const QString& name) {
        Clip c;
        c.id = 1;
        c.kind = ClipKind::Title;
        c.out = 24;
        c.title = st;
        Timeline tl;
        tl.video.resize(1);
        tl.video[0].clips << c;
        const QImage img = Check::renderFrame(tl, format, 0).convertToFormat(QImage::Format_RGB32);
        if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull())
            img.save(QString("%1/text3d-%2.png").arg(dump, name));
        return img;
    };
    TitleStyle flat;
    flat.text = "3D";
    flat.size = 120;
    flat.color = QColor(250, 200, 0);
    TitleStyle solid = flat;
    solid.look.solidOn = true;
    solid.look.solidDepth = 40;
    solid.look.solidRotY = -40;
    solid.look.solidSide = QColor(40, 90, 200);
    const QImage a = render(flat, "flat"), b = render(solid, "solid");
    CHECK(!a.isNull() && !b.isNull() && a != b);
    // The side colour shows (blue-ish pixels), the front's yellow too
    int blue = 0, yellow = 0;
    for (int y = 0; y < b.height(); ++y)
        for (int x = 0; x < b.width(); ++x) {
            const QRgb c = b.pixel(x, y);
            blue += qBlue(c) > qRed(c) + 40 && qBlue(c) > 60;
            yellow += qRed(c) > 150 && qGreen(c) > 100 && qBlue(c) < 80;
        }
    CHECK(blue > 200 && yellow > 500);
    // With a box: the box stays
    TitleStyle boxed = solid;
    boxed.boxOn = true;
    boxed.boxColor = QColor(255, 0, 0);
    const QImage bx = render(boxed, "box");
    int red = 0;
    for (int y = 0; y < bx.height(); ++y)
        for (int x = 0; x < bx.width(); ++x) red += bx.pixel(x, y) == qRgb(255, 0, 0);
    CHECK(red > 1000);
    // JSON: only when set; round trip; CLI short form
    QJsonObject plain;
    TitleLookJson::write(plain, TitleLook{});
    CHECK(!plain.contains("text3d"));
    QJsonObject o;
    TitleLookJson::write(o, solid.look);
    CHECK(o.contains("text3d"));
    TitleLook back;
    CHECK(TitleLookJson::read(o, back, true) && back == solid.look);
    TitleLook cli;
    QString err;
    CHECK(TitleLookJson::read(QJsonObject{{"text3d", true}}, cli, false, &err) && cli.solidOn);
    CHECK(TitleLookJson::read(QJsonObject{{"text3d", QJsonObject{{"depth", 12}, {"rot_y", 30}}}}, cli = {}, false, &err)
          && cli.solidOn && cli.solidDepth == 12 && cli.solidRotY == 30);
    CHECK(!TitleLookJson::read(QJsonObject{{"text3d", QJsonObject{{"fov", 500}}}}, cli, false, &err) && !err.isEmpty());
}

// SCHNEIDI_PICTURE_DUMP: a few pictures of the node to look at
void dump()
{
    const QString dir = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP");
    if (dir.isEmpty()) return;
    const QVector<QPair<QString, QVariantMap>> cases{
        {"front", {{"text", "3D Ok"}, {"rot_x", 0.0}, {"rot_y", 0.0}, {"bevel", 0.0}}},
        {"front-bevel", {{"text", "3D Ok"}, {"rot_x", 0.0}, {"rot_y", 0.0}}},
        {"turned-flat", {{"text", "3D Ok"}, {"bevel", 0.0}}},
        {"turned", {{"text", "3D Ok"}}},
        {"metal", {{"text", "3D Ok"}, {"color", QColor(230, 180, 60)}, {"side_color", QColor(150, 100, 30)},
                   {"metal", 1.0}, {"shine", 0.8}, {"rot_x", 25.0}, {"rot_y", 30.0}, {"depth", 50.0}}},
        {"thin", {{"text", "3D"}, {"bold", false}, {"depth", 40.0}, {"rot_y", -40.0}, {"color", QColor(250, 200, 0)},
                  {"side_color", QColor(40, 90, 200)}}},
        {"thin-flat", {{"text", "3D"}, {"bold", false}, {"depth", 40.0}, {"rot_y", -40.0}, {"bevel", 0.0},
                       {"color", QColor(250, 200, 0)}, {"side_color", QColor(40, 90, 200)}}},
    };
    for (const auto& [name, v] : cases) {
        QVariantMap p = v;
        p["size"] = 110.0;
        const Buf b = run(p, QSize(640, 360));
        if (b.isNull()) continue;
        QImage img(b.data(), b.w, b.h, b.stride(), QImage::Format_RGBA8888);
        QImage onGrey(img.size(), QImage::Format_RGB32);
        onGrey.fill(QColor(60, 70, 80));
        QPainter(&onGrey).drawImage(0, 0, img);
        onGrey.save(QString("%1/text3d-node-%2.png").arg(dir, name));
    }
}

void bench()
{
    if (qEnvironmentVariable("TEXT3D_BENCH").isEmpty()) return;
    const QVariantMap base{{"text", "Drei Worte hier"}, {"size", 140.0}, {"depth", 40.0}, {"bevel", 4.0}};
    const int frames = 30;
    for (int aa : {2, 3}) {
        QElapsedTimer timer;
        timer.start();
        for (int i = 0; i < frames; ++i) {
            QVariantMap v = base;
            v["rot_y"] = -40.0 + i * 3;
            v["rot_x"] = 10.0 + i;
            v["aa"] = double(aa);
            CHECK(!run(v, QSize(1920, 1080)).isNull());
        }
        std::printf("  bench 1080p 3 words, aa %d: %.1f ms/frame (%d threads)\n", aa, timer.nsecsElapsed() / 1e6 / frames,
                    QThread::idealThreadCount());
    }
    QVariantMap v = base;
    v["rot_y"] = -30.0;
    const QPainterPath path = lettersOf("Drei Worte hier", 140);
    QElapsedTimer timer;
    timer.start();
    Text3d::Mesh m;
    for (int i = 0; i < frames; ++i) m = Text3d::extrude(path, 40, 4);
    std::printf("  mesh alone: %.2f ms, %d triangles\n", timer.nsecsElapsed() / 1e6 / frames, m.triangles());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("test_text3d");
    testMesh();
    testNode();
    testCacheAndProject();
    testOldTitle();
    testTitle();
    dump();
    bench();
    return Check::result();
}
