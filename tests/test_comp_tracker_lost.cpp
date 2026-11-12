// SPDX-License-Identifier: AGPL-3.0-only
// Losing the tracked point (0.22, core/CompTrack.h): seen keyframes, the lost mode of followers, check and CLI.
// Synthetic pictures only: a patterned square on a straight path, hidden completely for 20 frames (20..39).
// - gaps from seen keys (open start/end, bridged short ones), visibility of mode fade (box filter over 0.2 s)
// - the analysis loses exactly the hidden frames; Editor::trackCompNode writes seen step keys (exact on every frame),
//   keeps what lies outside the analyzed range, one undo step
// - a following mask per mode: bridge = straight through (on the path), hold = at the last frame seen, fade = held and
//   faded out/in; gaps up to 5 frames are bridged in every mode; a project without seen keys renders as before
// - a following transform fades its picture
// - check: tracker_lost with the gap's range, tracker_ended for a track that never finds the point again
// - CLI (with ffmpeg): track_node answers lost:[[from, to]], node params lost:'hold', info lists the gaps, check
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompTrack.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompEval.h"
#include "engine/picture/CompNodes.h"
#include "engine/picture/CompTrack.h"
#include "ui/comp/CompShared.h"

#include "render_check.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <cstdio>

using namespace Picture::Comp;
using ::Comp::TrackGap;

namespace {

constexpr int W = 1920, H = 1080;
constexpr int kHideFrom = 20, kHideTo = 39; // the square is not in the picture on these frames

QPointF squarePath(int t) { return {500 + 7.0 * t, 400 + 3.0 * t}; }
QImage squareFrame(int t)
{
    QImage img(W, H, QImage::Format_RGB32);
    img.fill(QColor(70, 74, 80));
    if (t >= kHideFrom && t <= kHideTo) return img; // hidden (behind something as big as the picture)
    QPainter p(&img);
    const QPointF c = squarePath(t);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(230, 200, 60));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 80, 80));
    p.setBrush(QColor(40, 60, 200));
    p.drawRect(QRectF(c.x() - 40, c.y() - 40, 40, 40));
    p.drawRect(QRectF(c.x(), c.y(), 40, 40));
    return img;
}
QPointF norm(QPointF px) { return {px.x() / W, px.y() / H}; }

// tracker 1, mask 2 (follows 1), black 3, white 4, merge 5 = white where the mask is
EffectInstance maskGraph(const QVariantMap& tracker)
{
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    for (const char* type : {"tracker", "mask", "color", "color", "merge"}) g.nodes << CompNode{int(g.nodes.size()) + 1, type, {}};
    g.links = {{3, 5, "bg"}, {4, 5, "fg"}, {2, 5, "mask"}};
    g.out = 5;
    ::Comp::setGraph(e, g);
    for (auto it = tracker.cbegin(); it != tracker.cend(); ++it) e.params.insert("n1." + it.key(), it.value());
    const QVariantMap rest{{"n2.x", 0.3}, {"n2.y", 0.35}, {"n2.w", 0.12}, {"n2.h", 0.1}, {"n2.follow", 1},
                           {"n3.color", QColor(0, 0, 0)}, {"n4.color", QColor(255, 255, 255)}};
    for (auto it = rest.cbegin(); it != rest.cend(); ++it) e.params.insert(it.key(), it.value());
    return e;
}

Buf evalAt(const Clip& c, double t)
{
    Env env;
    env.size = QSize(192, 108);
    env.t = t;
    env.fps = 25;
    env.clip = &c;
    Buf out;
    const EffectInstance* e = EffectRegistry::instance(c, ::Comp::kEffectId);
    if (!e || !evaluate(*e, env, nullptr, &out)) return {};
    return out;
}

