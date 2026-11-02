// SPDX-License-Identifier: AGPL-3.0-only
// Auto-Reframe: camera path (dead zone, no overshoot, jumps only at cuts), fill geometry, the Editor op with undo,
// project round trip, the subject measurement on a synthetic video (a bright square: the crop follows it), the CLI ops
// 'reframe' and new_timeline with from + reframe.
#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/MediaAspect.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Reframe.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/MediaPool.h"
#include "engine/Engine.h"
#include "engine/ReframeAnalysis.h"

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

using Reframe::Key;
using Reframe::Target;

// Value of the path at frame t as the keyframes play it (ease in and out between keys)
double xAt(const QVector<Key>& keys, double t)
{
    if (t <= keys.front().frame) return keys.front().x;
    for (int i = 1; i < keys.size(); ++i)
        if (t <= keys[i].frame) {
            const double u = (t - keys[i - 1].frame) / double(keys[i].frame - keys[i - 1].frame);
            return keys[i - 1].x + (keys[i].x - keys[i - 1].x) * (1 - std::cos(u * M_PI)) / 2;
        }
    return keys.back().x;
}

QVector<Target> line(int from, int to, double x, double weight = 1, int step = 3)
{
    QVector<Target> out;
    for (int f = from; f <= to; f += step) out << Target{f, x, 0.5, weight, false};
    return out;
}

void testPath()
{
    Reframe::PathParams p;
    p.fps = 25;
    p.windowW = 0.316; // 9:16 out of 16:9
    // Jitter inside the dead zone: the camera stands still
    {
        QVector<Target> t;
        for (int f = 0; f < 200; f += 3) t << Target{f, 0.4 + 0.02 * std::sin(f * 0.7), 0.5, 1, false};
        const QVector<Key> k = Reframe::path(t, p);
        CHECK_EQ(k.size(), 1);
        CHECK(std::abs(k[0].x - 0.4) < 0.025);
        CHECK_EQ(k[0].y, 0.5); // y not cropped
    }
    // A step: one eased move from left to right, never beyond either side (no overshoot), monotonic
    {
        QVector<Target> t = line(0, 48, 0.3) + line(51, 150, 0.7);
        const QVector<Key> k = Reframe::path(t, p);
        CHECK(k.size() >= 2 && k.size() <= 3);
        CHECK(std::abs(k.front().x - 0.3) < 1e-9);
        CHECK(std::abs(k.back().x - 0.7) < 1e-9);
        double prev = 0;
        bool monotonic = true, inside = true;
        for (int f = 0; f <= 150; ++f) {
            const double x = xAt(k, f);
            monotonic &= x >= prev - 1e-9;
            inside &= x >= 0.3 - 1e-9 && x <= 0.7 + 1e-9;
            prev = x;
        }
        CHECK(monotonic);
        CHECK(inside);
        // The move starts when the subject left (offline) and takes a while (eased, not a jump)
        CHECK(k[k.size() - 2].frame >= 45 && k[k.size() - 2].frame <= 55);
        CHECK(k.back().frame - k[k.size() - 2].frame >= 6);
    }
    // The same step at a scene cut: a hard jump on the cut frame
    {
        QVector<Target> t = line(0, 48, 0.3) + line(51, 150, 0.7);
        t[17].cut = true; // frame 51
        CHECK_EQ(t[17].frame, 51);
        const QVector<Key> k = Reframe::path(t, p);
        CHECK_EQ(k.size(), 3);
        CHECK_EQ(k[1].frame, 50);
        CHECK(std::abs(k[1].x - 0.3) < 1e-9);
        CHECK_EQ(k[2].frame, 51);
        CHECK(std::abs(k[2].x - 0.7) < 1e-9);
    }
    // A cut whose new shot has its subject close to the old place: no jump (a flash or a menu counted as a cut)
    {
        QVector<Target> t = line(0, 48, 0.45) + line(51, 150, 0.5);
        t[17].cut = true;
        const QVector<Key> k = Reframe::path(t, p);
        CHECK_EQ(k.size(), 1);
    }
    // A move that would arrive just before the next cut is left out
    {
        QVector<Target> t = line(0, 90, 0.3) + line(93, 105, 0.7) + line(108, 200, 0.5);
        t[36].cut = true; // frame 108
        CHECK_EQ(t[36].frame, 108);
        const QVector<Key> k = Reframe::path(t, p);
        for (const Key& x : k) CHECK(x.frame >= 108 || std::abs(x.x - 0.3) < 1e-9);
    }
    // Unsure measurements (a game's busy picture) stay near the middle
    {
        const QVector<Key> k = Reframe::path(line(0, 100, 0.1, 0.05) + line(101, 200, 0.9, 0.05), p);
        for (const Key& x : k) CHECK(std::abs(x.x - 0.5) < 0.05);
    }
    // Kept where the window can go: the subject at the very edge = the window at the edge
    {
        const QVector<Key> k = Reframe::path(line(0, 100, 0.02), p);
        CHECK_EQ(k.size(), 1);
        CHECK(std::abs(k[0].x - 0.158) < 0.001);
    }
    // A short glitch (one wrong measurement) does not move the camera
    {
        QVector<Target> t = line(0, 150, 0.5);
        t[20].x = 0.95;
        CHECK_EQ(Reframe::path(t, p).size(), 1);
    }
    // Calmer = fewer moves for a subject that wanders
    {
        QVector<Target> t;
        for (int f = 0; f < 500; f += 3) t << Target{f, 0.5 + 0.3 * std::sin(f / 40.0), 0.5, 1, false};
        p.smoothness = 0;
        const int loose = Reframe::path(t, p).size();
        p.smoothness = 1;
        const int calm = Reframe::path(t, p).size();
        CHECK(calm < loose);
        p.smoothness = 0.5;
    }
    CHECK_EQ(Reframe::path({}, p).size(), 1);
}

