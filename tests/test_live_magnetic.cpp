// SPDX-License-Identifier: AGPL-3.0-only
// The magnetic main track and the AI's live edits (cli/Live.cpp): the ops of one `edit` keep their own meaning - a
// place at a time overwrites there, a delete leaves its gap between the ops - and V1 is packed once at the end of the
// step, inside its one undo step (undo gives back exactly the project before). With the switch off nothing packs.
#include "cli/Live.h"
#include "core/AppPaths.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "check.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUndoStack>

namespace {

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString& to, QString* error) override
    {
        if (!to.isEmpty()) path = to;
        return ProjectFile::save(project.data(), path, error);
    }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    bool liveOpen(const QStringList&, bool, QString*) override { return true; }
};

int g_first = 0;

void setup(Project& p)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({Check::absPath("/x/a.mp4"), "a.mp4", 300, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        for (const auto& [start, len] : {std::pair{0, 100}, std::pair{100, 60}}) {
            Clip v;
            v.id = p.newClipId();
            if (!g_first) g_first = v.id;
            v.mediaPath = Check::absPath("/x/a.mp4");
            v.start = start;
            v.in = start;
            v.out = start + len - 1;
            v.linkId = p.newLinkId();
            Clip a = v;
            a.id = p.newClipId();
            tl.video[0].clips << v;
            tl.audio[0].clips << a;
        }
    });
}

QJsonObject cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && !t.hasExpired(120000)) {
        p.waitForFinished(5);
        QCoreApplication::processEvents();
    }
    if (p.state() != QProcess::NotRunning) p.kill(), p.waitForFinished(1000);
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (!o["ok"].toBool()) std::printf("       %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

// V1 as "start-end" pairs
QString v1(const Project& p)
{
    QStringList s;
    for (const Clip& c : p.timeline().video[0].clips) s << QString("%1-%2").arg(c.start).arg(c.end());
    return s.join(' ');
}

} // namespace

int main(int argc, char* argv[])
{
    Check::isolateSettings();
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    AppPaths::setProgramNames();

    FakeApp fake;
    setup(fake.project);
    fake.path = tmp.path() + "/film.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return Check::result();

    // Delete the first clip, then place 20 frames at 120 (inside the second): the place overwrites there (no insert
    // at a cut), the delete's gap is not closed between the ops; at the end V1 is packed once
    const QString ops = QString(R"([{"op":"delete","clips":[%1]},{"op":"place","media":"%2","at":120,"in":0,"out":20}])")
                            .arg(g_first).arg(Check::absPath("/x/a.mp4"));
    fake.project.setMagneticMain(true);
    const Timeline before = fake.project.timeline();
    const int index = fake.project.undoStack()->index();
    CHECK(cli({"edit", fake.path, "--ops", ops})["live"].toBool());
    CHECK_EQ(v1(fake.project), QString("0-20 20-40 40-60"));
    CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    // Its sound in sync with it (packed along)
    CHECK_EQ(fake.project.timeline().audio[0].clips.size(), 3);
    CHECK_EQ(fake.project.timeline().audio[0].clips[1].start, 20);
    fake.project.undoStack()->undo();
    CHECK(fake.project.timeline() == before);
    CHECK(fake.project.magnetActive()); // (the hold ended with the step)

    // Switch off: the gap stays
    fake.project.setMagneticMain(false);
    CHECK(cli({"edit", fake.path, "--ops", ops})["live"].toBool());
    CHECK_EQ(v1(fake.project), QString("100-120 120-140 140-160"));
    return Check::result();
}
