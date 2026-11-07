// SPDX-License-Identifier: AGPL-3.0-only
// AI cutout (0.19): the model's pre/post-processing math (engine/PersonMatte), the matte cache file, temporal
// smoothing, the guided upsample keeping the picture's edges, the node "cutout" from a cached matte, the analysis and
// the CLI op cutout_node with a stand-in model (no download), the Editor op as one undo step, and a clear error
// without the extension. Synthetic pictures only (a red figure on green). The real model: test_comp_cutout_model.
#include "check.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/PersonMatte.h"
#include "engine/picture/CompCutout.h"
#include "engine/picture/CompNodes.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace Picture::Comp;

namespace {

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Stand-in model: "person" = pixels redder than green (the figure), as two-channel logits like PP-HumanSeg
PersonMatte::Segmenter standIn()
{
    PersonMatte::Segmenter s;
    s.spec = PersonMatte::Spec{};
    s.way = "stand-in";
    s.run = [spec = s.spec](const Inference::Tensor& in, QString*) -> std::optional<Inference::Tensor> {
        const size_t n = size_t(spec.width) * spec.height;
        Inference::Tensor out;
        out.shape = {1, 2, spec.height, spec.width};
        out.data.assign(2 * n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const float r = in.data[i], g = in.data[n + i];
            out.data[n + i] = (r - g) * 8.0f; // person logit, background 0
        }
        return out;
    };
    return s;
}

QImage figure(int w, int h, QRect square)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(0, 200, 0));
    QPainter p(&img);
    p.fillRect(square, QColor(220, 30, 30));
    return img;
}

void testPreprocess()
{
    // 2 x 1: red, blue -> R G B planes of (v/255 - 0.5)/0.5
    QImage img(2, 1, QImage::Format_RGB32);
    img.setPixel(0, 0, qRgb(255, 0, 0));
    img.setPixel(1, 0, qRgb(0, 0, 255));
    PersonMatte::Spec spec;
    spec.width = 2, spec.height = 1;
    Inference::Tensor t = PersonMatte::input(img, spec);
    CHECK(t.shape == (std::vector<int64_t>{1, 3, 1, 2}));
    CHECK(t.data == (std::vector<float>{1, -1, -1, -1, -1, 1}));
    spec.nhwc = true;
    spec.mean = 0, spec.std = 1; // MediaPipe style 0..1
    t = PersonMatte::input(img, spec);
    CHECK(t.shape == (std::vector<int64_t>{1, 1, 2, 3}));
    CHECK(t.data == (std::vector<float>{1, 0, 0, 0, 0, 1}));
    // Squashed: 4 x 2 -> 2 x 1, a gray level stays
    QImage gray(4, 2, QImage::Format_RGB32);
    gray.fill(qRgb(51, 51, 51));
    spec = PersonMatte::Spec{};
    spec.width = 2, spec.height = 1;
    t = PersonMatte::input(gray, spec);
    for (float v : t.data) CHECK(near(v, 51 / 127.5 - 1, 1e-5));
}

