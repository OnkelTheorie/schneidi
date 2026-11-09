// SPDX-License-Identifier: AGPL-3.0-only
// Title looks (0.19): gradient fill (top differs from bottom in the stop colours, also left/right), texture fill
// visible, 2.5D edge in the direction of its angle, drop shadow at its offset, glow around the letters, a plain title
// unchanged by an unused look, project file round trip (plain titles keep their old shape), the CLI op `title`
// options and the compositing node kinds text, shadow and glitch (single-node runner).
// SCHNEIDI_PICTURE_DUMP=<folder>: saves the rendered pictures there to look at.
#include "render_check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "engine/picture/CompNodes.h"
#include "engine/video/Text.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <clocale>
#include <cmath>
#include <functional>

namespace {

const ProjectFormat kFormat{640, 360, {25, 1}};
QString g_texture; // left half green, right half magenta

QImage render(const TitleStyle& st, const QString& name)
{
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Title;
    c.out = 24;
    c.title = st;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips << c;
    const QImage img = Check::renderFrame(tl, kFormat, 0).convertToFormat(QImage::Format_RGB32);
    if (const QString dump = qEnvironmentVariable("SCHNEIDI_PICTURE_DUMP"); !dump.isEmpty() && !img.isNull())
        img.save(QString("%1/title-style-%2.png").arg(dump, name));
    return img;
}

TitleStyle base()
{
    TitleStyle st;
    st.text = "HH";
    st.size = 140;
    st.bold = true;
    st.color = Qt::white;
    return st;
}

bool dark(QRgb p) { return qRed(p) < 20 && qGreen(p) < 20 && qBlue(p) < 20; }
bool white(QRgb p) { return qRed(p) > 235 && qGreen(p) > 235 && qBlue(p) > 235; }

// Bounding box of the pixels that match
QRect boxOf(const QImage& img, const std::function<bool(QRgb)>& match)
{
    QRect r;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (match(img.pixel(x, y))) r |= QRect(x, y, 1, 1);
    return r;
}

void testPlainUnchanged()
{
    std::printf("plain title unchanged\n");
    const TitleStyle plain = base();
    const QImage a = render(plain, "plain");
    CHECK(!boxOf(a, white).isEmpty());
    // Values of a look that is off (fill solid, groups off) change nothing
    TitleStyle unused = plain;
    unused.look.stops = {{0, Qt::red}, {1, Qt::blue}};
    unused.look.texture = g_texture;
    unused.look.edgeDepth = 40;
    unused.look.shadowX = 30;
    unused.look.glowRadius = 50;
    CHECK(!unused.look.active());
    CHECK(render(unused, "unused") == a);
    // Same with an outline and a box
    TitleStyle boxed = plain;
    boxed.outlineOn = boxed.boxOn = true;
    TitleStyle boxedUnused = boxed;
    boxedUnused.look = unused.look;
    CHECK(render(boxedUnused, "boxed-unused") == render(boxed, "boxed"));
}

void testGradient()
{
    std::printf("gradient\n");
    TitleStyle st = base();
    st.look.fill = TitleFill::Gradient;
    st.look.stops = {{0, QColor(255, 0, 0)}, {1, QColor(0, 0, 255)}};
    const QImage img = render(st, "gradient");
    const QRect ink = boxOf(img, [](QRgb p) { return !dark(p); });
    if (!CHECK(!ink.isEmpty())) return;
    CHECK(boxOf(img, white).isEmpty()); // no letter is drawn in the title colour
    // The top row of the letters is red, the bottom row blue (columns through the left stem of the first H)
    auto rowColor = [&](int y) {
        long r = 0, b = 0;
        for (int x = ink.left(); x <= ink.right(); ++x) {
            r += qRed(img.pixel(x, y));
            b += qBlue(img.pixel(x, y));
        }
        return std::pair{r, b};
    };
    const auto [tr, tb] = rowColor(ink.top() + 3);
    const auto [br, bb] = rowColor(ink.bottom() - 3);
    CHECK(tr > 3 * tb);
    CHECK(bb > 3 * br);
    // Angle 0: left to right
    st.look.gradAngle = 0;
    const QImage side = render(st, "gradient-0");
    const int y = ink.center().y();
    QRgb left = 0, right = 0;
    for (int x = ink.left(); x <= ink.right(); ++x)
        if (!dark(side.pixel(x, y))) {
            if (!left) left = side.pixel(x, y);
            right = side.pixel(x, y);
        }
    CHECK(qRed(left) > 3 * qBlue(left));
    CHECK(qBlue(right) > 3 * qRed(right));
    // Three stops: the middle shows the middle stop's colour
    st.look.gradAngle = 90;
    st.look.stops = {{0, QColor(255, 0, 0)}, {0.5, QColor(0, 255, 0)}, {1, QColor(0, 0, 255)}};
    const QImage three = render(st, "gradient-3");
    long g = 0, rb = 0;
    for (int x = ink.left(); x <= ink.right(); ++x) {
        g += qGreen(three.pixel(x, ink.center().y()));
        rb += qRed(three.pixel(x, ink.center().y())) + qBlue(three.pixel(x, ink.center().y()));
    }
    CHECK(g > 2 * rb);
}

void testTexture()
{
    std::printf("texture\n");
    TitleStyle st = base();
    st.look.fill = TitleFill::Texture;
    st.look.texture = g_texture;
    const QImage img = render(st, "texture");
    const QRect green = boxOf(img, [](QRgb p) { return qGreen(p) > 200 && qRed(p) < 60 && qBlue(p) < 60; });
    const QRect magenta = boxOf(img, [](QRgb p) { return qRed(p) > 200 && qBlue(p) > 200 && qGreen(p) < 60; });
    CHECK(!green.isEmpty());
    CHECK(!magenta.isEmpty());
    CHECK(green.center().x() < magenta.center().x()); // the picture's left half on the left letters
    CHECK(boxOf(img, white).isEmpty());
    // A missing picture: drawn in the plain colour (no crash, no empty title)
    st.look.texture = "/nonexistent/texture.png";
    CHECK(render(st, "texture-missing") == render(base(), "plain"));
}

void testEdge()
{
    std::printf("edge\n");
    const QRect letters = boxOf(render(base(), "plain"), white);
    auto red = [](QRgb p) { return qRed(p) > 60 && qGreen(p) < 40 && qBlue(p) < 40; };
    TitleStyle st = base();
    st.look.edgeOn = true;
    st.look.edgeDepth = 30;
    st.look.edgeAngle = 0; // to the right
    st.look.edgeColor = QColor(255, 0, 0);
    st.look.edgeLight = false;
    QRect edge = boxOf(render(st, "edge-0"), red);
    if (CHECK(!edge.isEmpty())) {
        CHECK(edge.right() > letters.right() + 20);
        CHECK(edge.left() >= letters.left());
        CHECK(std::abs(edge.top() - letters.top()) <= 2);
    }
    st.look.edgeAngle = 90; // down
    edge = boxOf(render(st, "edge-90"), red);
    if (CHECK(!edge.isEmpty())) {
        CHECK(edge.bottom() > letters.bottom() + 20);
        CHECK(edge.top() >= letters.top());
        CHECK(std::abs(edge.right() - letters.right()) <= 2);
    }
    st.look.edgeAngle = 225; // up left
    edge = boxOf(render(st, "edge-225"), red);
    if (CHECK(!edge.isEmpty())) {
        CHECK(edge.left() < letters.left() - 14);
        CHECK(edge.top() < letters.top() - 14);
    }
    // The letters stay in front
    const QImage front = render(st, "edge-front");
    CHECK(white(front.pixel(letters.left() + 5, letters.center().y())));
    // Light from the top: the edge brighter above than below (a column through the edge right of the letters)
    st.look.edgeAngle = 0;
    st.look.edgeLight = true;
    st.look.edgeColor = QColor(128, 0, 0);
    const QImage lit = render(st, "edge-lit");
    const int x = letters.right() + 10;
    int first = -1, last = -1;
    for (int y = 0; y < lit.height(); ++y)
        if (qRed(lit.pixel(x, y)) > 20) {
            if (first < 0) first = y;
            last = y;
        }
    if (CHECK(first >= 0 && last > first + 20)) CHECK(qRed(lit.pixel(x, first + 3)) > qRed(lit.pixel(x, last - 3)) + 30);
}

void testShadowGlow()
{
    std::printf("shadow, glow\n");
    const QRect letters = boxOf(render(base(), "plain"), white);
    auto green = [](QRgb p) { return qGreen(p) > 40 && qRed(p) < 30 && qBlue(p) < 30; };
    TitleStyle st = base();
    st.look.shadowOn = true;
    st.look.shadowX = 40;
    st.look.shadowY = 20;
    st.look.shadowSoft = 0;
    st.look.shadowOpacity = 1;
    st.look.shadowColor = QColor(0, 255, 0);
    const QRect shadow = boxOf(render(st, "shadow"), green);
    if (CHECK(!shadow.isEmpty())) {
        CHECK(std::abs(shadow.right() - (letters.right() + 40)) <= 2);
        CHECK(std::abs(shadow.bottom() - (letters.bottom() + 20)) <= 2);
        CHECK(shadow.left() > letters.left() + 30);
    }
    // Soft: it reaches further and fades
    st.look.shadowSoft = 12;
    const QRect soft = boxOf(render(st, "shadow-soft"), [](QRgb p) { return qGreen(p) > 8 && qRed(p) < 30; });
    CHECK(soft.right() > shadow.right() + 4);
    // Glow: light all around the letters
    TitleStyle g = base();
    g.color = QColor(0, 0, 255);
    g.look.glowOn = true;
    g.look.glowRadius = 20;
    g.look.glowColor = QColor(255, 255, 0);
    g.look.glowStrength = 0.8;
    const QImage glow = render(g, "glow");
    const QRect lit = boxOf(glow, [](QRgb p) { return qRed(p) > 10 && qGreen(p) > 10; });
    CHECK(lit.left() < letters.left() - 4 && lit.right() > letters.right() + 4);
    CHECK(lit.top() < letters.top() - 4 && lit.bottom() > letters.bottom() + 4);
}

void testProjectFile(const QString& dir)
{
    std::printf("project file\n");
    Project p;
    ProjectData d;
    d.format = kFormat;
    d.timeline.video.resize(1);
    p.load(d);
    p.edit("setup", [&](Timeline& tl) {
        Clip c;
        c.id = p.newClipId();
        c.kind = ClipKind::Title;
        c.out = 24;
        TitleLook& l = c.title.look;
        l.fill = TitleFill::Texture;
        l.stops = {{0, QColor(1, 2, 3)}, {0.3, QColor(4, 5, 6, 128)}, {1, QColor(7, 8, 9)}};
        l.gradAngle = 33;
        l.texture = g_texture;
        l.texScale = 2.5;
        l.texX = 11;
        l.texY = -12;
        l.edgeOn = true;
        l.edgeDepth = 21;
        l.edgeAngle = 135;
        l.edgeColor = QColor(10, 20, 30);
        l.edgeLight = false;
        l.shadowOn = true;
        l.shadowX = -3;
        l.shadowY = 4;
        l.shadowSoft = 5;
        l.shadowColor = QColor(1, 1, 1, 200);
        l.shadowOpacity = 0.25;
        l.glowOn = true;
        l.glowRadius = 7;
        l.glowColor = QColor(200, 100, 0);
        l.glowStrength = 0.75;
        tl.video[0].clips << c;
    });
    const QString path = dir + "/t.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline == p.timeline());
    const QJsonObject title = ProjectFile::clipJson(p.timeline().video[0].clips[0]).value("title").toObject();
    for (const char* key : {"fill", "gradient", "texture", "edge3d", "shadow", "glow"}) CHECK(title.contains(key));
    // A plain title keeps its old shape in the file
    Clip plain;
    plain.kind = ClipKind::Title;
    const QJsonObject old = ProjectFile::clipJson(plain).value("title").toObject();
    for (const char* key : {"fill", "gradient", "texture", "edge3d", "shadow", "glow"}) CHECK(!old.contains(key));
    // Bad values in a file: ignored (the default or the value before), the title stays readable
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly));
    QJsonValue doc = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    std::function<void(QJsonValue&)> spoil = [&](QJsonValue& v) {
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            for (auto it = a.begin(); it != a.end(); ++it) {
                QJsonValue x = *it;
                spoil(x);
                *it = x;
            }
            v = a;
        } else if (v.isObject()) {
            QJsonObject o = v.toObject();
            if (o.contains("glow") && o.contains("edge3d")) {
                o["glow"] = QJsonObject{{"radius", "huge"}, {"strength", 7}};
                o["fill"] = "plaid";
                o["gradient"] = 5;
            }
            for (auto it = o.begin(); it != o.end(); ++it) {
                QJsonValue x = *it;
                spoil(x);
                *it = x;
            }
            v = o;
        }
    };
    spoil(doc);
    ProjectData spoiled;
    CHECK(ProjectFile::fromJson(QJsonDocument(doc.toObject()).toJson(), path, &spoiled, &error));
    if (CHECK(!spoiled.timeline.video.isEmpty() && !spoiled.timeline.video[0].clips.isEmpty())) {
        const TitleLook& l = spoiled.timeline.video[0].clips[0].title.look;
        CHECK(l.fill == TitleFill::Solid);
        CHECK_EQ(l.glowRadius, TitleLook().glowRadius);
        CHECK_EQ(l.glowStrength, 1.0); // clamped
        CHECK_EQ(int(l.stops.size()), 0);
    }
}

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
        return e.code;
    }
    return "none";
}

