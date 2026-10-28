// schneidi-cli edit operations on timelines (sequences) and compound clips, like the Media Pool in the app
#include "cli/CommandsDetail.h"

#include "core/TimelineOps.h"
#include "engine/Engine.h"

#include <QFileInfo>

#include <cmath>

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
            fail("EXISTS", QString("there is a timeline '%1' already (id %2)").arg(q.name, QString::number(q.id)));
    const int id = s.project.addTimeline(name); // opens it
    // In the app the user sees the new timeline right away (open:false keeps showing the old one)
    if (op.value("switch").toBool(true)) s.selectTimeline(id, op.value("open").toBool(s.live));
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

// Project settings like the app's dialog: the resolution scales positions/sizes in every timeline; the frame
// rate (like DaVinci) only changes while no timeline has clips, then media lengths are counted anew
void opFormat(Session& s, const QJsonObject& op)
{
    ProjectFormat f = s.format();
    if (op.contains("width")) f.width = int(numberOf(op, "width", 16, 8192));
    if (op.contains("height")) f.height = int(numberOf(op, "height", 16, 8192));
    if (f.width % 2 || f.height % 2) fail("BAD_ARGUMENT", "width and height must be even");
    if (op.contains("fps")) {
        const FrameRate rate = nearestFrameRate(numberOf(op, "fps", 1, 120));
        if (rate != f.rate && s.project.frameRateLocked())
            fail("FAILED", "the frame rate can only change while all timelines are empty (like DaVinci); make a new "
                           "project with `new --fps` instead");
        f.rate = rate;
    }
    // The project open in the app: the resolution belongs to every timeline. While other timelines hold clips, a new
    // resolution needs the user's yes (an AI turned a landscape project portrait for one short)
    if (s.live && f.size() != s.format().size() && !op.value("confirm").toBool()) {
        QStringList others;
        for (const Sequence& q : s.project.sequences()) {
            if (q.id == s.project.currentSequence() || q.compound) continue;
            bool clips = false;
            for (const auto* tracks : {&q.timeline.video, &q.timeline.audio})
                for (const Track& t : *tracks) clips |= !t.clips.isEmpty();
            if (clips) others << "'" + q.name + "'";
        }
        if (!others.isEmpty())
            fail("NEEDS_CONFIRMATION",
                 QString("the resolution belongs to the whole project: %1x%2 would also change %3 (they hold clips, "
                         "they would get bars). Ask the user first; send confirm:true only after they said yes, or "
                         "leave the format as it is")
                     .arg(f.width)
                     .arg(f.height)
                     .arg(others.join(", ")));
    }
    const FrameRate oldRate = s.format().rate;
    s.project.setFormat(f);
    if (f.rate == oldRate) return;
    s.project.recountMedia(oldRate, [&f](const QString& path) { return Engine::probe(f, path).length; });
}

} // namespace

void addTimelineOps(QVector<OpDef>& ops)
{
    ops << OpDef{"timeline", "{op:'timeline', timeline, open?}", "the following ops edit this timeline",
                 "timeline = id or name from `info` (compound clips are timelines too). open:true = the app opens "
                 "this timeline next time; otherwise the app keeps showing the one the user had open.",
                 opTimeline}
        << OpDef{"new_timeline", "{op:'new_timeline', name?, switch?:true, open?}",
                 "new empty timeline; the following ops edit it unless switch:false; the app shows it unless "
                 "open:false", {}, opNewTimeline}
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
                 opPlaceTimeline}
        << OpDef{"format", "{op:'format', width?, height?, fps?, confirm?}",
                 "project resolution (scales positions in all timelines) and frame rate (only while all timelines are "
                 "empty)",
                 "With the project open in the app (@app) a new resolution is refused while other timelines hold "
                 "clips: it changes them too (they get bars). Ask the user first and send confirm:true only after "
                 "they said yes.",
                 opFormat};
}

} // namespace Cli::detail
