// SPDX-License-Identifier: AGPL-3.0-only
// Time range of compositing nodes (0.22): from/until for every kind with a picture output (core/CompGraph.h
// Comp::hasRange, inRange, rangeFrames/rangeValues), the engine taking a node out of the picture's way outside it
// (Picture::Comp::activeGraph: a filter or merge hands its input on, a generator gives nothing, a generator as the output
// is transparent; merge, media and 3D text), cuts and head trims keep the place in the timeline, Editor::setCompNodeRange
// (one undo step, refusals), the CLI (op node range in timeline times, `info` range, `effects kind:'nodes'`, a frame
// rendered inside/outside) and the timeline's bars (drag = one undo step, ends trim, a double-click asks for the Comp
// page, a low track shows no bars) with real settings: Russian and a big font.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>

namespace {

using namespace Picture::Comp;

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

EffectInstance comp(const QStringList& types, const QVector<CompLink>& links, int out, const QVariantMap& params = {})
{
    EffectInstance e;
    e.effectId = ::Comp::kEffectId;
    CompGraph g;
    for (int i = 0; i < types.size(); ++i) g.nodes << CompNode{i + 1, types[i], {}};
    g.links = links;
    g.out = out;
    ::Comp::setGraph(e, g);
    for (auto it = params.begin(); it != params.end(); ++it) e.params[it.key()] = it.value();
    return e;
}

Buf solid(int w, int h, QColor c)
{
    Buf b(Buf::Rgba, w, h);
    for (size_t i = 0; i < b.px.size(); i += 4) {
        b.px[i] = uint8_t(c.red());
        b.px[i + 1] = uint8_t(c.green());
        b.px[i + 2] = uint8_t(c.blue());
        b.px[i + 3] = uint8_t(c.alpha());
    }
    return b;
}

QColor at(const Buf& b, int x, int y)
{
    const uint8_t* p = b.data() + (size_t(y) * b.w + x) * 4;
    return QColor(p[0], p[1], p[2], p[3]);
}

void testCore()
{
    using namespace ::Comp;
    CHECK(hasRange("blur") && hasRange("merge") && hasRange("media") && hasRange("text") && hasRange("clip"));
    CHECK(!hasRange("mask") && !hasRange("matte") && !hasRange("tracker") && !hasRange(""));
    CHECK(hasRange("hologram")); // a newer version's kind: from/until mean the same for all
    // Every picture kind of the engine has from/until, masks and the tracker none
    for (const NodeType& k : all()) {
        const bool has = k.param("from") && k.param("until");
        if (!CHECK_EQ(has, k.output == Buf::Rgba && hasRange(k.id))) std::printf("       kind %s\n", qPrintable(k.id));
    }
    CHECK(inRange(0, 0, 0) && inRange(0, 0, 1e6));
    CHECK(!inRange(1, 2, 0.99) && inRange(1, 2, 1) && inRange(1, 2, 1.99) && !inRange(1, 2, 2));
    CHECK(inRange(-3, 2, 0) && !inRange(0, -1e-9, 0) && inRange(1, 0, 500));
    CHECK(!rangeSet(0, 0) && !rangeSet(-2, 0) && rangeSet(1, 0) && rangeSet(0, 3) && rangeSet(0, -1));

    // Frames <-> seconds, also at 29.97: every frame of the range shows, the one after does not
    Clip c;
    c.start = 100;
    c.in = 0;
    c.out = 299; // 300 frames
    for (double fps : {25.0, 30000.0 / 1001.0}) {
        double from = 0, until = 0;
        rangeValues(c, 130, 190, fps, &from, &until);
        NodeRange r{1, "blur", from, until};
        int first = 0, end = 0;
        rangeFrames(c, r, fps, &first, &end);
        CHECK_EQ(first, 130);
        CHECK_EQ(end, 190);
        CHECK(!inRange(from, until, 29 / fps) && inRange(from, until, 30 / fps) && inRange(from, until, 89 / fps)
              && !inRange(from, until, 90 / fps));
        rangeValues(c, 100, 400, fps, &from, &until); // the clip's start is "from the start"
        CHECK(near(from, 0) && near(until, 300 / fps));
    }
    int first = 0, end = 0;
    rangeFrames(c, NodeRange{1, "blur", 0, 0}, 25, &first, &end);
    CHECK(first == 100 && end == 400);
    rangeFrames(c, NodeRange{1, "blur", 2, -1}, 25, &first, &end); // over before the start
    CHECK(first == end);
    rangeFrames(c, NodeRange{1, "blur", 50, 60}, 25, &first, &end); // beyond the clip's end: clamped, empty
    CHECK(first == 400 && end == 400);

    const EffectInstance e = comp({"clip", "blur", "mask", "merge", "text"}, {{1, 2, "src"}, {2, 4, "bg"}, {5, 4, "fg"}}, 4,
                                  {{"n2.from", 1.0}, {"n2.until", 2.0}, {"n3.until", 4.0}, {"n5.until", 3.0}});
    const QVector<NodeRange> ranges = nodeRanges(e);
    CHECK_EQ(ranges.size(), 2); // the mask's until is not a range
    CHECK(ranges.size() == 2 && ranges[0].node == 2 && ranges[1].node == 5 && near(ranges[1].until, 3));
    CHECK(!activeRange(e, 1) && !activeRange(e, 3) && activeRange(e, 2) && !activeRange(e, 99));
}

void testEval()
{
    Env env;
    env.size = QSize(16, 8);
    env.fps = 25;
    Buf clipPic = solid(16, 8, QColor(0, 255, 0));
    env.clipPicture = [&]() -> const Buf* { return &clipPic; };
    env.mediaPicture = [](const QString&, double) { return solid(16, 8, QColor(0, 0, 255)); };
    const auto run = [&](const EffectInstance& e, double t, QColor* c, int x = 8, int y = 4) {
        env.t = t;
        Buf out;
        const bool ok = evaluate(e, env, nullptr, &out, nullptr);
        if (ok) *c = at(out, x, y);
        return ok;
    };
    QColor c;
    const QColor green(0, 255, 0), red(255, 0, 0), blue(0, 0, 255);
    // A generator in a merge's fg, 1 s .. 2 s: outside, the merge has no fg (its bg shows)
    const EffectInstance gen = comp({"clip", "color", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                                    {{"n2.color", "#ffff0000"}, {"n2.from", 1.0}, {"n2.until", 2.0}});
    CHECK(run(gen, 0, &c) && c == green);
    CHECK(run(gen, 24, &c) && c == green);
    CHECK(run(gen, 25, &c) && c == red);
    CHECK(run(gen, 49, &c) && c == red);
    CHECK(run(gen, 50, &c) && c == green);
    // The merge itself with the range: outside it hands its bg on
    EffectInstance merge = gen;
    merge.params.remove("n2.from");
    merge.params.remove("n2.until");
    merge.params["n3.from"] = 1.0;
    merge.params["n3.until"] = 2.0;
    CHECK(run(merge, 10, &c) && c == green);
    CHECK(run(merge, 30, &c) && c == red);
    // A media node laid in (an image for a few seconds), until 0 = to the end
    const EffectInstance media = comp({"clip", "media", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                                      {{"n2.path", "/x/b.png"}, {"n2.from", 2.0}});
    CHECK(run(media, 49, &c) && c == green);
    CHECK(run(media, 50, &c) && c == blue);
    CHECK(run(media, 5000, &c) && c == blue);
    // A filter (transform to the right half) outside its range: the picture unchanged; a chain of two both off
    const EffectInstance chain = comp({"clip", "transform", "transform"}, {{1, 2, "src"}, {2, 3, "src"}}, 3,
                                      {{"n2.x", 1.0}, {"n2.from", 1.0}, {"n3.until", 0.5}});
    CHECK(run(chain, 30, &c, 1, 4) && c.alpha() == 0); // 2 works (moved right), 3 not: the left edge is empty
    CHECK(run(chain, 20, &c, 1, 4) && c == green);    // neither works: the clip itself (output = node 1)
    // A generator as the output out of its range: transparent (not the clip's picture)
    const EffectInstance alone = comp({"clip", "color"}, {}, 2, {{"n2.color", "#ffff0000"}, {"n2.until", 1.0}});
    CHECK(run(alone, 10, &c) && c == red);
    CHECK(run(alone, 30, &c) && c.alpha() == 0);
    // activeGraph: nothing out of range = no copy
    EffectInstance active;
    CHECK(!activeGraph(gen, 30, 25, nullptr, &active));
    CHECK(activeGraph(gen, 10, 25, nullptr, &active));
    CompGraph g;
    CHECK(::Comp::graphOf(active, &g) && !g.linkInto(3, "fg") && g.linkInto(3, "bg"));
    // A keyframed until (a range that grows) is read at t
    Clip k;
    k.keys[Keys::effectParam(::Comp::kEffectId, "n2.until")] = {Keyframe{0, 1.0}, Keyframe{100, 5.0}};
    env.clip = &k;
    CHECK(run(gen, 60, &c) && c == red); // until(60) = 2.6 s > 2.4 s
    env.clip = nullptr;
    // 3D text: shows inside its range only (its own fade aside)
    env.size = QSize(160, 90);
    clipPic = solid(160, 90, QColor(0, 255, 0));
    const EffectInstance t3 = comp({"clip", "text3d", "merge"}, {{1, 3, "bg"}, {2, 3, "fg"}}, 3,
                                   {{"n2.from", 1.0}, {"n2.until", 3.0}, {"n2.fade", 0.0}, {"n2.size", 0.9}});
    const auto differs = [&](double t) {
        env.t = t;
        Buf out;
        if (!evaluate(t3, env, nullptr, &out, nullptr)) return -1;
        int n = 0;
        for (int y = 0; y < out.h; ++y)
            for (int x = 0; x < out.w; ++x) n += at(out, x, y) != QColor(0, 255, 0);
        return n;
    };
    CHECK_EQ(differs(10), 0);
    CHECK(differs(40) > 0);
    CHECK_EQ(differs(80), 0);
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    int id = 0;
    Fixture()
    {
        p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, false, false});
        p.edit("setup", [&](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
            Clip x;
            x.id = id = p.newClipId();
            x.mediaPath = "/x/a.mp4";
            x.start = 100;
            x.in = 50;
            x.out = 50 + 249; // 10 s
            x.effects << comp({"clip", "blur", "mask", "merge", "color"},
                              {{1, 2, "src"}, {2, 4, "bg"}, {5, 4, "fg"}, {3, 4, "mask"}}, 4,
                              {{"n2.from", 2.0}, {"n2.until", 6.0}, {"n5.from", 4.0}});
            tl.video[0].clips << x;
        });
    }
    double val(int clip, const QString& key)
    {
        const Clip* c = TimelineOps::findClip(p.timeline(), clip);
        const EffectInstance* e = c ? EffectRegistry::instance(*c, ::Comp::kEffectId) : nullptr;
        return e ? e->params.value(key).toDouble() : -999;
    }
};

void testEditor()
{
    Fixture f;
    const int before = f.p.undoStack()->index();
    QString err;
    CHECK(f.ed.setCompNodeRange(f.id, 2, 1.0, 3.5, &err));
    CHECK_EQ(f.p.undoStack()->index(), before + 1);
    CHECK(near(f.val(f.id, "n2.from"), 1) && near(f.val(f.id, "n2.until"), 3.5));
    CHECK(!f.ed.setCompNodeRange(f.id, 3, 1, 2, &err) && err.contains("no time range")); // a mask
    CHECK(!f.ed.setCompNodeRange(f.id, 2, 3, 2, &err) && err.contains("before"));
    CHECK(!f.ed.setCompNodeRange(f.id, 42, 1, 2, &err));
    CHECK(f.ed.setCompNodeRange(f.id, 2, 0, 0, &err)); // the whole clip again
    CHECK(!::Comp::activeRange(*EffectRegistry::instance(*TimelineOps::findClip(f.p.timeline(), f.id), ::Comp::kEffectId), 2));
    CHECK_EQ(f.p.undoStack()->index(), before + 2);
    f.p.undoStack()->undo();
    f.p.undoStack()->undo();
    CHECK(near(f.val(f.id, "n2.from"), 2) && near(f.val(f.id, "n2.until"), 6));
}

// A cut and a head trim keep every node's range where it was in the timeline (blur, color - not only text)
void testFollow()
{
    Fixture f;
    const int cut = 100 + 75; // 3 s into the clip
    f.ed.bladeAt(f.id, cut);
    int right = 0;
    for (const Clip& x : f.p.timeline().video[0].clips)
        if (x.start == cut) right = x.id;
    if (!CHECK(right)) return;
    CHECK(near(f.val(f.id, "n2.from"), 2) && near(f.val(f.id, "n2.until"), 6)); // the left piece keeps them
    CHECK(near(f.val(right, "n2.from"), -1) && near(f.val(right, "n2.until"), 3));
    CHECK(near(f.val(right, "n5.from"), 1));
    // In timeline frames nothing moved: blur 150..250 on both pieces together
    const Clip* rc = TimelineOps::findClip(f.p.timeline(), right);
    ::Comp::NodeRange r;
    CHECK(::Comp::activeRange(*EffectRegistry::instance(*rc, ::Comp::kEffectId), 2, &r));
    int first = 0, end = 0;
    ::Comp::rangeFrames(*rc, r, 25, &first, &end);
    CHECK(first == cut && end == 250);
    // A head trim of the left piece by 1 s: the blur still starts at frame 150
    f.ed.trimClip(f.id, TimelineOps::Edge::Start, 25, true);
    CHECK(near(f.val(f.id, "n2.from"), 1) && near(f.val(f.id, "n2.until"), 5));
}

QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << name << e.code << e.message;
        throw;
    }
}