const TitleLook& lookOf(const ProjectData& d)
{
    static TitleLook none;
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle()) return c.title.look;
    return none;
}

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
        d.format = kFormat;
        d.timeline.video.resize(1);
        p.load(d);
        QString error;
        CHECK(ProjectFile::save(p.data(), g_path, &error));
    }
    run("edit", {{"project", g_path},
                 {"ops", R"([{"op":"title","text":"GG","gradient":["#ff0000","#00ff00","#0000ff"],"edge3d":true,
                             "shadow":{"x":3,"y":4,"soft":2},"glow":{"radius":9,"on":false}}])"}});
    ProjectData d = saved();
    TitleLook l = lookOf(d);
    CHECK(l.fill == TitleFill::Gradient);
    CHECK_EQ(int(l.stops.size()), 3);
    if (l.stops.size() == 3) {
        CHECK_EQ(l.stops[1].at, 0.5);
        CHECK(l.stops[1].color == QColor(0, 255, 0));
    }
    CHECK(l.edgeOn);
    CHECK(l.shadowOn);
    CHECK_EQ(l.shadowX, 3.0);
    CHECK_EQ(l.shadowY, 4.0);
    CHECK(!l.glowOn);
    CHECK_EQ(l.glowRadius, 9.0);
    const int id = d.timeline.video[0].clips.isEmpty() ? 0 : d.timeline.video[0].clips[0].id;
    // Texture on the same title: the fill follows, the path is stored absolute
    run("edit", {{"project", g_path},
                 {"ops", QString(R"([{"op":"title","clip":%1,"texture":{"path":"%2","scale":2},"edge3d":false,
                                     "gradient":{"stops":[{"at":0,"color":"#ffffff"},{"at":1,"color":"#000000"}],"angle":0}}])")
                             .arg(id)
                             .arg(g_texture)}});
    l = lookOf(saved());
    CHECK(l.fill == TitleFill::Texture); // gradient and texture without fill: the texture (read last) fills
    CHECK_EQ(l.texture, g_texture);
    CHECK_EQ(l.texScale, 2.0);
    CHECK(!l.edgeOn);
    CHECK_EQ(l.gradAngle, 0.0);
    // fill given: wins over the shortcut
    run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"title","clip":%1,"fill":"solid"}])").arg(id)}});
    CHECK(lookOf(saved()).fill == TitleFill::Solid);
    // `info` shows the look in the form the op takes
    const QJsonObject info = run("info", {{"project", g_path}});
    CHECK(QJsonDocument(info).toJson().contains("\"shadow\"")); // still on (edge3d was switched off again)
    // Mistakes are named
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"gradient":["#ff0000"]}])").arg(id)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"fill":"plaid"}])").arg(id)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"texture":"/nonexistent.png"}])").arg(id)),
             QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"glow":{"strength":3}}])").arg(id)), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(QString(R"([{"op":"title","clip":%1,"shadow":"yes"}])").arg(id)), QString("BAD_ARGUMENT"));
    // The help names the options
    const QString help = QJsonDocument(run("help", {{"command", "edit"}, {"op", "title"}})).toJson();
    for (const char* key : {"gradient", "texture", "edge3d", "shadow", "glow"}) CHECK(help.contains(key));
}

