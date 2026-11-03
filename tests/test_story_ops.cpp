// SPDX-License-Identifier: AGPL-3.0-only
// Story sections, notes and hook variants (0.17 package "story"): core/Story (adding without overlap, the suggestion
// from the cuts, sections/notes/markers following inserted or removed time, a teaser in front), the Editor calls
// (one undo step each) and the edit ops section, sections, note and hook_variants on a project file (answers,
// errors, the original timeline unchanged by hook_variants).
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/Story.h"
#include "core/TimelineOps.h"

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>

#include <memory>

namespace {

Clip media(int id, int start, int in, int length, int link, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    c.linkId = link;
    return c;
}

// 25 fps, 300 frames: V1 1 [0,100) 2 [100,200) 3 [200,300) with linked sound 4, 5, 6 on A1; a subtitle 30-60
void setup(Project& p)
{
    p.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 1000, true, true, false});
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << media(1, 0, 0, 100, 1) << media(2, 100, 300, 100, 2) << media(3, 200, 600, 100, 3);
    tl.audio[0].clips << media(4, 0, 0, 100, 1) << media(5, 100, 300, 100, 2) << media(6, 200, 600, 100, 3);
    SubtitleTrack st;
    st.cues << SubtitleCue{7, 30, 60, "Hallo", {}};
    tl.subtitles << st;
    p.load(ProjectData{ProjectFormat{}, 0, p.media(), {}, tl, 7, 3});
}

void testPure()
{
    QVector<StorySection> s;
    CHECK(Story::add(s, {StoryKind::Build, {}, 50, 100}).isEmpty());
    CHECK(Story::add(s, {StoryKind::Hook, {}, 0, 50}).isEmpty()); // touching is fine
    CHECK(Story::add(s, {StoryKind::Turn, {}, 90, 120}).contains("overlaps the build section"));
    CHECK(!Story::add(s, {StoryKind::Turn, {}, 30, 30}).isEmpty());
    if (CHECK_EQ(s.size(), 2)) CHECK(s[0].kind == StoryKind::Hook); // sorted
    CHECK_EQ(Story::at(s, 49), 0);
    CHECK_EQ(Story::at(s, 50), 1);
    CHECK_EQ(Story::at(s, 100), -1);

    // Suggestion: hook to the first cut after 1 s (capped), resolution = the last shot, build between
    QVector<StorySection> g = Story::suggest(300, {100, 200, 10}, 25, 75);
    if (CHECK_EQ(g.size(), 3)) {
        CHECK_EQ(g[0].to, 75); // the cut at 100 is after the 3 s cap
        CHECK(g[1].kind == StoryKind::Build && g[1].from == 75 && g[1].to == 225); // the last shot 200.. is > 25 %
        CHECK(g[2].kind == StoryKind::Resolution && g[2].from == 225 && g[2].to == 300);
    }
    g = Story::suggest(300, {50, 250}, 25, 125);
    if (CHECK_EQ(g.size(), 3)) {
        CHECK_EQ(g[0].to, 50);
        CHECK_EQ(g[2].from, 250);
    }
    g = Story::suggest(40, {}, 25, 75);
    if (CHECK_EQ(g.size(), 1)) CHECK(g[0].to == 40);
    CHECK(Story::suggest(0, {}, 25, 75).isEmpty());

    // Inserted and removed time
    QVector<StorySection> sec{{StoryKind::Hook, {}, 0, 50}, {StoryKind::Build, {}, 50, 200}};
    QVector<TimelineNote> notes{{1, 10, "a", false}, {2, 120, "b", false}};
    QVector<int> markers{5, 150};
    Story::shift(sec, notes, markers, 0, 25);
    CHECK(sec[0].from == 25 && sec[0].to == 75 && sec[1].to == 225);
    CHECK(notes[0].at == 35 && markers[1] == 175);
    Story::shift(sec, notes, markers, 0, -80); // [0, 80) out: the hook goes, the build starts at 0
    if (CHECK_EQ(sec.size(), 1)) CHECK(sec[0].from == 0 && sec[0].to == 145);
    if (CHECK_EQ(notes.size(), 1)) CHECK_EQ(notes[0].at, 65);
    CHECK_EQ(markers, QVector<int>({95}));
}

