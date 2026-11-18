// SPDX-License-Identifier: AGPL-3.0-only
// Random editor operations on a timeline with linked clips (picture + sound), as the timeline does them (linked
// selection on): after each one the invariants of tests/timeline_guard.h (sorted, no overlap, unique ids, links in
// sync); at the end undo all = the start, redo all = the end. Fixed seeds: a failure names seed and step (TIMELINE_GUARD_DUMP=1 shows the
// timelines around it). Seeds/steps: INVARIANT_SEEDS, INVARIANT_STEPS.
// Linked clips that were in sync stay in sync too (since 0.21 a ripple that would take them apart is refused, a delete
// range keeps a sound with the piece it plays with).
#include "check.h"
#include "timeline_guard.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QUndoStack>
#include <random>

namespace {

using namespace TimelineOps;


QVector<int> allClips(const Timeline& tl)
{
    QVector<int> ids;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            for (const Clip& c : t.clips) ids << c.id;
    return ids;
}

int endOf(const Timeline& tl)
{
    int end = 0;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : tl.tracks(k))
            if (!t.clips.isEmpty()) end = std::max(end, t.clips.last().end());
    return end;
}

void run(unsigned seed, int steps)
{
    std::mt19937 rng(seed);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    Project p;
    TimelineGuard guard(p);
    Selection sel;
    Editor ed(&p, &sel);
    const QStringList files{"/x/a.mp4", "/x/b.mp4", "/x/m.wav"};
    p.addMedia({"/x/a.mp4", "a.mp4", 400, true, true, false});
    p.addMedia({"/x/b.mp4", "b.mp4", 250, true, true, false});
    p.addMedia({"/x/m.wav", "m.wav", 900, false, true, false});
    ed.addTrack(TrackKind::Video, 1);
    ed.addTrack(TrackKind::Audio, 1);
    ed.addTrack(TrackKind::Audio, 2);
    for (int i = 0; i < 6; ++i) ed.addMediaAt({files[pick(0, 1)]}, pick(0, 900), pick(0, 1));
    ed.addMediaAt({"/x/m.wav"}, pick(0, 300), 2);
    const int start = p.undoStack()->index();
    const Timeline first = p.timeline();

    QString last;
    for (int step = 0; step < steps; ++step) {
        const Timeline& tl = p.timeline();
        const QVector<int> ids = allClips(tl);
        const int before = guard.problemCount();
        if (ids.isEmpty()) {
            ed.addMediaAt({files[pick(0, 2)]}, pick(0, 600), pick(0, 1));
            continue;
        }
        const int id = ids[pick(0, int(ids.size()) - 1)];
        const Clip c = *findClip(tl, id);
        TrackRef ref;
        findClip(tl, id, &ref);
        const QVector<int> group = ed.withLinked({id});
        const int at = pick(0, std::max(1, endOf(tl) + 50));
        const Edge edge = pick(0, 1) ? Edge::End : Edge::Start;
        switch (pick(0, 17)) {
        case 0: last = "move"; ed.moveClips(group, pick(-80, 80), ref.kind, 0); break;
        case 1: last = "move track"; ed.moveClips(group, pick(-20, 20), ref.kind, pick(-1, 1)); break;
        case 2: last = "trim"; ed.trimClip(id, edge, pick(-60, 60)); break;
        case 3: last = "trim over"; ed.trimClip(id, edge, pick(-60, 60), true); break;
        case 4: last = "ripple trim"; ed.applyTrimEdit(ed.trimEdit(TrimKind::Ripple, id, edge), pick(-40, 40)); break;
        case 5: last = "roll"; ed.applyTrimEdit(ed.trimEdit(TrimKind::Roll, id, edge), pick(-30, 30)); break;
        case 6: last = "slip"; ed.applyTrimEdit(ed.trimEdit(TrimKind::Slip, id), pick(-30, 30)); break;
        case 7: last = "slide"; ed.applyTrimEdit(ed.trimEdit(TrimKind::Slide, id), pick(-30, 30)); break;
        case 8: last = "blade"; ed.bladeAt(id, c.start + pick(1, std::max(1, c.length() - 1))); break;
        case 9: last = "delete"; sel.set(QSet<int>(group.cbegin(), group.cend())); ed.deleteSelection(); break;
        case 10: {
            // one or two clips (each with its partners), possibly on different tracks at different times
            QVector<int> chosen = group;
            if (pick(0, 1)) chosen += ed.withLinked({ids[pick(0, int(ids.size()) - 1)]});
            last = "ripple delete";
            sel.set(QSet<int>(chosen.cbegin(), chosen.cend()));
            ed.rippleDeleteSelection();
            break;
        }
        case 11: {
            const int from = pick(0, endOf(tl));
            last = "delete range";
            ed.deleteRange(from, from + pick(1, 60), pick(0, 1));
            break;
        }
        case 12: {
            static const double speeds[] = {0.5, 0.75, 1.5, 2.0, 1.0};
            QVector<int> chosen = group;
            if (pick(0, 1)) chosen += ed.withLinked({ids[pick(0, int(ids.size()) - 1)]});
            last = "speed";
            ed.setClipSpeed(chosen, {speeds[pick(0, 4)], pick(0, 4) == 0}, pick(0, 2) > 0);
            break;
        }
        case 13: last = "add"; ed.addMediaAt({files[pick(0, 2)]}, at, pick(0, 2)); break;
        case 14: {
            last = "source edit";
            const auto mode = pick(0, 1) ? Editor::SourceEditMode::Insert : Editor::SourceEditMode::Overwrite;
            ed.sourceEdit(mode, files[pick(0, 1)], pick(0, 100), at);
            break;
        }
        case 15: last = "nudge"; sel.set(QSet<int>(group.cbegin(), group.cend())); ed.nudgeSelection(pick(-10, 10)); break;
        case 16: // not into the setup (a new step after it would drop the start from the history)
            last = "undo";
            if (p.undoStack()->index() > start) p.undoStack()->undo();
            break;
        case 17: last = "redo"; p.undoStack()->redo(); break;
        }
        if (guard.problemCount() > before) {
            std::printf("       seed %u, step %d: %s on clip %d\n", seed, step, qPrintable(last), id);
            if (guard.problemCount() > 20) return; // enough for one seed
        }
    }
    // The whole history back and forth: exactly the states
    const Timeline final = p.timeline();
    const int end = p.undoStack()->index();
    guard.stopSyncCheck(); // each step was checked on the way (undoing many at once passes states apart)
    p.undoStack()->setIndex(start);
    if (!CHECK(p.timeline() == first)) std::printf("       seed %u: undo all\n", seed);
    p.undoStack()->setIndex(end);
    if (!CHECK(p.timeline() == final)) std::printf("       seed %u: redo all\n", seed);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline_invariants");
    const int seeds = qEnvironmentVariableIntValue("INVARIANT_SEEDS") > 0 ? qEnvironmentVariableIntValue("INVARIANT_SEEDS") : 100;
    const int steps = qEnvironmentVariableIntValue("INVARIANT_STEPS") > 0 ? qEnvironmentVariableIntValue("INVARIANT_STEPS") : 150;
    for (int s = 1; s <= seeds; ++s) run(unsigned(s), steps);
    return Check::result();
}
