// SPDX-License-Identifier: AGPL-3.0-only
// Edit ops of the 0.17 package "story": story sections (section, sections auto), timeline notes (note) and hook
// variants (hook_variants: copies of the timeline with another opening). Core: core/Story.h, Editor (EditorStory.cpp);
// the checks that read the sections: core/ExportCheckStory.cpp (`check`).

#include "cli/CommandsDetail.h"

#include "core/ExportCheck.h"
#include "core/I18n.h"
#include "core/ProjectFormat.h"
#include "core/Story.h"
#include "core/TimelineOps.h"
#include "core/TimelineStory.h"
#include "core/TranscriptCut.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace Cli::detail {

namespace {

int lengthOf(Session& s) { return TimelineOps::endFrame(s.project.timeline()); }

int framesOf(Session& s, double seconds) { return int(std::lround(seconds * s.format().rate.fps())); }

// The longest a hook should be: 3 s in a vertical video, 5 s otherwise (`check` story_hook_late counts from there)
int hookMax(Session& s) { return framesOf(s, ExportCheck::isVertical(s.format()) ? 3.0 : 5.0); }

QString span(Session& s, int from, int to)
{
    return QString("%1-%2 (%3-%4 s)")
        .arg(tc(from, s.format()), tc(to, s.format()))
        .arg(seconds(from, s.format()), 0, 'f', 2)
        .arg(seconds(to, s.format()), 0, 'f', 2);
}

// "clip 12 'Interview'" for the picture clip under the frame (the highest visible video track), or empty
QString clipUnder(Session& s, int frame)
{
    const Timeline& tl = s.project.timeline();
    for (int i = int(tl.video.size()) - 1; i >= 0; --i) {
        if (tl.video[i].hidden) continue;
        for (const Clip& c : tl.video[i].clips)
            if (c.enabled && c.start <= frame && frame < c.end())
                return QString("clip %1 '%2'").arg(c.id).arg(c.displayName());
    }
    return {};
}

// Picture cuts of V1 (the main track: where one shot ends and the next starts), timeline frames
QVector<int> mainCuts(const Timeline& tl)
{
    QVector<int> cuts;
    if (tl.video.isEmpty()) return cuts;
    for (const Clip& c : tl.video[0].clips) cuts << c.start << c.end();
    return cuts;
}

QString sectionText(Session& s, const StorySection& sec)
{
    QString t = TimelineStory::kindName(sec.kind) + " " + span(s, sec.from, sec.to);
    if (!sec.name.isEmpty()) t += " '" + sec.name + "'";
    return t;
}

void opSection(Session& s, const QJsonObject& op)
{
    if (op.value("remove").toBool()) {
        const int at = timeOf(s, op, "at");
        if (at < 0) fail("BAD_ARGUMENT", "missing 'at': a time inside the section to remove");
        if (!s.editor.removeSectionAt(at)) fail("NOT_FOUND", QString("no section at %1").arg(tc(at, s.format())));
        s.notes << QString("section at %1 removed").arg(tc(at, s.format()));
        return;
    }
    const QString kindName = op.value("kind").toString();
    const std::optional<StoryKind> kind = TimelineStory::kindFromName(kindName);
    if (!kind)
        fail("BAD_ARGUMENT", QString("kind must be %1 (got '%2')").arg(TimelineStory::kindNames().join(", "), kindName));
    if (!op.contains("from") || !op.contains("to")) fail("BAD_ARGUMENT", "missing 'from' and 'to' (the section's range)");
    StorySection sec{*kind, op.value("name").toString().trimmed(), timeOf(s, op, "from"), timeOf(s, op, "to")};
    if (const QString error = s.editor.addSection(sec); !error.isEmpty()) fail("FAILED", "section: " + error);
    s.notes << "section " + sectionText(s, sec);
}

void opSections(Session& s, const QJsonObject& op)
{
    if (op.value("clear").toBool()) {
        const int n = int(s.project.timeline().sections.size());
        s.editor.setSections({}, T("Abschnitte entfernen"));
        s.notes << QString("%1 section%2 removed").arg(n).arg(n == 1 ? "" : "s");
        return;
    }
    if (!op.value("auto").toBool()) fail("BAD_ARGUMENT", "sections takes auto:true (a suggestion from the cuts) or clear:true");
    const Timeline& tl = s.project.timeline();
    const int length = lengthOf(s);
    if (length <= 0) s.failEmpty();
    const int before = int(tl.sections.size());
    const QVector<StorySection> sections =
        Story::suggest(length, mainCuts(tl), int(std::lround(s.format().rate.fps())), hookMax(s));
    s.editor.setSections(sections, T("Abschnitte vorschlagen"));
    QStringList parts;
    for (const StorySection& sec : sections) parts << sectionText(s, sec);
    s.notes << "sections: " + parts.join(", ") + (before ? QString(" (replacing %1 before)").arg(before) : QString())
                   + ". A suggestion from the cuts: hook = the opening, resolution = the last shot; refine with 'section' "
                     "(question, turn, cta) and check with `check`";
}

void opNote(Session& s, const QJsonObject& op)
{
    const ProjectFormat f = s.format();
    if (op.contains("done") || op.contains("open")) {
        const bool done = op.contains("done");
        const int id = op.value(done ? "done" : "open").toInt();
        if (!s.editor.setNoteDone(id, done)) fail("NOT_FOUND", QString("no note %1 (ids: `info`, 'notes')").arg(id));
        s.notes << QString("note %1 %2").arg(id).arg(done ? "done" : "open again");
        return;
    }
    if (op.contains("remove")) {
        const int id = op.value("remove").toInt();
        if (!s.editor.removeNote(id)) fail("NOT_FOUND", QString("no note %1 (ids: `info`, 'notes')").arg(id));
        s.notes << QString("note %1 removed").arg(id);
        return;
    }
    const QString text = op.value("text").toString().trimmed();
    if (text.isEmpty()) fail("BAD_ARGUMENT", "missing 'text' (or done/open/remove with a note id)");
    const int at = timeOf(s, op, "at");
    if (at < 0) fail("BAD_ARGUMENT", "missing 'at' (where the note belongs)");
    const int id = s.editor.addNote(at, text);
    const QString clip = clipUnder(s, at);
    s.notes << QString("note %1 at %2%3").arg(id).arg(tc(at, f), clip.isEmpty() ? QString() : " on " + clip);
}

// ---------- hook_variants

// The first question in the transcripts of the timeline: the words up to one that ends with '?'
QString spokenQuestion(Session& s)
{
    const QVector<TranscriptCut::Word> words =
        TranscriptCut::timelineWords(s.project.timeline(), s.project.media(), s.format().rate.fps());
    QStringList sentence;
    for (const TranscriptCut::Word& w : words) {
        const QString t = w.text.trimmed();
        if (t.isEmpty()) continue;
        sentence << t;
        if (t.endsWith('?')) return sentence.join(' ');
        if (t.endsWith('.') || t.endsWith('!')) sentence.clear();
    }
    return {};
}

QString freeName(Session& s, const QString& wanted)
{
    QSet<QString> used;
    for (const Sequence& q : s.project.sequences()) used.insert(s.project.sequenceName(q.id));
    QString name = wanted;
    for (int n = 2; used.contains(name); ++n) name = QString("%1 (%2)").arg(wanted).arg(n);
    return name;
}

void opHookVariants(Session& s, const QJsonObject& op)
{
    const int count = op.contains("count") ? op.value("count").toInt() : 3;
    if (count < 1 || count > 3) fail("BAD_ARGUMENT", "count must be 1..3");
    const int length = lengthOf(s);
    if (length <= 0) s.failEmpty();
    const ProjectFormat f = s.format();
    const int teaser = op.contains("length") ? timeOf(s, op, "length") : framesOf(s, 2.0);
    if (teaser <= 0) fail("BAD_ARGUMENT", "length must be at least one frame");
    // The strong moments: given (best first), else the markers
    QVector<int> strong;
    const QJsonValue at = op.value("at");
    for (const QJsonValue& v : at.isArray() ? at.toArray() : at.isUndefined() ? QJsonArray{} : QJsonArray{at})
        strong << timeOf(s, QJsonObject{{"at", v}}, "at");
    QString from = "at";
    if (strong.isEmpty()) {
        strong = s.project.timeline().markers;
        from = "markers";
    }
    strong.erase(std::remove_if(strong.begin(), strong.end(), [&](int x) { return x < 0 || x >= length; }), strong.end());
    QString question = op.value("question").toString().trimmed();
    if (question.isEmpty())
        for (const StorySection& sec : s.project.timeline().sections)
            if (sec.kind == StoryKind::Question && !sec.name.isEmpty()) {
                question = sec.name;
                break;
            }
    if (question.isEmpty()) question = spokenQuestion(s);

    const int original = s.project.currentSequence();
    const QString base = s.project.sequenceName(original);
    QStringList made, skipped;
    const auto variant = [&](const QString& letter) {
        const int copy = s.project.duplicateSequence(original);
        if (!copy) fail("FAILED", "could not copy the timeline");
        const QString name = freeName(s, T("Hook %1").arg(letter));
        s.project.renameSequence(copy, name);
        s.selectTimeline(copy);
        return name;
    };
    const auto hookSection = [&](int to, const QString& name) {
        QVector<StorySection> sections = s.project.timeline().sections;
        sections.erase(std::remove_if(sections.begin(), sections.end(),
                                      [&](const StorySection& x) { return x.kind == StoryKind::Hook || x.from < to; }),
                       sections.end());
        sections.prepend(StorySection{StoryKind::Hook, name, 0, std::min(to, lengthOf(s))});
        s.editor.setSections(sections, T("Abschnitte vorschlagen"));
    };
    for (int v = 0; v < count; ++v) {
        if (v == 0) { // A: the strongest moment as a teaser in front
            if (strong.isEmpty()) {
                skipped << "A (teaser: no strong moment - give at, e.g. the hit_s of `highlights` as {media, source}, or put markers)";
                continue;
            }
            const int a = std::clamp(strong.first() - framesOf(s, 0.5), 0, std::max(0, length - 1));
            const int b = std::min(length, a + teaser);
            const QString name = variant("A");
            if (!s.editor.prependTeaser(a, b)) fail("FAILED", "nothing to copy at " + span(s, a, b));
            hookSection(b - a, T("Teaser"));
            made << QString("'%1': teaser %2 in front").arg(name, span(s, a, b));
        } else if (v == 1) { // B: the question as a text over the opening
            if (question.isEmpty()) {
                skipped << "B (question: give question, name a question section, or transcribe the speech)";
                continue;
            }
            const QString name = variant("B");
            const int end = std::min(length, std::max(teaser, framesOf(s, 2.5)));
            applyOp(s, QJsonObject{{"op", "title"}, {"text", question}, {"at", 0}, {"duration", end}});
            hookSection(end, question);
            made << QString("'%1': the question as a title %2: \"%3\"").arg(name, span(s, 0, end), question);
        } else { // C: straight into the action, 0.5 s before the first strong moment
            int first = -1;
            for (int x : strong)
                if (first < 0 || x < first) first = x;
            const int cut = first - framesOf(s, 0.5);
            if (first < 0 || cut <= 0) {
                skipped << (first < 0 ? QString("C (into the action: no strong moment)")
                                      : QString("C (into the action: the first strong moment is at the start already)"));
                continue;
            }
            const QString name = variant("C");
            s.editor.deleteRange(0, cut, true);
            // The sections, notes and markers follow the cut (deleteRange moves clips and subtitles only)
            const Timeline& tl = s.project.timeline();
            QVector<StorySection> sections = tl.sections;
            QVector<TimelineNote> notes = tl.notes;
            QVector<int> markers = tl.markers;
            Story::shift(sections, notes, markers, 0, -cut);
            s.project.edit(T("Abschnitte vorschlagen"), [&](Timeline& t) {
                t.sections = sections;
                t.notes = notes;
                t.markers = markers;
            });
            hookSection(std::min(hookMax(s), lengthOf(s)), {});
            made << QString("'%1': starts at %2, the first strong moment - 0.5 s").arg(name, tc(cut, f));
        }
    }
    s.selectTimeline(original);
    if (made.isEmpty())
        fail("NOT_FOUND", "no hook variant could be made: " + skipped.join("; "));
    s.notes << QString("hook variants of '%1' (strong moments from %2): %3").arg(base, from, made.join("; "))
                   + (skipped.isEmpty() ? QString() : ". Skipped: " + skipped.join("; "))
                   + ". The original is unchanged; compare them with `render` review or `check`";
}

} // namespace