QString errorOf(const QJsonArray& ops)
{
    try {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, {{"project", g_project}, {"ops", ops}}), ctx);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

void testCli(const QString& dir)
{
    const QString still = dir + "/green.png";
    {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(0, 255, 0));
        CHECK(img.save(still));
    }
    g_project = dir + "/range.schneidi";
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    run("edit", {{"project", g_project},
                 {"ops", QJsonArray{QJsonObject{{"op", "place"}, {"media", still}, {"at", 50}, {"duration", "4s"}}}}});
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id; // 50 .. 150
    const QJsonObject graph = QJsonDocument::fromJson(R"({"nodes":[{"id":1,"type":"clip"},{"id":2,"type":"color",
        "params":{"color":"#ff0000"}},{"id":3,"type":"merge"},{"id":4,"type":"mask"}],
        "links":[[1,3,"bg"],[2,3,"fg"]],"out":3})").object();
    run("edit", {{"project", g_project}, {"ops", QJsonArray{QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"graph", graph}}}}});
    // range in timeline times: frames and seconds ("3s" = timeline frame 75)
    run("edit", {{"project", g_project},
                 {"ops", QJsonArray{QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 2}, {"range", QJsonArray{75, "4s"}}}}}});
    const QJsonObject info = run("info", {{"project", g_project}});
    const QString text = QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact));
    CHECK(text.contains(R"("range":[75,100])"));
    ProjectFile::load(g_project, &d, nullptr);
    const EffectInstance* e = EffectRegistry::instance(*TimelineOps::findClip(d.timeline, clip), ::Comp::kEffectId);
    CHECK(e && near(e->params.value("n2.from").toDouble(), 1) && near(e->params.value("n2.until").toDouble(), 2));
    // A frame inside and outside the range, rendered by the picture engine
    const auto colorAt = [&](int frame) {
        const QJsonObject r = run("frames", {{"source", g_project}, {"out", dir + QString("/f%1").arg(frame)},
                                            {"at", QString::number(frame)}, {"width", 160}});
        const QImage img(r["frames"].toArray().first().toObject()["file"].toString());
        return img.isNull() ? QColor() : img.pixelColor(80, 45);
    };
    const QColor in = colorAt(80), out = colorAt(110), before = colorAt(60);
    CHECK(in.red() > 200 && in.green() < 60);
    CHECK(out.green() > 200 && out.red() < 60);
    CHECK(before.green() > 200 && before.red() < 60);
    // Errors: a mask has none, outside the clip, to before from; range:false = the whole clip
    CHECK(errorOf({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 4}, {"range", QJsonArray{60, 70}}}}).contains("no time range"));
    CHECK(errorOf({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 2}, {"range", QJsonArray{10, 70}}}}).contains("inside clip"));
    CHECK(errorOf({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 2}, {"range", QJsonArray{90, 80}}}}).contains("after"));
    run("edit", {{"project", g_project},
                 {"ops", QJsonArray{QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 2}, {"range", false}}}}});
    CHECK(!QString::fromUtf8(QJsonDocument(run("info", {{"project", g_project}})).toJson()).contains("\"range\""));
    // params from/until on any picture node (seconds from the clip's start), effects lists them
    run("edit", {{"project", g_project},
                 {"ops", QJsonArray{QJsonObject{{"op", "node"}, {"clip", clip}, {"id", 3}, {"params", QJsonObject{{"until", 1.5}}}}}}});
    CHECK(QString::fromUtf8(QJsonDocument(run("info", {{"project", g_project}})).toJson(QJsonDocument::Compact)).contains(R"("range":[50,88])"));
    bool blurHas = false, maskHas = true;
    for (const QJsonValue& v : run("effects", {{"kind", "nodes"}})["nodes"].toArray()) {
        QStringList keys;
        for (const QJsonValue& p : v["params"].toArray()) keys << p["key"].toString();
        if (v["type"] == "blur") blurHas = keys.contains("from") && keys.contains("until");
        if (v["type"] == "mask") maskHas = keys.contains("from");
    }
    CHECK(blurHas && !maskHas);
    // help names the range
    CHECK(QString::fromUtf8(QJsonDocument(run("help", {{"command", "edit"}, {"op", "node"}})).toJson()).contains("range:[from, to]"));
}

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QApplication::sendEvent(&w, &e);
}