void testTeaser()
{
    Project p;
    setup(p);
    Timeline tl = p.timeline();
    tl.sections = {{StoryKind::Build, {}, 0, 300}};
    tl.markers = {150};
    int nextId = 100, nextLink = 10;
    CHECK(Story::prependTeaser(tl, 180, 230, [&] { return ++nextId; }, [&] { return ++nextLink; }));
    // In front: the end of 2 (180..200) and the start of 3 (200..230), with their sound, linked anew
    const QVector<Clip>& v = tl.video[0].clips;
    if (CHECK_EQ(v.size(), 5)) {
        CHECK(v[0].start == 0 && v[0].end() == 20 && v[0].in == 380 && v[0].id > 100);
        CHECK(v[1].start == 20 && v[1].end() == 50 && v[1].in == 600);
        CHECK(v[2].id == 1 && v[2].start == 50); // the timeline moved back by 50
        CHECK(v[4].id == 3 && v[4].start == 250 && v[4].end() == 350);
        CHECK(v[0].linkId != 2 && v[0].linkId == tl.audio[0].clips[0].linkId);
    }
    CHECK_EQ(tl.audio[0].clips.size(), 5);
    CHECK(tl.sections[0].from == 50 && tl.sections[0].to == 350);
    CHECK_EQ(tl.markers, QVector<int>({200}));
    CHECK(tl.subtitles[0].cues[0].start == 80); // the cue at 30 moved back; none in 180..230
    // A cue inside the range comes along, cut to it
    Timeline t2 = p.timeline();
    CHECK(Story::prependTeaser(t2, 40, 90, [&] { return ++nextId; }, [&] { return ++nextLink; }));
    if (CHECK_EQ(t2.subtitles[0].cues.size(), 2)) {
        CHECK(t2.subtitles[0].cues[0].start == 0 && t2.subtitles[0].cues[0].end == 20);
        CHECK(t2.subtitles[0].cues[1].start == 80);
    }
    Timeline empty;
    empty.video.resize(1);
    CHECK(!Story::prependTeaser(empty, 0, 10, [&] { return ++nextId; }, [&] { return ++nextLink; }));
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    setup(p);
    QUndoStack* undo = p.undoStack();
    const int start = undo->index();
    CHECK(ed.addSection({StoryKind::Hook, "Kill", 0, 50}).isEmpty());
    CHECK_EQ(undo->index(), start + 1);
    CHECK(!ed.addSection({StoryKind::Build, {}, 40, 100}).isEmpty());
    CHECK_EQ(undo->index(), start + 1); // refused: no step
    CHECK(ed.setSectionRange(0, 0, 60));
    CHECK(ed.removeSectionAt(10));
    CHECK(!ed.removeSectionAt(10));
    const int id = ed.addNote(70, "schneller");
    CHECK(id > 0);
    CHECK(ed.setNoteDone(id, true));
    CHECK(p.timeline().notes[0].done);
    CHECK(!ed.setNoteDone(id + 5, true));
    CHECK(ed.removeNote(id));
    CHECK(p.timeline().notes.isEmpty());
    undo->setIndex(start);
    CHECK(p.timeline().sections.isEmpty() && p.timeline().notes.isEmpty());
    CHECK(ed.prependTeaser(100, 150));
    CHECK_EQ(undo->index(), start + 1);
    CHECK_EQ(TimelineOps::endFrame(p.timeline()), 350);
    undo->undo();
    CHECK_EQ(TimelineOps::endFrame(p.timeline()), 300);
}

// ---------- schneidi-cli ----------

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QJsonObject edit(const QJsonArray& ops, const QJsonValue& timeline = {})
{
    QJsonObject a{{"project", g_path}, {"ops", ops}};
    if (!timeline.isUndefined()) a["timeline"] = timeline;
    return run("edit", a);
}