// Centre of the white pixels (0..1 of the picture) and their amount (sum of red / 255)
QPointF whiteCenter(const Buf& b, double* amount = nullptr)
{
    double sx = 0, sy = 0, n = 0;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x)
            if (const double v = b.px[(size_t(y) * b.w + x) * 4]; v > 0) {
                sx += v * (x + 0.5);
                sy += v * (y + 0.5);
                n += v;
            }
    if (amount) *amount = n / 255;
    return n > 0 ? QPointF(sx / n / b.w, sy / n / b.h) : QPointF(-1, -1);
}

double dist(QPointF a, QPointF b) { return std::hypot((a.x() - b.x()) * 192, (a.y() - b.y()) * 108); }

void testGaps()
{
    std::printf("gaps\n");
    using K = QVector<std::pair<int, double>>;
    CHECK(::Comp::trackGaps(K{}, 100).isEmpty());
    CHECK(::Comp::trackGaps(K{{0, 1}, {50, 1}}, 100).isEmpty());
    // Step keys: (9,1) (10,0) (29,0) (30,1) = lost 10..29
    QVector<TrackGap> g = ::Comp::trackGaps(K{{0, 1}, {9, 1}, {10, 0}, {29, 0}, {30, 1}}, 100);
    if (CHECK_EQ(g.size(), 1)) {
        CHECK(g[0] == (TrackGap{10, 29, false, false}));
        CHECK(!g[0].bridged());
    }
    // Ends lost: open to the clip's end; starts lost: open from 0
    g = ::Comp::trackGaps(K{{0, 1}, {39, 1}, {40, 0}}, 99);
    CHECK(g.size() == 1 && g[0] == (TrackGap{40, 99, false, true}));
    g = ::Comp::trackGaps(K{{12, 0}, {20, 0}, {21, 1}}, 99);
    CHECK(g.size() == 1 && g[0] == (TrackGap{0, 20, true, false}));
    // Short gaps are bridged, 5 frames still, 6 not
    g = ::Comp::trackGaps(K{{0, 1}, {9, 1}, {10, 0}, {14, 0}, {15, 1}, {29, 1}, {30, 0}, {35, 0}, {36, 1}}, 99);
    CHECK(g.size() == 2 && g[0].bridged() && g[0].length() == 5 && !g[1].bridged() && g[1].length() == 6);

    // Lost modes
    ::Comp::LostMode m = ::Comp::LostMode::Bridge;
    CHECK(::Comp::lostModeFromId("hold", &m) && m == ::Comp::LostMode::Hold);
    CHECK(!::Comp::lostModeFromId("nope", &m) && m == ::Comp::LostMode::Hold);
    CHECK_EQ(QString(::Comp::lostModeId(::Comp::LostMode::Fade)), QString("fade"));

    // Visibility of mode fade at 25 fps: 0.2 s = 5 frames out from the first lost frame, 5 in from the found one
    ::Comp::FollowTime ft;
    ft.fps = 25;
    ft.gaps = {TrackGap{10, 29, false, false}};
    const auto vis = [&](double t, ::Comp::LostMode mode = ::Comp::LostMode::Fade) {
        ft.t = t;
        ft.lost = mode;
        return ::Comp::followVisibility(ft);
    };
    CHECK_EQ(vis(9), 1.0);
    CHECK(std::abs(vis(10) - 0.8) < 1e-9);
    CHECK(std::abs(vis(12) - 0.4) < 1e-9);
    CHECK_EQ(vis(14), 0.0);
    CHECK_EQ(vis(29), 0.0);
    CHECK(std::abs(vis(30) - 0.2) < 1e-9);
    CHECK_EQ(vis(34), 1.0);
    CHECK_EQ(vis(20, ::Comp::LostMode::Hold), 1.0);
    CHECK_EQ(vis(20, ::Comp::LostMode::Bridge), 1.0);
    // A gap shorter than the fade does not reach 0; a bridged one does not fade at all
    ft.fps = 60; // 12 frames of fade: a gap of 6 frames goes half out
    ft.gaps = {TrackGap{10, 15, false, false}};
    CHECK(std::abs(vis(15) - 0.5) < 1e-9);
    ft.fps = 25;
    ft.gaps = {TrackGap{10, 14, false, false}};
    CHECK_EQ(vis(12), 1.0);
    // Open at the end: stays out
    ft.gaps = {TrackGap{10, 99, false, true}};
    CHECK_EQ(vis(500), 0.0);
    // No time (a follower outside a clip): seen
    CHECK_EQ(::Comp::followValue("seen", "point", [](const QString&) { return true; }, [](const QString&) { return 0.0; }), 1.0);
}