void testPostprocess()
{
    int w = 0, h = 0;
    QString err;
    // Two channels: softmax, channel 1 = person
    Inference::Tensor two{{1, 2, 1, 3}, {0, 0, 10, /*fg*/ 0, 10, 0}};
    std::vector<uint8_t> a = PersonMatte::alpha(two, &w, &h, &err);
    CHECK(w == 3 && h == 1 && err.isEmpty());
    CHECK(a == (std::vector<uint8_t>{128, 255, 0}));
    // NHWC two channels
    Inference::Tensor nhwc{{1, 1, 3, 2}, {0, 10, 10, 0, 0, 0}};
    a = PersonMatte::alpha(nhwc, &w, &h);
    CHECK(w == 3 && h == 1 && a == (std::vector<uint8_t>{255, 0, 128}));
    // One channel as probability: unchanged
    Inference::Tensor prob{{1, 1, 1, 3}, {0, 0.5f, 1}};
    CHECK(PersonMatte::alpha(prob, &w, &h) == (std::vector<uint8_t>{0, 128, 255}));
    // One channel as logits: sigmoid
    Inference::Tensor logits{{1, 1, 1, 3}, {-10, 0, 10}};
    CHECK(PersonMatte::alpha(logits, &w, &h) == (std::vector<uint8_t>{0, 128, 255}));
    // Unknown shape
    Inference::Tensor bad{{1, 5, 3, 3}, std::vector<float>(45, 0.0f)};
    CHECK(PersonMatte::alpha(bad, &w, &h, &err).empty() && !err.isEmpty());
    // The whole way through segment() with the stand-in: the figure is the person
    const QImage img = figure(320, 180, QRect(100, 40, 120, 100));
    a = PersonMatte::segment(standIn(), img, &w, &h, &err);
    CHECK(w == 192 && h == 192 && a.size() == size_t(192 * 192));
    if (a.size() == size_t(192 * 192)) {
        CHECK(a[size_t(96) * 192 + 96] > 250);
        CHECK(a[size_t(5) * 192 + 5] < 5);
    }
}

void testMatteSize()
{
    CHECK(matteSize({1920, 1080}) == QSize(455, 256));
    CHECK(matteSize({1080, 1920}) == QSize(256, 455));
    CHECK(matteSize({320, 180}) == QSize(320, 180)); // never bigger than the picture
}

void testCacheRoundTrip()
{
    MatteSeq m;
    m.w = 40, m.h = 30, m.from = 7;
    std::mt19937 rng(3);
    for (int f = 0; f < 3; ++f) {
        std::vector<uint8_t> one(size_t(m.w) * m.h);
        for (auto& v : one) v = uint8_t(rng());
        m.frames.push_back(one);
    }
    const QString id = "roundtrip";
    QString err;
    CHECK(writeMattes(cutoutCachePath(id), m, &err));
    const auto back = readMattes(cutoutCachePath(id), &err);
    if (CHECK(back.has_value())) {
        CHECK(back->w == 40 && back->h == 30 && back->from == 7);
        CHECK(back->frames == m.frames);
    }
    const auto file = mattesOf(id);
    if (CHECK(file != nullptr)) {
        CHECK(file->has(7) && file->has(9) && !file->has(6) && !file->has(10));
        CHECK(file->matte(8) == m.frames[1]);
        CHECK(file->matte(10).empty());
    }
    CHECK(!mattesOf("../escape"));
    CHECK(!mattesOf("missing"));
    // A broken file is no analysis
    QFile broken(cutoutCachePath("broken"));
    CHECK(broken.open(QIODevice::WriteOnly));
    broken.write("SCUTnonsense");
    broken.close();
    CHECK(!mattesOf("broken"));
    CHECK(removeCutoutCache(id));
    CHECK(!QFileInfo::exists(cutoutCachePath(id)));
    CHECK(!mattesOf(id));
}

void testSmoothing()
{
    // Flicker: a level of 160 that jumps +-20 every frame -> close to the mean afterwards
    MatteSeq m;
    m.w = 8, m.h = 1;
    for (int f = 0; f < 9; ++f) m.frames.push_back(std::vector<uint8_t>(8, uint8_t(f % 2 ? 180 : 140)));
    smoothMattes(m);
    for (int f = 2; f < 7; ++f) CHECK(std::abs(int(m.frames[size_t(f)][3]) - 160) <= 8);
    // A real movement (an edge 4 px per frame): 0 and 255 stay where they are, no ghost of the last frames
    MatteSeq moving;
    moving.w = 64, moving.h = 1;
    for (int f = 0; f < 6; ++f) {
        std::vector<uint8_t> one(64, 0);
        for (int x = 10 + 4 * f; x < 64; ++x) one[size_t(x)] = 255;
        moving.frames.push_back(one);
    }
    const MatteSeq before = moving;
    smoothMattes(moving);
    CHECK(moving.frames == before.frames);
}