void testGeometry()
{
    const QSizeF portrait(1080, 1920);
    const Reframe::Fill f = Reframe::fill(16.0 / 9.0, portrait);
    CHECK(std::abs(f.zoom - 1920.0 / 607.5) < 1e-6);
    CHECK(std::abs(f.windowW - 1 / f.zoom) < 1e-6); // 1080 of 1080 * zoom
    CHECK(std::abs(f.windowH - 1) < 1e-9);
    CHECK_EQ(Reframe::position(f, portrait, 0.5, 0.5), QPointF(0, 0));
    // Subject right of the middle: the picture moves left; at the edge it stops
    CHECK(Reframe::position(f, portrait, 0.6, 0.5).x() < 0);
    const double maxX = (f.picture.width() - 1080) / 2;
    CHECK(std::abs(Reframe::position(f, portrait, 1.0, 0.5).x() + maxX) < 1e-6);
    CHECK_EQ(Reframe::position(f, portrait, 0.5, 0.9).y(), 0.0); // nothing to move vertically
    // 9:16 footage in a 16:9 timeline: the height is cropped, y counts up
    const Reframe::Fill g = Reframe::fill(9.0 / 16.0, QSizeF(1920, 1080));
    CHECK(g.windowW > 0.999 && g.windowH < 0.33);
    CHECK(Reframe::position(g, QSizeF(1920, 1080), 0.5, 0.3).y() < 0); // the upper part = the picture moves down
    // Same shape: zoom 1
    CHECK_EQ(Reframe::fill(16.0 / 9.0, QSizeF(1920, 1080)).zoom, 1.0);
}