// ---------- node kinds ----------

using Picture::Comp::Buf;

Buf square(int w, int h, QRect r, QColor c)
{
    Buf b(Buf::Rgba, w, h);
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            uint8_t* p = b.data() + (size_t(y) * w + x) * 4;
            p[0] = uint8_t(c.red());
            p[1] = uint8_t(c.green());
            p[2] = uint8_t(c.blue());
            p[3] = uint8_t(c.alpha());
        }
    return b;
}
const uint8_t* px(const Buf& b, int x, int y) { return b.data() + (size_t(y) * b.w + x) * 4; }

void testTextNode()
{
    std::printf("node text\n");
    Picture::Comp::RunOptions o;
    o.size = {320, 180};
    o.scale = 0.5;
    Buf out;
    const QVariantMap params{{"text", "HH"}, {"size", 160}, {"bold", true}, {"fill", "gradient"},
                             {"c0", "#ffff0000"}, {"c1", "#ff0000ff"}, {"y", 0.4}};
    if (!CHECK(Picture::Comp::runNode("text", params, {}, o, &out))) return;
    QRect ink;
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            if (px(out, x, y)[3] > 200) ink |= QRect(x, y, 1, 1);
    if (!CHECK(!ink.isEmpty())) return;
    CHECK(out.area.contains(ink));
    CHECK(std::abs(ink.center().x() - 160) <= 3);
    CHECK(std::abs(ink.center().y() - 72) <= 12); // the block is centred (with the font's descent)
    auto at = [&](int y) {
        for (int x = ink.left(); x <= ink.right(); ++x)
            if (px(out, x, y)[3] == 255) return px(out, x, y);
        return px(out, 0, 0);
    };
    CHECK(at(ink.top() + 1)[0] > 3 * at(ink.top() + 1)[2]);
    CHECK(at(ink.bottom() - 1)[2] > 3 * at(ink.bottom() - 1)[0]);
    // Opacity 0.5: the same text at half the alpha (fades a label in and out); 0 = invisible
    QVariantMap half = params;
    half["opacity"] = 0.5;
    Buf faded;
    if (CHECK(Picture::Comp::runNode("text", half, {}, o, &faded)) && CHECK(faded.px.size() == out.px.size())) {
        int worst = 0;
        for (size_t i = 3; i < out.px.size(); i += 4) worst = std::max(worst, std::abs(int(faded.px[i]) - (int(out.px[i]) + 1) / 2));
        CHECK(worst <= 1);
    }
    // Visible range (seconds from the clip start, fade 0.5 s): nothing before/after, half in the middle of the fade
    {
        QVariantMap ranged = params;
        ranged["from"] = 2.0;
        ranged["until"] = 4.0;
        ranged["fade"] = 0.5;
        const auto alphaAt = [&](double seconds) {
            Picture::Comp::RunOptions at = o;
            at.fps = 25;
            at.t = seconds * 25;
            Buf b;
            if (!Picture::Comp::runNode("text", ranged, {}, at, &b)) return -1;
            int best = 0;
            for (size_t i = 3; i < b.px.size(); i += 4) best = std::max(best, int(b.px[i]));
            return best;
        };
        CHECK_EQ(alphaAt(1.0), -1);  // before: the node makes nothing
        CHECK_EQ(alphaAt(4.5), -1);  // after
        CHECK_EQ(alphaAt(3.0), 255); // inside
        CHECK(std::abs(alphaAt(2.25) - 128) <= 2); // half way into the fade-in
        const Picture::Comp::NodeType* k = Picture::Comp::find("text");
        CHECK(k && Picture::Comp::varies(*k, ranged) && !Picture::Comp::varies(*k, params)); // cached only without a range
    }
    // No text: nothing
    CHECK(!Picture::Comp::runNode("text", {{"text", " "}}, {}, o, &out));
}