TrackRun analysis()
{
    TrackStart st;
    st.a = norm(squarePath(0));
    return trackFrames(squareFrame, st, 0, 60);
}

void testAnalysis(const TrackRun& r)
{
    std::printf("analysis\n");
    CHECK(r.error.isEmpty());
    CHECK(!r.gaveUp);
    QVector<int> want;
    for (int t = kHideFrom; t <= kHideTo; ++t) want << t;
    if (!CHECK(r.lostFrames == want)) {
        std::printf("       lost:");
        for (int f : r.lostFrames) std::printf(" %d", f);
        std::printf("\n");
    }
    CHECK_EQ(r.foundAt, kHideTo + 1);
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    int id = 0;
    Fixture(const QVariantMap& tracker)
    {
        p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        ed.addMediaAt({"/x/a.mp4"}, 0, 0);
        id = p.timeline().video[0].clips[0].id;
        ed.modifyClips({id}, "Compositing", [&](Clip& c) { c.effects << maskGraph(tracker); });
    }
    const Clip& clip() const { return *TimelineOps::findClip(p.timeline(), id); }
    void setLost(const QString& mode)
    {
        ed.modifyClips({id}, "lost", [&](Clip& c) { EffectRegistry::instance(c, ::Comp::kEffectId)->params["n1.lost"] = mode; });
    }
};

