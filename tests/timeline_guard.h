// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Timeline invariants for the tests (0.21, block 2): what every editor operation must keep, checked after each one.
//  - per track: clips sorted by start, no overlap, start >= 0, at least 1 frame long
//  - ids: > 0 and unique within a timeline (clips and subtitle cues share the counter)
//  - linked clips stay in sync: two clips of one link group that were in sync (same file: same frame at the same place;
//    other files: side by side in time) keep their offset (start - in) to each other, unless the step changed how one
//    of them plays (speed, reverse, freeze, ramp) or is marked as a deliberate slip (TimelineGuard::allowSlip)
// TimelineCheck works on plain timelines (TimelineOps tests, loaded files); TimelineGuard watches a Project and checks
// after every timelineChanged (every edit, undo, redo, sequence switch), all sequences of the project.
#include "check.h"

#include "core/Project.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUndoStack>

namespace TimelineCheck {

inline QString name(TrackKind k, int i) { return QString("%1%2").arg(k == TrackKind::Video ? "V" : "A").arg(i + 1); }

// "V1: 3[0-40|0-39 L2] ..." (id[start-end|in-out link]) for finding a step (TIMELINE_GUARD_DUMP=1)
inline QString dumpIds(const Timeline& tl)
{
    QString s;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (int i = 0; i < tl.tracks(k).size(); ++i) {
            s += name(k, i) + ":";
            for (const Clip& c : tl.tracks(k)[i].clips)
                s += QString(" %1[%2-%3|%4-%5 L%6]").arg(c.id).arg(c.start).arg(c.end()).arg(c.in).arg(c.out).arg(c.linkId);
            s += "  ";
        }
    return s.trimmed();
}

// Broken structure (empty = fine)
inline QStringList problems(const Timeline& tl)
{
    QStringList out;
    QSet<int> ids;
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio}) {
        const QVector<Track>& tracks = tl.tracks(k);
        for (int i = 0; i < tracks.size(); ++i) {
            const QVector<Clip>& clips = tracks[i].clips;
            for (int j = 0; j < clips.size(); ++j) {
                const Clip& c = clips[j];
                if (c.id <= 0) out << QString("%1: clip without id at %2").arg(name(k, i)).arg(c.start);
                else if (ids.contains(c.id)) out << QString("%1: id %2 twice").arg(name(k, i)).arg(c.id);
                ids.insert(c.id);
                if (c.start < 0) out << QString("%1: clip %2 starts at %3").arg(name(k, i)).arg(c.id).arg(c.start);
                if (c.length() < 1) out << QString("%1: clip %2 is %3 frames long").arg(name(k, i)).arg(c.id).arg(c.length());
                if (j > 0 && c.start < clips[j - 1].start)
                    out << QString("%1: clip %2 (%3) before %4 (%5): not sorted")
                               .arg(name(k, i)).arg(c.id).arg(c.start).arg(clips[j - 1].id).arg(clips[j - 1].start);
                else if (j > 0 && c.start < clips[j - 1].end())
                    out << QString("%1: clip %2 [%3-%4) overlaps %5 [%6-%7)")
                               .arg(name(k, i)).arg(c.id).arg(c.start).arg(c.end())
                               .arg(clips[j - 1].id).arg(clips[j - 1].start).arg(clips[j - 1].end());
            }
        }
    }
    for (int i = 0; i < tl.subtitles.size(); ++i)
        for (const SubtitleCue& c : tl.subtitles[i].cues) {
            if (c.id <= 0) out << QString("ST%1: cue without id at %2").arg(i + 1).arg(c.start);
            else if (ids.contains(c.id)) out << QString("ST%1: id %2 twice").arg(i + 1).arg(c.id);
            ids.insert(c.id);
        }
    return out;
}

inline bool samePlayback(const Clip& a, const Clip& b)
{
    return a.speed == b.speed && a.reverse == b.reverse && a.freeze == b.freeze && a.ramp == b.ramp;
}