void testEditor(const QString& dir)
{
    MediaAspect::setProbe([](const QString&) { return 16.0 / 9.0; });
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    const int seq = p.addTimeline("Short", 0, QSize(1080, 1920));
    p.setCurrentSequence(seq);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, false, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.in = 10;
        v.out = 109;
        v.transform.rotation = 5;
        tl.video[0].clips << v;
        Clip t;
        t.id = p.newClipId();
        t.kind = ClipKind::Title;
        t.start = 100;
        t.out = 24;
        tl.video[0].clips << t;
    });
    const int id = p.timeline().video[0].clips[0].id;
    const int undoBefore = p.undoStack()->index();
    QHash<int, QVector<Key>> paths;
    paths[id] = {Key{0, 0.3, 0.5}, Key{40, 0.3, 0.5}, Key{60, 0.7, 0.5}};
    CHECK_EQ(ed.reframeTargets({}).size(), 1); // the title is left out
    CHECK_EQ(ed.reframeClips({}, Reframe::Mode::Auto, paths), 1);
    CHECK_EQ(p.undoStack()->index(), undoBefore + 1); // one step
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    CHECK(std::abs(c->transform.zoomX - 1920.0 / 607.5) < 1e-6);
    CHECK_EQ(c->transform.rotation, 5.0); // kept
    CHECK(c->keys.contains(AnimParam::PosX));
    CHECK(!c->keys.contains(AnimParam::PosY));
    CHECK_EQ(c->keys[AnimParam::PosX].size(), 3);
    CHECK_EQ(c->keys[AnimParam::PosX][0].frame, 10); // source frames (clip in + t)
    CHECK(Keys::valueAt(*c, AnimParam::PosX, 0) > 0); // left part = picture moved right
    CHECK(Keys::valueAt(*c, AnimParam::PosX, 99) < 0);
    CHECK(std::abs(Keys::valueAt(*c, AnimParam::PosX, 50)) < 1e-6); // half way (eased) through the middle

    // Project round trip
    const QString path = dir + "/reframe.schneidi";
    QString error;
    CHECK(ProjectFile::save(p.data(), path, &error));
    ProjectData d;
    CHECK(ProjectFile::load(path, &d, &error));
    Project q;
    q.load(d);
    q.setCurrentSequence(seq);
    const Clip* l = TimelineOps::findClip(q.timeline(), id);
    if (CHECK(l)) {
        CHECK(l->keys.value(AnimParam::PosX) == c->keys.value(AnimParam::PosX));
        CHECK_EQ(l->transform.zoomX, c->transform.zoomX);
    }

    // Center replaces the keyframes; fit gives the blurred fill; undo goes back step by step
    ed.reframeClips({id}, Reframe::Mode::Center);
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK(!c->keys.contains(AnimParam::PosX));
    CHECK_EQ(c->transform.posX, 0.0);
    ed.reframeClips({id}, Reframe::Mode::Fit);
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK_EQ(c->transform.zoomX, 1.0);
    CHECK(c->transform.fill == FitFill::Blur);
    p.undoStack()->undo();
    p.undoStack()->undo();
    p.undoStack()->undo();
    c = TimelineOps::findClip(p.timeline(), id);
    CHECK_EQ(c->transform.zoomX, 1.0);
    CHECK(c->transform.fill == FitFill::None);
    CHECK(c->keys.isEmpty());
    MediaAspect::setProbe({});
}

// The subject's position in the picture at timeline frame t (window centre, 0..1)
double centreAt(const Clip& c, int t, double aspect, QSizeF frame)
{
    const Reframe::Fill f = Reframe::fill(aspect, frame);
    return 0.5 - Keys::valueAt(c, AnimParam::PosX, t - c.start) / f.picture.width();
}

