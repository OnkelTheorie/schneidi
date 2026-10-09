// schneidi-cli command layer without media decoding: argument checks, edit operations on a saved project,
// dry run, errors leave the file untouched
#include "cli/Commands.h"
#include "core/Project.h"
#include "core/ProjectFile.h"

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
    return Check::result();
}