void testShadowNode()
{
    std::printf("node shadow\n");
    Picture::Comp::RunOptions o;
    o.size = {100, 80};
    o.scale = 2; // offsets are project pixels
    const Buf src = square(100, 80, QRect(20, 20, 20, 20), Qt::white);
    Buf out;
    const QVariantMap params{{"x", 10}, {"y", 5}, {"soft", 0}, {"opacity", 1}, {"color", "#ff00ff00"}};
    if (!CHECK(Picture::Comp::runNode("shadow", params, {{"src", src}}, o, &out))) return;
    CHECK(px(out, 30, 30)[0] == 255 && px(out, 30, 30)[3] == 255);   // the square stays in front
    CHECK(px(out, 55, 40)[1] == 255 && px(out, 55, 40)[0] == 0);     // shadow at +20, +10 output pixels
    CHECK(px(out, 55, 40)[3] == 255);
    CHECK(px(out, 61, 41)[3] == 0);                                  // beyond it nothing
    CHECK(px(out, 15, 30)[3] == 0);                                  // not on the other side
    // Opacity 0: the input as it is
    Buf same;
    CHECK(Picture::Comp::runNode("shadow", {{"opacity", 0}}, {{"src", src}}, o, &same));
    CHECK(same.px == src.px);
    // Half opacity: alpha about half where only the shadow is
    CHECK(Picture::Comp::runNode("shadow", {{"x", 10}, {"y", 5}, {"soft", 0}, {"opacity", 0.5}}, {{"src", src}}, o, &out));
    CHECK(std::abs(int(px(out, 55, 40)[3]) - 128) <= 2);
    // No input: nothing
    CHECK(!Picture::Comp::runNode("shadow", {}, {}, o, &out));
}

