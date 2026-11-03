// SPDX-License-Identifier: AGPL-3.0-only
// Pace and story checks of 0.17 (core/ExportCheck.h, `check`): what counts as a change (cuts, overlays, titles,
// subtitles, moving keyframes; hidden/disabled/holding ones not; close ones once), changes per second, the curve and
// per-section pace, pace_slow (vertical: a warning per stretch, 16:9: one info), break points for the suggestions;
// loud hits, the first strong moment, story_hook_late, the section checks (no hook, open question, late resolution,
// no cta, gaps) and their quiet good case; the retention CSV (time, M:SS, position in %, fractions) and its drops
// with the change before them. Then `check` on synthetic projects: a 6 s static vertical shot warns and its
// `suggest` ops are valid (dry run), apply as one step (one backup, restore brings the warning back) and solve it;
// zoom keyframes inside the shot keep it quiet; story checks from a project with sections; retention_drop at the
// cut 6.8 s before a drop at 7 s; the help names the new ids and the output.
#include "cli/Commands.h"
#include "core/ExportCheck.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include "check.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>

namespace {

using ExportCheck::ChangeKind;
using ExportCheck::Severity;

Clip media(int id, int start, int length, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.out = length - 1;
    return c;
}

Clip title(int id, int start, int length)
{
    Clip c = media(id, start, length, {});
    c.kind = ClipKind::Title;
    c.title.text = "Hi";
    return c;
}

Timeline oneTrack(const QVector<Clip>& clips, int tracks = 1)
{
    Timeline tl;
    tl.video.resize(tracks);
    tl.video[0].clips = clips;
    return tl;
}

QStringList checks(const QVector<ExportCheck::Issue>& issues)
{
    QStringList out;
    for (const ExportCheck::Issue& i : issues) out << i.check;
    return out;
}

QVector<int> framesOf(const QVector<ExportCheck::Change>& changes)
{
    QVector<int> out;
    for (const ExportCheck::Change& c : changes) out << c.frame;
    return out;
}

ProjectFormat vertical()
{
    ProjectFormat f;
    f.width = 1080;
    f.height = 1920;
    return f; // 25 fps
}

void testChanges()
{
    const ProjectFormat f; // 1920x1080, 25 fps
    Timeline tl = oneTrack({media(1, 0, 150), media(2, 150, 150)}, 4);
    tl.video[1].clips << title(3, 50, 25);
    tl.video[2].clips << media(4, 200, 50); // an overlay on V3
    tl.video[3].hidden = true;
    tl.video[3].clips << title(5, 230, 10); // hidden: no change
    tl.subtitles.resize(1);
    tl.subtitles[0].cues << SubtitleCue{9, 260, 280, "Go", {}};
    const QVector<ExportCheck::Change> c = ExportCheck::changes(tl, 0, 300, f);
    CHECK_EQ(framesOf(c), (QVector<int>{50, 75, 150, 200, 250, 260}));
    if (c.size() == 6) {
        CHECK(c[0].kind == ChangeKind::Text && c[0].clip == 3);
        CHECK(c[2].kind == ChangeKind::Cut && c[2].clip == 2);
        CHECK(c[3].kind == ChangeKind::Overlay && c[3].clip == 4);
        CHECK(c[5].kind == ChangeKind::Text && c[5].clip == 0);
    }
    // Only inside the range
    CHECK_EQ(framesOf(ExportCheck::changes(tl, 60, 200, f)), (QVector<int>{75, 150}));

    // Keyframes: a moving zoom counts at each key, a single key or equal neighbours hold still, a disabled clip
    // and a subtitle track that is off count nothing; changes closer than 0.25 s (6 frames) count once
    Timeline k = oneTrack({media(1, 0, 150)}, 2);
    k.video[0].clips[0].in = 10;
    k.video[0].clips[0].out = 159;
    k.video[0].clips[0].keys[AnimParam::ZoomX] = {{60, 1.0}, {110, 1.2}}; // source frames: timeline 50, 100
    k.video[0].clips[0].keys[AnimParam::PosX] = {{30, 5}};                // one key: still
    k.video[0].clips[0].keys[AnimParam::Opacity] = {{20, 0.2}, {40, 1}};  // not a motion parameter
    k.video[0].clips[0].keys[AnimParam::GradeExposure] = {{120, 0}, {121, 1}, {125, 0}}; // a flash: once
    Clip off = title(2, 20, 10);
    off.enabled = false;
    k.video[1].clips << off;
    k.subtitles.resize(1);
    k.subtitles[0].enabled = false;
    k.subtitles[0].cues << SubtitleCue{9, 70, 80, "x", {}};
    const QVector<ExportCheck::Change> m = ExportCheck::changes(k, 0, 150, f);
    CHECK_EQ(framesOf(m), (QVector<int>{50, 100, 110}));
    if (!m.isEmpty()) CHECK(m[0].kind == ChangeKind::Motion);
}

void testPace()
{
    const ProjectFormat f;
    // Cuts every 2 s in 10 s: 4 changes, 0.4 per second; the curve per second; per section
    Timeline tl = oneTrack({media(1, 0, 50), media(2, 50, 50), media(3, 100, 50), media(4, 150, 50), media(5, 200, 50)});
    tl.sections = {{StoryKind::Hook, "Start", 0, 100}, {StoryKind::Build, {}, 100, 250}};
    ExportCheck::Pace p = ExportCheck::pace(tl, 0, 250, f);
    CHECK_EQ(p.changes.size(), 4);
    CHECK(std::abs(p.changesPerS - 0.4) < 1e-9);
    CHECK_EQ(p.longest.length(), 50);
    CHECK(p.still.isEmpty());
    CHECK_EQ(p.curveStepS, 1.0);
    CHECK_EQ(p.curve.size(), 10);
    if (p.curve.size() == 10) CHECK_EQ(p.curve[2], 1.0); // the cut at 2 s
    if (CHECK_EQ(p.sections.size(), 2)) {
        CHECK(std::abs(p.sections[0].changesPerS - 0.25) < 1e-9); // the cut at 50 inside [0, 100)
        CHECK(std::abs(p.sections[1].changesPerS - 2 / 6.0) < 1e-9);
        CHECK_EQ(p.sections[1].longest.length(), 50);
    }
    // Cuts every 0.5 s: 19 changes in 10 s
    Timeline even = oneTrack({});
    for (int i = 0; i < 20; ++i) even.video[0].clips << media(i + 1, i * 25 / 2, (i + 1) * 25 / 2 - i * 25 / 2);
    p = ExportCheck::pace(even, 0, 250, f);
    CHECK_EQ(p.changes.size(), 19);
    CHECK(std::abs(p.changesPerS - 1.9) < 1e-9);
    // A long video: at most ~60 curve steps
    Timeline longOne = oneTrack({media(1, 0, 25 * 600)});
    p = ExportCheck::pace(longOne, 0, 25 * 600, f);
    CHECK_EQ(p.curveStepS, 10.0);
    CHECK_EQ(p.curve.size(), 60);

    // A 6 s static shot: too still. Vertical: a warning over the shot; 16:9: an info
    const Timeline still = oneTrack({media(1, 0, 150)}, 2);
    p = ExportCheck::pace(still, 0, 150, vertical());
    if (CHECK_EQ(p.still.size(), 1)) CHECK(p.still[0].from == 0 && p.still[0].to == 150);
    QVector<ExportCheck::Issue> issues = ExportCheck::paceIssues(p, 0, 150, vertical());
    CHECK_EQ(checks(issues), QStringList({"pace_slow"}));
    if (!issues.isEmpty()) CHECK(issues[0].severity == Severity::Warning && issues[0].from == 0 && issues[0].to == 150);
    issues = ExportCheck::paceIssues(ExportCheck::pace(still, 0, 150, f), 0, 150, f);
    if (CHECK_EQ(checks(issues), QStringList({"pace_slow"}))) CHECK(issues[0].severity == Severity::Info);
    // ... not with a zoom inside it (keys at 2 s and 4 s) or a text at 2.5-3.5 s, or a longer allowance
    Timeline zoomed = still;
    zoomed.video[0].clips[0].keys[AnimParam::ZoomX] = {{50, 1.0}, {100, 1.1}};
    CHECK(ExportCheck::paceIssues(ExportCheck::pace(zoomed, 0, 150, vertical()), 0, 150, vertical()).isEmpty());
    Timeline texted = still;
    texted.video[1].clips << title(2, 62, 25);
    CHECK(ExportCheck::paceIssues(ExportCheck::pace(texted, 0, 150, vertical()), 0, 150, vertical()).isEmpty());
    ExportCheck::Options patient;
    patient.maxStillS = 7;
    CHECK(ExportCheck::pace(still, 0, 150, vertical(), patient).still.isEmpty());
    // 16:9 with three still stretches: one info at the longest
    const Timeline three = oneTrack({media(1, 0, 100), media(2, 100, 150), media(3, 250, 120)});
    issues = ExportCheck::paceIssues(ExportCheck::pace(three, 0, 370, f), 0, 370, f);
    if (CHECK_EQ(issues.size(), 1)) CHECK(issues[0].from == 100 && issues[0].to == 250 && issues[0].message.contains("3"));

    // Break points: evenly inside, so no part is longer than 3 s (with a margin: an accent's change starts a little
    // before its hit); only where a picture is
    CHECK_EQ(ExportCheck::breakPoints(still, {0, 150}, f), (QVector<int>{50, 100}));
    const Timeline ten = oneTrack({media(1, 0, 250)});
    CHECK_EQ(ExportCheck::breakPoints(ten, {0, 250}, f), (QVector<int>{63, 125, 188}));
    Timeline titleOnly;
    titleOnly.video.resize(1);
    titleOnly.video[0].clips << title(1, 0, 150);
    CHECK(ExportCheck::breakPoints(titleOnly, {0, 150}, f).isEmpty());
    CHECK(ExportCheck::breakPoints(ten, {0, 70}, f).isEmpty()); // short enough
}

void testStory()
{
    const ProjectFormat f;
    const ProjectFormat v = vertical();
    // Loud hits: a burst after quiet, not a tone from the first frame on
    std::vector<float> peaks(200, 0.001f);
    for (int i = 60; i < 64; ++i) peaks[size_t(i)] = 0.5f;
    for (int i = 120; i < 122; ++i) peaks[size_t(i)] = 0.5f;
    CHECK_EQ(ExportCheck::loudMoments(peaks, 10, 25), (QVector<int>{70, 130}));
    const std::vector<float> tone(200, 0.3f);
    CHECK(ExportCheck::loudMoments(tone, 0, 25).isEmpty());

    // story_hook_late: first strong moment later than 2 s (vertical) / 5 s (16:9)
    const Timeline plain = oneTrack({media(1, 0, 500)}, 2);
    Timeline late = plain;
    late.video[1].clips << title(2, 100, 25); // text at 4 s
    QVector<ExportCheck::Issue> issues = ExportCheck::story(late, 0, 500, v, {}, false, {});
    if (CHECK_EQ(checks(issues), QStringList({"story_hook_late"})))
        CHECK(issues[0].severity == Severity::Warning && issues[0].to == 100);
    CHECK(ExportCheck::story(late, 0, 500, f, {}, false, {}).isEmpty()); // 16:9: 4 s is fine
    issues = ExportCheck::story(late, 0, 500, f, {150}, false, {});       // ... a hit at 6 s is after the text
    CHECK(issues.isEmpty());
    issues = ExportCheck::story(plain, 0, 500, f, {150}, true, {});
    if (CHECK_EQ(checks(issues), QStringList({"story_hook_late"}))) CHECK(issues[0].severity == Severity::Info);
    CHECK(ExportCheck::story(plain, 0, 500, v, {40}, true, {}).isEmpty()); // a hit at 1.6 s
    CHECK(ExportCheck::story(plain, 0, 500, v, {}, false, {}).isEmpty());  // nothing known: nothing said
    CHECK_EQ(checks(ExportCheck::story(plain, 0, 500, v, {}, true, {})), QStringList({"story_hook_late"}));
    CHECK(ExportCheck::story(late, 50, 500, v, {}, false, {}).isEmpty()); // not from the start
    // strongMoments: the earliest of loud, word and text, and what it is
    QString by;
    CHECK_EQ(ExportCheck::strongMoments(late, 0, 500, v, &peaks, {90}, &by), (QVector<int>{60, 90, 100, 120}));
    CHECK_EQ(by, QString("loud"));
    CHECK_EQ(ExportCheck::strongMoments(late, 0, 500, v, nullptr, {90, 95}, &by), (QVector<int>{90, 100}));
    CHECK_EQ(by, QString("word"));

    // Without sections, for a platform: a call to action is guessed from text or speech near the end
    ExportCheck::Options shorts;
    shorts.platform = "YouTube Shorts";
    issues = ExportCheck::story(plain, 0, 500, v, {10}, true, {}, shorts);
    if (CHECK_EQ(checks(issues), QStringList({"story_no_cta"}))) CHECK(issues[0].severity == Severity::Info);
    CHECK(ExportCheck::story(plain, 0, 500, v, {10}, true, {470}, shorts).isEmpty());
    Timeline outro = plain;
    outro.video[1].clips << title(3, 440, 60);
    CHECK(ExportCheck::story(outro, 0, 500, v, {10}, true, {}, shorts).isEmpty());
    CHECK(ExportCheck::story(plain, 0, 500, v, {10}, true, {}).isEmpty()); // no platform: no guess

    // Sections: a good story is quiet
    Timeline good = plain;
    good.sections = {{StoryKind::Hook, {}, 0, 50},          {StoryKind::Build, {}, 50, 200},
                     {StoryKind::Question, {}, 200, 260},   {StoryKind::Turn, {}, 260, 350},
                     {StoryKind::Resolution, {}, 350, 450}, {StoryKind::Cta, {}, 450, 500}};
    CHECK(ExportCheck::story(good, 0, 500, v, {10}, true, {}, shorts).isEmpty());
    // No hook at the start (it starts at 2 s)
    Timeline t = good;
    t.sections[0].kind = StoryKind::Free;
    CHECK_EQ(checks(ExportCheck::story(t, 0, 500, v, {10}, true, {})), QStringList({"story_no_hook"}));
    t.sections[1].kind = StoryKind::Hook;
    issues = ExportCheck::story(t, 0, 500, v, {10}, true, {});
    if (CHECK_EQ(checks(issues), QStringList({"story_no_hook"}))) CHECK(issues[0].to == 50);
    // A question without a turn or resolution after it
    t = good;
    t.sections[3].kind = StoryKind::Build;
    t.sections[4].kind = StoryKind::Build;
    issues = ExportCheck::story(t, 0, 500, v, {10}, true, {});
    if (CHECK_EQ(checks(issues), QStringList({"story_open_question"})))
        CHECK(issues[0].from == 200 && issues[0].to == 260 && issues[0].severity == Severity::Warning);
    // The resolution after 90 %
    t = good;
    t.sections = {{StoryKind::Hook, {}, 0, 50}, {StoryKind::Build, {}, 50, 470}, {StoryKind::Resolution, {}, 470, 490},
                  {StoryKind::Cta, {}, 490, 500}};
    issues = ExportCheck::story(t, 0, 500, v, {10}, true, {});
    if (CHECK_EQ(checks(issues), QStringList({"story_resolution_late"}))) CHECK(issues[0].severity == Severity::Warning);
    // A resolution to the end, no cta: no cta (info) and, for a platform, no room for it (info)
    t = good;
    t.sections.removeLast();
    t.sections.last().to = 500;
    CHECK_EQ(checks(ExportCheck::story(t, 0, 500, v, {10}, true, {})), QStringList({"story_no_cta"}));
    issues = ExportCheck::story(t, 0, 500, v, {10}, true, {}, shorts);
    CHECK_EQ(checks(issues), QStringList({"story_resolution_late", "story_no_cta"}));
    if (issues.size() == 2) CHECK(issues[0].severity == Severity::Info);
    // Time without a section (half a second at least): between and at the end
    t = good;
    t.sections[1].to = 180;       // 180-200 = 0.8 s
    t.sections[4].to = 440;       // 440-450 = 0.4 s: too short to say
    t.sections.last().to = 480;   // 480-500 = 0.8 s
    issues = ExportCheck::story(t, 0, 500, v, {10}, true, {});
    CHECK_EQ(checks(issues), QStringList({"story_gap", "story_gap"}));
    if (issues.size() == 2) CHECK(issues[0].from == 180 && issues[0].to == 200 && issues[1].from == 480);
}

void testRetention()
{
    const ProjectFormat f;
    QString error;
    // Seconds with a header, decimal commas and %, semicolons
    QVector<ExportCheck::RetentionPoint> c =
        ExportCheck::retentionFromCsv("Zeit;Zuschauerbindung\n0;100 %\n1,5;92,5 %\n0:03;88\n", 0, &error);
    if (CHECK_EQ(c.size(), 3)) {
        CHECK_EQ(c[1].seconds, 1.5);
        CHECK_EQ(c[1].percent, 92.5);
        CHECK_EQ(c[2].seconds, 3.0);
    }
    // Position in % of the video (YouTube Studio), retention as fractions, extra columns
    c = ExportCheck::retentionFromCsv("Video position (%),Relative retention,Absolute audience retention (%)\n"
                                      "0,0.5,1.0\n50,0.6,0.8\n100,0.4,0.6\n",
                                      20, &error);
    if (CHECK_EQ(c.size(), 3)) {
        CHECK_EQ(c[1].seconds, 10.0);
        CHECK(std::abs(c[1].percent - 80) < 1e-9); // the "audience retention" column
    }
    // Quoted cells with decimal commas and commas between them (LibreOffice / a German export), a byte order mark
    c = ExportCheck::retentionFromCsv(QString(QChar(0xfeff)) + "\"0\",\"100\"\n\"0,5\",\"85,3\"\n\"2\",\"80\"\n", 0, &error);
    if (CHECK_EQ(c.size(), 3)) {
        CHECK_EQ(c[0].percent, 100.0); // the first row is data, not a header
        CHECK_EQ(c[1].seconds, 0.5);
        CHECK(std::abs(c[1].percent - 85.3) < 1e-9);
    }
    CHECK(ExportCheck::retentionFromCsv("time,ret\n", 10, &error).isEmpty() && !error.isEmpty());
    CHECK(ExportCheck::retentionFromCsv("Video position (%),r\n0,1\n100,0.5\n", 0, &error).isEmpty());

    // A steady fall of 1 point a second with a drop of 15 points at 7 s; cuts at 3 s and 6.8 s
    QString csv = "time,retention\n";
    for (int s = 0; s <= 15; ++s) csv += QString("%1,%2\n").arg(s).arg(100 - s - (s >= 7 ? 15 : 0));
    c = ExportCheck::retentionFromCsv(csv, 15);
    const Timeline tl = oneTrack({media(1, 0, 75), media(2, 75, 95), media(3, 170, 205)});
    const QVector<ExportCheck::Issue> drops = ExportCheck::retention(c, ExportCheck::changes(tl, 0, 375, f), 0, f);
    if (CHECK_EQ(checks(drops), QStringList({"retention_drop"}))) {
        CHECK_EQ(drops[0].at, 170);
        CHECK_EQ(drops[0].clip, 3);
        CHECK(drops[0].from <= 150 && drops[0].to >= 175);
        CHECK(drops[0].message.contains("6"));
    }
    // A steady fall alone: no drop; the first second does not count
    csv = "time,retention\n0,100\n1,70\n";
    for (int s = 2; s <= 15; ++s) csv += QString("%1,%2\n").arg(s).arg(70 - s);
    CHECK(ExportCheck::retention(ExportCheck::retentionFromCsv(csv, 15), {}, 0, f).isEmpty());
}

// ---------- `check` on projects

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return QJsonObject{{"failed", e.code}};
    }
}