void testFollowers(const TrackRun& r)
{
    std::printf("followers\n");
    const QPointF a = norm(squarePath(0));
    const QVariantMap tracker{{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}};
    Fixture f(tracker);
    // A project from before 0.22: no seen keys - bridged, whatever the mode says
    CHECK(f.ed.trackCompNode(f.id, 1, r.points));
    const QPointF before30 = whiteCenter(evalAt(f.clip(), 30));
    CHECK(::Comp::trackGaps(f.clip(), 1).isEmpty());

    const int undoBefore = f.p.undoStack()->index();
    CHECK(f.ed.trackCompNode(f.id, 1, r.points, {r.lostFrames}));
    CHECK_EQ(f.p.undoStack()->index(), undoBefore + 1);
    // Seen: exact steps on every frame
    const AnimParam seen = Keys::effectParam(::Comp::kEffectId, "n1.seen");
    bool exact = true;
    for (int t = 0; t <= 60; ++t)
        exact = exact && Keys::valueAt(f.clip(), seen, t) == ((t >= kHideFrom && t <= kHideTo) ? 0.0 : 1.0);
    CHECK(exact);
    const QVector<TrackGap> gaps = ::Comp::trackGaps(f.clip(), 1);
    CHECK(gaps.size() == 1 && gaps[0] == (TrackGap{kHideFrom, kHideTo, false, false}));

    // The mask's place where the square is seen (start: as drawn, 0.3/0.35, moved by the path's way)
    const auto expected = [&](int t) {
        const QPointF d = norm(squarePath(t)) - norm(squarePath(0));
        return QPointF(0.3, 0.35) + d;
    };
    // Default = fade: held at frame 19 and fading out, gone in the middle, back on the path after
    double amount19 = 0, amount = 0;
    CHECK(dist(whiteCenter(evalAt(f.clip(), 19), &amount19), expected(19)) < 1.0);
    const QPointF at21 = whiteCenter(evalAt(f.clip(), 21), &amount);
    CHECK(dist(at21, expected(19)) < 1.0);
    if (!CHECK(std::abs(amount / amount19 - 0.6) < 0.03)) std::printf("       frame 21: %.3f of the full mask\n", amount / amount19);
    whiteCenter(evalAt(f.clip(), 30), &amount);
    CHECK_EQ(amount, 0.0);
    const QPointF at40 = whiteCenter(evalAt(f.clip(), 40), &amount);
    CHECK(dist(at40, expected(40)) < 1.0 && std::abs(amount / amount19 - 0.2) < 0.03);
    whiteCenter(evalAt(f.clip(), 45), &amount);
    CHECK(std::abs(amount / amount19 - 1) < 0.01);

    // Hold: stays at frame 19, fully there
    f.setLost("hold");
    const QPointF held = whiteCenter(evalAt(f.clip(), 30), &amount);
    if (!CHECK(dist(held, expected(19)) < 1.0 && std::abs(amount / amount19 - 1) < 0.01))
        std::printf("       hold: %.4f, %.4f (expected %.4f, %.4f), amount %.3f\n", held.x(), held.y(), expected(19).x(),
                    expected(19).y(), amount / amount19);
    CHECK(dist(whiteCenter(evalAt(f.clip(), 40)), expected(40)) < 1.0);

    // Bridge: straight through the gap (the path is straight: on it), the same as without seen keys
    f.setLost("bridge");
    const QPointF bridged = whiteCenter(evalAt(f.clip(), 30), &amount);
    CHECK(dist(bridged, expected(30)) < 1.0 && std::abs(amount / amount19 - 1) < 0.01);
    CHECK(dist(bridged, before30) < 0.01);

    // Undo takes the seen keys away again
    f.p.undoStack()->undo(); // lost mode
    f.p.undoStack()->undo(); // lost mode
    f.p.undoStack()->undo(); // the analysis with lost frames
    CHECK(::Comp::trackGaps(f.clip(), 1).isEmpty());

    // A short gap (5 frames) is bridged in every mode; 6 frames are not
    for (int len : {5, 6}) {
        Fixture s(tracker);
        QVector<::Comp::TrackPoint> pts;
        QVector<int> lost;
        for (const ::Comp::TrackPoint& q : r.points)
            if (q.frame < 10 || q.frame >= 10 + len) pts << q;
        for (int t = 10; t < 10 + len; ++t) lost << t;
        // The path through the gap from the analysis' own points (on the straight line)
        QVector<::Comp::TrackPoint> all;
        for (int t = 0; t <= 19; ++t) all << ::Comp::TrackPoint{t, norm(squarePath(t)).x(), norm(squarePath(t)).y(), 1, 0};
        pts.clear();
        for (const ::Comp::TrackPoint& q : all)
            if (!lost.contains(q.frame)) pts << q;
        CHECK(s.ed.trackCompNode(s.id, 1, pts, {lost}));
        for (const char* mode : {"fade", "hold"}) {
            s.setLost(mode);
            const QPointF c = whiteCenter(evalAt(s.clip(), 12), &amount);
            if (len == 5) {
                if (!CHECK(dist(c, expected(12)) < 1.0 && std::abs(amount / amount19 - 1) < 0.01))
                    std::printf("       %d frames, %s: not bridged\n", len, mode);
            } else if (!CHECK(dist(c, expected(12)) > 1.0 || amount / amount19 < 0.99)) {
                std::printf("       %d frames, %s: bridged\n", len, mode);
            }
        }
    }

    // Re-tracking a part keeps the seen value outside it: a new analysis of frames 50..60 without losses
    Fixture k(tracker);
    CHECK(k.ed.trackCompNode(k.id, 1, r.points, {r.lostFrames}));
    QVector<::Comp::TrackPoint> tail;
    for (const ::Comp::TrackPoint& q : r.points)
        if (q.frame >= 50) tail << q;
    CHECK(k.ed.trackCompNode(k.id, 1, tail, {}));
    const QVector<TrackGap> kept = ::Comp::trackGaps(k.clip(), 1);
    CHECK(kept.size() == 1 && kept[0] == (TrackGap{kHideFrom, kHideTo, false, false}));
    // ... and one over the gap that found it everywhere removes it
    QVector<::Comp::TrackPoint> over;
    for (int t = 15; t <= 45; ++t) over << ::Comp::TrackPoint{t, norm(squarePath(t)).x(), norm(squarePath(t)).y(), 1, 0};
    CHECK(k.ed.trackCompNode(k.id, 1, over, {}));
    CHECK(::Comp::trackGaps(k.clip(), 1).isEmpty());
    for (int t = 0; t <= 60; ++t) exact = exact && Keys::valueAt(k.clip(), seen, t) == 1.0;
    CHECK(exact);
}