void addStoryOps(QVector<OpDef>& ops)
{
    ops << OpDef{"section", "{op:'section', kind, from, to, name?} or {op:'section', remove:true, at}",
                 "a story section on the timeline",
                 "kind: hook, build, question, turn, resolution, cta or free; sections do not overlap (remove one "
                 "first). Clips belong to a section by their time. `check` reads them (story_no_hook, "
                 "story_open_question, story_resolution_late, story_no_cta, story_gap); `info` lists them.",
                 opSection}
        << OpDef{"sections", "{op:'sections', auto:true} or {op:'sections', clear:true}",
                 "suggest the story sections from the cuts",
                 "auto replaces the sections: hook = the opening up to the first cut after 1 s (at most 3 s in a "
                 "vertical video, else 5 s), resolution = the last shot (the last 15-25 %), build = the rest. Refine "
                 "with 'section'.",
                 opSections}
        << OpDef{"note", "{op:'note', at, text} or {op:'note', done:id} or {op:'note', open:id} or {op:'note', remove:id}",
                 "a note at a moment (\"faster here\"), to work through",
                 "The answer names the note's id and the clip under it; `info` lists the notes ('notes', done ones with "
                 "done:true). Work through the open notes and tick them off with done.",
                 opNote}
        << OpDef{"hook_variants", "{op:'hook_variants', count?, at?:[times], question?, length?}",
                 "copies of the timeline with other openings, to compare",
                 "New timelines 'Hook A/B/C' (the original stays): A = the strongest moment (the first of at, else the "
                 "first marker) as a teaser of length (2 s) in front; B = the question as a title over the opening "
                 "(question, else the name of a question section, else the first spoken question of the "
                 "transcripts); C = straight into the action from 0.5 s before the earliest strong moment. Each gets a "
                 "hook section. at: timeline times or {media, source} (e.g. the hit_s of `highlights`). A variant "
                 "without what it needs is skipped (the answer says why).",
                 opHookVariants};
}

} // namespace Cli::detail
