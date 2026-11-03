// SPDX-License-Identifier: AGPL-3.0-only
// The 0.17 talk ops (jl_cut, room_tone) and story ops (section, sections, note, hook_variants) live in the app: each `edit` call is one undo step, undo gives back exactly the
// project before, redo the change, and the result matches the same ops run on the project file.
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QUndoStack>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <functional>
#include <random>

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
    // The user's hand-over: a modal "save changes?" question (its own event loop) while an AI sends an edit
    std::function<void()> duringQuestion;
    bool liveOpen(const QStringList&, bool byUser, QString*) override
    {
        if (byUser && duringQuestion) duringQuestion();
        return true; // the user kept the project
    }
};

constexpr int kRate = 48000;

// A take: room (125 Hz hum) everywhere, words (noise) at 0-4 and 6-20 s, a quieter room at 10-12 s
bool writeTake(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    std::vector<qint16> s(size_t(20 * kRate));
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> noise(-0.1, 0.1);
    for (size_t i = 0; i < s.size(); ++i) {
        const double t = double(i) / kRate;
        const bool quiet = t >= 10 && t < 12, words = !quiet && (t < 4 || t >= 6);
        const double v = (quiet ? 0.002 : 0.004) * std::sin(2 * M_PI * 125 * t) + (words ? noise(rng) : 0.0);
        s[i] = qint16(std::lround(v * 32767));
    }
    const quint32 data = quint32(s.size() * 2);
    char h[44];
    memcpy(h, "RIFF", 4);
    qToLittleEndian<quint32>(36 + data, h + 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, h + 16);
    qToLittleEndian<quint16>(1, h + 20);
    qToLittleEndian<quint16>(1, h + 22);
    qToLittleEndian<quint32>(kRate, h + 24);
    qToLittleEndian<quint32>(kRate * 2, h + 28);
    qToLittleEndian<quint16>(2, h + 32);
    qToLittleEndian<quint16>(16, h + 34);
    memcpy(h + 36, "data", 4);
    qToLittleEndian<quint32>(data, h + 40);
    f.write(h, 44);
    for (qint16 v : s) {
        char b[2];
        qToLittleEndian<qint16>(v, b);
        f.write(b, 2);
    }
    return true;
}

Clip clip(int id, const QString& path, int start, int in, int length, int link)
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

// V1/A1: linked picture + sound, cuts at 100 and 200 (material before 2 and after 2); A2: two pieces of the take with a
// gap of 2 s between them
void setup(Project& p, const QString& take)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 300, true, true, false});
    p.addMedia({take, "take.wav", 500, false, true, false});
    p.edit("setup", [&](Timeline& tl) {
        tl.audio.resize(2);
        for (int k = 0; k < 3; ++k) {
            const int link = p.newLinkId();
            tl.video[0].clips << clip(p.newClipId(), "/x/a.mp4", k * 100, k == 0 ? 200 : 20 + k * 30, 100, link);
            tl.audio[0].clips << clip(p.newClipId(), "/x/a.mp4", k * 100, k == 0 ? 200 : 20 + k * 30, 100, link);
        }
        tl.audio[1].clips << clip(p.newClipId(), take, 0, 0, 110, 0) << clip(p.newClipId(), take, 160, 140, 100, 0);
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
    if (!o["ok"].toBool()) std::printf("       %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

// The project as JSON without the id counters (they go on counting on undo) and the current timeline
QByteArray state(const Project& p)
{
    Project q; // seen from the first timeline: which one the app shows is no part of the edit
    q.load(p.data());
    q.setCurrentSequence(q.sequences().first().id);
    ProjectData d = q.data();
    d.lastClipId = d.lastLinkId = d.lastSequenceId = 0;
    return ProjectFile::toJson(d, "/x/film.schneidi");
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName("schneidi");

    const QString take = tmp.path() + "/take.wav";
    if (!CHECK(writeTake(take))) return Check::result();
    FakeApp fake;
    setup(fake.project, take);
    fake.path = tmp.path() + "/film.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    fake.project.markSaved();
    const QString copy = tmp.path() + "/copy.schneidi";
    CHECK(QFile::copy(fake.path, copy));
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return Check::result();
    QProcessEnvironment noLive = QProcessEnvironment::systemEnvironment();
    noLive.insert("XDG_DATA_HOME", tmp.path() + "/other"); // no registry: no running app

    const QStringList calls{
        R"([{"op":"jl_cut","at":100,"audio":"-0.4s"}])",
        R"([{"op":"jl_cut","clip":5,"audio":"+12"}])",
        R"([{"op":"room_tone","track":"A2"}])",
        R"([{"op":"jl_cut","at":100,"audio":0},{"op":"room_tone","track":"A2"}])",
        // Jump cuts (0.17 package K): the three clips of a.mp4 skip parts of it
        R"([{"op":"hide_jumps","center":"middle"}])",
        // Story sections, notes and hook variants (0.17 package B; hook_variants last: redoing it opens the last
        // timeline it made, so an op after it would edit that one in the app but the original on the file)
        R"([{"op":"section","kind":"hook","from":0,"to":"2s","name":"Kill"}])",
        R"([{"op":"sections","auto":true}])",
        R"([{"op":"note","at":"0:03","text":"schneller"},{"op":"note","done":1}])",
        R"([{"op":"hook_variants","at":["3s"],"question":"Wer?"}])",
    };
    for (const QString& ops : calls) {
        const QByteArray before = state(fake.project);
        const int index = fake.project.undoStack()->index();
        const QJsonObject live = cli({"edit", fake.path, "--ops", ops});
        CHECK(live["live"].toBool());
        const QJsonObject file = cli({"edit", copy, "--ops", ops}, noLive);
        CHECK(file["ok"].toBool() && !file["live"].toBool());
        if (!CHECK_EQ(fake.project.undoStack()->index(), index + 1)) std::printf("       ops %s\n", qPrintable(ops));
        ProjectData d;
        ProjectFile::load(copy, &d, nullptr);
        Project onFile;
        onFile.load(d);
        if (!CHECK(state(onFile) == state(fake.project))) std::printf("       differs after %s\n", qPrintable(ops));
        CHECK(state(fake.project) != before);
        const QByteArray after = state(fake.project);
        fake.project.undoStack()->undo();
        if (!CHECK(state(fake.project) == before)) std::printf("       undo differs after %s\n", qPrintable(ops));
        fake.project.undoStack()->redo();
        if (!CHECK(state(fake.project) == after)) std::printf("       redo differs after %s\n", qPrintable(ops));
    }
    // The room tone landed: A2 has no gap left
    const auto& a2 = fake.project.timeline().audio[1].clips;
    CHECK(a2.size() > 2);
    for (int k = 0; k + 1 < a2.size(); ++k) CHECK_EQ(a2[k].end(), a2[k + 1].start);
    return Check::result();
}
