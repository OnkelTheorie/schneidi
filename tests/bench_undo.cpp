// SPDX-License-Identifier: AGPL-3.0-only
// Memory and time of snapshot undo (Project::edit) on a large timeline: every edit keeps the timeline before and
// after; implicit sharing should make a step cost only the parts that changed.
//   build-tests/tests/bench_undo [clips] [edits]
#include "check.h"

#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>

namespace {

// Resident memory in MB (Linux; 0 elsewhere – a developer tool)
double rssMb()
{
    QFile f("/proc/self/statm");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QList<QByteArray> parts = f.readAll().split(' ');
    return parts.size() > 1 ? parts[1].toLongLong() * 4096.0 / (1024 * 1024) : 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("bench-undo");
    const int clips = argc > 1 ? std::max(10, atoi(argv[1])) : 500;
    const int edits = argc > 2 ? std::max(1, atoi(argv[2])) : 300;

    Project p;
    Timeline tl;
    TimelineOps::ensureTracks(tl, TrackKind::Video, 4);
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 4);
    // Half the clips on V1-V4, their sound on A1-A4 (linked), with a few keyframes and an effect each
    int id = 0;
    for (int i = 0; i < clips / 2; ++i) {
        Clip v;
        v.id = ++id;
        v.mediaPath = QString("/media/clip%1.mp4").arg(i % 40);
        v.start = (i / 4) * 100;
        v.in = 10;
        v.out = 109;
        v.linkId = i + 1;
        v.keys[AnimParam::Opacity] = {Keyframe{10, 100}, Keyframe{60, 50}};
        v.effects << EffectInstance{"grade", {{"liftY", 0.1}}};
        Clip a = v;
        a.id = ++id;
        a.effects.clear();
        a.keys.clear();
        a.keys[AnimParam::Volume] = {Keyframe{10, 0}, Keyframe{60, -10}};
        tl.video[i % 4].clips << v;
        tl.audio[i % 4].clips << a;
    }
    p.setTimeline(tl);
    const double before = rssMb();
    QElapsedTimer t;
    t.start();
    for (int e = 0; e < edits; ++e)
        p.edit("volume", [e, clips](Timeline& x) { // like the editor's ops: the clip by id
            if (Clip* c = TimelineOps::findClip(x, 2 * (e % (clips / 2)) + 2)) c->volumeDb = -(e % 20);
        });
    const double ms = t.nsecsElapsed() / 1e6;
    const double after = rssMb();
    std::printf("%d clips, %d edits: %.3f ms per edit, %.1f MB for the undo history (%.1f KB per step), "
                "undo steps %d\n",
                clips, edits, ms / edits, after - before, (after - before) * 1024 / edits, p.undoStack()->count());
    t.restart();
    for (int e = 0; e < edits; ++e) p.undoStack()->undo();
    std::printf("undo all: %.3f ms per step\n", t.nsecsElapsed() / 1e6 / edits);
    return 0;
}