// Review fixes: face trackers hold by default, gaps of a trimmed clip, losses at the end, giving up over an old path
void testEdges(const TrackRun& r)
{
    std::printf("edges\n");
    const QPointF a = norm(squarePath(0));
    const auto expected = [&](int t) { return QPointF(0.3, 0.35) + norm(squarePath(t)) - norm(squarePath(0)); };
    double full = 0, amount = 0;
    // A face tracker without a lost value (an older blur_face graph, one built by hand) holds: the blur stays
    {
        Fixture f({{"mode", "face"}, {"ax", a.x()}, {"ay", a.y()}});
        CHECK(f.ed.trackCompNode(f.id, 1, r.points, {r.lostFrames}));
        whiteCenter(evalAt(f.clip(), 19), &full);
        const QPointF c = whiteCenter(evalAt(f.clip(), 30), &amount);
        if (!CHECK(full > 0 && std::abs(amount / full - 1) < 0.01 && dist(c, expected(19)) < 1.0))
            std::printf("       face: %.3f of the mask at %.4f, %.4f\n", full > 0 ? amount / full : 0, c.x(), c.y());
        f.setLost("auto");
        whiteCenter(evalAt(f.clip(), 30), &amount);
        CHECK(std::abs(amount / full - 1) < 0.01);
        CHECK(::Comp::lostModeOf("", "face") == ::Comp::LostMode::Hold);
        CHECK(::Comp::lostModeOf("auto", "point") == ::Comp::LostMode::Fade);
        CHECK(::Comp::lostModeOf("fade", "face") == ::Comp::LostMode::Fade);
    }
    // Trimmed: the gaps shown are cut to the clip; one trimmed off is gone; a cut gap is no "short" one
    {
        Fixture f({{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}});
        CHECK(f.ed.trackCompNode(f.id, 1, r.points, {r.lostFrames}));
        f.ed.modifyClips({f.id}, "trim", [](Clip& c) { c.in += 35; });
        QVector<TrackGap> g = ::Comp::trackGaps(f.clip(), 1);
        if (CHECK_EQ(g.size(), 1)) {
            CHECK_EQ(g[0].from, 0);
            CHECK_EQ(g[0].to, kHideTo - 35);
            CHECK(!g[0].bridged());
        }
        f.ed.modifyClips({f.id}, "trim", [](Clip& c) { c.in += 10; });
        CHECK(::Comp::trackGaps(f.clip(), 1).isEmpty());
        // Rendering still holds a half trimmed gap at the frame before it (source 19)
        f.setLost("hold");
        f.ed.modifyClips({f.id}, "trim", [](Clip& c) { c.in -= 20; }); // in = 25: clip frame 0 = source 25
        CHECK(dist(whiteCenter(evalAt(f.clip(), 0)), expected(19)) < 1.0);
    }
    // Lost at the end without giving up (the last frames blurred): no gap, no tracker_ended, nothing vanishes
    {
        Fixture f({{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}});
        QVector<::Comp::TrackPoint> pts;
        for (const ::Comp::TrackPoint& q : r.points)
            if (q.frame <= 57) pts << q;
        CHECK(f.ed.trackCompNode(f.id, 1, pts, {{58, 59, 60}, false, 60}));
        CHECK(::Comp::trackGaps(f.clip(), 1).isEmpty());
        whiteCenter(evalAt(f.clip(), 80), &amount);
        CHECK(amount > 0);
        for (const ExportCheck::Issue& i : ExportCheck::comp(f.p.timeline(), 0, 100, f.p.timelineFormat()))
            CHECK(i.check != QLatin1String("tracker_ended"));
    }
    // Giving up over an earlier full path: its keys after the loss go, the rest of the asked range is lost
    {
        Fixture f({{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}});
        CHECK(f.ed.trackCompNode(f.id, 1, r.points));
        QVector<::Comp::TrackPoint> first;
        QVector<int> gone;
        for (const ::Comp::TrackPoint& q : r.points)
            if (q.frame < kHideFrom) first << q;
        for (int t = kHideFrom; t < kHideFrom + 30; ++t) gone << t;
        CHECK(f.ed.trackCompNode(f.id, 1, first, {gone, true, 60}));
        const QVector<int> xs = Keys::keyTimes(f.clip(), {Keys::effectParam(::Comp::kEffectId, "n1.x")});
        CHECK(!xs.isEmpty() && xs.last() < kHideFrom);
        const QVector<TrackGap> g = ::Comp::trackGaps(f.clip(), 1);
        CHECK(g.size() == 1 && g[0].from == kHideFrom && g[0].openEnd);
        whiteCenter(evalAt(f.clip(), 55), &amount);
        CHECK_EQ(amount, 0.0); // fade: gone
    }
}