QString errorOf(const QJsonArray& ops)
{
    try {
        edit(ops);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

std::unique_ptr<Project> loaded()
{
    ProjectData d;
    ProjectFile::load(g_path, &d, nullptr);
    auto p = std::make_unique<Project>();
    p->load(d);
    return p;
}

QString text(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

int sequenceNamed(const Project& p, const QString& name)
{
    for (const Sequence& q : p.sequences())
        if (p.sequenceName(q.id) == name) return q.id;
    return 0;
}

void testOps(const QString& dir)
{
    g_path = dir + "/story.schneidi";
    {
        Project p;
        setup(p);
        QString error;
        if (!CHECK(ProjectFile::save(p.data(), g_path, &error))) return;
    }
    // section: add, overlap refused, remove
    QJsonObject r = edit({QJsonObject{{"op", "section"}, {"kind", "hook"}, {"from", 0}, {"to", "2s"}, {"name", "Kill"}}});
    CHECK(text(r).contains("section hook"));
    if (const auto p = loaded(); CHECK_EQ(p->timeline().sections.size(), 1)) {
        CHECK(p->timeline().sections[0].to == 50 && p->timeline().sections[0].name == "Kill");
    }
    CHECK(errorOf({QJsonObject{{"op", "section"}, {"kind", "build"}, {"from", "1s"}, {"to", "4s"}}}).contains("overlaps"));
    CHECK(errorOf({QJsonObject{{"op", "section"}, {"kind", "climax"}, {"from", 0}, {"to", 10}}}).contains("kind must be"));
    CHECK(errorOf({QJsonObject{{"op", "section"}, {"kind", "hook"}, {"from", 60}, {"to", 60}}}).startsWith("FAILED"));
    edit({QJsonObject{{"op", "section"}, {"remove", true}, {"at", 10}}});
    CHECK(loaded()->timeline().sections.isEmpty());
    CHECK(errorOf({QJsonObject{{"op", "section"}, {"remove", true}, {"at", 10}}}).startsWith("NOT_FOUND"));

    // sections auto: from the cuts (100, 200 at 25 fps; the hook cap of 5 s is after the first cut)
    r = edit({QJsonObject{{"op", "sections"}, {"auto", true}}});
    CHECK(text(r).contains("sections: hook"));
    if (const auto p = loaded(); CHECK_EQ(p->timeline().sections.size(), 3)) {
        const auto& s = p->timeline().sections;
        CHECK(s[0].kind == StoryKind::Hook && s[0].to == 100);
        CHECK(s[1].kind == StoryKind::Build && s[1].from == 100 && s[1].to == 225);
        CHECK(s[2].kind == StoryKind::Resolution && s[2].to == 300);
    }
    edit({QJsonObject{{"op", "sections"}, {"clear", true}}});
    CHECK(loaded()->timeline().sections.isEmpty());

    // note: the answer names id and clip; done; remove; unknown id
    r = edit({QJsonObject{{"op", "note"}, {"at", "0:05"}, {"text", "schneller"}}});
    CHECK(text(r).contains("note 1 at") && text(r).contains("clip 2"));
    edit({QJsonObject{{"op", "note"}, {"done", 1}}});
    if (const auto p = loaded(); CHECK_EQ(p->timeline().notes.size(), 1)) {
        CHECK(p->timeline().notes[0].done && p->timeline().notes[0].at == 125 && p->timeline().notes[0].text == "schneller");
    }
    edit({QJsonObject{{"op", "note"}, {"open", 1}}});
    CHECK(!loaded()->timeline().notes[0].done);
    CHECK(errorOf({QJsonObject{{"op", "note"}, {"done", 9}}}).startsWith("NOT_FOUND"));
    CHECK(errorOf({QJsonObject{{"op", "note"}, {"at", 5}}}).contains("missing 'text'"));
    edit({QJsonObject{{"op", "note"}, {"remove", 1}}});
    CHECK(loaded()->timeline().notes.isEmpty());

    // hook_variants: nothing strong, no question: refused
    CHECK(errorOf({QJsonObject{{"op", "hook_variants"}}}).startsWith("NOT_FOUND"));
    // With a strong moment at 8.4 s (frame 210, in clip 3) and a question
    const Timeline original = loaded()->timeline();
    r = edit({QJsonObject{{"op", "hook_variants"}, {"at", QJsonArray{"8.4s"}}, {"question", "Wer gewinnt?"}}});
    CHECK(text(r).contains("Hook A") && text(r).contains("Hook B") && text(r).contains("Hook C"));
    {
        const auto p = loaded();
        CHECK(p->timeline() == original); // the open one is the original, unchanged
        const auto first = [&](const QString& name) -> Clip {
            const int id = sequenceNamed(*p, name);
            if (!id) return {};
            for (const Sequence& q : p->sequences())
                if (q.id == id) {
                    Clip best;
                    best.start = 1 << 30;
                    for (const Track& t : q.timeline.video)
                        for (const Clip& c : t.clips)
                            if (c.start < best.start) best = c;
                    return best;
                }
            return {};
        };
        const auto sectionsOf = [&](const QString& name) {
            for (const Sequence& q : p->sequences())
                if (q.id == sequenceNamed(*p, name)) return q.timeline.sections;
            return QVector<StorySection>{};
        };
        // A: 7.9 s..9.9 s (frames 197..247) copied in front: starts with the end of clip 2 (source 397)
        const Clip a = first("Hook A");
        CHECK(a.mediaPath == "/x/a.mp4" && a.start == 0 && a.in == 397);
        if (const auto s = sectionsOf("Hook A"); CHECK(!s.isEmpty())) CHECK(s[0].kind == StoryKind::Hook && s[0].to == 50);
        // B: the question as a title from 0
        bool titled = false;
        for (const Sequence& q : p->sequences())
            if (q.id == sequenceNamed(*p, "Hook B"))
                for (const Track& t : q.timeline.video)
                    for (const Clip& c : t.clips) titled |= c.isTitle() && c.start == 0 && c.title.text == "Wer gewinnt?";
        CHECK(titled);
        if (const auto s = sectionsOf("Hook B"); CHECK(!s.isEmpty())) CHECK(s[0].name == "Wer gewinnt?");
        // C: starts 0.5 s before the moment (frame 197 = source 397 of clip 2)
        const Clip c = first("Hook C");
        CHECK(c.start == 0 && c.in == 397);
        if (const auto s = sectionsOf("Hook C"); CHECK(!s.isEmpty())) CHECK(s[0].kind == StoryKind::Hook);
    }
    // A second call: new names, B skipped without a question (the answer says why)
    r = edit({QJsonObject{{"op", "hook_variants"}, {"at", QJsonArray{"8.4s"}}, {"count", 2}}});
    CHECK(text(r).contains("Hook A (2)") && text(r).contains("Skipped: B"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("story-ops");
    QTemporaryDir dir;
    testPure();
    testTeaser();
    testEditor();
    testOps(dir.path());
    return Check::result();
}
