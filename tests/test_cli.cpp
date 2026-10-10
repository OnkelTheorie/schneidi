// schneidi-cli command layer without media decoding: argument checks, edit operations on a saved project,
// dry run, errors leave the file untouched
#include "cli/Commands.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

#include <QLockFile>

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using Check::dump;

namespace {

QString g_path;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code;
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

    // Arguments: types, unknown and missing parameters
    CHECK_EQ(errorOf("info", {}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("info", {{"project", g_path}, {"bogus", 1}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("info", {{"project", dir.filePath("missing.schneidi")}}), QString("NOT_FOUND"));
    const QJsonObject help = run("help", {});
    CHECK_EQ(help["commands"].toArray().size(), int(Cli::commands().size()));

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

    // A failing op saves nothing (also not the ops before it)
    CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", R"([{"op":"split","at":50},{"op":"trim","clip":999}])"}}),
             QString("NOT_FOUND"));
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-90|20-99]  V2:  A1: a[0-10|0-9] a[10-90|20-99]  A2:"));

    // Clear, then cut a source range list one after another (jump cut), title on top, delete with linked audio
    edit(R"([{"op":"clear"},
             {"op":"keep","media":"/x/a.mp4","ranges":[[0,10],[30,40],{"from":"2s","to":"3s"}]},
             {"op":"title","text":"Hi","at":5,"duration":10}])");
    CHECK_EQ(saved(), QString("V1: a[0-10|0-9] a[10-20|30-39] a[20-45|50-74]  V2: T[5-15|0-9]  "
                              "A1: a[0-10|0-9] a[10-20|30-39] a[20-45|50-74]  A2:"));
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
        CHECK_EQ(r["timeline"]["name"].toString(), QString("Short"));
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
    return Check::result();
}