void testGrowBlur()
{
    std::vector<uint8_t> m(size_t(20) * 20, 0);
    m[size_t(10) * 20 + 10] = 255;
    std::vector<uint8_t> g = m;
    growMatte(g, 20, 20, 2);
    CHECK(g[size_t(12) * 20 + 12] == 255 && g[size_t(13) * 20 + 10] == 0);
    growMatte(g, 20, 20, -2);
    CHECK(g == m);
    std::vector<uint8_t> b = m;
    blurMatte(b, 20, 20, 2);
    CHECK(b[size_t(10) * 20 + 10] < 255 && b[size_t(10) * 20 + 11] > 0);
}

// Width of the edge along one row: pixels neither (nearly) in nor out
int edgeWidth(const uint8_t* row, int w)
{
    int n = 0;
    for (int x = 0; x < w; ++x) n += row[x] > 25 && row[x] < 230;
    return n;
}

void testRefineKeepsEdges()
{
    // A sharp figure at 640 x 360, its matte only 64 x 36 and blurred (what a small model sees)
    const int W = 640, H = 360, lw = 64, lh = 36;
    const QRect sq(203, 97, 211, 151); // edges between the small pixels on purpose
    Buf pic(Buf::Rgba, W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = pic.data() + (size_t(y) * W + x) * 4;
            const bool in = sq.contains(x, y);
            p[0] = in ? 220 : 0, p[1] = in ? 30 : 200, p[2] = in ? 30 : 0, p[3] = 255;
        }
    std::vector<uint8_t> low(size_t(lw) * lh);
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            int inside = 0;
            for (int yy = y * 10; yy < y * 10 + 10; ++yy)
                for (int xx = x * 10; xx < x * 10 + 10; ++xx) inside += sq.contains(xx, yy);
            low[size_t(y) * lw + x] = uint8_t(inside * 255 / 100);
        }
    blurMatte(low, lw, lh, 1);
    std::vector<uint8_t> plain(size_t(W) * H), guided(size_t(W) * H);
    refineMatte(low.data(), lw, lh, nullptr, W, H, plain.data());
    refineMatte(low.data(), lw, lh, pic.data(), W, H, guided.data());
    const int row = 170;
    const uint8_t* g = guided.data() + size_t(row) * W;
    const uint8_t* b = plain.data() + size_t(row) * W;
    // The step between the last pixel outside and the first inside: big guided, small bilinear (a ramp)
    const int jumpGuided = g[203] - g[202], jumpPlain = b[203] - b[202];
    if (!CHECK(jumpGuided > 120 && jumpPlain < 30))
        std::printf("       jump guided %d (%d -> %d), bilinear %d\n", jumpGuided, g[202], g[203], jumpPlain);
    CHECK(g[300] > 240); // inside
    CHECK(g[100] < 15);  // outside
    // With the node's default edge ramp (threshold 0.5) the edge is at most a few pixels wide; bilinear stays a ramp
    std::vector<uint8_t> ramped(static_cast<size_t>(W));
    for (int x = 0; x < W; ++x) ramped[size_t(x)] = uint8_t(std::clamp((g[x] / 255.0 - 0.5) / 0.5 + 0.5, 0.0, 1.0) * 255);
    std::vector<uint8_t> rampedPlain(static_cast<size_t>(W));
    for (int x = 0; x < W; ++x) rampedPlain[size_t(x)] = uint8_t(std::clamp((b[x] / 255.0 - 0.5) / 0.5 + 0.5, 0.0, 1.0) * 255);
    const int wGuided = edgeWidth(ramped.data(), W), wPlain = edgeWidth(rampedPlain.data(), W);
    if (!CHECK(wGuided <= 4 && wPlain >= 12)) std::printf("       edge width guided %d, bilinear %d\n", wGuided, wPlain);

    // Time at 1920 x 1080 with a 455 x 256 matte (rows on threads)
    const int FW = 1920, FH = 1080;
    Buf big(Buf::Rgba, FW, FH);
    std::vector<uint8_t> bigLow(size_t(455) * 256, 128), out(size_t(FW) * FH);
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 5; ++i) refineMatte(bigLow.data(), 455, 256, big.data(), FW, FH, out.data());
    std::printf("refineMatte 1920x1080: %.1f ms\n", t.nsecsElapsed() / 1e6 / 5);
}