void testTransform()
{
    std::printf("transform\n");
    Buf src(Buf::Rgba, 160, 90);
    for (size_t i = 0; i < src.px.size(); i += 4) {
        src.px[i] = 200;
        src.px[i + 1] = 100;
        src.px[i + 2] = 50;
        src.px[i + 3] = 255;
    }
    EffectInstance e{::Comp::kEffectId, {}, true};
    CompGraph g;
    g.nodes << CompNode{1, "clip", {}} << CompNode{2, "tracker", {}} << CompNode{3, "transform", {}};
    g.links = {{1, 2, "src"}, {2, 3, "src"}};
    g.out = 3;
    e.params = {{"n2.mode", "point"}, {"n2.rx", 0.5}, {"n2.ry", 0.5}, {"n3.follow", 2}};
    ::Comp::setGraph(e, g);
    Clip c;
    c.out = 100;
    c.effects << e;
    // Seen keys: lost 10..29
    const AnimParam seen = Keys::effectParam(::Comp::kEffectId, "n2.seen");
    const AnimParam x = Keys::effectParam(::Comp::kEffectId, "n2.x");
    for (auto [t, v] : QVector<std::pair<int, double>>{{0, 1}, {9, 1}, {10, 0}, {29, 0}, {30, 1}}) Keys::setKey(c, seen, t, v);
    Keys::setKey(c, x, 0, 0.5);
    Keys::setKey(c, x, 9, 0.5);
    Keys::setKey(c, x, 30, 0.5);
    const auto alphaAt = [&](double t) {
        Env env;
        env.size = QSize(src.w, src.h);
        env.t = t;
        env.fps = 25;
        env.clip = &c;
        env.clipPicture = [&]() -> const Buf* { return &src; };
        Buf out;
        if (!evaluate(c.effects[0], env, nullptr, &out)) return -1;
        return int(out.px[(size_t(45) * out.w + 80) * 4 + 3]);
    };
    CHECK_EQ(alphaAt(5), 255);
    CHECK(std::abs(alphaAt(11) - 153) <= 2); // 0.6
    CHECK_EQ(alphaAt(20), 0);
    CHECK_EQ(alphaAt(40), 255);
}