void testAnalysis(const QString& dir, const QString& jumping, const QString& gliding, const QString& busy,
                  const QString& cut)
{
    const ProjectFormat f{1080, 1920, FrameRate{25, 1}};
    // The square sits left (x 40..80 of 320) for 2 s, then right (240..280) for 2 s
    {
        Clip c;
        c.id = 1;
        c.mediaPath = jumping;
        c.out = 99;
        const auto n = ReframeAnalysis::need(c, f.rate, 100);
        if (!CHECK(n)) return;
        CHECK(std::abs(n->to - 4.0) < 0.05);
        QString error;
        const auto samples = ReframeAnalysis::analyze(*n, &error);
        if (!CHECK(samples)) {
            std::printf("  %s\n", qPrintable(error));
            return;
        }
        CHECK(samples->size() >= 30);
        // Measured where the square is (a still square stands out by its detail, the rest is black)
        int near = 0;
        for (const auto& s : *samples) near += std::abs(s.x - (s.seconds < 2 ? 0.1875 : 0.8125)) < 0.08;
        CHECK(near > samples->size() * 8 / 10);
        CHECK(ReframeAnalysis::cached(*n)); // stored
        const auto paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 100; });
        if (!CHECK(paths.contains(1))) return;
        Clip r = c;
        Reframe::apply(r, Reframe::Mode::Auto, paths[1], 16.0 / 9.0, QSizeF(1080, 1920));
        const double half = Reframe::fill(16.0 / 9.0, QSizeF(1080, 1920)).windowW / 2;
        // The square (its centre) in view, a second into each half
        CHECK(std::abs(centreAt(r, 25, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.1875) < half - 0.05);
        CHECK(std::abs(centreAt(r, 90, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.8125) < half - 0.05);
        // Rendered: the bright square shows in the short
        Timeline tl;
        tl.video.resize(1);
        tl.video[0].clips << r;
        for (int t : {25, 90}) {
            const QImage img = Check::renderFrame(tl, f, t, QSize(108, 192));
            int bright = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) bright += qGray(img.pixel(x, y)) > 200;
            CHECK(bright > 50);
        }
    }
    // Retimed clips of the same file: played backwards the square starts right, at double speed it jumps after 1 s
    {
        const double half = Reframe::fill(16.0 / 9.0, QSizeF(1080, 1920)).windowW / 2;
        Clip c;
        c.id = 11;
        c.mediaPath = jumping;
        c.out = 99;
        c.reverse = true;
        auto paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 100; });
        if (CHECK(paths.contains(11))) {
            Clip r = c;
            Reframe::apply(r, Reframe::Mode::Auto, paths[11], 16.0 / 9.0, QSizeF(1080, 1920));
            CHECK(std::abs(centreAt(r, 25, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.8125) < half - 0.05);
            CHECK(std::abs(centreAt(r, 90, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.1875) < half - 0.05);
        }
        c.id = 12;
        c.reverse = false;
        c.speed = 2;
        c.out = 49;
        paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 100; });
        if (CHECK(paths.contains(12))) {
            Clip r = c;
            Reframe::apply(r, Reframe::Mode::Auto, paths[12], 16.0 / 9.0, QSizeF(1080, 1920));
            CHECK(std::abs(centreAt(r, 12, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.1875) < half - 0.05);
            CHECK(std::abs(centreAt(r, 45, 16.0 / 9.0, QSizeF(1080, 1920)) - 0.8125) < half - 0.05);
        }
        // A clip shorter than the sampling step still gets the subject (the nearest sample), not the middle
        c.id = 13;
        c.speed = 1;
        c.in = 81;
        c.out = 82;
        paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 100; });
        if (CHECK(paths.contains(13)) && CHECK(!paths[13].isEmpty())) CHECK(std::abs(paths[13].front().x - 0.8125) < 0.1);
    }
    // A square gliding from left to right: the window keeps it in view all the way
    {
        Clip c;
        c.id = 2;
        c.mediaPath = gliding;
        c.out = 99;
        const auto paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 100; });
        if (!CHECK(paths.contains(2))) return;
        Clip r = c;
        Reframe::apply(r, Reframe::Mode::Auto, paths[2], 16.0 / 9.0, QSizeF(1080, 1920));
        const double half = Reframe::fill(16.0 / 9.0, QSizeF(1080, 1920)).windowW / 2;
        int seen = 0;
        for (int t = 5; t < 100; t += 5) {
            const double square = (40 + 240.0 * t / 100 + 20) / 320; // overlay x = 40 + 60 t (s)
            seen += std::abs(centreAt(r, t, 16.0 / 9.0, QSizeF(1080, 1920)) - square) < half;
        }
        CHECK(seen >= 17);
        CHECK(r.keys.value(AnimParam::PosX).size() >= 3);
    }
    // The same on a busy, moving ground (test pattern with sweeping lines): mostly in view
    {
        Clip c;
        c.id = 3;
        c.mediaPath = busy;
        c.out = 149;
        const auto paths = ReframeAnalysis::paths({c}, f, 0.5, [](const QString&) { return 150; });
        if (!CHECK(paths.contains(3))) return;
        Clip r = c;
        Reframe::apply(r, Reframe::Mode::Auto, paths[3], 16.0 / 9.0, QSizeF(1080, 1920));
        const double half = Reframe::fill(16.0 / 9.0, QSizeF(1080, 1920)).windowW / 2;
        int seen = 0, total = 0;
        for (int t = 5; t < 150; t += 5, ++total) {
            const double s = t / 25.0;
            const double square = ((s < 3 ? 40 + 100 * s : 540 - (s - 3) * 150) + 30) / 640;
            const double at = centreAt(r, t, 16.0 / 9.0, QSizeF(1080, 1920));
            seen += std::abs(at - square) < half;
            if (qEnvironmentVariableIsSet("SCHNEIDI_REFRAME_DEBUG")) std::printf("  %.2f s: square %.2f window %.2f\n", s, square, at);
        }
        std::printf("  busy ground: square in view %d of %d times\n", seen, total);
        CHECK(seen * 10 >= total * 7);
    }
    // A scene cut (other picture) is found on its frame
    {
        Clip c;
        c.mediaPath = cut;
        c.out = 99;
        const auto samples = ReframeAnalysis::analyze(*ReframeAnalysis::need(c, f.rate, 100));
        if (!CHECK(samples)) return;
        int cuts = 0;
        double at = -1;
        for (const auto& s : *samples)
            if (s.cut) ++cuts, at = s.seconds;
        CHECK_EQ(cuts, 1);
        CHECK(std::abs(at - 2.0) < 0.05);
        const QVector<Target> t = ReframeAnalysis::targets(c, f.rate, 100, *samples);
        int cutFrame = -1;
        for (const Target& x : t)
            if (x.cut) cutFrame = x.frame;
        CHECK_EQ(cutFrame, 50);
    }
    (void)dir;
}