void testGlitchNode()
{
    std::printf("node glitch\n");
    Picture::Comp::RunOptions o;
    o.size = {160, 90};
    Buf src(Buf::Rgba, 160, 90);
    for (int y = 0; y < 90; ++y)
        for (int x = 0; x < 160; ++x) {
            uint8_t* p = src.data() + (size_t(y) * 160 + x) * 4;
            p[0] = uint8_t(x * 255 / 159);
            p[1] = uint8_t(y * 255 / 89);
            p[2] = uint8_t((x + y) & 0xff);
            p[3] = 255;
        }
    Buf a, b, c, d, zero;
    const QVariantMap params{{"strength", 0.6}, {"seed", 7}};
    o.frameNo = 3;
    CHECK(Picture::Comp::runNode("glitch", params, {{"src", src}}, o, &a));
    CHECK(Picture::Comp::runNode("glitch", params, {{"src", src}}, o, &b));
    CHECK(a.px == b.px);  // deterministic
    CHECK(a.px != src.px); // and visible
    o.frameNo = 4;
    CHECK(Picture::Comp::runNode("glitch", params, {{"src", src}}, o, &c));
    CHECK(c.px != a.px); // a new pattern every frame
    o.frameNo = 3;
    CHECK(Picture::Comp::runNode("glitch", {{"strength", 0.6}, {"seed", 8}}, {{"src", src}}, o, &d));
    CHECK(d.px != a.px); // another seed, another pattern
    CHECK(Picture::Comp::runNode("glitch", {{"strength", 0}}, {{"src", src}}, o, &zero));
    CHECK(zero.px == src.px);
    // Mild by default: most pixels stay close to the input
    Buf mild;
    CHECK(Picture::Comp::runNode("glitch", {}, {{"src", src}}, o, &mild));
    int same = 0;
    for (size_t i = 0; i < src.px.size(); i += 4)
        if (std::abs(int(mild.px[i + 1]) - int(src.px[i + 1])) < 40) ++same;
    CHECK(same > int(src.px.size() / 4) * 6 / 10);
    // The kind says it changes per frame
    CHECK(Picture::Comp::find("glitch") && Picture::Comp::find("glitch")->varying);
    CHECK(Picture::Comp::find("text") && !Picture::Comp::find("text")->varying);
}

