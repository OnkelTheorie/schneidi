// SPDX-License-Identifier: AGPL-3.0-only
// Shared building blocks of 0.17: story sections and notes on a timeline (project file round trip, broken entries,
// duplicate timeline, frame rate change, resolution change, undo, `info`), and the 'why' key every edit op takes.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/TimelineStory.h"

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

Timeline storyTimeline()
{
    Timeline tl = emptyTimeline();
    tl.sections = {{StoryKind::Hook, "Opening kill", 0, 50}, {StoryKind::Build, {}, 50, 200},
                   {StoryKind::Cta, "Follow", 200, 250}};
    tl.notes = {{1, 30, "music too loud here", false}, {2, 120, "cut earlier", true}};
    return tl;
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString messageOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

void testFile(const QString& dir)
{
    ProjectData d;
    d.timeline = storyTimeline();
    Sequence other{2, "Short", false, 0, storyTimeline(), false, {}, 0, 0};
    other.timeline.notes = {{7, 5, "second timeline", false}};
    d.sequences = {Sequence{1, "Timeline 1", false, 0, d.timeline, false, {}, 0, 0}, other};
    d.currentSequence = 1;
    const QString path = dir + "/story.schneidi";
    QString error;
    CHECK(ProjectFile::save(d, path, &error));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(back.timeline.sections == d.timeline.sections);
    CHECK(back.timeline.notes == d.timeline.notes);
    CHECK_EQ(back.sequences.size(), 2);
    if (back.sequences.size() == 2) CHECK(back.sequences[1].timeline.notes == other.timeline.notes);
    CHECK(back.timeline == d.timeline);
    // Optional: no keys without content, and no new format version (older versions ignore the fields)
    Timeline plain = emptyTimeline();
    ProjectData p2;
    p2.timeline = plain;
    const QByteArray json = ProjectFile::toJson(p2, path);
    CHECK(!json.contains("\"sections\"") && !json.contains("\"notes\""));
    CHECK_EQ(QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object().value("version").toInt(),
             QJsonDocument::fromJson(json).object().value("version").toInt());

    // Broken entries (hand edits): empty section dropped, unknown kind = free, sorted, missing/duplicate note ids
    const QJsonArray sections = QJsonDocument::fromJson(R"([{"kind":"turn","from":100,"to":120},
        {"kind":"bogus","from":10,"to":20},{"kind":"hook","from":30,"to":30}])").array();
    const QVector<StorySection> s = TimelineStory::sectionsFromJson(sections);
    CHECK_EQ(s.size(), 2);
    if (s.size() == 2) {
        CHECK(s[0].kind == StoryKind::Free && s[0].from == 10);
        CHECK(s[1].kind == StoryKind::Turn);
    }
    const QVector<TimelineNote> n = TimelineStory::notesFromJson(
        QJsonDocument::fromJson(R"([{"id":3,"at":9,"text":"a"},{"id":3,"at":1,"text":"b"},{"at":5,"text":"c"}])").array());
    CHECK_EQ(n.size(), 3);
    if (n.size() == 3) {
        CHECK_EQ(n[0].at, 1);
        CHECK(n[0].id != n[1].id && n[1].id != n[2].id && n[0].id != n[2].id);
    }
    CHECK(TimelineStory::kindFromName("CTA") == StoryKind::Cta);
    CHECK_EQ(TimelineStory::kindNames().size(), 7);
}

void testProject()
{
    Project p;
    ProjectData d;
    d.timeline = storyTimeline();
    p.load(d);
    const int first = p.currentSequence();
    // Duplicate timeline: sections and notes come along
    const int copy = p.duplicateSequence(first);
    const Sequence* c = p.sequence(copy);
    CHECK(c && c->timeline.sections == d.timeline.sections && c->timeline.notes == d.timeline.notes);
    // Undo: the timeline snapshot holds them
    p.edit("notes", [](Timeline& tl) { tl.notes.clear(); tl.sections.removeLast(); });
    CHECK(p.timeline().notes.isEmpty());
    p.undoStack()->undo();
    CHECK(p.timeline().notes == d.timeline.notes && p.timeline().sections == d.timeline.sections);
    // Frame rate change (empty timelines): times stay at the same moment
    ProjectFormat f = p.format();
    const FrameRate from = f.rate;
    f.rate = {50, 1};
    Timeline rt = d.timeline;
    retimeTimeline(rt, from, f.rate);
    const double k = f.rate.fps() / from.fps();
    CHECK_EQ(rt.sections[1].from, int(std::lround(50 * k)));
    CHECK_EQ(rt.sections[1].to, int(std::lround(200 * k)));
    CHECK_EQ(rt.notes[1].at, int(std::lround(120 * k)));
    p.setFormat(f);
    CHECK(p.timeline().sections == rt.sections && p.timeline().notes == rt.notes);
    // A new resolution leaves them alone
    Timeline sc = d.timeline;
    scaleTimeline(sc, {1920, 1080}, {1080, 1920});
    CHECK(sc.sections == d.timeline.sections && sc.notes == d.timeline.notes);
}