void testCli(const QString& dir, const QString& jumping)
{
    // Project 16:9 with the jumping square and a title; a 9:16 copy reframed in one call
    const QString path = dir + "/cli.schneidi";
    {
        Project p;
        p.addMedia({jumping, "jumping.mp4", 100, true, false, false});
        p.edit("setup", [&](Timeline& tl) {
            Clip v;
            v.id = p.newClipId();
            v.mediaPath = jumping;
            v.out = 99;
            tl.video[0].clips << v;
            Clip t;
            t.id = p.newClipId();
            t.kind = ClipKind::Title;
            t.out = 24;
            if (tl.video.size() < 2) tl.video.resize(2);
            tl.video[1].clips << t;
        });
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    auto run = [&](const QString& name, const QJsonObject& args) -> const QJsonObject {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        return cmd->run(Cli::normalize(*cmd, args), ctx);
    };
    auto errorOf = [&](const QString& ops) {
        try {
            run("edit", {{"project", path}, {"ops", ops}});
        } catch (const Cli::Error& e) {
            return e.code;
        }
        return QString("none");
    };
    auto load = [&](Project& q) {
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(path, &d, &error));
        q.load(d);
    };
    int source = 0;
    {
        Project q;
        load(q);
        source = q.currentSequence();
    }
    const QJsonObject done = run("edit", {{"project", path}, {"ops", QString(R"([{"op":"new_timeline","from":%1,
                 "aspect":"9:16","reframe":"auto","smoothness":0.4}])").arg(source)}});
    bool noted = false;
    for (const QJsonValue& n : done["notes"].toArray()) noted |= n.toString().contains("reframed 1 clip");
    CHECK(noted);
    int copy = 0;
    {
        Project q;
        load(q);
        for (const Sequence& x : q.sequences())
            if (x.id != source && !x.compound) copy = x.id;
        if (!CHECK(copy)) return;
        CHECK(q.sequence(copy)->name.endsWith(" 9:16"));
        CHECK_EQ(q.sequenceFormat(copy).size(), QSize(1080, 1920));
        const Clip& v = q.sequence(copy)->timeline.video[0].clips[0];
        CHECK(v.transform.zoomX > 3.1);
        CHECK(v.keys.contains(AnimParam::PosX));
        CHECK_EQ(q.sequence(copy)->timeline.video[1].clips[0].transform.zoomX, 1.0); // the title stays
        // The 16:9 edit stays as it was
        const Clip& o = q.sequence(source)->timeline.video[0].clips[0];
        CHECK_EQ(o.transform.zoomX, 1.0);
        CHECK(o.keys.isEmpty());
    }
    // op reframe on a track, mode center: the keyframes go
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"timeline","timeline":%1},
                 {"op":"reframe","track":"V1","mode":"center"}])").arg(copy)}});
    {
        Project q;
        load(q);
        const Clip& v = q.sequence(copy)->timeline.video[0].clips[0];
        CHECK(!v.keys.contains(AnimParam::PosX));
        CHECK(v.transform.zoomX > 3.1);
    }
    CHECK_EQ(errorOf(R"([{"op":"reframe","mode":"wild"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"reframe","smoothness":3}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"new_timeline","aspect":"9:16","reframe":"auto"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"reframe","track":"A1"}])"), QString("BAD_ARGUMENT"));
    bool helped = false;
    for (const QJsonValue& v : Cli::detail::editOpsHelp()) helped |= v["op"].toString() == "reframe";
    CHECK(helped);
}

