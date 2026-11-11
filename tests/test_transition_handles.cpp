// SPDX-License-Identifier: AGPL-3.0-only
// A dissolve dropped onto a cut where one clip has no material beyond it (the right clip starts at the start of its
// recording, or the left one ends at its end): it is aligned to the side that has material and keeps its full
// length instead of shrinking to a frame. Centred where both sides have material. The same for a transition added at
// the cut nearest to the playhead (Editor::addTransitions: the "add transition" action, a transition double-clicked in
// the effects panel, the CLI op 'transition'), which still made it 1 frame.
#include "check.h"
#include "timeline_guard.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>

namespace {

Clip mk(int id, int start, int in, int len)
{
    Clip c;
    c.id = id;
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.mediaPath = "/x/a.mp4";
    return c;
}

TimelineOps::TransitionSpan spanOf(const Project& p)
{
    for (const auto& s : TimelineOps::transitions(p.timeline().video[0], [](const Clip&) { return 1000; }))
        if (s.leftId == 1 && s.rightId == 2) return s;
    return {};
}

void setup(Project& p, int leftIn, int rightIn)
{
    p.reset();
    p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
    p.edit("x", [&](Timeline& tl) { tl.video[0].clips = {mk(1, 0, leftIn, 100), mk(2, 100, rightIn, 100)}; });
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Project p;
    TimelineGuard guard_p{p};
    Selection sel;
    Editor ed(&p, &sel);
    const int full = std::max(1, p.fps());

    // Both sides have material: centred, full length
    setup(p, 100, 300);
    CHECK(ed.addTransitionAt(1, 2, {}));
    CHECK_EQ(spanOf(p).length(), full);
    CHECK(TimelineOps::findClip(p.timeline(), 1)->transOutStyle.align == TransitionAlign::Center);

    // The right clip starts at the start of its recording: starts at the cut, full length (was 1 frame)
    setup(p, 100, 0);
    CHECK(ed.addTransitionAt(1, 2, {}));
    CHECK_EQ(spanOf(p).length(), full);
    CHECK_EQ(spanOf(p).start, 100);
    CHECK(TimelineOps::findClip(p.timeline(), 2)->transInStyle.align == TransitionAlign::Start);

    // The left clip ends at the end of its recording: ends at the cut
    setup(p, 900, 300);
    CHECK(ed.addTransitionAt(1, 2, {}));
    CHECK_EQ(spanOf(p).length(), full);
    CHECK_EQ(spanOf(p).end, 100);
    CHECK(TimelineOps::findClip(p.timeline(), 1)->transOutStyle.align == TransitionAlign::End);

    // An alignment the user chose is kept (only the default centred one moves): "start at the cut" with only 10
    // frames after the left clip stays a 10-frame dissolve starting at the cut, although ending at it would be longer
    setup(p, 890, 300);
    TransitionStyle start;
    start.align = TransitionAlign::Start;
    CHECK(ed.addTransitionAt(1, 2, start));
    CHECK(TimelineOps::findClip(p.timeline(), 1)->transOutStyle.align == TransitionAlign::Start);
    CHECK_EQ(spanOf(p).length(), 10);

    // Added at the cut nearest to the playhead: the same (was 1 frame)
    setup(p, 100, 0);
    TransitionStyle iris;
    iris.type = TransitionType::Luma;
    iris.luma = ":/lumas/kreis";
    ed.addTransitions(98, iris);
    CHECK_EQ(spanOf(p).length(), full);
    CHECK_EQ(spanOf(p).start, 100);
    setup(p, 900, 300);
    ed.addTransitions(103, std::nullopt);
    CHECK_EQ(spanOf(p).length(), full);
    CHECK_EQ(spanOf(p).end, 100);
    setup(p, 100, 300); // both sides: centred
    ed.addTransitions(100, std::nullopt);
    CHECK_EQ(spanOf(p).length(), full);
    CHECK(TimelineOps::findClip(p.timeline(), 1)->transOutStyle.align == TransitionAlign::Center);
    return Check::result();
}