void testCheck(const TrackRun& r)
{
    std::printf("check\n");
    const QPointF a = norm(squarePath(0));
    Fixture f({{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}});
    const auto issues = [&](const char* id) {
        QVector<ExportCheck::Issue> out;
        for (const ExportCheck::Issue& i : ExportCheck::comp(f.p.timeline(), 0, 100, f.p.timelineFormat()))
            if (i.check == QLatin1String(id)) out << i;
        return out;
    };
    CHECK(issues("tracker_lost").isEmpty());
    CHECK(f.ed.trackCompNode(f.id, 1, r.points, {r.lostFrames}));
    const QVector<ExportCheck::Issue> lost = issues("tracker_lost");
    if (CHECK_EQ(lost.size(), 1)) {
        CHECK_EQ(lost[0].from, kHideFrom);
        CHECK_EQ(lost[0].to, kHideTo + 1);
        CHECK_EQ(lost[0].clip, f.id);
        CHECK(lost[0].message.contains("00:00"));
        std::printf("       %s\n", qPrintable(lost[0].message));
    }
    CHECK(issues("tracker_ended").isEmpty());
    CHECK(!CompUi::trackerWarning(f.clip(), 1, 25).isEmpty());
    CHECK(CompUi::trackerWarning(f.clip(), 2, 25).isEmpty());
    // Never found again: tracker_ended from the first lost frame to the clip's end
    QVector<::Comp::TrackPoint> first;
    QVector<int> gone;
    for (const ::Comp::TrackPoint& q : r.points)
        if (q.frame < kHideFrom) first << q;
    for (int t = kHideFrom; t < kHideFrom + 50; ++t) gone << t;
    Fixture e({{"mode", "point"}, {"ax", a.x()}, {"ay", a.y()}});
    CHECK(e.ed.trackCompNode(e.id, 1, first, {gone, true, 99}));
    QVector<ExportCheck::Issue> ended;
    for (const ExportCheck::Issue& i : ExportCheck::comp(e.p.timeline(), 0, 100, e.p.timelineFormat()))
        if (i.check == QLatin1String("tracker_ended")) ended << i;
    if (CHECK_EQ(ended.size(), 1)) {
        CHECK_EQ(ended[0].from, kHideFrom);
        CHECK_EQ(ended[0].to, 100);
    }
    // A tracker nobody follows: no warning (nothing on the picture depends on it)
    e.ed.modifyClips({e.id}, "unfollow", [&](Clip& c) { EffectRegistry::instance(c, ::Comp::kEffectId)->params["n2.follow"] = 0; });
    for (const ExportCheck::Issue& i : ExportCheck::comp(e.p.timeline(), 0, 100, e.p.timelineFormat()))
        CHECK(i.check != QLatin1String("tracker_ended"));
}

// ---- CLI ----

QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        Check::report(false, "cli error", __FILE__, __LINE__);
        return {};
    }
}
QJsonObject edit(const QJsonArray& ops) { return run("edit", {{"project", g_project}, {"ops", ops}}); }
QString text(const QJsonValue& v) { return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)); }