// The app: Media Pool "copy as 9:16 timeline" (one undo step) and the clip menu's Auto-Reframe
void testApp(const QString& jumping)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    w.importFiles({jumping}, true);
    auto* ed = w.findChild<Editor*>();
    auto* p = w.findChild<Project*>();
    auto* pool = w.findChild<MediaPool*>();
    if (!CHECK(ed && p && pool)) return;
    const int source = p->currentSequence();
    const int steps = p->undoStack()->index();
    if (!CHECK(p->timeline().video.size() > 0 && p->timeline().video[0].clips.size() == 1)) return;
    emit pool->reframedCopyRequested(source);
    const int copy = p->currentSequence();
    CHECK(copy != source);
    CHECK_EQ(p->undoStack()->index(), steps + 1); // one step
    CHECK_EQ(p->timelineFormat().size(), QSize(1080, 1920));
    CHECK(p->sequence(copy) && p->sequence(copy)->name.endsWith(" 9:16"));
    const Clip c = p->timeline().video[0].clips[0];
    CHECK(c.transform.zoomX > 3.1);
    CHECK(c.keys.contains(AnimParam::PosX)); // the square jumps from left to right
    // The clip menu's Auto-Reframe on the clip after it was centered by hand
    ed->reframeClips({c.id}, Reframe::Mode::Center);
    CHECK(!TimelineOps::findClip(p->timeline(), c.id)->keys.contains(AnimParam::PosX));
    QAction* a = InputBindings::instance().action("auto_reframe");
    if (CHECK(a)) {
        a->trigger();
        CHECK(TimelineOps::findClip(p->timeline(), c.id)->keys.contains(AnimParam::PosX));
    }
    p->undoStack()->undo();
    p->undoStack()->undo();
    p->undoStack()->undo();
    CHECK(!p->sequence(copy));
    CHECK_EQ(p->undoStack()->index(), steps);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("reframe");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testPath();
    testGeometry();
    testEditor(tmp.path());
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QStringList h264{"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
    const QStringList black{"-f", "lavfi", "-i", "color=c=black:s=320x180:r=25:d=4", "-f", "lavfi", "-i",
                            "color=c=white:s=40x40:r=25:d=4"};
    const QString jumping = Check::makeMedia(
        tmp.filePath("jumping.mp4"),
        black + QStringList{"-filter_complex", "[0][1]overlay=x='if(lt(t,2),40,240)':y=70"} + h264);
    const QString gliding = Check::makeMedia(tmp.filePath("gliding.mp4"),
                                             black + QStringList{"-filter_complex", "[0][1]overlay=x='40+60*t':y=70"} + h264);
    const QString cut = Check::makeMedia(
        tmp.filePath("cut.mp4"),
        QStringList{"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=2", "-f", "lavfi", "-i",
                    "smptebars=s=320x180:r=25:d=2", "-filter_complex", "[0][1]concat=n=2:v=1"} + h264);
    const QString busy = Check::makeMedia(
        tmp.filePath("busy.mp4"),
        QStringList{"-f", "lavfi", "-i", "testsrc2=s=640x360:r=25:d=6", "-f", "lavfi", "-i", "color=c=white:s=60x60:r=25:d=6",
                    "-filter_complex",
                    "[0]hue=s=0,eq=brightness=-0.35[b];[b][1]overlay=x='if(lt(t,3),40+100*t,540-(t-3)*150)':y=150"} + h264);
    if (!CHECK(!jumping.isEmpty() && !gliding.isEmpty() && !busy.isEmpty() && !cut.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testAnalysis(tmp.path(), jumping, gliding, busy, cut);
    testCli(tmp.path(), jumping);
    testApp(jumping);
    return Check::result();
}
