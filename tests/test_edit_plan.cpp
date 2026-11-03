// SPDX-License-Identifier: AGPL-3.0-only
// Plans (`edit` plan:true, then plan_id + run or discard; plan-review package of 0.17): a plan changes nothing (the
// file stays byte for byte, the app's project and undo history stay), answers every step with its why, a summary and
// what it would change; running some of its steps applies exactly those as one edit (one undo step in the app), with
// the same result live as on the file; a run is refused once the project changed (PLAN_STALE), a plan is gone once run
// or discarded and after a day, unknown ids are NOT_FOUND. In the app: the host hears about plans; in plan mode the
// app's own chat may only plan (PLAN_MODE), the user runs the plan through Server::runForChat.
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUndoStack>

#include <utility>

namespace {

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    bool planOnly = false;
    QStringList plans; // "made p1", "ran p1", "discarded p2" (+ " chat" from the own chat)
    QJsonObject lastPlan;
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString&, QString*) override { return false; }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    // (no `override`: the test builds against a host without plans and fails there instead of not building)
    void livePlan(const QString& state, const QJsonObject& plan, bool ownChat)
    {
        plans << state + " " + plan["id"].toString() + (ownChat ? " chat" : "");
        lastPlan = plan;
    }
    bool livePlanOnly() const { return planOnly; }
};

// One 300-frame media file (not on disk: edits only need its length) on V1/A1
void setup(Project& p)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 300, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.out = 199;
        v.linkId = p.newLinkId();
        Clip a = v;
        a.id = p.newClipId();
        tl.video[0].clips << v;
        tl.audio[0].clips << a;
    });
}

bool waitServing(QProcess& p, int timeoutMs = 120000)
{
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning) {
        if (t.hasExpired(timeoutMs)) {
            p.kill();
            p.waitForFinished(1000);
            return false;
        }
        p.waitForFinished(5);
        QCoreApplication::processEvents();
    }
    return true;
}

QJsonObject cli(const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(SCHNEIDI_CLI, args);
    p.closeWriteChannel();
    if (!waitServing(p)) std::printf("       timeout: schneidi-cli %s\n", qPrintable(args.join(' ')));
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (o.isEmpty()) std::printf("       no JSON: %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

QString code(const QJsonObject& o) { return o["ok"].toBool() ? QString("ok") : o["error"]["code"].toString(); }

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

Timeline fileTimeline(const QString& path)
{
    ProjectData d;
    ProjectFile::load(path, &d, nullptr);
    return d.timeline;
}

QString planFile(const QString& project, const QString& id)
{
    return QDir(ProjectFile::backupDir(project)).filePath("plans/" + id + ".json");
}

const QString kPlanOps = R"([{"op":"split","at":50,"why":"cut before the jump"},
                             {"op":"split","at":120,"why":"second cut"},
                             {"op":"marker","at":10,"why":"mark the hook"}])";