// Linked pairs whose offset to each other changed from `before` to `after` (ids in `slipped` are left out)
inline QStringList syncProblems(const Timeline& before, const Timeline& after, const QSet<int>& slipped = {})
{
    auto index = [](const Timeline& tl) {
        QHash<int, Clip> m;
        for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
            for (const Track& t : tl.tracks(k))
                for (const Clip& c : t.clips)
                    if (c.linkId > 0) m.insert(c.id, c);
        return m;
    };
    const QHash<int, Clip> was = index(before), now = index(after);
    QHash<int, QVector<int>> groups; // link id (after) -> clip ids present before and after in the same group
    for (auto it = now.cbegin(); it != now.cend(); ++it) {
        const auto old = was.constFind(it.key());
        if (old == was.cend() || slipped.contains(it.key())) continue;
        if (old->linkId != it->linkId || !samePlayback(*old, *it)) continue; // relinked, or plays otherwise now
        groups[it->linkId] << it.key();
    }
    QStringList out;
    for (auto g = groups.cbegin(); g != groups.cend(); ++g) {
        const QVector<int>& ids = g.value();
        for (int i = 0; i < ids.size(); ++i)
            for (int j = i + 1; j < ids.size(); ++j) {
                const Clip &a0 = was[ids[i]], &b0 = was[ids[j]], &a1 = now[ids[i]], &b1 = now[ids[j]];
                if (!samePlayback(a1, b1)) continue; // offsets in different material frames
                const int d0 = (a0.start - a0.in) - (b0.start - b0.in), d1 = (a1.start - a1.in) - (b1.start - b1.in);
                // Only pairs in sync before: the same file at the same place, or other files side by side in time (a
                // pair taken apart earlier - on purpose or not - was reported then)
                const bool sameFile = a0.mediaPath == b0.mediaPath;
                if (sameFile ? d0 != 0 : (a0.start >= b0.end() || b0.start >= a0.end())) continue;
                // Back in sync (same file, same frame at the same place: undo of a deliberate slip) is no problem
                if (d0 != d1 && !(d1 == 0 && a1.mediaPath == b1.mediaPath))
                    out << QString("link %1: clips %2/%3 out of sync by %4 (was %5, is %6; %2 [%7-%8) -> [%9-%10), "
                                   "%3 [%11-%12) -> [%13-%14))")
                               .arg(g.key()).arg(ids[i]).arg(ids[j]).arg(d1 - d0).arg(d0).arg(d1)
                               .arg(a0.start).arg(a0.end()).arg(a1.start).arg(a1.end())
                               .arg(b0.start).arg(b0.end()).arg(b1.start).arg(b1.end());
            }
    }
    return out;
}

// For tests on plain timelines: CHECK that the structure is fine (and that linked clips kept their sync to `before`)
inline bool verify(const Timeline& tl, const char* file, int line, const QString& what = {})
{
    const QStringList p = problems(tl);
    return Check::report(p.isEmpty(), qPrintable("timeline invariants " + what), file, line, p.join("\n       "));
}
inline bool verifySync(const Timeline& before, const Timeline& after, const char* file, int line, const QString& what = {})
{
    const QStringList p = problems(after) + syncProblems(before, after);
    return Check::report(p.isEmpty(), qPrintable("timeline invariants " + what), file, line, p.join("\n       "));
}

} // namespace TimelineCheck

#define CHECK_TIMELINE(tl) TimelineCheck::verify((tl), __FILE__, __LINE__, #tl)
#define CHECK_TIMELINE_SYNC(before, after) TimelineCheck::verifySync((before), (after), __FILE__, __LINE__, #after)

// Watches a Project: after every change the invariants of all its sequences, and the sync of linked clips against the
// state before (same sequence only). Each problem counts as a failed check, named by the undo step that made it.
class TimelineGuard {
public:
    explicit TimelineGuard(Project& p, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : m_project(p), m_file(file), m_line(line)
    {
        remember();
        m_connection = QObject::connect(&p, &Project::timelineChanged, [this] { check(); });
    }
    ~TimelineGuard() { QObject::disconnect(m_connection); }
    TimelineGuard(const TimelineGuard&) = delete;
    TimelineGuard& operator=(const TimelineGuard&) = delete;

    // The next change moves these clips against their partners on purpose (moving/trimming one side of a link)
    void allowSlip(const QVector<int>& ids) { for (int id : ids) m_slipped.insert(id); }
    // Sync of linked clips not checked from now on (the structure still is): for steps that take linked clips apart
    // on purpose and the undo/redo runs over them
    void stopSyncCheck() { m_syncCheck = false; }
    int problemCount() const { return m_problems; }

private:
    void remember()
    {
        m_before = m_project.timeline();
        m_sequence = m_project.currentSequence();
        m_generation = m_project.generation();
    }
    void check()
    {
        QStringList p;
        for (const Sequence& s : m_project.sequences())
            for (const QString& x : TimelineCheck::problems(s.timeline)) p << QString("seq %1: %2").arg(s.id).arg(x);
        // Another project loaded: nothing to compare with
        if (m_syncCheck && m_project.currentSequence() == m_sequence && m_project.generation() == m_generation)
            p << TimelineCheck::syncProblems(m_before, m_project.timeline(), m_slipped);
        if (!p.isEmpty()) {
            const QUndoStack* u = m_project.undoStack();
            // During QUndoStack::push the index still points before the new step (push calls redo first)
            const QString step = !u ? QString("(no history)")
                                 : QString("\"%1\" / \"%2\"").arg(u->index() > 0 ? u->text(u->index() - 1) : QString("-"),
                                                                   u->index() < u->count() ? u->text(u->index()) : QString("-"));
            m_problems += int(p.size());
            if (qEnvironmentVariableIsSet("TIMELINE_GUARD_DUMP")) // both states, for finding the step
                p << "before: " + TimelineCheck::dumpIds(m_before) << "after:  " + TimelineCheck::dumpIds(m_project.timeline());
            Check::report(false, qPrintable(QString("timeline invariants at step %1 (undo index %2; a new step is the second)")
                                                .arg(step).arg(u ? u->index() : -1)),
                          m_file, m_line, p.join("\n       "));
        }
        m_slipped.clear();
        remember();
    }

    Project& m_project;
    const char* m_file;
    int m_line;
    Timeline m_before;
    int m_sequence = 0;
    quint64 m_generation = 0;
    QSet<int> m_slipped;
    bool m_syncCheck = true;
    int m_problems = 0;
    QMetaObject::Connection m_connection;
};
