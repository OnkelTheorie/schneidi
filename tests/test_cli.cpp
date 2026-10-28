// schneidi-cli command layer without media decoding: argument checks, edit operations on a saved project,
// dry run, errors leave the file untouched
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "cli/Mcp.h"
#include "core/Project.h"
#include "core/EffectRegistry.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"

#include <QLockFile>

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cmath>
#include <utility>

using Check::dump;

namespace {

QString g_path;

// `edit` answers with the whole timeline here (the tests look at it; the short default answer: testEditAnswer)
const QJsonObject run(const QString& name, const QJsonObject& raw0)
{
    QJsonObject raw = raw0;
    if (name == "edit" && !raw.contains("full")) raw["full"] = true;
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        throw;
    }
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

QString messageOf(const QString& name, const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

QJsonObject edit(const char* ops, bool dry = false)
{
    return run("edit", {{"project", g_path}, {"ops", QString(ops)}, {"dry_run", dry}});
}

QString saved()
{
    ProjectData d;
    QString error;
    ProjectFile::load(g_path, &d, &error);
    return dump(d.timeline);
}

// Project with one 100-frame media file (not on disk: edits only need its length) on V1/A1
void writeProject()
{
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v; v.id = p.newClipId(); v.mediaPath = "/x/a.mp4"; v.out = 99; v.linkId = p.newLinkId();
        Clip a = v; a.id = p.newClipId();
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
    });
    QString error;
    CHECK(ProjectFile::save(p.data(), g_path, &error));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("cli");
    QTemporaryDir dir;
    g_path = dir.filePath("t.schneidi");

    // Every edit op name exists once (a second "track" once hid the track settings op)
    {
        QStringList names;
        for (const Cli::detail::OpDef& op : Cli::detail::editOps()) {
            if (!CHECK(!names.contains(op.name))) std::printf("       op %s twice\n", qPrintable(op.name));
            names << op.name;
        }
    }