// ---- On a project file ----
void fileTests(const QString& path, const QProcessEnvironment& env)
{
    const QByteArray before = fileBytes(path);
    QJsonObject r = cli({"edit", path, "--plan", "--ops", kPlanOps}, env);
    if (!CHECK_EQ(code(r), QString("ok"))) return;
    CHECK(fileBytes(path) == before); // nothing changed, no backup either
    CHECK(!std::as_const(r)["saved"].toBool());
    CHECK(!r.contains("backup"));
    const QJsonObject plan = std::as_const(r)["plan"].toObject();
    const QString id = plan["id"].toString();
    CHECK_EQ(id, QString("p1"));
    CHECK(QFileInfo::exists(planFile(path, id)));
    const QJsonArray steps = plan["steps"].toArray();
    if (CHECK_EQ(steps.size(), 3)) {
        CHECK_EQ(steps[0]["n"].toInt(), 1);
        CHECK_EQ(steps[2]["n"].toInt(), 3);
        CHECK_EQ(steps[0]["why"].toString(), QString("cut before the jump"));
        CHECK_EQ(steps[1]["why"].toString(), QString("second cut"));
        CHECK_EQ(steps[2]["why"].toString(), QString("mark the hook"));
        CHECK_EQ(steps[0]["op"]["op"].toString(), QString("split"));
        CHECK_EQ(steps[0]["op"]["why"].toString(), QString("cut before the jump")); // the op as given
        // What each step changes on its own: the split adds the right halves (V1 + A1) and shortens the left ones
        CHECK_EQ(steps[0]["changes"]["added"].toArray().size(), 2);
        CHECK_EQ(steps[0]["changes"]["changed"].toArray().size(), 2);
        CHECK_EQ(steps[1]["changes"]["added"].toArray().size(), 2);
        CHECK(steps[2]["changes"].toObject().isEmpty()); // a marker: no clip
        CHECK(steps[0]["summary"].toString().startsWith("split: 2 added"));
    }
    CHECK(!plan["summary"].toString().isEmpty());
    CHECK(plan["next"].toString().contains("plan_id"));

    // Run steps 1 and 3: exactly those, saved like an edit
    r = cli({"edit", path, "--plan_id", id, "--run", "[1,3]"}, env);
    if (!CHECK_EQ(code(r), QString("ok"))) return;
    CHECK(std::as_const(r)["saved"].toBool());
    CHECK_EQ(std::as_const(r)["plan"]["ran"].toArray(), (QJsonArray{1, 3}));
    CHECK_EQ(std::as_const(r)["plan"]["skipped"].toArray(), (QJsonArray{2}));
    CHECK_EQ(std::as_const(r)["why"][0]["step"].toInt(), 1); // the reasons by the plan's step numbers
    CHECK_EQ(std::as_const(r)["why"][1]["step"].toInt(), 3);
    Timeline tl = fileTimeline(path);
    if (CHECK_EQ(tl.video[0].clips.size(), 2)) CHECK_EQ(tl.video[0].clips[1].start, 50);
    CHECK_EQ(tl.markers.size(), 1);
    CHECK(!QFileInfo::exists(planFile(path, id))); // run: gone
    r = cli({"edit", path, "--plan_id", id, "--run", "all"}, env);
    CHECK_EQ(code(r), QString("NOT_FOUND"));

    // A plan on a project that changed since: refused, nothing runs
    r = cli({"edit", path, "--plan", "--ops", R"([{"op":"split","at":150,"why":"x"}])"}, env);
    const QString stale = std::as_const(r)["plan"]["id"].toString();
    CHECK_EQ(stale, QString("p2")); // ids are never given twice
    CHECK_EQ(code(cli({"edit", path, "--ops", R"([{"op":"marker","at":20}])"}, env)), QString("ok"));
    const QByteArray changed = fileBytes(path);
    r = cli({"edit", path, "--plan_id", stale, "--run", "all"}, env);
    CHECK_EQ(code(r), QString("PLAN_STALE"));
    CHECK(std::as_const(r)["error"]["message"].toString().contains("plan again"));
    CHECK(fileBytes(path) == changed);

    // Discard: nothing changes, the plan is gone
    r = cli({"edit", path, "--plan", "--ops", R"([{"op":"split","at":160}])"}, env);
    const QString dropped = std::as_const(r)["plan"]["id"].toString();
    r = cli({"edit", path, "--plan_id", dropped, "--discard"}, env);
    CHECK_EQ(code(r), QString("ok"));
    CHECK(std::as_const(r)["plan"]["discarded"].toBool());
    CHECK(fileBytes(path) == changed);
    CHECK_EQ(code(cli({"edit", path, "--plan_id", dropped, "--run", "all"}, env)), QString("NOT_FOUND"));

    // Unknown or made-up ids, and wrong combinations
    CHECK_EQ(code(cli({"edit", path, "--plan_id", "p999", "--run", "all"}, env)), QString("NOT_FOUND"));
    CHECK_EQ(code(cli({"edit", path, "--plan_id", "../../x", "--run", "all"}, env)), QString("NOT_FOUND"));
    r = cli({"edit", path, "--plan", "--ops", R"([{"op":"split","at":170},{"op":"split","at":180}])"}, env);
    const QString two = std::as_const(r)["plan"]["id"].toString();
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two, "--run", "[3]"}, env)), QString("BAD_ARGUMENT"));
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two, "--run", "[0]"}, env)), QString("BAD_ARGUMENT"));
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two}, env)), QString("BAD_ARGUMENT"));
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two, "--run", "all", "--discard"}, env)), QString("BAD_ARGUMENT"));
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two, "--run", "all", "--ops", "[]"}, env)), QString("BAD_ARGUMENT"));
    CHECK_EQ(code(cli({"edit", path, "--run", "all"}, env)), QString("BAD_ARGUMENT"));
    CHECK(fileBytes(path) == changed);
    // A plan with an op that fails is no plan
    r = cli({"edit", path, "--plan", "--ops", R"([{"op":"split","at":170},{"op":"delete","clips":[999]}])"}, env);
    CHECK_EQ(code(r), QString("NOT_FOUND"));
    CHECK(std::as_const(r)["error"]["message"].toString().startsWith("op 1"));

    // Expired (older than a day): gone
    QFile old(planFile(path, two));
    if (CHECK(old.open(QIODevice::ReadWrite))) {
        CHECK(old.setFileTime(QDateTime::currentDateTime().addSecs(-25 * 3600), QFileDevice::FileModificationTime));
        old.close();
    }
    CHECK_EQ(code(cli({"edit", path, "--plan_id", two, "--run", "all"}, env)), QString("NOT_FOUND"));
    CHECK(!QFileInfo::exists(planFile(path, two)));
    CHECK(fileBytes(path) == changed);
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8()); // registry of running apps, backups and plans
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi");

    FakeApp fake;
    setup(fake.project);
    fake.path = tmp.path() + "/film.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    fake.project.markSaved();
    const QString copy = tmp.path() + "/copy.schneidi";
    CHECK(QFile::copy(fake.path, copy));
    const QString other = tmp.path() + "/other.schneidi";
    CHECK(QFile::copy(fake.path, other));
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return Check::result();
    // Files edited without the app: no registry, no running app
    QProcessEnvironment noLive = QProcessEnvironment::systemEnvironment();
    noLive.insert("XDG_DATA_HOME", tmp.path() + "/data-file");

    // Backups and plans of the file runs live under the other data folder: what the test looks at
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data-file").toUtf8());
    fileTests(other, noLive);
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());

    // ---- Live: the same plan in the app and on the file copy ----
    const QByteArray data = ProjectFile::toJson(fake.project.data(), fake.path);
    const int undo0 = fake.project.undoStack()->index();
    QJsonObject live = cli({"edit", fake.path, "--plan", "--ops", kPlanOps});
    CHECK_EQ(code(live), QString("ok"));
    CHECK(std::as_const(live)["live"].toBool());
    CHECK(ProjectFile::toJson(fake.project.data(), fake.path) == data); // nothing changed in the app
    CHECK_EQ(fake.project.undoStack()->index(), undo0);                   // and no undo step
    CHECK(!fake.project.undoStack()->canRedo());
    CHECK(fileBytes(fake.path) == fileBytes(copy));                       // nor the file
    CHECK_EQ(std::as_const(live)["plan"]["steps"].toArray().size(), 3);
    CHECK_EQ(std::as_const(live)["plan"]["steps"][1]["why"].toString(), QString("second cut"));
    CHECK_EQ(fake.plans, QStringList{"made p1"});
    CHECK_EQ(fake.lastPlan["steps"].toArray().size(), 3); // the app gets the plan to show
    QJsonObject onFile = cli({"edit", copy, "--plan", "--ops", kPlanOps}, noLive);
    CHECK_EQ(code(onFile), QString("ok"));
    // Same steps live and on the file
    CHECK_EQ(std::as_const(live)["plan"]["steps"], std::as_const(onFile)["plan"]["steps"]);

    // Run 1 and 3: one undo step in the app, the file untouched; the same result as on the file
    live = cli({"edit", fake.path, "--plan_id", "p1", "--run", "1,3"});
    CHECK_EQ(code(live), QString("ok"));
    CHECK(std::as_const(live)["live"].toBool());
    CHECK_EQ(fake.project.undoStack()->index(), undo0 + 1);
    CHECK_EQ(fake.project.undoStack()->undoText(), T("KI: Plan %1").arg("p1"));
    CHECK(fileBytes(fake.path) == fileBytes(copy));
    CHECK_EQ(fake.plans.last(), QString("ran p1"));
    CHECK_EQ(fake.lastPlan["ran"].toArray(), (QJsonArray{1, 3}));
    onFile = cli({"edit", copy, "--plan_id", std::as_const(onFile)["plan"]["id"].toString(), "--run", "1,3"}, noLive);
    CHECK_EQ(code(onFile), QString("ok"));
    CHECK(fileTimeline(copy) == fake.project.timeline());
    CHECK_EQ(fake.project.timeline().video[0].clips.size(), 2);
    CHECK_EQ(fake.project.timeline().markers.size(), 1);
    // Undo: back to before the run, as one step
    fake.project.undoStack()->undo();
    CHECK(fake.project.timeline() == fileTimeline(fake.path)); // (the id counters go on counting on undo)

    // Live stale: the user edits after the plan (undo brings the planned state back)
    QJsonObject r = cli({"edit", fake.path, "--plan", "--ops", R"([{"op":"split","at":60,"why":"x"}])"});
    const QString id = std::as_const(r)["plan"]["id"].toString();
    fake.project.edit("user", [](Timeline& tl) { tl.markIn = 7; });
    const int undo1 = fake.project.undoStack()->index();
    r = cli({"edit", fake.path, "--plan_id", id, "--run", "all"});
    CHECK_EQ(code(r), QString("PLAN_STALE"));
    CHECK_EQ(fake.project.undoStack()->index(), undo1);
    CHECK_EQ(fake.project.timeline().video[0].clips.size(), 1);
    fake.project.undoStack()->undo(); // the state the plan was made on: it runs again
    CHECK_EQ(code(cli({"edit", fake.path, "--plan_id", id, "--run", "all"})), QString("ok"));
    CHECK_EQ(fake.project.timeline().video[0].clips.size(), 2);
    fake.project.undoStack()->undo();
    r = cli({"edit", fake.path, "--plan", "--ops", R"([{"op":"split","at":70}])"});
    const QString keep = std::as_const(r)["plan"]["id"].toString();
    // Discard live: the app hears it
    CHECK_EQ(code(cli({"edit", fake.path, "--plan_id", keep, "--discard"})), QString("ok"));
    CHECK_EQ(fake.plans.last(), "discarded " + keep);
    CHECK_EQ(code(cli({"edit", fake.path, "--plan_id", keep, "--run", "all"})), QString("NOT_FOUND"));

    // ---- Plan mode of the app's own chat: only plans; the user runs them through the app ----
    QProcessEnvironment chatEnv = QProcessEnvironment::systemEnvironment();
    const QHash<QString, QString> chat = server.chatEnvironment();
    for (auto it = chat.begin(); it != chat.end(); ++it) chatEnv.insert(it.key(), it.value());
    fake.planOnly = true;
    const int undo2 = fake.project.undoStack()->index();
    r = cli({"edit", "@app", "--ops", R"([{"op":"split","at":40}])"}, chatEnv);
    CHECK_EQ(code(r), QString("PLAN_MODE"));
    CHECK_EQ(fake.project.undoStack()->index(), undo2);
    // An AI from outside is not the chat: plan mode is the chat's setting
    CHECK_EQ(code(cli({"edit", fake.path, "--ops", R"([{"op":"marker","at":5}])"})), QString("ok"));
    fake.project.undoStack()->undo();
    r = cli({"edit", "@app", "--plan", "--ops", R"([{"op":"split","at":40,"why":"a"},{"op":"split","at":90,"why":"b"}])"},
            chatEnv);
    CHECK_EQ(code(r), QString("ok"));
    const QString chatPlan = std::as_const(r)["plan"]["id"].toString();
    CHECK_EQ(fake.plans.last(), "made " + chatPlan + " chat");
    CHECK_EQ(code(cli({"edit", "@app", "--plan_id", chatPlan, "--run", "all"}, chatEnv)), QString("PLAN_MODE"));
    const int undo3 = fake.project.undoStack()->index();
    const QJsonObject reply =
        server.runForChat("edit", {{"project", "@app"}, {"plan_id", chatPlan}, {"run", QJsonArray{2}}});
    CHECK(std::as_const(reply)["ok"].toBool());
    CHECK_EQ(fake.project.undoStack()->index(), undo3 + 1);
    CHECK_EQ(fake.project.undoStack()->text(undo3), T("KI: Plan %1").arg(chatPlan)); // the Undo menu's text, translated
    if (CHECK_EQ(fake.project.timeline().video[0].clips.size(), 2)) CHECK_EQ(fake.project.timeline().video[0].clips[1].start, 90);
    CHECK_EQ(fake.plans.last(), "ran " + chatPlan + " chat");
    // Gone after the run
    const QJsonObject again = server.runForChat("edit", {{"project", "@app"}, {"plan_id", chatPlan}, {"run", QJsonArray{1}}});
    CHECK_EQ(again["error"]["code"].toString(),
             QString("NOT_FOUND"));
    fake.planOnly = false;
    return Check::result();
}