// ms per frame at 1920x1080 (rough, for the report)
void measure()
{
    if (qEnvironmentVariableIsEmpty("SCHNEIDI_TITLE_STYLE_BENCH")) return;
    Picture::Comp::RunOptions o;
    o.size = {1920, 1080};
    const Buf src = square(1920, 1080, QRect(600, 400, 700, 300), Qt::white);
    auto time = [&](const char* name, const QString& kind, const QVariantMap& params) {
        Buf out;
        Picture::Comp::runNode(kind, params, {{"src", src}}, o, &out);
        QElapsedTimer t;
        t.start();
        const int n = 10;
        for (int i = 0; i < n; ++i) {
            o.frameNo = i;
            Picture::Comp::runNode(kind, params, {{"src", src}}, o, &out);
        }
        std::printf("  %-28s %.2f ms\n", name, t.nsecsElapsed() / 1e6 / n);
    };
    time("text plain", "text", {{"size", 200}});
    time("text gradient", "text", {{"size", 200}, {"fill", "gradient"}});
    time("text edge 30", "text", {{"size", 200}, {"edge", true}, {"edge_depth", 30}});
    time("text shadow+glow", "text", {{"size", 200}, {"shadow", true}, {"glow", true}});
    time("shadow soft 6", "shadow", {});
    time("glitch default", "glitch", {});
}

} // namespace

