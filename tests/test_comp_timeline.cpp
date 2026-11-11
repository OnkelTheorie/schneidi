// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graphs through timeline edits: values that count from the clip's start (a media node's offset in
// frames, the visible range from/until of text nodes in seconds) follow when the clip's first frame moves into its
// content - the right piece of a cut, head trims (trim, overwrite trim, ripple, roll, slide's neighbor, overwriting
// the head) - so the picture stays at the same place in the timeline; slip and moving keep them. Undo restores.
#include "check.h"

#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QUndoStack>

#include <cmath>

namespace {

using TimelineOps::Edge;
using TimelineOps::TrimKind;

// Graph of B: a background file (offset 10 frames, speed 2), titles with visible ranges
EffectInstance compOfB()
{
    EffectInstance e;
    e.effectId = Comp::kEffectId;
    CompGraph g;
    g.nodes = {{1, "clip", {}}, {2, "media", {}}, {3, "text", {}}, {4, "text", {}}, {5, "text3d", {}},
               {6, "text", {}}, {7, "merge", {}}};
    g.links = {{2, 7, "bg"}, {1, 7, "fg"}};
    g.out = 7;
    Comp::setGraph(e, g);
    e.params["n2.path"] = "/x/bg.mp4";
    e.params["n2.offset"] = 10.0;
    e.params["n2.speed"] = 2.0;
    e.params["n3.from"] = 6.0; // 6 s .. 9 s after B's start
    e.params["n3.until"] = 9.0;
    e.params["n4.from"] = 1.0; // 1 s .. 3 s: over before a cut at 5 s
    e.params["n4.until"] = 3.0;
    e.params["n6.until"] = 8.0; // from the start up to 8 s
    return e;
}

double val(Project& p, int id, const char* key)
{
    const Clip* c = TimelineOps::findClip(p.timeline(), id);
    const EffectInstance* e = c ? EffectRegistry::instance(*c, Comp::kEffectId) : nullptr;
    return e ? e->params.value(QString::fromLatin1(key)).toDouble() : -999;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

// The node is invisible over the whole clip (Picture::Comp::visibleWeight: a range with until > 0 that ends at or
// before its start)
bool hidden(Project& p, int id, int node)
{
    const double from = val(p, id, qPrintable(QString("n%1.from").arg(node)));
    const double until = val(p, id, qPrintable(QString("n%1.until").arg(node)));
    return until > 0 && until <= std::max(0.0, from);
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    int z = 0, a = 0, b = 0, c = 0;
    double fps = 25;
    QString before;

    Fixture()
    {
        p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, false, false});
        p.addMedia({"/x/c.mp4", "c.mp4", 60, true, false, false});
        fps = p.frameRate();
        // Z 0..50, A 50..250, B 250..750 (the comp clip), C 750..1050; all with material on both sides
        p.edit("setup", [&](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Video, 1);
            const auto add = [&](int start, int in, int len) {
                Clip x;
                x.id = p.newClipId();
                x.mediaPath = "/x/a.mp4";
                x.start = start;
                x.in = in;
                x.out = in + len - 1;
                tl.video[0].clips << x;
                return x.id;
            };
            z = add(0, 100, 50);
            a = add(50, 100, 200);
            b = add(250, 100, 500);
            c = add(750, 100, 300);
            TimelineOps::findClip(tl, b)->effects << compOfB();
        });
        before = Check::dump(p.timeline());
    }
    void undoAll()
    {
        while (p.undoStack()->index() > 1) p.undoStack()->undo();
        CHECK(Check::dump(p.timeline()) == before);
    }
    // B moved its first frame `frames` into its content (the values of the graph followed)
    void checkMoved(int id, int frames, const char* what)
    {
        std::printf("%s\n", what);
        const double s = frames / fps;
        CHECK(near(val(p, id, "n2.offset"), 10 + frames * 2.0)); // the same background frame at the same time
        if (!CHECK(near(val(p, id, "n3.from"), 6 - s))) qWarning() << val(p, id, "n3.from");
        CHECK(near(val(p, id, "n3.until"), 9 - s));
        CHECK(near(val(p, id, "n6.from"), 0)); // from the start stays from the start
        CHECK(near(val(p, id, "n6.until"), 8 - s));
        CHECK(near(val(p, id, "n2.speed"), 2));
    }
    void checkUnmoved(int id)
    {
        CHECK(near(val(p, id, "n2.offset"), 10));
        CHECK(near(val(p, id, "n3.from"), 6));
        CHECK(near(val(p, id, "n3.until"), 9));
        CHECK(near(val(p, id, "n4.until"), 3));
    }
};