void testNode()
{
    // Cached matte: frames 0..2, the left half is the person
    MatteSeq m;
    m.w = 32, m.h = 18, m.from = 0;
    std::vector<uint8_t> half(size_t(32) * 18, 0);
    for (int y = 0; y < 18; ++y)
        for (int x = 0; x < 16; ++x) half[size_t(y) * 32 + x] = 255;
    m.frames = {half, half, half};
    CHECK(writeMattes(cutoutCachePath("node"), m));

    Buf src(Buf::Rgba, 64, 36);
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = src.data() + (size_t(y) * 64 + x) * 4;
            p[0] = x < 32 ? 220 : 0, p[1] = x < 32 ? 30 : 200, p[2] = 40, p[3] = 255;
        }
    const QHash<QString, Buf> in{{"src", src}};
    RunOptions opt;
    opt.size = {64, 36};
    Buf out;
    CHECK(runNode("cutout", {{"cache", "node"}}, in, opt, &out));
    CHECK(out.kind == Buf::Rgba && out.w == 64 && out.h == 36);
    if (out.w == 64) {
        const uint8_t* l = out.data() + (size_t(18) * 64 + 10) * 4;
        const uint8_t* r = out.data() + (size_t(18) * 64 + 50) * 4;
        CHECK(l[3] == 255 && r[3] == 0);
        CHECK(l[0] == 220 && l[1] == 30 && l[2] == 40); // the picture itself unchanged
        const uint8_t* edgeIn = out.data() + (size_t(18) * 64 + 31) * 4;
        const uint8_t* edgeOut = out.data() + (size_t(18) * 64 + 32) * 4;
        CHECK(edgeIn[3] > 200 && edgeOut[3] < 55); // the edge where the picture's is
    }
    // As a mask, inverted
    CHECK(runNode("cutout", {{"cache", "node"}, {"output", "mask"}, {"invert", true}}, in, opt, &out));
    CHECK(out.kind == Buf::Mask && out.px[size_t(18) * 64 + 10] == 0 && out.px[size_t(18) * 64 + 50] == 255);
    // grow moves the edge out (project pixels)
    CHECK(runNode("cutout", {{"cache", "node"}, {"output", "mask"}, {"grow", 8}}, in, opt, &out));
    CHECK(out.kind == Buf::Mask && out.px[size_t(18) * 64 + 36] > 128);
    // Not analysed, an unknown analysis, a frame outside the analysis: nothing
    CHECK(!runNode("cutout", {}, in, opt, &out));
    CHECK(!runNode("cutout", {{"cache", "nothing"}}, in, opt, &out));
    opt.t = 3;
    CHECK(!runNode("cutout", {{"cache", "node"}}, in, opt, &out));
    // And the hint says so
    CHECK(!cutoutHint("").isEmpty());
    CHECK(!cutoutHint("nothing").isEmpty());
    CHECK(cutoutHint("node").isEmpty());
    const NodeType* k = find("cutout");
    CHECK(k && k->varying && k->inputs == QStringList{"src"});
    CHECK(::Comp::pixelKeys("cutout").contains("grow") && ::Comp::pixelKeys("cutout").contains("soft"));

    // Time of the node at 1920 x 1080 (455 x 256 matte)
    MatteSeq big;
    big.w = 455, big.h = 256;
    big.frames = {std::vector<uint8_t>(size_t(455) * 256, 200)};
    writeMattes(cutoutCachePath("big"), big);
    RunOptions o2;
    o2.size = {1920, 1080};
    const QHash<QString, Buf> bigIn{{"src", Buf(Buf::Rgba, 1920, 1080)}};
    runNode("cutout", {{"cache", "big"}}, bigIn, o2, &out); // loads the file once
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 5; ++i) runNode("cutout", {{"cache", "big"}}, bigIn, o2, &out);
    std::printf("node cutout 1920x1080: %.1f ms per frame\n", t.nsecsElapsed() / 1e6 / 5);
}

