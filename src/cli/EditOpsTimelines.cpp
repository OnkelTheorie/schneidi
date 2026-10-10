// schneidi-cli edit operations on timelines (sequences) and compound clips, like the Media Pool in the app
#include "cli/CommandsDetail.h"

#include "core/TimelineOps.h"

namespace Cli::detail {

namespace {

// `timeline` of the op, default: the one being edited
int targetSequence(Session& s, const QJsonObject& op)
{
    return op.contains("timeline") ? s.sequenceOf(op.value("timeline")) : s.project.currentSequence();
}

void opTimeline(Session& s, const QJsonObject& op)
{
    if (!op.contains("timeline")) fail("BAD_ARGUMENT", "missing 'timeline' (id or name from `info`)");
    s.selectTimeline(op.value("timeline"), op.value("open").toBool());
}

void opNewTimeline(Session& s, const QJsonObject& op)
{
    const int previous = s.project.currentSequence();
    const QString name = op.value("name").toString().trimmed();
    for (const Sequence& q : s.project.sequences())
        if (!name.isEmpty() && q.name.compare(name, Qt::CaseInsensitive) == 0)
            fail("EXISTS", QString("there is a timeline '%1' already (id %2)").arg(q.name).arg(q.id));
    const int id = s.project.addTimeline(name); // opens it
    if (op.value("switch").toBool(true)) s.selectTimeline(id, op.value("open").toBool());
    else s.project.setCurrentSequence(previous);
}

void opRenameTimeline(Session& s, const QJsonObject& op)
{
    const QString name = op.value("name").toString().trimmed();
    if (name.isEmpty()) fail("BAD_ARGUMENT", "missing 'name'");
    s.project.renameSequence(targetSequence(s, op), name);
}

void opDuplicateTimeline(Session& s, const QJsonObject& op)
{
    const int copy = s.project.duplicateSequence(targetSequence(s, op));
    if (!copy) fail("FAILED", "could not duplicate the timeline");
    if (op.contains("name")) s.project.renameSequence(copy, op.value("name").toString());
    if (op.value("switch").toBool()) s.selectTimeline(copy);
}

void opRemoveTimeline(Session& s, const QJsonObject& op)
{
    if (!op.contains("timeline")) fail("BAD_ARGUMENT", "missing 'timeline' (id or name)");
    const int id = s.sequenceOf(op.value("timeline"));
    if (!s.project.canRemoveSequence(id))
        fail("FAILED", "cannot remove that timeline (the last one, or a compound clip in use)");
    s.project.removeSequence(id);
}

void opCompound(Session& s, const QJsonObject& op)
{
    const QVector<int> ids = clipIds(s, op);
    s.selection.set(QSet<int>(ids.begin(), ids.end()));
    const int current = s.project.currentSequence();
    if (!s.editor.createCompoundClip(op.value("name").toString())) fail("FAILED", "could not make a compound clip");
    s.project.setCurrentSequence(current);
}

void opDecompose(Session& s, const QJsonObject& op)
{
    if (!s.editor.decomposeCompoundClips(clipIds(s, op))) fail("FAILED", "none of the clips is a compound clip");
}

void opPlaceTimeline(Session& s, const QJsonObject& op)
{
    if (!op.contains("timeline")) fail("BAD_ARGUMENT", "missing 'timeline' (id or name of the timeline to place)");
    const int id = s.sequenceOf(op.value("timeline"));
    const int at = op.contains("at") ? timeOf(s, op, "at") : TimelineOps::endFrame(s.project.timeline());
    if (!s.editor.addSequenceAt(id, at, parseTrack(op.value("track"))))
        fail("FAILED", "cannot place that timeline here (empty, or it contains this timeline)");
}

} // namespace

void addTimelineOps(QVector<OpDef>& ops)
{
    ops << OpDef{"timeline", "{op:'timeline', timeline, open?}", "the following ops edit this timeline",
                 "timeline = id or name from `info` (compound clips are timelines too). open:true = the app opens "
                 "this timeline next time; otherwise the app keeps showing the one the user had open.",
                 opTimeline}
        << OpDef{"new_timeline", "{op:'new_timeline', name?, switch?:true, open?}",
                 "new empty timeline; the following ops edit it unless switch:false", {}, opNewTimeline}
        << OpDef{"rename_timeline", "{op:'rename_timeline', timeline?, name}", {}, {}, opRenameTimeline}
        << OpDef{"duplicate_timeline", "{op:'duplicate_timeline', timeline?, name?, switch?:false}",
                 "copy of a timeline (e.g. a version to try something)", {}, opDuplicateTimeline}
        << OpDef{"remove_timeline", "{op:'remove_timeline', timeline}", "not the last one, not a compound clip in use",
                 {}, opRemoveTimeline}
        << OpDef{"compound", "{op:'compound', clips, name?}", "combine clips into one compound clip (its own timeline)",
                 "The new compound clip covers the span of the clips; its content is a timeline named `name` that "
                 "can be edited with {op:'timeline'}.",
                 opCompound}
        << OpDef{"decompose", "{op:'decompose', clips}", "put a compound clip's content back in place", {}, opDecompose}
        << OpDef{"place_timeline", "{op:'place_timeline', timeline, at?, track?}",
                 "put another timeline (or compound clip) as one clip into this one (default: at the end)", {},
                 opPlaceTimeline};
}

} // namespace Cli::detail