void testSplit()
{
    Fixture f;
    const int cut = 250 + int(std::lround(5 * f.fps)); // 5 s into B
    f.ed.bladeAt(f.b, cut);
    int right = 0;
    for (const Clip& x : f.p.timeline().video[0].clips)
        if (x.start == cut) right = x.id;
    if (!CHECK(right && right != f.b)) return;
    f.checkUnmoved(f.b); // the left piece keeps its values
    f.checkMoved(right, cut - 250, "split: the right piece");
    // The title of 1..3 s ended before the cut: invisible on the right piece (not "the whole clip")
    CHECK(hidden(f.p, right, 4));
    CHECK(!hidden(f.p, f.b, 4));
    // text3d without a range stays without one
    CHECK(near(val(f.p, right, "n5.from"), 0) && near(val(f.p, right, "n5.until"), 0));
    f.undoAll();
}

void testTrims()
{
    {
        Fixture f;
        f.ed.trimClip(f.b, Edge::Start, 25);
        f.checkMoved(f.b, 25, "trim the head in");
        f.ed.trimClip(f.b, Edge::Start, -10);
        f.checkMoved(f.b, 15, "trim the head out again");
        f.ed.trimClip(f.b, Edge::End, -30);
        f.checkMoved(f.b, 15, "the tail changes nothing");
        f.undoAll();
    }
    {
        Fixture f;
        f.ed.trimClip(f.b, Edge::Start, 25, true); // overwrite mode
        f.checkMoved(f.b, 25, "trim the head in (overwrite)");
        f.undoAll();
    }
    {
        Fixture f;
        f.ed.applyTrimEdit(f.ed.trimEdit(TrimKind::Ripple, f.b, Edge::Start), 25);
        CHECK_EQ(TimelineOps::findClip(f.p.timeline(), f.b)->start, 250); // ripple: B stays, its content moves
        f.checkMoved(f.b, 25, "ripple the head");
        f.undoAll();
    }
    {
        Fixture f;
        f.ed.applyTrimEdit(f.ed.trimEdit(TrimKind::Roll, f.a, Edge::End), 25);
        CHECK_EQ(TimelineOps::findClip(f.p.timeline(), f.b)->start, 275);
        f.checkMoved(f.b, 25, "roll the cut A|B");
        f.undoAll();
    }
    {
        Fixture f;
        f.ed.applyTrimEdit(f.ed.trimEdit(TrimKind::Slide, f.a), 25);
        CHECK_EQ(TimelineOps::findClip(f.p.timeline(), f.b)->start, 275);
        f.checkMoved(f.b, 25, "slide A: B's head");
        f.undoAll();
    }
    {
        Fixture f;
        std::printf("slip B: nothing moves in the timeline\n");
        f.ed.applyTrimEdit(f.ed.trimEdit(TrimKind::Slip, f.b), 25);
        CHECK_EQ(TimelineOps::findClip(f.p.timeline(), f.b)->in, 125);
        f.checkUnmoved(f.b);
        f.undoAll();
    }
    {
        Fixture f;
        std::printf("a clip placed over B's head\n");
        f.ed.addMediaAt({"/x/c.mp4"}, 230, 0); // c.mp4 is 60 long: ends 40 frames into B
        const Clip* nb = TimelineOps::findClip(f.p.timeline(), f.b);
        if (CHECK(nb && nb->start == 290)) f.checkMoved(f.b, 40, "overwritten head");
        f.undoAll();
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("comp_timeline");
    testSplit();
    testTrims();
    return Check::result();
}