void testAnalysis()
{
    // A figure moving right 6 px per frame; 640 x 360
    const auto frame = [](int f) { return figure(640, 360, QRect(200 + 6 * f, 100, 160, 180)); };
    QElapsedTimer t;
    t.start();
    const CutoutRun r = cutoutFrames(frame, 0, 9, standIn(), "analysis");
    CHECK(r.error.isEmpty() && !r.canceled && r.frames == 10 && r.from == 0 && r.to == 9);
    std::printf("analysis with the stand-in model at 640x360: %.1f ms per frame\n", r.msPerFrame);
    const auto m = readMattes(cutoutCachePath("analysis"));
    if (CHECK(m.has_value() && m->frames.size() == 10)) {
        CHECK(m->w == 455 && m->h == 256);
        for (int f : {0, 5, 9}) {
            const auto& one = m->frames[size_t(f)];
            // centre of the figure (x 280 + 6f of 640, y 190 of 360) person, a corner not
            const int x = int((280 + 6 * f) * 455 / 640.0), y = int(190 * 256 / 360.0);
            CHECK(one[size_t(y) * 455 + x] > 240);
            CHECK(one[size_t(10) * 455 + 10] < 10);
        }
    }
    // Backwards range = the same range; cancel = nothing written
    std::atomic<bool> cancel{true};
    const CutoutRun c = cutoutFrames(frame, 3, 1, standIn(), "canceled", {}, &cancel);
    CHECK(c.canceled && c.from == 1 && c.to == 3 && !QFileInfo::exists(cutoutCachePath("canceled")));
    // A failing model: its error, nothing written
    PersonMatte::Segmenter broken = standIn();
    broken.run = [](const Inference::Tensor&, QString* e) -> std::optional<Inference::Tensor> {
        *e = "model broke";
        return std::nullopt;
    };
    const CutoutRun b = cutoutFrames(frame, 0, 2, broken, "broken-model");
    CHECK(b.error == "model broke" && !QFileInfo::exists(cutoutCachePath("broken-model")));
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    const int id = p.timeline().video[0].clips[0].id;
    CHECK(!ed.cutoutCompNode(id, 2, "abc")); // no graph
    CompGraph g;
    g.nodes << CompNode{1, "clip", {}} << CompNode{2, "cutout", {}};
    g.links << CompLink{1, 2, "src"};
    g.out = 2;
    ed.modifyClips({id}, "Comp", [&](Clip& c) {
        EffectInstance e{::Comp::kEffectId, {}, true};
        ::Comp::setGraph(e, g);
        c.effects << e;
    });
    QUndoStack& undo = *p.undoStack();
    const int before = undo.index();
    QString err;
    CHECK(!ed.cutoutCompNode(id, 1, "abc", &err) && err.contains("not a cutout"));
    CHECK(!ed.cutoutCompNode(id, 3, "abc"));
    CHECK_EQ(undo.index(), before);
    CHECK(ed.cutoutCompNode(id, 2, "abc"));
    CHECK_EQ(undo.index(), before + 1);
    const auto value = [&] {
        const Clip* c = TimelineOps::findClip(p.timeline(), id);
        return EffectRegistry::instance(*c, ::Comp::kEffectId)->params.value("n2.cache").toString();
    };
    CHECK_EQ(value(), QString("abc"));
    undo.undo();
    CHECK(value().isEmpty());
    // The analysis of the clip: errors for a missing node and a title
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    const auto s = standIn();
    CHECK(!cutoutNode(p.format(), *c, 1, 0, 5, {}, nullptr, &s).error.isEmpty());
    Clip title = *c;
    title.kind = ClipKind::Title;
    CHECK(!cutoutNode(p.format(), title, 2, 0, 5, {}, nullptr, &s).error.isEmpty());
}

// ---- CLI ----
QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString messageOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

QJsonObject edit(const QJsonArray& ops) { return run("edit", {{"project", g_project}, {"ops", ops}}); }
QString editError(const QJsonArray& ops) { return messageOf("edit", {{"project", g_project}, {"ops", ops}}); }