void testCli(const QString& dir)
{
    std::printf("cli\n");
    if (!Check::haveFfmpeg()) {
        std::printf("       skipped: no ffmpeg\n");
        return;
    }
    // A patterned square moving right, the whole picture covered on frames 30..49 (as if something passed in front)
    const QString media = Check::makeMedia(
        dir + "/lost.mp4",
        {"-f", "lavfi", "-i", "color=c=0x464a50:s=640x360:r=25:d=4", "-f", "lavfi", "-i",
         "color=c=0xe6c83c:s=80x80:r=25:d=4,drawbox=x=0:y=0:w=40:h=40:color=0x283cc8:t=fill,"
         "drawbox=x=40:y=40:w=40:h=40:color=0x283cc8:t=fill",
         "-filter_complex",
         "[0][1]overlay=x='100+3*n':y=150,drawbox=x=0:y=0:w=640:h=360:color=0x464a50:t=fill:enable='between(n,30,49)'",
         "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", "-frames:v", "100"});
    if (!CHECK(!media.isEmpty())) return;
    g_project = dir + "/lost.schneidi";
    run("new", {{"project", g_project}, {"width", 640}, {"height", 360}, {"fps", 25}, {"media", QJsonArray{media}}});
    edit({QJsonObject{{"op", "append"}, {"media", media}}});
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return;
    const int clip = d.timeline.video[0].clips.first().id;
    const QJsonObject added = edit({QJsonObject{{"op", "node"}, {"clip", clip},
                                                {"add", QJsonObject{{"type", "tracker"},
                                                                    {"params", QJsonObject{{"ax", 140.0 / 640}, {"ay", 190.0 / 360}}}}}},
                                    QJsonObject{{"op", "node"}, {"clip", clip}, {"add", QJsonObject{{"type", "blur"}}}},
                                    QJsonObject{{"op", "node"}, {"clip", clip},
                                                {"add", QJsonObject{{"type", "mask"}, {"params", QJsonObject{{"follow", 2}}}}},
                                                {"input", false}},
                                    QJsonObject{{"op", "node"}, {"clip", clip}, {"link", QJsonArray{4, 3, "mask"}}}});
    const int tr = added["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
    CHECK_EQ(tr, 2);
    const QJsonObject a = edit({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", tr}}});
    const QJsonObject note = a["notes"].toArray().first().toObject()["track_node"].toObject();
    const QJsonArray lost = note["lost"].toArray();
    if (!CHECK(lost.size() == 1 && lost[0].toArray().at(0).toInt() == 30 && lost[0].toArray().at(1).toInt() == 49))
        std::printf("       %s\n", qPrintable(text(note)));
    CHECK(note["gave_up"] == false);

    // info: the tracker's gaps
    const QJsonObject info = run("info", {{"project", g_project}});
    const QString infoText = QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact));
    CHECK(infoText.contains(R"("lost":[[30,49]])"));

    // The lost mode through node params; an unknown one is an error
    CHECK(!edit({QJsonObject{{"op", "node"}, {"clip", clip}, {"id", tr}, {"params", QJsonObject{{"lost", "hold"}}}}}).isEmpty());
    ProjectFile::load(g_project, &d, nullptr);
    const EffectInstance* e = EffectRegistry::instance(*TimelineOps::findClip(d.timeline, clip), ::Comp::kEffectId);
    CHECK(e && e->params.value(QString("n%1.lost").arg(tr)).toString() == "hold");
    bool failed = false;
    try {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, {{"project", g_project},
                                       {"ops", QJsonArray{QJsonObject{{"op", "node"}, {"clip", clip}, {"id", tr},
                                                                      {"params", QJsonObject{{"lost", "nope"}}}}}}}),
                 ctx);
    } catch (const Cli::Error&) {
        failed = true;
    }
    CHECK(failed);

    // check: the gap as a warning with its range
    const QJsonObject chk = run("check", {{"source", g_project}, {"sound", false}});
    bool found = false;
    for (const QJsonValue& v : chk["issues"].toArray())
        if (v.toObject()["check"].toString() == "tracker_lost") found = true;
    if (!CHECK(found)) std::printf("       %s\n", qPrintable(QString::fromUtf8(QJsonDocument(chk).toJson(QJsonDocument::Compact)).left(600)));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_tracker_lost");
    Check::initEngine();
    QTemporaryDir dir;
    testGaps();
    const TrackRun r = analysis();
    testAnalysis(r);
    testFollowers(r);
    testEdges(r);
    testTransform();
    testCheck(r);
    testCli(dir.path());
    return Check::result();
}