void testUi()
{
    // Real settings: Russian, a big font
    I18n::install("ru");
    QFont big = QApplication::font();
    big.setPointSizeF(15);
    QApplication::setFont(big);
    Fixture f; // clip 100..350, blur 150..250, color from 200 to the end, a mask (no bar)
    TimelineView tv(&f.ed);
    tv.resize(1400, 400);
    tv.show();
    QApplication::processEvents();
    const auto x = [&](double frame) {
        return int(std::lround(TimelineView::kHeaderW + (frame - tv.view().leftFrame) * tv.view().pxPerFrame));
    };
    // Find the bars by the cursor: the blur's bar is over frame 170, the color's from 200 on
    const auto barY = [&](int frame, int skipBelow) { // the lowest y with the bar's cursor
        for (int y = tv.height() - 1; y >= TimelineView::kRulerH; --y) {
            if (y >= skipBelow) continue;
            mouse(tv, QEvent::MouseMove, {x(frame), y}, Qt::NoButton);
            if (tv.cursor().shape() == Qt::OpenHandCursor) return y;
        }
        return -1;
    };
    const int yBlur = barY(175, 10000);
    const int yColor = barY(300, 10000); // the second bar lies above the first one
    if (!CHECK(yBlur > 0 && yColor > 0 && yBlur != yColor)) return;
    CHECK_EQ(barY(120, 10000), -1); // before both ranges: no bar there
    // Drag the blur's bar by 25 frames: one undo step, 175..275
    const int undo = f.p.undoStack()->index();
    mouse(tv, QEvent::MouseButtonPress, {x(175), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(185), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(200), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {x(200), yBlur}, Qt::NoButton);
    CHECK_EQ(f.p.undoStack()->index(), undo + 1);
    CHECK(near(f.val(f.id, "n2.from"), 3) && near(f.val(f.id, "n2.until"), 7));
    CHECK_EQ(TimelineOps::findClip(f.p.timeline(), f.id)->start, 100); // the clip did not move
    // Its end edge: shorter by 50 frames (275 -> 225)
    mouse(tv, QEvent::MouseButtonPress, {x(275) - 2, yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(250), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(225) - 2, yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {x(225) - 2, yBlur}, Qt::NoButton);
    CHECK_EQ(f.p.undoStack()->index(), undo + 2);
    CHECK(near(f.val(f.id, "n2.from"), 3) && near(f.val(f.id, "n2.until"), 5));
    // A click without moving changes nothing
    mouse(tv, QEvent::MouseButtonPress, {x(190), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {x(190), yBlur}, Qt::NoButton);
    CHECK_EQ(f.p.undoStack()->index(), undo + 2);
    // Snapping: the start dragged near the playhead lands on it
    tv.setPlayhead(140);
    mouse(tv, QEvent::MouseButtonPress, {x(175) + 1, yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(160), yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseMove, {x(141) + 1, yBlur}, Qt::LeftButton);
    mouse(tv, QEvent::MouseButtonRelease, {x(141) + 1, yBlur}, Qt::NoButton);
    CHECK(near(f.val(f.id, "n2.from"), 40 / 25.0));
    // A double-click asks for the node on the Comp page
    int asked = 0, askedClip = 0;
    QObject::connect(&tv, &TimelineView::compNodeOpenRequested, [&](int clip, int node) {
        askedClip = clip;
        asked = node;
    });
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(x(300), yColor), tv.mapToGlobal(QPointF(x(300), yColor)),
                    Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(&tv, &dbl);
    CHECK(asked == 5 && askedClip == f.id);
    I18n::install("en");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("comp_range");
    Check::initEngine();
    QTemporaryDir dir;
    testCore();
    testEval();
    testEditor();
    testFollow();
    testCli(dir.path());
    testUi();
    return Check::result();
}