void testCli(const QString& dir)
{
    const QString path = dir + "/cli.schneidi";
    ProjectData d;
    d.timeline = storyTimeline();
    QString error;
    CHECK(ProjectFile::save(d, path, &error));
    // info shows them with seconds
    const QJsonObject info = run("info", {{"project", path}});
    const QJsonArray sections = info["timeline"]["sections"].toArray();
    CHECK_EQ(sections.size(), 3);
    CHECK_EQ(sections[0]["kind"].toString(), QString("hook"));
    CHECK_EQ(sections[0]["name"].toString(), QString("Opening kill"));
    CHECK(sections[1]["from_s"].toDouble() > 0);
    CHECK_EQ(info["timeline"]["notes"].toArray().size(), 2);
    CHECK_EQ(info["timeline"]["notes"][1]["done"].toBool(), true);

    // 'why' on any op: accepted, echoed by op index, not saved anywhere
    const QJsonObject edit = run("edit", {{"project", path}, {"ops", QJsonArray{
        QJsonObject{{"op", "marker"}, {"at", 10}, {"why", "the first kill"}},
        QJsonObject{{"op", "marks"}, {"in", 0}, {"out", 40}},
        QJsonObject{{"op", "add_subtitle_track"}, {"why", "captions for viewers without sound"}},
        QJsonObject{{"op", "new_timeline"}, {"name", "Short"}, {"why", "a 9:16 version"}}}}});
    const QJsonArray why = edit["why"].toArray();
    CHECK_EQ(why.size(), 3);
    if (why.size() == 3) {
        CHECK_EQ(why[0]["index"].toInt(), 0);
        CHECK_EQ(why[0]["op"].toString(), QString("marker"));
        CHECK_EQ(why[0]["why"].toString(), QString("the first kill"));
        CHECK_EQ(why[1]["index"].toInt(), 2);
        CHECK_EQ(why[2]["op"].toString(), QString("new_timeline"));
    }
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &error));
    CHECK(!ProjectFile::toJson(back, path).contains("the first kill"));
    CHECK(back.timeline.sections == d.timeline.sections); // an edit keeps them
    // No why given: no key in the answer
    CHECK(!run("edit", {{"project", path}, {"dry_run", true}, {"ops", QJsonArray{QJsonObject{{"op", "marker"}, {"at", 3}}}}})
               .contains("why"));
    // Other unknown keys are still refused (the key list does not advertise why), why must be a text
    const QString typo = messageOf("edit", {{"project", path}, {"ops", QJsonArray{
        QJsonObject{{"op", "marker"}, {"at", 3}, {"whyy", "x"}}}}});
    CHECK(typo.startsWith("BAD_ARGUMENT") && typo.contains("unknown key 'whyy'") && !typo.contains("why,"));
    CHECK(messageOf("edit", {{"project", path}, {"ops", QJsonArray{QJsonObject{{"op", "marker"}, {"at", 3}, {"why", 5}}}}})
              .contains("'why' must be a text"));
    // Every op takes it
    for (const Cli::detail::OpDef& def : Cli::detail::editOps())
        if (!CHECK(Cli::detail::editOpKeys(def.name).contains("why"))) std::printf("       %s\n", qPrintable(def.name));
    CHECK(Cli::detail::editHelp().contains("why:"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("story-fields");
    QTemporaryDir dir;
    testFile(dir.path());
    testProject();
    testCli(dir.path());
    return Check::result();
}
