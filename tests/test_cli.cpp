// schneidi-cli command layer without media decoding: argument checks, edit operations on a saved project,
// dry run, errors leave the file untouched
#include "cli/Commands.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"

#include <QLockFile>

#include "check.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include <cmath>

using Check::dump;

namespace {

QString g_path;

const QJsonObject run(const QString& name, const QJsonObject& raw)
{
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
        // Copy the grade (only) to the second clip, then remove the first one's grade
        edit(QString(R"([{"op":"copy_attributes","from":%1,"clips":[%2],"attributes":["color"]},
                         {"op":"color","clips":[%1],"reset":true}])").arg(v1).arg(v2).toUtf8().constData());
        tl = run("info", {{"project", g_path}})["timeline"].toObject();
        CHECK(!tl["tracks"][0]["clips"][0].toObject().contains("effects"));
        CHECK_EQ(tl["tracks"][0]["clips"][1]["effects"][0]["params"]["saturation"].toDouble(), 70.0);
        CHECK(!tl["tracks"][0]["clips"][1].toObject().contains("transform"));
        CHECK_EQ(errorOf("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"blur.nope","at":1}])").arg(v1)}}),
                 QString("BAD_ARGUMENT"));
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
        CHECK_EQ(std::as_const(media)["bin"].toString(), QString("Day 1"));
        CHECK_EQ(std::as_const(media)["color"].toString(), QString("orange"));
        CHECK_EQ(std::as_const(media)["mark_in"].toInt(), 5);
        // Copy and insert (insert moves the later clips; source marks of the media stay)
        ed(R"([{"op":"clear"},{"op":"append","media":"/x/a.mp4","in":0,"out":10}])");
        c = ids();
        ed(QString(R"([{"op":"copy","clips":[%1],"at":20},{"op":"insert","media":"/x/a.mp4","at":5,"in":50,"out":53}])").arg(c[0]));
        CHECK_EQ(v1(), QString("V1: a[0-5|0-4] a[5-8|50-52] a[8-13|5-9] a[23-33|0-9]"));
        CHECK_EQ(run("info", {{"project", g_path}})["media"][0]["mark_in"].toInt(), 5);
        CHECK_EQ(run("info", {{"project", g_path}})["timeline"]["mark_in"].toInt(), 2);
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
    }

    // --- With real media (ffmpeg): loudness of a file and of the mix, normalize, mixer
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initMlt();
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
        const QJsonObject perClip = run("loudness", {{"source", proj}, {"clips", QJsonArray{clip}}});
        CHECK(std::abs(perClip["clips"][0]["loudness"]["integrated_lufs"].toDouble() - fileLufs) < 0.3);
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
    }
    return Check::result();
}