    // Arguments: types, unknown and missing parameters
    CHECK_EQ(errorOf("info", {}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("info", {{"project", g_path}, {"bogus", 1}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("info", {{"project", dir.filePath("missing.schneidi")}}), QString("NOT_FOUND"));
    // Names of another command's parameter (`file` vs `source`, `output` for `out`): taken when unambiguous; an
    // unknown one names the right ones
    {
        const Cli::Command* frames = Cli::find("frames");
        const QJsonObject n = Cli::normalize(*frames, {{"file", g_path}, {"at", QJsonArray{0}}});
        CHECK(n.contains("source") && !n.contains("file"));
        const Cli::Command* loudness = Cli::find("loudness");
        CHECK(Cli::normalize(*loudness, {{"project", g_path}}).contains("source"));
        const Cli::Command* render = Cli::find("render");
        CHECK(Cli::normalize(*render, {{"project", g_path}, {"output", dir.filePath("x.mp4")}}).contains("out"));
        const Cli::Command* probe = Cli::find("probe");
        CHECK(Cli::normalize(*probe, {{"source", g_path}}).contains("file"));
        QString message;
        try {
            Cli::normalize(*frames, {{"source", g_path}, {"file", g_path}}); // both: no guess
        } catch (const Cli::Error& e) {
            message = e.message;
        }
        CHECK(message.contains("'file'") && message.contains("source") && message.contains("sheet"));
    }
    // Times beyond int: an error, not an undefined cast
    {
        const ProjectFormat fmt;
        auto timeError = [&](const QJsonValue& v) {
            try {
                Cli::detail::parseTime(v, fmt, "at");
            } catch (const Cli::Error& e) {
                return e.code;
            }
            return QString("none");
        };
        CHECK_EQ(Cli::detail::parseTime(QJsonValue(250), fmt, "at"), 250);
        CHECK_EQ(Cli::detail::parseTime(QJsonValue("4.5s"), fmt, "at"), int(std::lround(4.5 * fmt.rate.fps())));
        CHECK_EQ(timeError(QJsonValue(1e12)), QString("BAD_ARGUMENT"));
        CHECK_EQ(timeError(QJsonValue("1e10s")), QString("BAD_ARGUMENT"));
        // Times with colons (25 fps): timecode with frames, clock time, decimal seconds - never guessed into frames
        const auto at = [&](const char* t) { return Cli::detail::parseTime(QJsonValue(t), fmt, "at"); };
        CHECK_EQ(at("00:00:04:20"), 120);
        CHECK_EQ(at("00:00:04;20"), 120);
        CHECK_EQ(at("01:00:00:00"), 90000);
        CHECK_EQ(at("00:01:02.5"), 1563); // 62.5 s, not 1:02 + 5 frames
        CHECK_EQ(at("00:00:04.500"), 113);
        CHECK_EQ(at("1:30"), 2250); // minutes:seconds
        CHECK_EQ(at("1:02.5"), 1563);
        CHECK_EQ(at("00:01:30"), 2250); // hours:minutes:seconds
        CHECK_EQ(at("90:00"), 135000); // 90 minutes
        for (const char* bad : {"00:00:04:50", "00:00:04:25", "00:61:00:00", "1:75", "00:00:04:20.5", "1:2:3:4:5",
                                "1;30", "00:00;04:20", "1:", ":30", "1:3a", "1:30.", "00:00:04;20;1"})
            CHECK_EQ(timeError(QJsonValue(bad)), QString("BAD_ARGUMENT"));
        QString frameMessage;
        try {
            at("00:00:04:50");
        } catch (const Cli::Error& e) {
            frameMessage = e.message;
        }
        CHECK(frameMessage.contains("frame 50") && frameMessage.contains("25 frames"));
        // A number with a fraction (a small model's seconds): the error says how to write it
        for (const QJsonValue& v : {QJsonValue(6.95), QJsonValue("6.95")}) {
            QString message;
            try {
                Cli::detail::parseTime(v, fmt, "by");
            } catch (const Cli::Error& e) {
                message = e.message;
            }
            CHECK(message.contains("\"6.95s\"") && message.contains("whole numbers"));
        }
    }
    const QJsonObject help = run("help", {});
    CHECK_EQ(help["commands"].toArray().size(), int(Cli::commands().size()));
    CHECK(help.contains("servers") && help.contains("usage"));
    // One command: only that (the MCP/HTTP server options were 2.5 KB of every `help edit --op …` answer)
    const QJsonObject one = run("help", {{"command", "watch"}});
    CHECK(one["commands"].toArray().size() == 1 && !one.contains("servers") && !one.contains("usage"));

    writeProject();
    const QString start = "V1: a[0-100|0-99]  V2:  A1: a[0-100|0-99]  A2:";
    CHECK_EQ(saved(), start);

    // Times as frames, seconds (25 fps) and timecode; ripple delete of a range on all tracks
    edit(R"([{"op":"delete_range","from":"0.4s","to":"00:00:00:20"}])");
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));

    // Dry run: result shown, file unchanged
    const QJsonObject dry = edit(R"([{"op":"split","at":50}])", true);
    CHECK(!dry["saved"].toBool());
    CHECK_EQ(dry["timeline"]["tracks"][0]["clips"].toArray().size(), 3);
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));

    // testEditAnswer: by default an edit answers with what changed (new clips with their ids, as `info` shows them)
    // and the timeline as a summary - an AI reads every answer
    {
        const auto answer = [](const char* ops) {
            return run("edit", {{"project", g_path}, {"ops", QString(ops)}, {"dry_run", true}, {"full", false}});
        };
        QJsonObject r = answer(R"([{"op":"split","at":50}])");
        const QJsonObject changes = r["changes"].toObject();
        const QJsonArray added = changes["added"].toArray(), changed = changes["changed"].toArray();
        CHECK_EQ(added.size(), 2); // the right halves on V1 and A1
        CHECK_EQ(changed.size(), 2);
        CHECK(!changes.contains("removed"));
        CHECK_EQ(added[0].toObject().value("start").toInt(), 50);
        CHECK(added[0].toObject().value("id").toInt() > 0);
        CHECK(!added[0].toObject().contains("timeline")); // only items of another timeline name it
        CHECK_EQ(changed[0].toObject().value("end").toInt(), 50);
        CHECK(!changed[0].toObject().contains("media")); // changed clips: their place and what changed
        CHECK(changed[0].toObject().contains("src_out"));
        const QJsonObject tl = r["timeline"].toObject();
        CHECK_EQ(tl["tracks"].toArray().at(0).toObject().value("clips").toInt(), 3);
        CHECK_EQ(tl["clips"].toInt(), 6);
        CHECK(tl["master"].isObject());
        const qsizetype compact = QJsonDocument(r).toJson(QJsonDocument::Compact).size();
        const qsizetype full = QJsonDocument(edit(R"([{"op":"split","at":50}])", true)).toJson(QJsonDocument::Compact).size();
        CHECK(compact < full);
        // Removed: ids and tracks; subtitles count like clips
        const int first = changed[0].toObject().value("id").toInt();
        r = answer(QString(R"([{"op":"delete","clips":[%1]},{"op":"subtitle","text":"Hi","at":5}])").arg(first).toUtf8().constData());
        const QJsonArray removed = r["changes"].toObject().value("removed").toArray();
        CHECK_EQ(removed.size(), 2); // with its linked partner
        CHECK(removed.at(0).toObject().value("id").toInt() == first || removed.at(1).toObject().value("id").toInt() == first);
        bool cue = false;
        for (const QJsonValue& v : r["changes"].toObject().value("added").toArray())
            cue |= v.toObject().value("track").toString() == "ST1" && v.toObject().value("text").toString() == "Hi";
        CHECK(cue);
        CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));
    }

    // In the app's own chat a command without a project works on "@app" - not `new` (it makes a file)
    {
        qputenv("SCHNEIDI_LIVE_SERVER", "test");
        QJsonObject args{{"summary", true}};
        CHECK(Cli::defaultToAppProject(*Cli::find("info"), args) && args["project"] == "@app");
        QJsonObject path{{"path", "/x/p.schneidi"}}; // under another name: normalize takes it
        CHECK(!Cli::defaultToAppProject(*Cli::find("info"), path));
        QJsonObject fresh{{"template", "vertical"}};
        CHECK(!Cli::defaultToAppProject(*Cli::find("new"), fresh) && !fresh.contains("project"));
        qunsetenv("SCHNEIDI_LIVE_SERVER");
        QJsonObject outside{{"summary", true}};
        CHECK(!Cli::defaultToAppProject(*Cli::find("info"), outside));
    }

    // An analysis without a file (small models in the app's chat): the error lists the project's media to copy
    CHECK(messageOf("highlights", {{"project", g_path}}).contains("; the media of the project: /x/a.mp4"));

    // A tool that is an op of edit (small models call "new_timeline"): the error has the call to make; others list the tools
    {
        const auto message = [](const QString& tool, const QJsonObject& args) {
            const QJsonObject r = Cli::Mcp::callTool({{"name", tool}, {"arguments", args}}, {}, {});
            return QJsonDocument::fromJson(r["content"][0]["text"].toString().toUtf8())["error"]["message"].toString();
        };
        const QString op = message("new_timeline", {{"project", "/x/p.schneidi"}, {"name", "Kills"}});
        CHECK(op.contains("an op of the edit tool"));
        CHECK(op.contains(R"({"ops":[{"name":"Kills","op":"new_timeline"}],"project":"/x/p.schneidi"})"));
        const QString other = message("cut_all", {});
        CHECK(other.startsWith("unknown tool cut_all (tools: ") && other.contains("edit") && other.contains("highlights"));
    }

    // No media file: the error says what 'media' is; "file"/"path" (other commands' names) are taken as 'media'
    CHECK(messageOf("edit", {{"project", g_path}, {"ops", R"([{"op":"place","at":0}])"}}).contains("'media' is the file"));
    {
        const QJsonObject r = run("edit", {{"project", g_path}, {"ops", R"([{"op":"append","file":"/x/a.mp4","in":0,"out":5}])"},
                                           {"dry_run", true}});
        bool aliased = false;
        for (const QJsonValue& n : r["notes"].toArray()) aliased |= n.toString().contains("'file' read as 'media'");
        CHECK(aliased && r["changes"].toObject().contains("added"));
        CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));
    }

    // A failing op saves nothing (also not the ops before it)
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"split","at":50},{"op":"trim","clip":999}])"}}),
             QString("NOT_FOUND"));
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));

    // Several ranges at once, all in times before the cut (words of `transcribe`)
    edit(R"([{"op":"delete_range","ranges":[[60,70],[5,8]]}])");
    CHECK_EQ(saved(), QString("V1: a[0-5|0-4] a[5-7|8-9] a[7-57|20-69] a[57-77|80-99]  V2:  "
                              "A1: a[0-5|0-4] a[5-7|8-9] a[7-57|20-69] a[57-77|80-99]  A2:"));

    // Clear, then cut a source range list one after another (jump cut), title on top, delete with linked audio
    edit(R"([{"op":"clear"},
             {"op":"keep","media":"/x/a.mp4","ranges":[[0,10],[30,40],{"from":"2s","to":"3s"}]},
             {"op":"title","text":"Hi","at":5,"duration":10}])");
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-20|30-39] a[20-45|50-74]  V2: T[5-15|0-9]  "
                              "A1: a[0-10|0-9] a[10-20|30-39] a[20-45|50-74]  A2:"));
    // One clip under 'clips' (gemma4: trim clips:[1]) is that clip, with a note; several: one op each; "7" is 7
    {
        const int first = run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"][0]["id"].toInt();
        const QJsonObject list = run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"trim","clips":[%1],"edge":"end","to":5}])").arg(first)},
                                              {"dry_run", true}});
        CHECK(!list["changes"]["changed"].toArray().isEmpty() && list["notes"][0].toString().contains("read as clip:"));
        const QJsonObject text = run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"trim","clip":"%1","edge":"end","to":5}])").arg(first)},
                                              {"dry_run", true}});
        CHECK(!text["changes"]["changed"].toArray().isEmpty());
        CHECK(messageOf("edit", {{"project", g_path}, {"ops", R"([{"op":"trim","clips":[1,2],"edge":"end","to":5}])"}})
                  .contains("trim takes one clip: 'clip'"));
    }
    // keep with one range written another way (small models): taken with a note; no range at all: a whole example
    for (const char* op : {R"({"op":"keep","media":"/x/a.mp4","ranges":[0,10]})", R"({"op":"keep","media":"/x/a.mp4","range":[0,10]})",
                            R"({"op":"keep","media":"/x/a.mp4","from":0,"to":10})"}) {
        const QJsonObject r = run("edit", {{"project", g_path}, {"ops", QString("[%1]").arg(op)}, {"dry_run", true}});
        CHECK_EQ(r["changes"]["added"].toArray().size(), 2); // V1 and A1
        CHECK(r["notes"][0].toString().contains("read as ranges:[[from, to]]"));
    }
    CHECK(run("edit", {{"project", g_path}, {"ops", R"([{"op":"keep","media":"/x/a.mp4","ranges":[{"start":0,"end":10}]}])"},
                       {"dry_run", true}})["changes"]["added"].toArray().size() == 2);
    CHECK(messageOf("edit", {{"project", g_path}, {"ops", R"([{"op":"keep","media":"/x/a.mp4"}])"}})
              .contains(R"("ranges":[["2s","5.5s"],["10s","12s"]])"));
    const QJsonObject info = run("info", {{"project", g_path}});
    const int secondId = info["timeline"]["tracks"][0]["clips"][1]["id"].toInt();
    edit(QString(R"([{"op":"delete","clips":[%1],"ripple":true}])").arg(secondId).toUtf8().constData());
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-35|50-74]  V2: T[5-15|0-9]  A1: a[0-10|0-9] a[10-35|50-74]  A2:"));

    // Unknown op and bad time report the op index
    try {
        edit(R"([{"op":"marker","at":3},{"op":"split","at":"x"}])");
        CHECK(false);
    } catch (const Cli::Error& e) {
        CHECK(e.message.startsWith("op 1 (split)"));
    }

    // Subtitles (list and single cue), an effect with checked parameters, tracks
    edit(R"([{"op":"subtitles","cues":[{"from":0,"to":10,"text":"One"},{"from":20,"to":"1s","text":"Two"}],"name":"AI"},
             {"op":"subtitle","text":"Three","at":30,"duration":100},
             {"op":"add_track","kind":"audio"},
             {"op":"remove_track","track":"V2"}])");
    QJsonObject tl = run("info", {{"project", g_path}})["timeline"].toObject();
    const QJsonArray cues = tl["subtitles"][0]["cues"].toArray();
    CHECK_EQ(tl["subtitles"][0]["name"].toString(), QString("AI"));
    CHECK_EQ(cues.size(), 3);
    CHECK_EQ(cues[1]["end"].toInt(), 25);
    CHECK_EQ(cues[2]["end"].toInt(), 130);
    // A cue stretched over the next one stops at it, and the answer says so (an AI merging two cues extended the
    // first before deleting the second and saw nothing happen)
    const QJsonObject stretched = run("edit", {{"project", g_path}, {"dry_run", true},
                                               {"ops", QString(R"([{"op":"subtitle","id":%1,"to":28}])").arg(cues[0]["id"].toInt())}});
    CHECK(stretched["notes"].toArray().at(0).toString().contains("is 0-20 instead of 0-28"));
    CHECK(!run("edit", {{"project", g_path}, {"dry_run", true},
                        {"ops", QString(R"([{"op":"subtitle","id":%1,"to":15}])").arg(cues[0]["id"].toInt())}})
               .contains("notes"));
    CHECK_EQ(tl["tracks"].toArray().size(), 4); // V1, A1, A2, A3
    const int firstId = tl["tracks"][0]["clips"][0]["id"].toInt();
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"effect","clips":[%1],"effect":"blur",
             "params":{"radius":3}}])").arg(firstId)}}), QString("BAD_ARGUMENT"));
    edit(QString(R"([{"op":"effect","clips":[%1],"effect":"blur","params":{"strength":500}},
                     {"op":"delete","clips":[%2]},
                     {"op":"subtitle","id":%3,"text":"Two!"}])")
             .arg(firstId).arg(cues[0]["id"].toInt()).arg(cues[1]["id"].toInt()).toUtf8().constData());
    tl = run("info", {{"project", g_path}})["timeline"].toObject();
    CHECK_EQ(tl["tracks"][0]["clips"][0]["effects"][0]["params"]["strength"].toDouble(), 100.0); // clamped
    CHECK_EQ(tl["subtitles"][0]["cues"].toArray().size(), 2);
    CHECK_EQ(tl["subtitles"][0]["cues"][0]["text"].toString(), QString("Two!"));

    // Restore = undo of the last edit (and restore again = redo)
    run("restore", {{"project", g_path}});
    tl = run("info", {{"project", g_path}})["timeline"].toObject();
    CHECK(!tl["tracks"][0]["clips"][0].toObject().contains("effects"));
    CHECK_EQ(tl["subtitles"][0]["cues"].toArray().size(), 3);
    run("restore", {{"project", g_path}});
    tl = run("info", {{"project", g_path}})["timeline"].toObject();
    CHECK_EQ(tl["subtitles"][0]["cues"].toArray().size(), 2);
    CHECK_EQ(errorOf("restore", {{"project", g_path}, {"backup", "999"}}), QString("NOT_FOUND"));

    // Someone else (the app) saving: writing commands wait, then give up; reading works
    {
        const auto held = ProjectFile::lock(g_path, 0);
        CHECK(held != nullptr);
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"marker","at":1}])"}}), QString("PROJECT_LOCKED"));
        CHECK_EQ(errorOf("info", {{"project", g_path}}), QString("none"));
    }
    edit(R"([{"op":"marker","at":1}])");

    // Several timelines: a new one gets edited by the following ops, the app keeps opening the old one;
    // `timeline` selects one for edit/info, place_timeline nests it, compound/decompose
    {
        const QJsonObject r = edit(R"([{"op":"new_timeline","name":"Short"},
                                       {"op":"append","media":"/x/a.mp4","in":0,"out":20}])");
        CHECK_EQ(std::as_const(r)["timeline"]["name"].toString(), QString("Short"));
        CHECK_EQ(r["timelines"].toArray().size(), 2);
        ProjectData d;
        ProjectFile::load(g_path, &d, nullptr);
        CHECK_EQ(d.currentSequence, 1); // still the first one
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"new_timeline","name":"short"}])"}}),
                 QString("EXISTS"));
        CHECK_EQ(errorOf("info", {{"project", g_path}, {"timeline", "Nope"}}), QString("NOT_FOUND"));
        QJsonObject other = run("info", {{"project", g_path}, {"timeline", "short"}})["timeline"].toObject();
        CHECK_EQ(other["length"].toInt(), 20);
        const int shortId = other["id"].toInt();
        // The open timeline empty, the cut in the new one: `check` and `render` name both (an AI got "the timeline is
        // empty" and had to guess that the file still opens Timeline 1)
        const QString emptyOpen = g_path + ".empty.schneidi";
        QFile::remove(emptyOpen);
        QFile::copy(g_path, emptyOpen);
        run("edit", {{"project", emptyOpen}, {"ops", R"([{"op":"clear"}])"}});
        const QString empty = messageOf("check", {{"project", emptyOpen}});
        CHECK(empty.startsWith("EMPTY: the timeline 1 ") && empty.contains(QString("%1 \"Short\"").arg(shortId)));
        CHECK(messageOf("render", {{"project", emptyOpen}, {"out", g_path + ".mp4"}}).contains("pass 'timeline'"));

        edit(R"([{"op":"clear"},{"op":"place_timeline","timeline":"Short","at":0}])");
        QJsonObject tl = run("info", {{"project", g_path}})["timeline"].toObject();
        CHECK_EQ(tl["tracks"][0]["clips"][0]["kind"].toString(), QString("compound"));
        CHECK_EQ(tl["length"].toInt(), 20);
        // the nested timeline cannot go
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"remove_timeline","timeline":%1}])").arg(shortId)}}),
                 QString("FAILED"));
        const int nested = tl["tracks"][0]["clips"][0]["id"].toInt();
        edit(QString(R"([{"op":"decompose","clips":[%1]},{"op":"remove_timeline","timeline":%2},
                         {"op":"split","at":10}])").arg(nested).arg(shortId).toUtf8().constData());
        tl = run("info", {{"project", g_path}})["timeline"].toObject();
        CHECK_EQ(tl["tracks"][0]["clips"].toArray().size(), 2);
        CHECK_EQ(tl["tracks"][0]["clips"][0]["kind"].toString(), QString("media"));
        const int a = tl["tracks"][0]["clips"][0]["id"].toInt(), b = tl["tracks"][0]["clips"][1]["id"].toInt();
        const QJsonObject c = edit(QString(R"([{"op":"compound","clips":[%1,%2],"name":"Both"}])").arg(a).arg(b).toUtf8().constData());
        CHECK_EQ(c["timeline"]["tracks"][0]["clips"].toArray().size(), 1);
        CHECK_EQ(c["timelines"][1]["name"].toString(), QString("Both"));
        CHECK(c["timelines"][1]["compound"].toBool());
        // Edit inside the compound clip, open it next time in the app
        edit(R"([{"op":"timeline","timeline":"Both","open":true},{"op":"marker","at":2}])");
        ProjectFile::load(g_path, &d, nullptr);
        CHECK_EQ(d.timeline.markers, QVector<int>{2});
        edit(R"([{"op":"duplicate_timeline","name":"Copy","switch":true},{"op":"rename_timeline","name":"Copy 2"}])");
        CHECK_EQ(run("info", {{"project", g_path}})["timelines"].toArray().size(), 3);
        CHECK_EQ(run("info", {{"project", g_path}, {"timeline", "Copy 2"}})["timeline"].toObject()["markers"].toArray().at(0).toInt(), 2);
    }

    // Look: grade (wheels, sliders, reset), transform, keyframes (times = timeline frames), copy attributes
    {
        edit(R"([{"op":"timeline","timeline":1,"open":true},{"op":"clear"},
                 {"op":"append","media":"/x/a.mp4","in":0,"out":40},{"op":"append","media":"/x/a.mp4","in":50,"out":90}])");
        QJsonObject tl = run("info", {{"project", g_path}})["timeline"].toObject();
        const int v1 = tl["tracks"][0]["clips"][0]["id"].toInt(), v2 = tl["tracks"][0]["clips"][1]["id"].toInt();
        int a1 = 0, a1Track = 0;
        for (int i = 0; i < tl["tracks"].toArray().size(); ++i)
            if (tl["tracks"][i]["track"].toString() == "A1") {
                a1 = tl["tracks"][i]["clips"][0]["id"].toInt();
                a1Track = i;
            }
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"color","clips":[%1],"lift":{"q":1}}])").arg(v1)}}),
                 QString("BAD_ARGUMENT"));
        edit(QString(R"([{"op":"color","clips":[%1],"gain":{"y":1.2,"b":0.9},"saturation":70,"temperature":9000},
                         {"op":"transform","clips":[%1],"zoom":1.5,"x":100,"opacity":80},
                         {"op":"keyframe","clips":[%1],"param":"opacity","keys":[{"at":5,"value":0},{"at":30,"value":100,"ease":"ease_in"}]},
                         {"op":"keyframe","clips":[%1],"param":"grade.liftY","at":10,"value":0.1},
                         {"op":"keyframe","clips":[%2],"param":"volume","at":20,"value":-6}])")
                 .arg(v1).arg(a1).toUtf8().constData());
        const auto firstClip = [] { return run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"][0].toObject(); };
        const QJsonObject c = firstClip();
        const QJsonObject grade = c["effects"][0]["params"].toObject();
        CHECK_EQ(c["effects"][0]["effect"].toString(), QString("grade"));
        CHECK_EQ(grade["gainY"].toDouble(), 1.2);
        CHECK_EQ(grade["gainB"].toDouble(), 0.9);
        CHECK_EQ(grade["temperature"].toDouble(), 4000.0); // clamped
        CHECK_EQ(c["transform"]["zoom_x"].toDouble(), 1.5);
        CHECK_EQ(c["transform"]["x"].toDouble(), 100.0);
        const QJsonArray op = c["keyframes"]["opacity"].toArray();
        CHECK_EQ(op.size(), 2);
        CHECK_EQ(op[1]["at"].toInt(), 30);
        CHECK_EQ(op[1]["ease"].toString(), QString("ease_in"));
        CHECK_EQ(c["keyframes"]["grade.liftY"][0]["value"].toDouble(), 0.1);
        CHECK_EQ(run("info", {{"project", g_path}})["timeline"]["tracks"][a1Track]["clips"][0]["keyframes"]["volume"][0]["value"].toDouble(), -6.0);
        // Keyframed opacity: transform sets a keyframe at `at` instead of the static value
        edit(QString(R"([{"op":"transform","clips":[%1],"opacity":50,"at":20},
                         {"op":"keyframe","clips":[%1],"param":"opacity","at":5,"remove":true},
                         {"op":"color","clips":[%1],"reset":["gainY"]}])").arg(v1).toUtf8().constData());
        const QJsonObject c2 = firstClip();
        CHECK_EQ(c2["keyframes"]["opacity"].toArray().size(), 2);
        CHECK_EQ(c2["keyframes"]["opacity"][0]["at"].toInt(), 20);
        CHECK_EQ(c2["effects"][0]["params"]["gainY"].toDouble(), 1.0);
        // Move a keyframe (value and ease stay); Inspector section switches (values stay, `info` shows them off)
        edit(QString(R"([{"op":"keyframe","clips":[%1],"param":"opacity","at":20,"move_to":25},
                         {"op":"transform","clips":[%1],"transform_on":false,"opacity_on":false}])").arg(v1).toUtf8().constData());
        const QJsonObject c3 = firstClip();
        CHECK_EQ(c3["keyframes"]["opacity"][0]["at"].toInt(), 25);
        CHECK_EQ(c3["keyframes"]["opacity"][0]["value"].toDouble(), 50.0);
        CHECK_EQ(c3["transform"]["transform_on"].toBool(true), false);
        CHECK_EQ(c3["transform"]["opacity_on"].toBool(true), false);
        CHECK(!c3["transform"].toObject().contains("crop_on"));
        CHECK_EQ(c3["transform"]["zoom_x"].toDouble(), 1.5);
        edit(QString(R"([{"op":"transform","clips":[%1],"transform_on":true,"opacity_on":true}])").arg(v1).toUtf8().constData());
        CHECK(!firstClip()["transform"].toObject().contains("transform_on"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"opacity","at":3,"move_to":9}])").arg(v1)}}),
                 QString("NOT_FOUND"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"transform","clips":[%1],"crop_on":1}])").arg(v1)}}),
                 QString("BAD_ARGUMENT"));
        // Copy the grade (only) to the second clip, then remove the first one's grade
        edit(QString(R"([{"op":"copy_attributes","from":%1,"clips":[%2],"attributes":["color"]},
                         {"op":"color","clips":[%1],"reset":true}])").arg(v1).arg(v2).toUtf8().constData());
        tl = run("info", {{"project", g_path}})["timeline"].toObject();
        CHECK(!tl["tracks"][0]["clips"][0].toObject().contains("effects"));
        CHECK_EQ(tl["tracks"][0]["clips"][1]["effects"][0]["params"]["saturation"].toDouble(), 70.0);
        CHECK(!tl["tracks"][0]["clips"][1].toObject().contains("transform"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"blur.nope","at":1}])").arg(v1)}}),
                 QString("BAD_ARGUMENT"));

        // Windows: add (first free number), shape/fill/correction, keyframes, listed apart in `info`, remove
        const QJsonObject added = run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"window","clips":[%1],"add":"rectangle",
            "x":70,"width":20,"fill":"pixelate","amount":30,"saturation":0}])").arg(v1)}});
        CHECK(added["notes"].toArray().contains(QJsonValue("window 1 added")));
        edit(QString(R"([{"op":"window","clips":[%1],"add":"ellipse","invert":true,"exposure":-1},
                         {"op":"keyframe","clips":[%1],"param":"grade.w1.x","keys":[{"at":0,"value":20},{"at":20,"value":80}]}])")
                 .arg(v1).toUtf8().constData());
        const QJsonObject w = firstClip();
        const QJsonArray wins = w["effects"][0]["windows"].toArray();
        CHECK_EQ(wins.size(), 2);
        CHECK_EQ(wins[0]["window"].toInt(), 1);
        CHECK_EQ(wins[0]["shape"].toString(), QString("rectangle"));
        CHECK_EQ(wins[0]["fill"].toString(), QString("pixelate"));
        CHECK_EQ(wins[0]["amount"].toDouble(), 30.0);
        CHECK_EQ(wins[0]["saturation"].toDouble(), 0.0);
        CHECK_EQ(wins[0]["width"].toDouble(), 20.0);
        CHECK_EQ(wins[1]["shape"].toString(), QString("ellipse"));
        CHECK_EQ(wins[1]["invert"].toBool(), true);
        CHECK(!w["effects"][0]["params"].toObject().contains("w1.x"));
        CHECK_EQ(w["keyframes"]["grade.w1.x"][1]["value"].toDouble(), 80.0);
        // A keyframed value gets a keyframe at `at`
        edit(QString(R"([{"op":"window","clips":[%1],"window":1,"x":50,"at":10}])").arg(v1).toUtf8().constData());
        const QJsonObject wk = firstClip();
        CHECK_EQ(wk["keyframes"]["grade.w1.x"].toArray().size(), 3);
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"window","clips":[%1],"window":3,"x":1}])").arg(v1)}}),
                 QString("NOT_FOUND"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"window","clips":[%1],"window":1,"fill":"smear"}])").arg(v1)}}),
                 QString("BAD_ARGUMENT"));
        edit(QString(R"([{"op":"window","clips":[%1],"window":1,"remove":true}])").arg(v1).toUtf8().constData());
        const QJsonObject w2 = firstClip();
        CHECK_EQ(w2["effects"][0]["windows"].toArray().size(), 1);
        CHECK_EQ(w2["effects"][0]["windows"][0]["window"].toInt(), 2);
        CHECK(!w2["keyframes"].toObject().contains("grade.w1.x"));
        edit(QString(R"([{"op":"window","clips":[%1],"window":2,"remove":true}])").arg(v1).toUtf8().constData());
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"track_window","clips":[%1],"window":1}])").arg(v1)}}),
                 QString("NOT_FOUND"));
    }

    // Edit page: trim modes, transitions, title style, tracks, link, speed ramp, markers, media pool, copy, insert
    {
        const auto ids = [] {
            const QJsonArray clips = run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"].toArray();
            QVector<int> out;
            for (const QJsonValue& c : clips) out << c["id"].toInt();
            return out;
        };
        const auto v1 = [] { return saved().section("  ", 0, 0); };
        const auto ed = [](const QString& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); };
        ed(R"([{"op":"clear"},{"op":"append","media":"/x/a.mp4","in":0,"out":40},{"op":"append","media":"/x/a.mp4","in":50,"out":90}])");
        CHECK_EQ(v1(), QString("V1: a[0-40|0-39] a[40-80|50-89]"));
        QVector<int> c = ids();
        ed(QString(R"([{"op":"trim","clip":%1,"edge":"end","by":-5,"mode":"roll"}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[0-35|0-34] a[35-80|45-89]"));
        ed(QString(R"([{"op":"trim","clip":%1,"by":3,"mode":"slip"}])").arg(c[1]));
        CHECK_EQ(v1(), QString("V1: a[0-35|0-34] a[35-80|48-92]"));
        ed(QString(R"([{"op":"trim","clip":%1,"edge":"end","by":-10,"mode":"ripple"}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[0-25|0-24] a[25-70|48-92]"));
        ed(QString(R"([{"op":"trim","clip":%1,"edge":"end","to":10}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[0-10|0-9] a[25-70|48-92]"));
        ed(QString(R"([{"op":"move","clips":[%1],"to":10}])").arg(c[1]));
        // Transition: dip to color, 10 frames, at the cut; then remove
        QJsonObject r = ed(R"([{"op":"transition","at":11,"type":"dip_color","color":"#ffffff","length":10,"kind":"video"}])");
        QJsonObject left = std::as_const(r)["timeline"]["tracks"][0]["clips"][0].toObject();
        CHECK_EQ(std::as_const(left)["transition_out"].toInt(), 10);
        CHECK_EQ(std::as_const(left)["transition_out_style"]["type"].toString(), QString("dip_color"));
        CHECK_EQ(std::as_const(left)["transition_out_style"]["color"].toString(), QString("#ffffff"));
        CHECK(!std::as_const(r)["timeline"]["tracks"][2]["clips"][0].toObject().contains("transition_out")); // video only
        r = ed(R"([{"op":"transition","at":10,"remove":true}])");
        CHECK(!std::as_const(r)["timeline"]["tracks"][0]["clips"][0].toObject().contains("transition_out"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"transition","at":10,"type":"zoom"}])"}}), QString("BAD_ARGUMENT"));
        // Title with style, then change it
        r = ed(R"([{"op":"title","text":"Hello
World","at":0,"duration":20,"track":"V2","color":"#ff0000","bold":true,"box":true,"y":-300,"align":"left"}])");
        QJsonObject title = std::as_const(r)["timeline"]["tracks"][1]["clips"][0].toObject();
        CHECK_EQ(std::as_const(title)["style"]["color"].toString(), QString("#ff0000"));
        CHECK_EQ(std::as_const(title)["style"]["align"].toString(), QString("left"));
        CHECK_EQ(std::as_const(title)["end"].toInt(), 20);
        r = ed(QString(R"([{"op":"title","clip":%1,"text":"Bye","size":120}])").arg(std::as_const(title)["id"].toInt()));
        title = std::as_const(r)["timeline"]["tracks"][1]["clips"][0].toObject();
        CHECK_EQ(std::as_const(title)["text"].toString(), QString("Bye"));
        CHECK_EQ(std::as_const(title)["style"]["size"].toDouble(), 120.0);
        CHECK(std::as_const(title)["style"]["bold"].toBool());
        // Tracks: name, color, lock (locked tracks are left alone by later ops); subtitle track style
        r = ed(R"([{"op":"track","track":"V2","name":"Titles","color":"teal","lock":true},
                   {"op":"add_subtitle_track","name":"DE"},{"op":"track","track":"ST2","size":60,"box":false},
                   {"op":"mixer","track":"A1","pan":-50}])");
        CHECK_EQ(std::as_const(r)["timeline"]["tracks"][1]["name"].toString(), QString("Titles"));
        CHECK(std::as_const(r)["timeline"]["tracks"][1]["locked"].toBool());
        const QJsonArray subs = std::as_const(r)["timeline"]["subtitles"].toArray();
        CHECK_EQ(subs.last()["name"].toString(), QString("DE"));
        CHECK_EQ(subs.last()["style"]["size"].toDouble(), 60.0);
        CHECK(!subs.last()["style"]["box"].toBool());
        ed(QString(R"([{"op":"track","track":"V2","lock":false},{"op":"remove_track","track":"ST%1"}])").arg(subs.size()));
        CHECK_EQ(run("info", {{"project", g_path}})["timeline"]["subtitles"].toArray().size(), int(subs.size()) - 1);
        // Unlink: moving the video leaves the audio
        c = ids();
        ed(QString(R"([{"op":"unlink","clips":[%1]},{"op":"move","clips":[%1],"by":5}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[5-15|0-9] a[15-55|53-92]")); // overwrites the start of the next clip
        CHECK(saved().contains("A1: a[0-10|0-9]"));
        // Speed ramp: 2x from frame 30 on (the rest of the clip gets shorter)
        r = ed(QString(R"([{"op":"speed_ramp","clip":%1,"points":[{"at":30,"speed":2}]}])").arg(c[1]));
        const QJsonObject ramped = std::as_const(r)["timeline"]["tracks"][0]["clips"][1].toObject();
        CHECK_EQ(std::as_const(ramped)["speed_ramp"][0]["at"].toInt(), 30);
        CHECK_EQ(std::as_const(ramped)["end"].toInt(), 30 + 13); // 25 frames at 2x
        ed(QString(R"([{"op":"speed_ramp","clip":%1,"clear":true},{"op":"speed","clips":[%1],"freeze":true,"ripple":false}])").arg(c[1]));
        CHECK(run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"][1]["freeze"].toBool());
        // Markers and In/Out; Media Pool organisation
        r = ed(R"([{"op":"marker","at":1,"remove":true},{"op":"marker","at":7},{"op":"marks","in":2,"out":12},
                   {"op":"media","media":"/x/a.mp4","bin":"Interviews/Day 1","color":"orange","flags":["red"],"in":5}])");
        CHECK_EQ(std::as_const(r)["timeline"]["markers"].toArray(), QJsonArray{7});
        CHECK_EQ(std::as_const(r)["timeline"]["mark_out"].toInt(), 12);
        const QJsonObject media = run("info", {{"project", g_path}})["media"][0].toObject();
        CHECK_EQ(std::as_const(media)["bin"].toString(), QString("Interviews/Day 1"));
        // Bins: rename, move to the top level, remove (the media move up, nothing is deleted)
        ed(R"([{"op":"bin","bin":"interviews/day 1","rename":"Monday"},{"op":"bin","bin":"Archive"},
               {"op":"bin","bin":"Interviews/Monday","into":"Archive"}])");
        CHECK_EQ(run("info", {{"project", g_path}})["media"][0]["bin"].toString(), QString("Archive/Monday"));
        ed(R"([{"op":"bin","bin":"Archive/Monday","remove":true}])");
        CHECK_EQ(run("info", {{"project", g_path}})["media"][0]["bin"].toString(), QString("Archive"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"bin","bin":"Nope","remove":true}])"}}), QString("NOT_FOUND"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"bin","bin":"Archive","into":"Archive/Sub"}])"}}),
                 QString("BAD_ARGUMENT"));
        CHECK_EQ(std::as_const(media)["color"].toString(), QString("orange"));
        CHECK_EQ(std::as_const(media)["mark_in"].toInt(), 5);
        // Copy and insert (insert moves the later clips; source marks of the media stay)
        ed(R"([{"op":"clear"},{"op":"append","media":"/x/a.mp4","in":0,"out":10}])");
        c = ids();
        ed(QString(R"([{"op":"copy","clips":[%1],"at":20},{"op":"insert","media":"/x/a.mp4","at":5,"in":50,"out":53}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[0-5|0-4] a[5-8|50-52] a[8-13|5-9] a[23-33|0-9]"));
        CHECK_EQ(run("info", {{"project", g_path}})["media"][0]["mark_in"].toInt(), 5);
        CHECK_EQ(run("info", {{"project", g_path}})["timeline"]["mark_in"].toInt(), 2);
        ed(R"([{"op":"replace","media":"/x/a.mp4","at":6,"in":60},{"op":"fit_to_fill","media":"/x/a.mp4","at":40,"to":45,"in":0,"out":10}])");
        CHECK(v1().startsWith("V1: a[0-5|0-4] a[5-8|59-61] "));
        const QJsonObject fit = run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"].toArray().last().toObject();
        CHECK_EQ(fit["start"].toInt(), 40);
        CHECK_EQ(fit["end"].toInt(), 45);
        CHECK_EQ(fit["speed"].toDouble(), 2.0);
    }

    // Titles with a duration, slip with keyframes (AI test runs 2026-10-25)
    {
        const auto ed = [](const QString& ops) { return run("edit", {{"project", g_path}, {"ops", ops}}); };
        const auto track = [](int i) { return saved().section("  ", i, i); };
        ed(R"([{"op":"clear"},{"op":"place","media":"/x/a.mp4","at":40,"in":0,"out":30,"track":"V2"}])");
        CHECK_EQ(track(1), QString("V2: a[40-70|0-29]"));
        // Only [at, at + duration) changes: the title was placed with 5 s first, which overwrote the clip after it
        ed(R"([{"op":"title","text":"Frei.","at":25,"duration":15,"track":"V2"}])");
        CHECK_EQ(track(1), QString("V2: T[25-40|0-14] a[40-70|0-29]"));
        // Without a track: above the clips in its own range (none on V2 before frame 25), not in 5 s
        ed(R"([{"op":"title","text":"Low","at":0,"duration":20}])");
        CHECK_EQ(track(0), QString("V1: T[0-20|0-19]"));
        // A new duration keeps the start: longer overwrites only the added range, shorter frees the end
        const auto clipsOf = [](int i) { return run("info", {{"project", g_path}})["timeline"]["tracks"][i]["clips"].toArray(); };
        const int title = clipsOf(1)[0].toObject()["id"].toInt();
        ed(QString(R"([{"op":"title","clip":%1,"duration":20}])").arg(title));
        CHECK_EQ(track(1), QString("V2: T[25-45|0-19] a[45-70|5-29]"));
        ed(QString(R"([{"op":"title","clip":%1,"duration":5,"text":"F"}])").arg(title));
        CHECK_EQ(track(1), QString("V2: T[25-30|0-4] a[45-70|5-29]"));
        CHECK_EQ(clipsOf(1)[0].toObject()["text"].toString(), QString("F"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"title","at":0,"duration":0}])"}}), QString("BAD_ARGUMENT"));
        // Slip shows other source frames; the keyframes stay where they are in the clip (they moved with the content
        // before, some out of the clip)
        const int a = clipsOf(1)[1].toObject()["id"].toInt();
        ed(QString(R"([{"op":"keyframe","clips":[%1],"param":"opacity","keys":[{"at":45,"value":0},{"at":69,"value":100}]},
                      {"op":"trim","clip":%1,"mode":"slip","by":5}])").arg(a));
        const QJsonObject slipped = clipsOf(1)[1].toObject();
        CHECK_EQ(slipped["src_in"].toInt(), 10);
        const QJsonArray keys = slipped["keyframes"]["opacity"].toArray();
        if (CHECK_EQ(keys.size(), 2)) {
            CHECK_EQ(keys[0]["at"].toInt(), 45);
            CHECK_EQ(keys[1]["at"].toInt(), 69);
        }
        // The help says what slip does with keyframes and what crop values mean
        const QJsonObject help = run("help", {{"command", "edit"}, {"op", "trim,transform"}});
        const QString text = QJsonDocument(help).toJson();
        CHECK(text.contains("keyframes stay where they are in the clip"));
        CHECK(text.contains("windows (x, y, size, rotation) moves with the content"));
        CHECK(text.contains("timeline pixels on that fitted picture"));
    }

    // Project format (resolution scales positions, frame rate locked while clips exist), LUT by name, SRT export
    {
        const QJsonObject r = run("edit", {{"project", g_path}, {"ops", R"([{"op":"clear"},
            {"op":"append","media":"/x/a.mp4","in":0,"out":20},
            {"op":"subtitle","text":"Hi","at":0,"duration":10}])"}});
        const int id = std::as_const(r)["timeline"]["tracks"][0]["clips"][0]["id"].toInt();
        run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"transform","clips":[%1],"x":100},
            {"op":"color","clips":[%1],"lut":"teal-orange"},{"op":"format","width":960,"height":540}])").arg(id)}});
        const QJsonObject info = run("info", {{"project", g_path}});
        CHECK_EQ(info["format"]["width"].toInt(), 960);
        CHECK_EQ(info["timeline"]["tracks"][0]["clips"][0]["transform"]["x"].toDouble(), 50.0);
        CHECK(info["timeline"]["tracks"][0]["clips"][0]["effects"][0]["params"]["lut"].toString().endsWith("teal-orange.cube"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"format","fps":30}])"}}), QString("FAILED"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"color","clips":[%1],"lut":"nope"}])").arg(id)}}),
                 QString("NOT_FOUND"));
        const QString srt = dir.filePath("subs.srt");
        CHECK(run("render", {{"project", g_path}, {"out", srt}, {"format", "srt"}})["cues"].toInt() >= 1); // visible track
        QFile f(srt);
        CHECK(f.open(QIODevice::ReadOnly) && f.readAll().contains("Hi"));
        CHECK_EQ(errorOf("render", {{"project", g_path}, {"out", dir.filePath("x.mp4")}, {"preset", "Nope"}}), QString("NOT_FOUND"));
        // Frame rate of an empty project: media lengths follow
        const QString empty = dir.filePath("empty.schneidi");
        run("new", {{"project", empty}});
        run("edit", {{"project", empty}, {"ops", R"([{"op":"format","fps":50}])"}});
        CHECK_EQ(run("info", {{"project", empty}})["format"]["fps"].toDouble(), 50.0);
        // Replacing it: the old state goes into the backups (restore brings it back), not while someone saves
        CHECK_EQ(errorOf("new", {{"project", empty}}), QString("EXISTS"));
        {
            const auto held = ProjectFile::lock(empty, 0);
            CHECK_EQ(errorOf("new", {{"project", empty}, {"overwrite", true}}), QString("PROJECT_LOCKED"));
        }
        CHECK_EQ(run("info", {{"project", empty}})["format"]["fps"].toDouble(), 50.0);
        const QJsonObject replaced = run("new", {{"project", empty}, {"overwrite", true}});
        CHECK(!replaced["backup"].toString().isEmpty());
        CHECK_EQ(run("info", {{"project", empty}})["format"]["fps"].toDouble(), 25.0);
        run("restore", {{"project", empty}});
        CHECK_EQ(run("info", {{"project", empty}})["format"]["fps"].toDouble(), 50.0);
    }

    // Audio effect (noise reduction) on audio clips only; stabilization with a choice parameter; `effects` kinds
    {
        writeProject();
        const QJsonObject tl = run("info", {{"project", g_path}})["timeline"].toObject();
        const int video = tl["tracks"][0]["clips"][0]["id"].toInt();
        int audio = 0;
        for (const QJsonValue& t : tl["tracks"].toArray())
            if (t["track"].toString() == "A1" && !t["clips"].toArray().isEmpty()) audio = t["clips"][0]["id"].toInt();
        CHECK(audio > 0);
        CHECK_EQ(errorOf("edit", {{"project", g_path},
                                  {"ops", QString(R"([{"op":"effect","clips":[%1],"linked":false,"effect":"denoise"}])").arg(video)}}),
                 QString("BAD_ARGUMENT"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"effect","clips":[%1],"effect":"stabilize",
                 "params":{"border":"wobbly"}}])").arg(video)}}), QString("BAD_ARGUMENT"));
        edit(QString(R"([{"op":"effect","clips":[%1],"effect":"denoise","params":{"reduction":20,"track":false}},
                         {"op":"effect","clips":[%2],"effect":"stabilize","params":{"smoothing":30,"border":"black"}}])")
                 .arg(audio).arg(video).toUtf8().constData());
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(g_path, &d, &error));
        const Clip* a = TimelineOps::findClip(d.timeline, audio);
        const Clip* v = TimelineOps::findClip(d.timeline, video);
        CHECK(a && EffectRegistry::value(*a, "denoise", "reduction").toDouble() == 20.0
              && !EffectRegistry::value(*a, "denoise", "track").toBool());
        CHECK(v && EffectRegistry::value(*v, "stabilize", "border").toString() == "black"
              && EffectRegistry::value(*v, "stabilize", "smoothing").toDouble() == 30.0);
        const QJsonArray fx = run("effects", {})["effects"].toArray();
        auto kindOf = [&](const QString& id) {
            for (const QJsonValue& e : fx)
                if (e["effect"].toString() == id) return e["kind"].toString();
            return QString();
        };
        CHECK_EQ(kindOf("denoise"), QString("audio"));
        CHECK_EQ(kindOf("stabilize"), QString("video"));
        const QJsonArray params = run("effects", {{"effect", "stabilize"}})["effects"][0]["params"].toArray();
        bool analysis = false;
        for (const QJsonValue& p : params)
            if (p["key"].toString() == "shakiness") analysis = p["analysis"].toBool();
        CHECK(analysis);
    }

    // --- With real media (ffmpeg): loudness of a file and of the mix, normalize, mixer
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
    // Several audio streams (game recording: game, Discord …): ops for audio clips change only the given stream's
    // clip, the video clip stands for all of them, linked:true takes all partners too
    {
        const QString multi = Check::makeMedia(dir.filePath("streams.mkv"),
                                               {"-f", "lavfi", "-i", "color=c=gray:s=320x240:r=25:d=2", "-f", "lavfi", "-i",
                                                "sine=f=440:d=2", "-f", "lavfi", "-i", "sine=f=660:d=2", "-map", "0",
                                                "-map", "1", "-map", "2", "-c:v", "libx264", "-c:a", "aac"});
        if (CHECK(!multi.isEmpty())) {
            const QString proj = dir.filePath("streams.schneidi");
            run("new", {{"project", proj}});
            const QJsonObject placed = run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(multi)}});
            CHECK(!placed.contains("notes")); // two streams, no Mix among them
            QHash<QString, int> ids; // track -> its clip
            for (const QJsonValue& t : run("info", {{"project", proj}})["timeline"]["tracks"].toArray())
                if (!t["clips"].toArray().isEmpty()) ids[t["track"].toString()] = t["clips"][0]["id"].toInt();
            if (CHECK(ids.contains("V1") && ids.contains("A1") && ids.contains("A2"))) {
                run("edit", {{"project", proj},
                             {"ops", QString(R"([{"op":"volume","clips":[%1],"db":6},
                                                 {"op":"effect","clips":[%1],"effect":"denoise"},
                                                 {"op":"effect","clips":[%2],"effect":"stabilize"}])")
                                         .arg(ids["A2"]).arg(ids["V1"])}});
                ProjectData d;
                QString error;
                CHECK(ProjectFile::load(proj, &d, &error));
                const Clip* a1 = TimelineOps::findClip(d.timeline, ids["A1"]);
                const Clip* a2 = TimelineOps::findClip(d.timeline, ids["A2"]);
                CHECK(a1 && a1->volumeDb == 0.0 && !EffectRegistry::has(*a1, "denoise"));
                CHECK(a2 && a2->volumeDb == 6.0 && EffectRegistry::has(*a2, "denoise"));
                run("edit", {{"project", proj},
                             {"ops", QString(R"([{"op":"effect","clips":[%1],"effect":"denoise","remove":true},
                                                 {"op":"volume","clips":[%2],"db":-3,"linked":true}])")
                                         .arg(ids["V1"]).arg(ids["A2"])}});
                CHECK(ProjectFile::load(proj, &d, &error));
                a1 = TimelineOps::findClip(d.timeline, ids["A1"]);
                a2 = TimelineOps::findClip(d.timeline, ids["A2"]);
                CHECK(a1 && a1->volumeDb == -3.0 && a2 && a2->volumeDb == -3.0);
                CHECK(a2 && !EffectRegistry::has(*a2, "denoise"));
            }
        }
    }
    // A recording with a Mix stream next to its single streams (OBS, clip tools): placed with every stream playing,
    // the answer says that everything is heard twice and which clips turn the Mix off; the game template mutes it
    // (an AI cut a 9:16 short from such a clip in the vertical template, measured -14 LUFS and never noticed)
    {
        const QString mixed = Check::makeMedia(
            dir.filePath("mixed.mkv"),
            {"-f", "lavfi", "-i", "color=c=gray:s=320x240:r=25:d=2", "-f", "lavfi", "-i", "sine=f=440:d=2", "-f", "lavfi",
             "-i", "sine=f=660:d=2", "-f", "lavfi", "-i", "sine=f=880:d=2", "-map", "0", "-map", "1", "-map", "2", "-map", "3",
             "-c:v", "libx264", "-c:a", "aac", "-metadata:s:a:0", "title=Mix", "-metadata:s:a:1", "title=Game",
             "-metadata:s:a:2", "title=Mic"});
        if (CHECK(!mixed.isEmpty())) {
            const QString proj = dir.filePath("mixed.schneidi");
            run("new", {{"project", proj}, {"template", "vertical"}});
            const QJsonObject r = run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"keep","media":"%1","ranges":[[0,10],[20,30]]}])").arg(mixed)}});
            const QString note = r["notes"].toArray().at(0).toString();
            int mixA = 0, mixB = 0;
            for (const QJsonValue& c : r["changes"]["added"].toArray())
                if (c["track"].toString() == "A1") (mixA ? mixB : mixA) = c["id"].toInt();
            if (!CHECK(note.contains("mixed.mkv: its Mix stream plays together with its single streams (Game, Mic)")
                       && note.contains(QString("{op:'disable', clips:[%1,%2]}").arg(std::min(mixA, mixB)).arg(std::max(mixA, mixB)))))
                std::printf("       note: %s\n", qPrintable(note));
            const QString gameProj = dir.filePath("mixed-game.schneidi");
            run("new", {{"project", gameProj}, {"template", "game"}});
            CHECK(!run("edit", {{"project", gameProj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(mixed)}}).contains("notes"));
        }
    }
    {
        const QString noise = Check::makeMedia(dir.filePath("noise.wav"),
                                               {"-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.2:duration=6:seed=3:sample_rate=48000",
                                                "-ac", "2", "-c:a", "pcm_s16le"});
        const QString proj = dir.filePath("audio.schneidi");
        run("new", {{"project", proj}});
        const QJsonObject file = run("loudness", {{"source", noise}})["loudness"].toObject();
        const double fileLufs = file["integrated_lufs"].toDouble();
        CHECK(fileLufs < -10 && fileLufs > -40);
        CHECK(file["true_peak_db"].toDouble() >= file["sample_peak_db"].toDouble());
        run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(noise)}});
        const auto mix = [&] { return run("loudness", {{"source", proj}})["loudness"]["integrated_lufs"].toDouble(); };
        CHECK(std::abs(mix() - fileLufs) < 0.3);
        const int clip = run("info", {{"project", proj}})["timeline"]["tracks"][2]["clips"][0]["id"].toInt();
        run("edit", {{"project", proj}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-20}])").arg(clip)}});
        CHECK(std::abs(mix() + 20) < 0.3);
        run("edit", {{"project", proj}, {"ops", R"([{"op":"mixer","track":"master","volume_db":-6}])"}});
        CHECK(std::abs(mix() + 26) < 0.3);
        run("edit", {{"project", proj}, {"ops", R"([{"op":"mixer","track":"A1","mute":true}])"}});
        CHECK(mix() < -100);
        // info shows what the header/mixer ops set (also in the summary), the master always
        {
            const QString sp = dir.filePath("state.schneidi");
            run("new", {{"project", sp}});
            run("edit", {{"project", sp}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(noise)}});
            QJsonObject master = run("info", {{"project", sp}, {"summary", true}})["timeline"]["master"].toObject();
            CHECK_EQ(master["volume_db"].toDouble(), 0.0);
            CHECK(master.contains("limiter") && !master["limiter"].toBool());
            CHECK_EQ(master["limiter_db"].toDouble(), -1.0);
            run("edit", {{"project", sp}, {"ops", R"([{"op":"mixer","track":"A1","mute":true},
                {"op":"mixer","track":"A2","solo":true},{"op":"track","track":"V2","hidden":true,"lock":true},
                {"op":"mixer","track":"master","volume_db":-3,"limiter":true,"limiter_db":-2}])"}});
            for (const bool summary : {true, false}) {
                const QJsonObject tl = run("info", {{"project", sp}, {"summary", summary}})["timeline"].toObject();
                const QJsonArray tracks = tl["tracks"].toArray(); // V1, V2, A1, A2
                CHECK(!tracks[0].toObject().contains("hidden") && !tracks[0].toObject().contains("muted")); // defaults
                CHECK(tracks[1]["hidden"].toBool() && tracks[1]["locked"].toBool());
                CHECK(tracks[2]["muted"].toBool() && !tracks[2].toObject().contains("solo"));
                CHECK(tracks[3]["solo"].toBool() && !tracks[3].toObject().contains("muted"));
                master = tl["master"].toObject();
                CHECK_EQ(master["volume_db"].toDouble(), -3.0);
                CHECK(master["limiter"].toBool());
                CHECK_EQ(master["limiter_db"].toDouble(), -2.0);
            }
            run("edit", {{"project", sp}, {"ops", R"([{"op":"mixer","track":"master","limiter":false}])"}});
            master = run("info", {{"project", sp}, {"summary", true}})["timeline"]["master"].toObject();
            CHECK(master.contains("limiter") && !master["limiter"].toBool());
            CHECK_EQ(master["limiter_db"].toDouble(), -2.0); // the ceiling stays for the next time
        }
        const QJsonObject perClip = run("loudness", {{"source", proj}, {"clips", QJsonArray{clip}}});
        CHECK(std::abs(perClip["clips"][0]["loudness"]["integrated_lufs"].toDouble() - fileLufs) < 0.3);
        // Loudness up to the top: the answer warns about the true peak
        {
            const QString hot = Check::makeMedia(dir.filePath("hot.wav"),
                                                 {"-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.9:duration=3:seed=3:sample_rate=48000",
                                                  "-ac", "2", "-c:a", "pcm_s16le"});
            const QString hp = dir.filePath("hot.schneidi");
            run("new", {{"project", hp}});
            run("edit", {{"project", hp}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(hot)}});
            const int id = run("info", {{"project", hp}})["timeline"]["tracks"][2]["clips"][0]["id"].toInt();
            const QJsonObject loud = run("edit", {{"project", hp}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":-6}])").arg(id)}});
            CHECK(QJsonDocument(loud["notes"].toArray()).toJson().contains("dBTP"));
        }
        // Normalize beyond the clip's +12 dB: allow_track_gain puts the rest on the track, the other clip stays
        {
            const QString faint = Check::makeMedia(dir.filePath("faint.wav"),
                                                   {"-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.004:duration=3:seed=3:sample_rate=48000",
                                                    "-ac", "2", "-c:a", "pcm_s16le"});
            const QString tp = dir.filePath("track.schneidi");
            run("new", {{"project", tp}});
            run("edit", {{"project", tp}, {"ops", QString(R"([{"op":"append","media":"%1"},{"op":"append","media":"%2"}])").arg(faint, noise)}});
            const QJsonArray clips = run("info", {{"project", tp}})["timeline"]["tracks"][2]["clips"].toArray();
            const int quiet = clips[0]["id"].toInt(), other = clips[1]["id"].toInt();
            const auto part = [&](const char* from, const char* to) {
                return run("loudness", {{"source", tp}, {"from", from}, {"to", to}})["loudness"]["integrated_lufs"].toDouble();
            };
            const double otherBefore = part("3s", "6s");
            const double faintLufs = run("loudness", {{"source", tp}, {"clips", QJsonArray{quiet}}})["clips"][0]["loudness"]["integrated_lufs"].toDouble();
            const double target = faintLufs + 18; // 6 dB more than the clip volume can give
            // relative (several voice clips evened out together): the same note, not silently 6 dB short (an AI
            // normalized a whispering interview to -16 and only the master fader showed it)
            const QJsonObject rel = run("edit", {{"project", tp}, {"dry_run", true}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":%2,"relative":true}])").arg(quiet).arg(target)}});
            const QString relNote = rel["notes"].toArray().at(0).toString();
            if (!CHECK(relNote.contains("clip volume ends at +12 dB: they stay 6.0 dB under the target")
                       || relNote.contains("they stay 5.9 dB") || relNote.contains("they stay 6.1 dB")))
                std::printf("       note: %s\n", qPrintable(relNote));
            const QJsonObject r = run("edit", {{"project", tp}, {"ops", QString(R"([{"op":"normalize","clips":[%1],"mode":"lufs","target":%2,"allow_track_gain":true}])").arg(quiet).arg(target)}});
            CHECK(QJsonDocument(r["notes"].toArray()).toJson().contains("allow_track_gain"));
            const QJsonObject track = run("info", {{"project", tp}})["timeline"]["tracks"][2].toObject();
            CHECK(std::abs(track["volume_db"].toDouble() - 6) < 0.2);
            if (!CHECK(std::abs(part("0s", "3s") - target) < 0.3)) std::printf("       quiet %.2f, want %.2f\n", part("0s", "3s"), target);
            CHECK(std::abs(part("3s", "6s") - otherBefore) < 0.2);
            CHECK_EQ(track["clips"][1]["id"].toInt(), other);
        }
        // Silence of the timeline sound: timeline frames, tracks by number or by audio stream name
        {
            const QString gap = Check::makeMedia(dir.filePath("gap.mka"),
                                                 {"-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.3:duration=6:seed=3:sample_rate=48000",
                                                  "-af", "volume=enable='between(t,2,4)':volume=0", "-ac", "2", "-c:a", "flac",
                                                  "-metadata:s:a:0", "title=Mikro"});
            const QString gp = dir.filePath("gap.schneidi");
            run("new", {{"project", gp}});
            run("edit", {{"project", gp}, {"ops", QString(R"([{"op":"append","media":"%1"},{"op":"append","media":"%1"}])").arg(gap)}});
            const int fps = int(std::lround(run("info", {{"project", gp}})["format"]["fps"].toDouble()));
            const QJsonObject si = run("silence", {{"file", gp}, {"tracks", "mikro"}, {"from", "6s"}});
            const QJsonArray pauses = si["silence"].toArray();
            CHECK_EQ(pauses.size(), 1); // only the one in the second clip (from 6s on)
            CHECK(std::abs(pauses[0]["from"].toInt() - 8 * fps) <= fps / 5 + 1);
            CHECK(std::abs(pauses[0]["to"].toInt() - 10 * fps) <= fps / 5 + 1);
            CHECK_EQ(run("silence", {{"file", gp}, {"tracks", "A1"}})["silence"].toArray().size(), 2);
            CHECK_EQ(errorOf("silence", {{"file", gp}, {"tracks", "Discord"}}), QString("BAD_ARGUMENT"));
        }
        // Render with a preset (audio only) and a range
        run("edit", {{"project", proj}, {"ops", R"([{"op":"mixer","track":"A1","mute":false}])"}});
        QString wavPreset; // name in the test's UI language
        for (const RenderPreset& p : RenderPresets::builtins())
            if (p.settings.format == "wav") wavPreset = p.name;
        const QJsonObject rendered = run("render", {{"project", proj}, {"out", dir.filePath("out")}, {"preset", wavPreset.toUpper()},
                                                    {"from", 0}, {"to", "2s"}});
        CHECK(rendered["out"].toString().endsWith(".wav"));
        CHECK_EQ(rendered["preset"].toString(), wavPreset);
        CHECK(QFileInfo(rendered["out"].toString()).size() > 100000);
        // Video encoder: the answer says which one wrote the file
        CHECK_EQ(errorOf("render", {{"project", proj}, {"out", dir.filePath("x.mp4")}, {"encoder", "gpu"}}),
                 QString("BAD_ARGUMENT"));
        const QJsonObject sw = run("render", {{"project", proj}, {"out", dir.filePath("sw.mp4")}, {"encoder", "software"},
                                              {"height", 180}, {"to", "1s"}});
        CHECK_EQ(sw["encoder"].toString(), QString("libx264"));
        CHECK(!sw.contains("encoder_note"));
        const QJsonObject autoEnc = run("render", {{"project", proj}, {"out", dir.filePath("auto.mp4")}, {"height", 180},
                                                   {"to", "1s"}});
        CHECK(autoEnc["encoder"].toString().startsWith("h264_") || autoEnc["encoder"].toString() == "libx264");
        // Scopes of a mid-grey frame: luma in the middle, nothing clipped, no cast
        const QString grey = Check::makeMedia(dir.filePath("grey.mp4"),
                                              {"-f", "lavfi", "-i", "color=c=0x808080:s=320x180:d=1:r=25", "-c:v", "libx264",
                                               "-pix_fmt", "yuv420p"});
        const QJsonObject sc = run("scopes", {{"source", grey}, {"out", dir.filePath("scopes.png")}, {"types", "waveform,histogram"}});
        const QJsonObject st = sc["stats"].toObject();
        CHECK(std::abs(st["luma_mean"].toInt() - 512) < 25);
        CHECK_EQ(st["white_clipped_pct"].toDouble(), 0.0);
        CHECK(std::abs(st["cast"]["red"].toDouble()) < 2);
        CHECK(QImage(dir.filePath("scopes.png")).width() == 960);
        CHECK_EQ(errorOf("scopes", {{"source", grey}, {"types", "rainbow"}}), QString("BAD_ARGUMENT"));
        // Import into an empty project of another frame rate: a note how to match it; the same rate: nothing to say
        const QString fifty = Check::makeMedia(dir.filePath("fifty.mp4"),
                                               {"-f", "lavfi", "-i", "color=c=gray:s=320x180:d=1:r=50", "-c:v", "libx264",
                                                "-pix_fmt", "yuv420p"});
        const QString rateProject = dir.filePath("rate.schneidi");
        run("new", {{"project", rateProject}});
        const QJsonObject other = run("import", {{"project", rateProject}, {"files", QJsonArray{fifty}}});
        CHECK(other["notes"].toArray().at(0).toString().contains("`format` with fps 50"));
        CHECK(!run("import", {{"project", rateProject}, {"files", QJsonArray{grey}}}).contains("notes"));
    }
    // 0.7.1 additions: help for single ops, info summary, relative times, frames crop/sources, probe of several
    // files, peaks, region events, ducking, linked fades, mixer target_lufs
    {
        const QJsonObject h = run("help", {{"command", "edit"}, {"op", "speed_ramp,marker"}});
        CHECK_EQ(h["commands"][0]["ops"].toArray().size(), 2);
        CHECK_EQ(errorOf("help", {{"command", "edit"}, {"op", "nope"}}), QString("NOT_FOUND"));

        // Tone bursts at 2-3 s and 6-7 s over a picture with a white box appearing top right at 4 s
        const QString clip = Check::makeMedia(
            dir.filePath("bursts.mp4"),
            {"-f", "lavfi", "-i", "color=c=0x303030:s=320x180:d=10:r=25,drawbox=x=240:y=10:w=60:h=30:c=white:t=fill:enable='gte(t,4)'",
             "-f", "lavfi", "-i", "aevalsrc='if(between(t,2,3)+between(t,6,7),0.5*sin(2*PI*440*t),0)':d=10:s=48000",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QString music = Check::makeMedia(dir.filePath("music.wav"), {"-f", "lavfi", "-i", "sine=frequency=300:duration=10"});
        const QString proj = dir.filePath("new.schneidi");
        run("new", {{"project", proj}, {"fps", 25}, {"width", 320}, {"height", 180}, {"overwrite", true}});
        const auto ed = [&](const QString& ops) { return run("edit", {{"project", proj}, {"ops", ops}}); };
        ed(QString("[{\"op\":\"append\",\"media\":\"%1\",\"in\":\"1s\"},{\"op\":\"append\",\"media\":\"%1\"},"
                   "{\"op\":\"place\",\"media\":\"%2\",\"at\":0,\"track\":\"A2\"}]").arg(clip, music));
        const QJsonObject sum = run("info", {{"project", proj}, {"summary", true}});
        CHECK_EQ(sum["timeline"]["clips"].toInt(), 5);
        CHECK(sum["timeline"]["tracks"][0]["clips"].isDouble());
        CHECK(sum["media"].isDouble());

        // Relative times: source 3 s of the first clip (in = 1 s) sits at 2 s; a speed ramp before it does not
        // shift a marker given by source time
        QJsonObject r = ed("[{\"op\":\"marker\",\"at\":{\"clip\":1,\"source\":\"3s\"}},{\"op\":\"marker\",\"at\":{\"clip\":3,\"offset\":10}},"
                           "{\"op\":\"marker\",\"at\":{\"clip\":3,\"from_end\":5}}]");
        QJsonArray markers = std::as_const(r)["timeline"]["markers"].toArray();
        CHECK_EQ(markers.size(), 3);
        CHECK_EQ(markers.at(0).toInt(), 50);
        CHECK_EQ(markers.at(1).toInt(), 235);
        CHECK_EQ(markers.at(2).toInt(), 470);
        r = ed(QString("[{\"op\":\"speed_ramp\",\"clip\":1,\"points\":[{\"at\":{\"clip\":1,\"source\":\"2s\"},\"speed\":0.5}]},"
                       "{\"op\":\"marker\",\"at\":{\"media\":\"%1\",\"source\":\"8s\"}}]").arg(clip));
        markers = std::as_const(r)["timeline"]["markers"].toArray();
        // The first clip that plays source 8 s: clip 1 (in 1 s, half speed from source 2 s): 25 + 150 * 2 = 325
        CHECK(markers.contains(QJsonValue(325)));

        CHECK_EQ(errorOf("edit", {{"project", proj}, {"ops", "[{\"op\":\"marker\",\"at\":{\"clip\":1}}]"}}), QString("none"));
        CHECK_EQ(errorOf("edit", {{"project", proj}, {"ops", "[{\"op\":\"marker\",\"at\":{\"clip\":3,\"source\":\"20s\"}}]"}}),
                 QString("BAD_ARGUMENT"));

        // frames: crop of the box region and two sources on one sheet
        Cli::Context ctx;
        ctx.mcp = true;
        const Cli::Command* frames = Cli::find("frames");
        const QJsonObject fr = frames->run(Cli::normalize(*frames, {{"source", clip}, {"at", QJsonArray{"5s"}}, {"crop", "240,10,60,30"}}), ctx);
        CHECK_EQ(fr["frames"].toArray().size(), 1);
        if (CHECK(ctx.images.size() == 1)) {
            CHECK_EQ(ctx.images[0].width(), 60);
            CHECK(qGray(ctx.images[0].pixel(30, 15)) > 200); // the white box
        }
        Cli::Context ctx2;
        ctx2.mcp = true;
        const QJsonObject two = frames->run(
            Cli::normalize(*frames, {{"sources", QJsonArray{clip, clip}}, {"count", 2}, {"sheet", true}, {"crop", "0.5,0,0.5,0.5"}}), ctx2);
        CHECK_EQ(two["sources"].toArray().size(), 2);
        CHECK_EQ(ctx2.images.size(), 1);
        CHECK_EQ(errorOf("frames", {{"source", clip}, {"at", QJsonArray{0}}, {"crop", "1,2,3"}, {"out", dir.path()}}),
                 QString("BAD_ARGUMENT"));
        // Contact sheet (0.10): fixed columns, tile width, a source with its own region and times
        Cli::Context ctx3;
        ctx3.mcp = true;
        const QJsonObject own = frames->run(
            Cli::normalize(*frames, {{"sources", QJsonArray{clip, QJsonObject{{"file", clip}, {"crop", "240,10,60,30"},
                                                                             {"at", QJsonArray{"5s", "6s", "7s"}}}}},
                                     {"count", 2}, {"sheet", true}, {"columns", 5}, {"tile_width", 200}}),
            ctx3);
        CHECK_EQ(own["sources"][0]["frames"].toArray().size(), 2);
        CHECK_EQ(own["sources"][1]["frames"].toArray().size(), 3); // own times replace 'count'
        CHECK_EQ(own["sheet_size"][0].toInt(), 5 * 200);           // 5 tiles of 200 px in one row
        if (CHECK(ctx3.images.size() == 1)) {
            const QImage& s = ctx3.images[0];
            // tile 4 = the cropped white box of the second source, scaled to the tile width (fitted into the cell)
            CHECK(qGray(s.pixel(3 * 200 + 100, s.height() / 2)) > 200);
        }
        CHECK_EQ(errorOf("frames", {{"sources", QJsonArray{QJsonObject{{"file", clip}, {"width", 10}}}}, {"at", QJsonArray{0}}}),
                 QString("BAD_ARGUMENT"));
        // A sheet named .png is a PNG (it was JPEG data under that name), any other name stays JPEG
        const auto magic = [](const QString& path) {
            QFile f(path);
            return f.open(QIODevice::ReadOnly) ? f.read(4) : QByteArray();
        };
        run("frames", {{"source", clip}, {"count", 2}, {"sheet", true}, {"out", dir.filePath("sheet.png")}});
        CHECK_EQ(magic(dir.filePath("sheet.png")), QByteArray("\x89PNG"));
        run("frames", {{"source", clip}, {"count", 2}, {"sheet", true}, {"out", dir.filePath("sheet.jpg")}});
        CHECK(magic(dir.filePath("sheet.jpg")).startsWith("\xff\xd8"));
        // Folders on the way are made; a name without extension is a folder (a model wrote "C:/tmp/super_clip_check")
        run("frames", {{"source", clip}, {"count", 2}, {"sheet", true}, {"out", dir.filePath("new/check")}});
        CHECK(magic(dir.filePath("new/check/sheet.jpg")).startsWith("\xff\xd8"));
        run("frames", {{"source", clip}, {"count", 2}, {"sheet", true}, {"out", dir.filePath("more/s.png")}});
        CHECK_EQ(magic(dir.filePath("more/s.png")), QByteArray("\x89PNG"));

        const QJsonObject pr = run("probe", {{"files", QJsonArray{clip, dir.filePath("missing.mp4")}}});
        CHECK_EQ(pr["files"].toArray().size(), 2);
        CHECK(pr["files"][1].toObject().contains("error"));

        // peaks: the two bursts; region events: the box at 4 s
        const QJsonArray peaks = run("peaks", {{"file", clip}, {"threshold_db", -20}, {"min", "2s"}})["peaks"].toArray();
        if (CHECK_EQ(peaks.size(), 2)) {
            CHECK(std::abs(peaks[0]["s"].toDouble() - 2.5) < 0.6);
            CHECK(std::abs(peaks[1]["s"].toDouble() - 6.5) < 0.6);
        }
        const QJsonArray changes = run("scenes", {{"file", clip}, {"region", "0.7,0,0.3,0.3"}})["changes"].toArray();
        if (CHECK_EQ(changes.size(), 1)) CHECK(std::abs(changes[0]["s"].toDouble() - 4.0) < 0.3);
        // Looked at with a 9:16 project (an AI cutting a short passes it for the frame rate): the recording keeps its
        // own picture - a crop, a region and the scopes are parts of it, not of a letterboxed 9:16 frame (the crop of
        // the kill feed came out black)
        const QString tall = dir.filePath("tall.schneidi");
        run("new", {{"project", tall}, {"fps", 50}, {"width", 1080}, {"height", 1920}});
        Cli::Context ctxTall;
        ctxTall.mcp = true;
        const QJsonObject inTall = frames->run(
            Cli::normalize(*frames, {{"source", clip}, {"project", tall}, {"at", QJsonArray{"5s"}}, {"crop", "240,10,60,30"}}),
            ctxTall);
        CHECK_EQ(inTall["frames"][0]["frame"].toInt(), 250); // frames count in the project's rate
        if (CHECK(ctxTall.images.size() == 1)) {
            CHECK_EQ(ctxTall.images[0].width(), 60);
            CHECK(qGray(ctxTall.images[0].pixel(30, 15)) > 200); // the white box
        }
        Cli::Context ctxWide;
        ctxWide.mcp = true;
        frames->run(Cli::normalize(*frames, {{"source", clip}, {"project", tall}, {"at", QJsonArray{"5s"}}}), ctxWide);
        if (CHECK(ctxWide.images.size() == 1)) CHECK(ctxWide.images[0].width() > ctxWide.images[0].height());
        const QJsonArray tallChanges =
            run("scenes", {{"file", clip}, {"region", "240,10,60,30"}, {"project", tall}})["changes"].toArray();
        if (CHECK_EQ(tallChanges.size(), 1)) CHECK(std::abs(tallChanges[0]["s"].toDouble() - 4.0) < 0.3);
        const QJsonObject tallScopes =
            run("scopes", {{"source", clip}, {"project", tall}, {"at", "2s"}, {"out", dir.filePath("tall-scopes.png")}});
        CHECK_EQ(tallScopes["stats"]["black_clipped_pct"].toDouble(), 0.0); // no letterbox bars in the numbers
        // peaks group (0.10): each tone burst is one group
        const QJsonArray groups = run("peaks", {{"file", clip}, {"threshold_db", -20}, {"group", "1s"}})["groups"].toArray();
        if (CHECK_EQ(groups.size(), 2)) {
            CHECK(std::abs(groups[0]["from_s"].toDouble() - 2.0) < 0.2);
            CHECK(groups[0]["to_s"].toDouble() < 3.2);
            CHECK(groups[0]["hits"].toInt() >= 2);
        }
        // Colour events (0.10): a yellow mark that appears at 3 s and stays counts, a yellow flash at 6 s and a
        // white box do not
        const QString marks = Check::makeMedia(
            dir.filePath("marks.mp4"),
            {"-f", "lavfi", "-i",
             "color=c=0x303030:s=320x180:d=8:r=25,drawbox=x=250:y=20:w=30:h=12:c=0xe7e371:t=fill:enable='gte(t,3)',"
             "drawbox=x=200:y=50:w=60:h=30:c=0xe7e371:t=fill:enable='between(t,6,6.12)',"
             "drawbox=x=200:y=90:w=60:h=30:c=white:t=fill:enable='gte(t,5)'",
             "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        const QJsonObject colorEvents = run("scenes", {{"file", marks}, {"region", "0.5,0,0.5,0.8"}, {"color", "#e7e371"}});
        const QJsonArray appeared = colorEvents["changes"].toArray();
        if (CHECK_EQ(appeared.size(), 1)) {
            CHECK(std::abs(appeared[0]["s"].toDouble() - 3.0) < 0.15);
            CHECK_EQ(appeared[0]["kind"].toString(), QString("appear"));
        }
        CHECK(colorEvents["most_pct"].toDouble() > 0.5);
        CHECK_EQ(errorOf("scenes", {{"file", marks}, {"color", "#e7e371"}}), QString("BAD_ARGUMENT")); // no region
        // A semi-transparent kill feed line (CS2): an opaque inner outline and a see-through outer one that only
        // matches the colour on a dark background. The background turns light (3 s) and dark again (4.5 s): the same
        // line looking stronger is no new kill (it was counted again before); the second line at 6 s is
        const QString lineAt1 = "x=400:y=20:w=200:h=26:t=2:c=0xd01010:enable='gte(t,1)',"
                                "drawbox=x=398:y=18:w=204:h=30:t=2:c=0xd01010@0.5:enable='gte(t,1)'";
        const QString lineAt6 = "x=400:y=62:w=200:h=26:t=2:c=0xd01010:enable='gte(t,6)',"
                                "drawbox=x=398:y=60:w=204:h=30:t=2:c=0xd01010@0.5:enable='gte(t,6)'";
        const QString feed = Check::makeMedia(
            dir.filePath("feed.mkv"),
            {"-f", "lavfi", "-i",
             "color=c=0x202020:s=640x360:d=8:r=25,drawbox=x=0:y=0:w=iw:h=ih:c=0x8090b0:t=fill:enable='between(t,3,4.5)',"
             "drawbox=" + lineAt1 + ",drawbox=" + lineAt6,
             "-c:v", "ffv1", "-pix_fmt", "bgr0"});
        const QJsonObject feedArgs{{"file", feed}, {"region", "0.5,0,0.5,0.4"}, {"color", "#961414"}, {"tolerance", 60}};
        QJsonObject feedBoth = feedArgs;
        feedBoth["only"] = "both";
        const QJsonArray lines = run("scenes", feedBoth)["changes"].toArray();
        if (CHECK_EQ(lines.size(), 2)) {
            CHECK(std::abs(lines[0]["s"].toDouble() - 1.0) < 0.15);
            CHECK(std::abs(lines[1]["s"].toDouble() - 6.0) < 0.15);
            CHECK_EQ(lines[1]["kind"].toString(), QString("appear"));
        }
        int feedKills = 0;
        for (const QJsonValue& c : run("highlights", feedArgs)["candidates"].toArray()) feedKills += c["kills"].toInt();
        CHECK_EQ(feedKills, 2);
        // highlights (0.10): the mark as a kill with lead-in/out; without a kill feed the two tone bursts
        const QJsonObject hl = run("highlights", {{"file", marks}, {"region", "0.5,0,0.5,0.8"}, {"color", "#e7e371"}});
        if (CHECK_EQ(hl["candidates"].toArray().size(), 1)) {
            const QJsonObject c = hl["candidates"][0].toObject();
            CHECK_EQ(c["kills"].toInt(), 1);
            CHECK(std::abs(c["from_s"].toDouble() - 1.0) < 0.15); // 2 s before the kill at 3 s
            CHECK(std::abs(c["to_s"].toDouble() - 4.0) < 0.15);   // 1 s after
            CHECK_EQ(hl["keep"].toArray().size(), 1);
        }
        // A banner in the kill feed's colour (Valorant's yellow "BUY PHASE") is no kill: listed apart
        const QString banner = Check::makeMedia(
            dir.filePath("banner.mp4"),
            {"-f", "lavfi", "-i",
             "color=c=0x303030:s=320x180:d=6:r=25,drawbox=x=250:y=20:w=30:h=12:c=0xe7e371:t=fill:enable='gte(t,2)',"
             "drawbox=x=170:y=60:w=140:h=60:c=0xe7e371:t=fill:enable='gte(t,4)'",
             "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        const QJsonObject hb = run("highlights", {{"file", banner}, {"region", "0.5,0,0.5,0.8"}, {"color", "#e7e371"}});
        int bannerKills = 0;
        for (const QJsonValue& c : hb["candidates"].toArray()) bannerKills += c["kills"].toInt();
        CHECK_EQ(bannerKills, 1);
        if (CHECK_EQ(hb["banners"].toArray().size(), 1)) CHECK(std::abs(hb["banners"][0].toDouble() - 4.0) < 0.15);
        const QJsonObject hs = run("highlights", {{"files", QJsonArray{clip, clip}}, {"top", 3}});
        CHECK_EQ(hs["files"][0]["candidates"].toArray().size(), 2);
        CHECK_EQ(hs["best"].toArray().size(), 3);
        CHECK(hs["best"][0]["file"].isString());
        CHECK_EQ(errorOf("highlights", {{"file", marks}}), QString("NO_AUDIO")); // neither sound nor a kill feed
        // Timeline exchange (0.10): export as FCP7 XML, import into a new project = the same clips
        const QJsonObject ex = run("export_timeline", {{"project", proj}, {"out", dir.filePath("cut.xml")}});
        CHECK(QFileInfo::exists(dir.filePath("cut.xml")));
        CHECK_EQ(errorOf("export_timeline", {{"project", proj}, {"out", dir.filePath("cut.xml")}}), QString("EXISTS"));
        run("new", {{"project", dir.filePath("imp.schneidi")}, {"overwrite", true}});
        const QJsonObject im = run("import", {{"project", dir.filePath("imp.schneidi")}, {"timeline_file", dir.filePath("cut.xml")}});
        CHECK(ex["clips"].toInt() > 0);
        CHECK_EQ(im["timeline"]["clips"].toInt(), ex["clips"].toInt());
        CHECK_EQ(errorOf("import", {{"project", dir.filePath("imp.schneidi")}, {"timeline_file", clip}}), QString("BAD_ARGUMENT"));

        // Ducking of the music under A1, fades of video + linked sound
        r = ed("[{\"op\":\"duck\",\"track\":\"A2\",\"under\":\"A1\",\"amount_db\":-10},{\"op\":\"fade\",\"clips\":[3],\"out\":\"1s\"}]");
        const QJsonArray mkeys = std::as_const(r)["timeline"]["tracks"][3]["clips"][0]["keyframes"]["volume"].toArray();
        CHECK(mkeys.size() >= 4);
        bool low = false;
        for (const QJsonValue& k : mkeys) low = low || std::abs(k["value"].toDouble() + 10) < 0.01;
        CHECK(low);
        CHECK_EQ(std::as_const(r)["timeline"]["tracks"][0]["clips"][1]["fade_out"].toInt(), 25);
        CHECK_EQ(std::as_const(r)["timeline"]["tracks"][2]["clips"][1]["fade_out"].toInt(), 25);

        // Master to a loudness target (no limiter: one measurement, exact)
        r = ed("[{\"op\":\"mixer\",\"track\":\"A2\",\"mute\":true},{\"op\":\"mixer\",\"track\":\"master\",\"target_lufs\":-20}]");
        const QJsonObject l = run("loudness", {{"source", proj}})["loudness"].toObject();
        CHECK(std::abs(l["integrated_lufs"].toDouble() + 20) < 0.5);
    }
    return Check::result();
}