void testCli(const QString& dir)
{
    const QString still = QDir(dir).filePath("figure.png");
    CHECK(figure(320, 180, QRect(120, 50, 80, 100)).save(still));
    g_project = QDir(dir).filePath("cutout.schneidi");
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    edit({QJsonObject{{"op", "append"}, {"media", still}, {"duration", "1s"}}});
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id;
    // clip(1) -> cutout(2)
    QJsonObject add = QJsonDocument::fromJson(R"({"op":"node","add":{"type":"cutout"}})").object();
    add["clip"] = clip;
    const QJsonObject a = edit({add});
    const int node = a["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
    CHECK(node > 0);
    // info: the node says it is not analysed yet
    {
        const QJsonObject info = run("info", {{"project", g_project}});
        const QString text = QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact));
        CHECK(text.contains("cutout_node"));
    }
    const QJsonObject op{{"op", "cutout_node"}, {"clip", clip}, {"node", node}, {"from", 0}, {"to", 9}};
    // Without the extension: a clear error naming it
    PersonMatte::setStandIn(std::nullopt);
    qunsetenv("SCHNEIDI_CUTOUT_MODEL");
    const QString missing = editError({op});
    if (!CHECK(missing.startsWith("NEEDS_EXTENSION") && missing.contains("cutout"))) std::printf("       %s\n", qPrintable(missing));
    QString why;
    CHECK(!PersonMatte::available(&why) && why.contains("cutout"));
    // With the stand-in
    PersonMatte::setStandIn(standIn());
    const QJsonObject r = edit({op});
    const QJsonObject note = r["notes"].toArray().first().toObject()["cutout_node"].toObject();
    if (!CHECK(note["frames"].toInt() == 10 && !note["cache"].toString().isEmpty()))
        std::printf("       %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
    ProjectFile::load(g_project, &d, nullptr);
    const Clip* c = TimelineOps::findClip(d.timeline, clip);
    const EffectInstance* e = c ? EffectRegistry::instance(*c, ::Comp::kEffectId) : nullptr;
    CHECK(e && e->params.value(QString("n%1.cache").arg(node)).toString() == note["cache"].toString());
    CHECK(mattesOf(note["cache"].toString()) != nullptr);
    // The node's matte as gray: the figure white, the green black
    {
        QJsonObject f{{"source", g_project}, {"out", QDir(dir).filePath("frames")}, {"width", 320}, {"clip", clip},
                      {"node", node}, {"view", "matte"}};
        const QJsonObject fr = run("frames", f);
        const QImage img(fr["frames"].toArray().first().toObject()["file"].toString());
        if (CHECK(!img.isNull())) {
            const int sx = img.width() * 160 / 320, sy = img.height() * 100 / 180;
            CHECK(qGray(img.pixel(sx, sy)) > 200);
            CHECK(qGray(img.pixel(img.width() / 20, img.height() / 2)) < 40);
        }
    }
    // Errors: not a cutout, no such node
    CHECK(editError({QJsonObject{{"op", "cutout_node"}, {"clip", clip}, {"node", 1}}}).contains("not a cutout"));
    CHECK(editError({QJsonObject{{"op", "cutout_node"}, {"clip", clip}, {"node", 9}}}).contains("no node 9"));
    // help knows it
    const QString help = QString::fromUtf8(
        QJsonDocument(run("help", {{"command", "edit"}, {"op", "cutout_node"}})).toJson(QJsonDocument::Compact));
    CHECK(help.contains("green screen"));
    PersonMatte::setStandIn(std::nullopt);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_cutout");
    Check::initEngine();
    QTemporaryDir dir;
    QDir(cutoutCacheDir()).removeRecursively();
    testPreprocess();
    testPostprocess();
    testMatteSize();
    testCacheRoundTrip();
    testSmoothing();
    testGrowBlur();
    testRefineKeepsEdges();
    testNode();
    testAnalysis();
    testEditor();
    testCli(dir.path());
    QDir(cutoutCacheDir()).removeRecursively();
    return Check::result();
}