QStringList checksOf(const QJsonObject& r)
{
    QStringList out;
    for (const QJsonValue& v : r["issues"].toArray()) out << v["check"].toString();
    return out;
}

QJsonObject issueOf(const QJsonObject& r, const QString& check)
{
    for (const QJsonValue& v : r["issues"].toArray())
        if (v["check"].toString() == check) return v.toObject();
    return {};
}

int firstClip(const QString& project)
{
    for (const QJsonValue& t : run("info", {{"project", project}}).value("timeline")["tracks"].toArray())
        for (const QJsonValue& c : t["clips"].toArray()) return c["id"].toInt();
    return 0;
}

QString ops(const QJsonArray& a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); }

void testCommand(const QString& dir)
{
    const QString gray = Check::makeMedia(dir + "/gray.mp4", {"-f", "lavfi", "-i", "color=c=gray:s=270x480:r=25:d=16",
                                                              "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    if (!CHECK(!gray.isEmpty())) return;
    // A 6 s static shot in a vertical video: pace_slow over it, the pace numbers, a suggestion
    const QString project = dir + "/still.schneidi";
    run("new", {{"project", project}, {"width", 270}, {"height", 480}, {"fps", 25}});
    run("edit", {{"project", project}, {"ops", QString(R"([{"op":"place","media":"%1","at":0,"in":0,"out":"6s"}])").arg(gray)}});
    QJsonObject r = run("check", {{"source", project}, {"sound", false}});
    const QJsonObject slow = issueOf(r, "pace_slow");
    CHECK_EQ(slow["severity"].toString(), QString("warning"));
    CHECK_EQ(slow["from"].toInt(), 0);
    CHECK_EQ(slow["to"].toInt(), 150);
    const QJsonObject pace = r["pace"].toObject();
    CHECK_EQ(pace["changes"].toInt(), 0);
    CHECK_EQ(pace["longest_s"].toDouble(), 6.0);
    CHECK_EQ(pace["at"].toInt(), 0);
    CHECK_EQ(pace["curve"].toArray().size(), 6);
    CHECK_EQ(pace["still"].toArray().size(), 1);
    CHECK_EQ(r.value("story")["sections"].toInt(), 0);
    CHECK(r.value("story")["note"].isString());
    const QJsonObject suggest = r["suggest"].toObject();
    const QJsonArray fixOps = suggest["ops"].toArray();
    if (CHECK_EQ(fixOps.size(), 1)) {
        CHECK_EQ(fixOps[0]["op"].toString(), QString("accent"));
        CHECK_EQ(fixOps[0]["at"].toArray(), QJsonArray({50, 100}));
        CHECK(fixOps[0]["why"].toString().startsWith("pace_slow"));
    }
    CHECK(!r.contains("fix")); // a creative suggestion is not a safe fix (`short` applies `fix` by itself)
    // Valid ops: a dry run takes them and changes nothing
    const QJsonObject dry = run("edit", {{"project", project}, {"ops", ops(fixOps)}, {"dry_run", true}});
    CHECK(!dry.contains("failed"));
    CHECK(issueOf(run("check", {{"source", project}, {"sound", false}}), "pace_slow").contains("check"));
    // Applied: one step (one backup), the shot is no longer too still; restore brings it back
    const int backups = run("backups", {{"project", project}})["backups"].toArray().size();
    run("edit", {{"project", project}, {"ops", ops(fixOps)}});
    CHECK_EQ(run("backups", {{"project", project}})["backups"].toArray().size(), backups + 1);
    r = run("check", {{"source", project}, {"sound", false}});
    CHECK(!checksOf(r).contains("pace_slow"));
    CHECK(!r.contains("suggest"));
    CHECK(r.value("pace")["kinds"]["motion"].toInt() >= 1);
    run("restore", {{"project", project}});
    CHECK(checksOf(run("check", {{"source", project}, {"sound", false}})).contains("pace_slow"));
    // A slow zoom with keys at 2 s and 4 s inside the shot: quiet; max_still_s gives more room
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"zoom","keys":[{"at":"2s","value":1},{"at":"4s","value":1.1}]}])")
                             .arg(firstClip(project))}});
    r = run("check", {{"source", project}, {"sound", false}});
    CHECK(!checksOf(r).contains("pace_slow"));
    CHECK_EQ(r.value("pace")["changes"].toInt(), 2);
    CHECK_EQ(r.value("pace")["longest_s"].toDouble(), 2.0);
    run("restore", {{"project", project}});
    CHECK(!checksOf(run("check", {{"source", project}, {"sound", false}, {"max_still_s", 7}})).contains("pace_slow"));
    CHECK_EQ(run("check", {{"source", project}, {"max_still_s", 0.1}})["failed"].toString(), QString("BAD_ARGUMENT"));
    // Story from the project file's sections: a question left open, no cta; per-section pace
    {
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(project, &d, &error));
        d.timeline.sections = {{StoryKind::Hook, {}, 0, 50}, {StoryKind::Question, "Why?", 50, 150}};
        for (Sequence& s : d.sequences) s.timeline.sections = d.timeline.sections;
        CHECK(ProjectFile::save(d, project, &error));
    }
    r = run("check", {{"source", project}, {"sound", false}});
    CHECK(checksOf(r).contains("story_open_question"));
    CHECK(checksOf(r).contains("story_no_cta"));
    CHECK(!checksOf(r).contains("story_no_hook"));
    CHECK_EQ(r.value("story")["sections"].toInt(), 2);
    CHECK_EQ(r.value("pace")["sections"].toArray().size(), 2);
    CHECK_EQ(r.value("pace")["sections"][1]["kind"].toString(), QString("question"));
    CHECK_EQ(r.value("pace")["sections"][1]["name"].toString(), QString("Why?"));

    // Retention: cuts at 3 s and 6.8 s, a drop at 7 s -> found at the cut 6.8 s
    const QString wide = dir + "/wide.schneidi";
    run("new", {{"project", wide}, {"width", 320}, {"height", 180}, {"fps", 25}});
    run("edit", {{"project", wide},
                 {"ops", QString(R"([{"op":"place","media":"%1","at":0,"in":0,"out":"3s"},
                                    {"op":"place","media":"%1","at":"3s","in":"3s","out":"6.8s"},
                                    {"op":"place","media":"%1","at":"6.8s","in":"6.8s","out":"15s"}])")
                             .arg(gray)}});
    QString csv = "time,retention\n";
    for (int s = 0; s <= 15; ++s) csv += QString("0:%1,%2\n").arg(s, 2, 10, QChar('0')).arg(100 - s - (s >= 7 ? 15 : 0));
    QFile file(dir + "/retention.csv");
    if (CHECK(file.open(QIODevice::WriteOnly))) file.write(csv.toUtf8());
    file.close();
    r = run("check", {{"source", wide}, {"sound", false}, {"retention", file.fileName()}});
    const QJsonObject drop = issueOf(r, "retention_drop");
    CHECK_EQ(drop["severity"].toString(), QString("info"));
    CHECK_EQ(drop["at"].toInt(), 170);
    CHECK_EQ(drop["at_s"].toDouble(), 6.8);
    CHECK(drop["clip"].toInt() > 0);
    CHECK_EQ(r.value("retention")["points"].toInt(), 16);
    CHECK_EQ(r.value("retention")["drops"].toInt(), 1);
    CHECK_EQ(run("check", {{"source", wide}, {"sound", false}, {"retention", dir + "/none.csv"}})["failed"].toString(),
             QString("NOT_FOUND"));

    // The help documents the ids and the answer
    const Cli::Command* check = Cli::find("check");
    QStringList params;
    for (const Cli::Param& p : check->params) params << p.name;
    CHECK(params.contains("max_still_s") && params.contains("retention"));
    for (const char* word : {"pace_slow", "changes_per_s", "longest_s", "curve", "suggest", "story_hook_late",
                             "story_no_hook", "story_open_question", "story_resolution_late", "story_no_cta",
                             "story_gap", "retention_drop"})
        CHECK(check->summary.contains(QLatin1String(word)));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("story_pace");
    testChanges();
    testPace();
    testStory();
    testRetention();
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initEngine();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8()); // backups of the test projects
    testCommand(dir.path());
    return Check::result();
}