// The soft blur of shadows/glows keeps a fully opaque plane opaque and rounds (no loss of one step per pass)
void testBlurPlaneRounds()
{
    std::printf("blur plane rounds\n");
    for (const double reach : {3.0, 7.0, 20.0, 61.0}) {
        const int w = 300, h = 200;
        std::vector<uint8_t> full(size_t(w) * h, 255), half(size_t(w) * h, 128);
        Text::blurPlane(full, w, h, reach);
        Text::blurPlane(half, w, h, reach);
        const size_t mid = size_t(h / 2) * w + w / 2;
        CHECK(full[mid] == 255 && half[mid] == 128);
    }
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("title_style");
    I18n::setLanguage("en");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    QImage tex(200, 100, QImage::Format_RGB32);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x) tex.setPixel(x, y, x < 100 ? qRgb(0, 255, 0) : qRgb(255, 0, 255));
    g_texture = tmp.filePath("tex.png");
    CHECK(tex.save(g_texture));
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testBlurPlaneRounds();
    testPlainUnchanged();
    testGradient();
    testTexture();
    testEdge();
    testShadowGlow();
    testProjectFile(tmp.path());
    testCli(tmp.path());
    testTextNode();
    testShadowNode();
    testGlitchNode();
    measure();
    return Check::result();
}
