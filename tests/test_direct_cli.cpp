// SPDX-License-Identifier: AGPL-3.0-only
// `direct` (0.25): recipe short and highlights suggest -> apply on synthetic recordings (lavfi picture, synthetic
// sounds): suggestions change nothing, apply is one edit with the project's "direction" set; material that is no game
// gives moments from laughter, cheering and movement; sessions by id or object, expired and stale ones refused; a
// canceled suggest or apply fails with CANCELED and leaves no half project. Prints the suggest time of 10 clips cold
// and warm (DIRECT_MEASURE=1 makes them 30 s long as in the plan's target).
#include "cli/Commands.h"
#include "core/AppPaths.h"
#include "core/Direction.h"
#include "engine/Extensions.h"
#include "cli/Live.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include "check.h"
#include "synth_sounds.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <thread>

namespace {

// A fake whisper-cli (this program under that name): one word "hallo" at 0.1-0.9 s
int fakeWhisper(int argc, char** argv)
{
    QString out;
    for (int i = 1; i + 1 < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "-of") out = QString::fromLocal8Bit(argv[i + 1]);
    const QJsonObject json{{"result", QJsonObject{{"language", "de"}}},
                           {"transcription", QJsonArray{QJsonObject{{"text", " Hallo"},
                                                                    {"offsets", QJsonObject{{"from", 100}, {"to", 900}}}}}}};
    QFile o(out + ".json");
    if (!o.open(QIODevice::WriteOnly)) return 4;
    o.write(QJsonDocument(json).toJson());
    return 0;
}

// The fake whisper on the PATH and a (fake) model where the extensions keep it; returns how often it ran (a counter file)
void installFakeWhisper(const QString& dir)
{
    const QString suffix = QFileInfo(QCoreApplication::applicationFilePath()).suffix();
    const QString fake = QDir(dir).filePath(suffix.isEmpty() ? "bin/whisper-cli" : "bin/whisper-cli." + suffix);
    QDir().mkpath(QDir(dir).filePath("bin"));
    QFile::copy(QCoreApplication::applicationFilePath(), fake);
    QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
    qputenv("PATH", (QDir::toNativeSeparators(QDir(dir).filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH")).toUtf8());
    const QString models = QDir(Extensions::rootDir()).filePath("whisper/models");
    QDir().mkpath(models);
    QFile m(QDir(models).filePath("ggml-small-q5_1.bin"));
    if (m.open(QIODevice::WriteOnly)) m.write("model");
}

QJsonObject run(const QString& name, const QJsonObject& raw, const std::atomic<bool>* cancel = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    ctx.cancel = cancel;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
    }
}

// A "holiday clip": a moving picture with speech, laughter at ~10 s and cheering at 20-23 s
QString holiday(const QString& dir, const QString& name, double length, unsigned seed)
{
    const int rate = 48000;
    Synth::Builder b(rate, length, seed);
    Synth::speech(b, 1.5, 5.0);
    if (length > 12) Synth::laughter(b, 10.0);
    if (length > 24) Synth::cheering(b, 20.0, 3.0);
    b.floor();
    const QString wav = QDir(dir).filePath(name + ".wav");
    if (!Synth::writeWav(wav, b.s, rate)) return {};
    return Check::makeMedia(QDir(dir).filePath(name + ".mp4"),
                            {"-f", "lavfi", "-i", QString("testsrc2=s=640x360:r=30:d=%1").arg(length), "-i", wav,
                             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
}

// The app as the Direct page sees it: its project, nothing else (cf. test_fit_music)
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

// Two microphones of a conversation: A talks 0.2-2.9 and 5.0-6.6 and 9.0-10.0 s, B answers 2.9-5.4 s; both quiet
// about 6.6-9.0 s (cf. test_pause_cut)
QString twoMics(const QString& dir)
{
    const int rate = 48000;
    Synth::Builder ma(rate, 11.0, 11), mb(rate, 11.0, 12);
    ma.floor();
    Synth::speech(ma, 0.2, 2.7);
    Synth::speech(ma, 5.0, 1.6);
    Synth::speech(ma, 9.0, 1.0, 120);
    mb.floor();
    Synth::speech(mb, 2.9, 2.5, 180);
    const QString a = QDir(dir).filePath("mic-a.wav"), b = QDir(dir).filePath("mic-b.wav");
    Synth::writeWav(a, ma.s, rate);
    Synth::writeWav(b, mb.s, rate);
    const QString project = QDir(dir).filePath("talk.schneidi");
    run("new", {{"project", project}, {"width", 320}, {"height", 180}, {"fps", 25}});
    run("edit", {{"project", project},
                 {"ops", QJsonArray{QJsonObject{{"op", "place"}, {"media", a}, {"at", 0}, {"track", "A1"}},
                                    QJsonObject{{"op", "place"}, {"media", b}, {"at", 0}, {"track", "A2"}}}}});
    return project;
}

// schneidi-cli as a process: {stdout lines, stderr lines}
std::pair<QStringList, QStringList> cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!p.waitForFinished(120000)) {
        p.kill();
        p.waitForFinished(1000);
    }
    const auto lines = [](const QByteArray& b) { return QString::fromUtf8(b).split('\n', Qt::SkipEmptyParts); };
    return {lines(p.readAllStandardOutput()), lines(p.readAllStandardError())};
}

QStringList labelsOf(const QJsonArray& suggestions)
{
    QStringList all;
    for (const QJsonValue& s : suggestions)
        for (const QJsonValue& l : s["labels"].toArray()) all << l.toString();
    for (const QJsonValue& s : suggestions) all << s["reason"]["key"].toString();
    return all;
}

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("direct_cli");
    Check::initEngine();
    QTemporaryDir dir;

    // ---- Without material: the errors of the contract
    CHECK_EQ(run("direct", {{"recipe", "talk"}, {"files", QJsonArray{}}})["error"].toString(), QString("NO_MATERIAL"));
    CHECK_EQ(run("direct", {{"recipe", "dance"}})["error"].toString(), QString("UNKNOWN_RECIPE"));
    CHECK_EQ(run("direct", {{"recipe", "short"}})["error"].toString(), QString("NO_MATERIAL"));
    CHECK_EQ(run("direct", {{"session", "d999"}, {"apply", QJsonObject{{"on", "all"}}}})["error"].toString(),
             QString("SESSION_EXPIRED"));
    // ---- The command line: --session reaches the command (QGuiApplication took -session/--session and its value
    // out of the arguments as its own session-management option: "apply needs the session"), as an id and as JSON;
    // so do the other names Qt knows (--title, --name, --display, --reverse)
    {
        const auto [out, err] = cli({"direct", "--session", "d999", "--apply", R"({"on":[1,2]})"});
        if (CHECK_EQ(out.size(), 1))
            CHECK_EQ(QJsonDocument::fromJson(out.last().toUtf8())["error"]["code"].toString(), QString("SESSION_EXPIRED"));
        const auto [out2, err2] = cli({"direct", "--session", R"({"id":"d1"})", "--apply", R"({"on":"all"})"});
        if (CHECK_EQ(out2.size(), 1)) {
            const QString message = QJsonDocument::fromJson(out2.last().toUtf8())["error"]["message"].toString();
            CHECK(message.startsWith("session:"));
            if (!CHECK(!message.contains("apply needs the session"))) std::printf("       %s\n", qPrintable(message));
        }
        const auto [out3, err3] = cli({"help", "edit", "--op", "title", "--reverse"});
        if (CHECK(!out3.isEmpty())) CHECK(out3.last().contains("reverse")); // refused as unknown: it got here
    }
    if (!Check::haveFfmpeg()) return Check::result();

    // ---- Recipe short on a "game" recording with two loud moments (folder named like a game's, game none)
    QDir().mkpath(dir.filePath("clips/cs2"));
    const QString clip = Check::makeMedia(
        dir.filePath("clips/cs2/cs2_clip_test.mp4"),
        {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=40", "-f", "lavfi", "-i",
         "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,9,9.6),0.4*sin(2*PI*440*t),0)+if(between(t,29,29.6),0.9*sin(2*PI*440*t),0)':d=40:s=48000",
         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!clip.isEmpty())) return Check::result();
    // stdout carries only the answer, progress lines go to stderr (loudness, highlights): a tool that shows both
    // streams together mixes them, the streams themselves never
    for (const QStringList& args : {QStringList{"loudness", clip}, QStringList{"highlights", clip, "--game", "none"}}) {
        const auto [out, err] = cli(args);
        if (CHECK_EQ(out.size(), 1)) CHECK(QJsonDocument::fromJson(out[0].toUtf8())["ok"].toBool());
        else std::printf("       %s\n", qPrintable(out.join('\n')));
        CHECK(std::any_of(err.begin(), err.end(), [](const QString& l) { return l.startsWith("{\"progress\""); }));
    }
    const QJsonObject suggested = run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}},
                                                 {"options", QJsonObject{{"game", "none"}, {"variants", 2}}}});
    const QJsonObject session = suggested["session"].toObject();
    CHECK(session["id"].toString().startsWith("d"));
    CHECK(suggested["next"].toString().contains("apply"));
    CHECK(!QFileInfo::exists(dir.filePath("clips/cs2/cs2_clip_test-short.schneidi"))); // suggesting changes nothing
    const QJsonArray suggestions = session["suggestions"].toArray();
    int on = 0;
    bool both = false, loud = true;
    for (const QJsonValue& s : suggestions) {
        if (!s["on"].toBool()) continue;
        ++on;
        CHECK_EQ(s["variant"].toInt(), 1);
        loud &= s["reason"]["key"].toString() == "loud";
        CHECK(!s["reason"]["text"].toString().isEmpty());
        both |= s["from_s"].toDouble() <= 9.3 && s["to_s"].toDouble() >= 29.3;
    }
    CHECK(on >= 1 && loud);
    CHECK_EQ(session["material"][0]["has_video"].toBool(), true);
    CHECK_EQ(session["material"][0]["analysis"]["sound"].toString(), QString("done"));
    CHECK(session["length_s"].toDouble() >= 14.9 && session["length_s"].toDouble() <= 45.1);
    CHECK_EQ(session["order"].toArray().size(), on);
    Q_UNUSED(both);

    // dry_run and plan need a project; a plan changes nothing
    const QString proj = dir.filePath("out/short.schneidi");
    QDir().mkpath(dir.filePath("out"));
    run("new", {{"project", proj}, {"media", QJsonArray{clip}}});
    const QJsonObject planned = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                               {"project", proj}, {"plan", true}});
    CHECK(planned["plan"]["id"].toString().startsWith("p"));
    CHECK_EQ(run("info", {{"project", proj}, {"summary", true}})["timelines"].toArray().size(), 1);
    const QJsonObject dry = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                           {"project", proj}, {"dry_run", true}});
    CHECK(dry["dry_run"].toBool());
    CHECK_EQ(run("info", {{"project", proj}, {"summary", true}})["timelines"].toArray().size(), 1);
    // Apply: one edit, the cut and the 9:16 copy, the project remembers the session
    const QJsonObject applied = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                               {"project", proj}});
    CHECK(!applied.contains("error"));
    CHECK_EQ(applied["timeline"].toString(), QString("Short"));
    CHECK_EQ(applied["cut_timeline"].toString(), QString("Short-Schnitt"));
    CHECK_EQ(applied["moments"].toArray().size(), on);
    CHECK(applied["check"].toObject().contains("issues"));
    {
        const QJsonObject info = run("info", {{"project", proj}, {"timeline", "Short"}, {"summary", true}});
        CHECK_EQ(info["timeline"]["height"].toInt(), 1920);
        CHECK(std::abs(info["timeline"]["length_s"].toDouble() - applied["length_s"].toDouble()) < 0.1);
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(proj, &d, &error));
        CHECK_EQ(d.direction["id"].toString(), session["id"].toString());
        CHECK_EQ(d.direction["recipe"].toString(), QString("short"));
        // The project opens on the result, not on its empty Timeline 1 (videotest 21.11.)
        CHECK_EQ(run("info", {{"project", proj}, {"summary", true}})["timeline"].toObject()["name"].toString(), QString("Short"));
        // Its sound leveled like `short` does: the master to -14 LUFS behind the limiter
        CHECK(QJsonDocument(applied["done"].toArray()).toJson().contains("leveled to"));
        const QJsonObject l = run("loudness", {{"source", proj}, {"timeline", "Short"}});
        CHECK(std::abs(l["loudness"]["integrated_lufs"].toDouble() + 14) < 1.0);
    }
    // A choice of its own: only the first suggestion, moved edges, as an object (the page sends what it shows)
    {
        QJsonObject changed = session;
        const int n = suggestions[0]["n"].toInt();
        const QJsonObject one = run("direct", {{"session", changed}, {"project", proj},
                                               {"apply", QJsonObject{{"on", QJsonArray{n}},
                                                                     {"adjust", QJsonArray{QJsonObject{{"n", n}, {"from_s", 2.0}, {"to_s", 7.5}}}}}}});
        CHECK_EQ(one["moments"].toArray().size(), 1);
        CHECK_EQ(one["length_s"].toDouble(), 5.5);
        CHECK_EQ(one["timeline"].toString(), QString("Short 2"));
        // Expired (made two days ago) and a list naming no suggestion
        changed["made"] = QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODate);
        CHECK_EQ(run("direct", {{"session", changed}, {"apply", QJsonObject{{"on", "all"}}}, {"project", proj}})["error"].toString(),
                 QString("SESSION_EXPIRED"));
        CHECK_EQ(run("direct", {{"session", session}, {"apply", QJsonObject{{"on", QJsonArray{99}}}}, {"project", proj}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
    }
    // Stale: the recording changed since the session was made
    {
        const QString copy = dir.filePath("clips/copy.mp4");
        CHECK(QFile::copy(clip, copy));
        const QJsonObject s = run("direct", {{"recipe", "short"}, {"files", QJsonArray{copy}}})["session"].toObject();
        QFile f(copy);
        CHECK(f.open(QIODevice::Append));
        f.write("x");
        f.close();
        CHECK_EQ(run("direct", {{"session", s["id"]}, {"apply", QJsonObject{{"on", "all"}}}, {"project", proj}})["error"].toString(),
                 QString("SESSION_STALE"));
    }

    // ---- In the app, as the Direct page calls it: suggest as a background job, apply through runForUser = ONE undo
    // step (the cut and the 9:16 copy), undo takes it all back, a failure leaves nothing
    {
        FakeApp w;
        w.path = dir.filePath("out/app.schneidi");
        run("new", {{"project", w.path}, {"media", QJsonArray{clip}}});
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(w.path, &d, &error));
        w.project.load(d);
        QJsonObject reply;
        QEventLoop loop;
        Cli::Live::runInBackground("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}},
                                              {"options", QJsonObject{{"game", "none"}}}},
                                   {}, [&](const QJsonObject& r) { reply = r; loop.quit(); }, &w);
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
        CHECK(reply["ok"].toBool());
        const QJsonObject s = reply["result"].toObject()["session"].toObject();
        CHECK(!s["suggestions"].toArray().isEmpty());
        CHECK(!w.project.isModified()); // suggesting changes nothing, not even "modified"
        // Applying in the background is refused (it edits)
        QJsonObject refused;
        QEventLoop loop2;
        Cli::Live::runInBackground("direct", {{"session", s}, {"apply", QJsonObject{{"on", "all"}}}}, {},
                                   [&](const QJsonObject& r) { refused = r; loop2.quit(); }, &w);
        loop2.exec();
        CHECK_EQ(refused["error"].toObject()["code"].toString(), QString("NOT_IN_BACKGROUND"));

        const int steps = w.project.undoStack()->count();
        const int timelines = int(w.project.sequences().size());
        const QJsonObject r = Cli::Live::runForUser(&w, "direct", {{"session", s}, {"apply", QJsonObject{{"on", "all"}}}},
                                                    "Regie: Übernehmen");
        if (!CHECK(r["ok"].toBool())) std::printf("       %s\n", QJsonDocument(r).toJson().constData());
        CHECK_EQ(w.project.undoStack()->count(), steps + 1);
        CHECK_EQ(int(w.project.sequences().size()), timelines + 2); // Short-Schnitt and Short
        CHECK_EQ(r["result"]["timeline"].toString(), QString("Short"));
        CHECK_EQ(w.project.direction()["id"].toString(), s["id"].toString());
        CHECK(w.project.isModified());
        w.project.undoStack()->undo();
        CHECK_EQ(int(w.project.sequences().size()), timelines);
        // A session that fails while it is applied: nothing changed, no step
        QJsonObject bad = s;
        QJsonObject options = bad["options"].toObject();
        options["aspect"] = "16:9";
        bad["options"] = options;
        const int before = w.project.undoStack()->index();
        const QJsonObject f = Cli::Live::runForUser(&w, "direct", {{"session", bad}, {"apply", QJsonObject{{"on", "all"}}}},
                                                    "Regie: Übernehmen");
        CHECK(!f["ok"].toBool());
        CHECK_EQ(w.project.undoStack()->index(), before);
        CHECK_EQ(int(w.project.sequences().size()), timelines);
    }

    // ---- Recipe highlights on material that is no game: laughter, cheering and movement give moments
    const QString beach = holiday(dir.path(), "beach", 30, 3);
    if (!CHECK(!beach.isEmpty())) return Check::result();
    {
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{beach}},
                                             {"options", QJsonObject{{"length_s", "all"}, {"order", "time"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        const QStringList labels = labelsOf(list);
        if (!CHECK(labels.contains("laughter") || labels.contains("cheering")))
            std::printf("       %s\n", QJsonDocument(list).toJson().constData());
        CHECK(labels.contains("motion") || labels.contains("scene_change") || labels.contains("loud"));
        // In time order
        double last = -1;
        for (const QJsonValue& v : r["session"]["order"].toArray()) {
            for (const QJsonValue& s : list)
                if (s["n"].toInt() == v.toInt()) {
                    CHECK(s["from_s"].toDouble() > last);
                    last = s["from_s"].toDouble();
                }
        }
        const QJsonObject made = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK(!made.contains("error"));
        CHECK_EQ(made["timeline"].toString(), QString("Highlights"));
        CHECK(QFileInfo::exists(made["project"].toString()));
        // in the folder for projects, never next to the recording (a recorder's folder; videotest 21.11.)
        CHECK_EQ(QFileInfo(made["project"].toString()).absolutePath(), QFileInfo(AppPaths::projectFolder()).absoluteFilePath());
        CHECK(QFileInfo(made["project"].toString()).absolutePath() != QFileInfo(beach).absolutePath());
        // A second recipe on the same material reads the caches
        QElapsedTimer t;
        t.start();
        const QJsonObject again = run("direct", {{"recipe", "short"}, {"files", QJsonArray{beach}}});
        CHECK(!again.contains("error"));
        std::printf("       second recipe on the same clip: %lld ms\n", t.elapsed());
    }

    // ---- Review fixes (0.25): sessions, sources, validation
    {
        // Sessions live in one folder: a project-bound suggest's `next` (the id alone) works; another project is refused
        const QString bound = dir.filePath("out/bound.schneidi");
        run("new", {{"project", bound}, {"media", QJsonArray{clip}}});
        const QJsonObject b = run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}}, {"project", bound},
                                             {"options", QJsonObject{{"game", "none"}}}});
        CHECK(b["next"].toString().startsWith("direct session:'" + b["session"]["id"].toString() + "'"));
        const QJsonObject viaNext = run("direct", {{"session", b["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK_EQ(viaNext["project"].toString(), bound);
        CHECK_EQ(run("direct", {{"session", b["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}, {"project", proj}})["error"].toString(),
                 QString("SESSION_PROJECT_MISMATCH"));
        // A plan lists only the user's steps (no hidden _direction)
        const QJsonObject p2 = run("direct", {{"session", b["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}, {"plan", true}});
        CHECK(!QJsonDocument(p2["plan"].toObject()).toJson().contains("_direction"));
        // A project given that does not exist yet is made
        const QString fresh = dir.filePath("out/made-here.schneidi");
        const QJsonObject made = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}}, {"project", fresh}});
        CHECK(!made.contains("error"));
        CHECK(QFileInfo::exists(fresh));

        // Validation: order, variant, adjust, platform length, overlapping edges merged
        const QJsonArray sg = session["suggestions"].toArray();
        const int n1 = sg[0]["n"].toInt();
        const auto apply = [&](const QJsonObject& ap) {
            return run("direct", {{"session", session["id"]}, {"apply", ap}, {"project", fresh}});
        };
        CHECK_EQ(apply({{"on", "all"}, {"order", QJsonArray{999}}})["error"].toString(), QString("BAD_ARGUMENT"));
        CHECK_EQ(apply({{"variant", 7}})["error"].toString(), QString("BAD_ARGUMENT"));
        CHECK_EQ(apply({{"on", QJsonArray{n1}}, {"adjust", QJsonArray{QJsonObject{{"n", n1}, {"from_s", "x"}}}}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        CHECK_EQ(apply({{"on", QJsonArray{n1}}, {"adjust", QJsonArray{QJsonObject{{"n", n1}, {"from_s", 0.0}, {"to_s", 40.0}}}}})["error"].toString(),
                 QString()); // 40 s fits the 60 s of Shorts
        QJsonObject longer = session;
        QJsonObject lo = longer["options"].toObject();
        lo["for"] = "reels"; // ...but not 90 s... the clip is 40 s: two copies overlapping merge
        longer["options"] = lo;
        if (sg.size() >= 2) {
            const int n2 = sg[1]["n"].toInt();
            const QJsonObject merged = apply({{"on", QJsonArray{n1, n2}},
                                              {"adjust", QJsonArray{QJsonObject{{"n", n1}, {"from_s", 5.0}, {"to_s", 15.0}},
                                                                    QJsonObject{{"n", n2}, {"from_s", 12.0}, {"to_s", 20.0}}}}});
            if (CHECK_EQ(merged["moments"].toArray().size(), 1)) CHECK_EQ(merged["length_s"].toDouble(), 15.0);
        }
        // An inline session naming a file outside its material is refused
        QJsonObject foreign = session;
        QJsonArray fs = foreign["suggestions"].toArray();
        QJsonObject f0 = fs[0].toObject();
        f0["file"] = dir.filePath("clips/other.mp4");
        fs[0] = f0;
        foreign["suggestions"] = fs;
        CHECK_EQ(run("direct", {{"session", foreign}, {"apply", QJsonObject{{"on", "all"}}}, {"project", fresh}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        // Short checks its aspect when suggesting
        CHECK_EQ(run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}}, {"options", QJsonObject{{"aspect", "16:9"}}}})["error"].toString(),
                 QString("BAD_ARGUMENT"));

        // A game recording without a kill: the end of the recording, as `short` always did; no event or picture analysis
        const QString noKill = Check::makeMedia(dir.filePath("clips/cs2/cs2_clip_quiet.mp4"),
                                                {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=20", "-f", "lavfi", "-i",
                                                 "aevalsrc='0.02*sin(2*PI*220*t)':d=20:s=48000", "-c:v", "libx264",
                                                 "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QJsonObject g = run("direct", {{"recipe", "short"}, {"files", QJsonArray{noKill}}})["session"].toObject();
        const QJsonArray gs = g["suggestions"].toArray();
        if (CHECK(!gs.isEmpty())) {
            CHECK_EQ(gs[0]["reason"]["key"].toString(), QString("fallback_end"));
            CHECK(std::abs(gs[0]["to_s"].toDouble() - 20) < 0.1);
        }
        CHECK_EQ(g["material"][0]["analysis"]["events"].toString(), QString("none"));
        CHECK_EQ(g["material"][0]["analysis"]["picture"].toString(), QString("none"));
        // A phone-like clip with laughter: a laughter moment among the short's suggestions
        const QStringList phone = labelsOf(run("direct", {{"recipe", "short"}, {"files", QJsonArray{beach}}})["session"].toObject()["suggestions"].toArray());
        if (!CHECK(phone.contains("laughter") || phone.contains("cheering"))) std::printf("       %s\n", qPrintable(phone.join(",")));
        // A recording whose tail is broken still gives suggestions
        const QString whole = Check::makeMedia(dir.filePath("clips/tail.mkv"), {"-i", beach, "-c", "copy"});
        QFile t(whole);
        if (CHECK(t.open(QIODevice::ReadWrite))) {
            const qint64 size = t.size();
            t.resize(size * 2 / 3);
            t.close();
        }
        const QJsonObject broken = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{whole}}});
        CHECK(!broken.contains("error"));
        CHECK(!broken["session"]["suggestions"].toArray().isEmpty());
        // Nothing stands out (a still grey picture, a quiet hum): the fallback is as long as asked for
        const QString still = Check::makeMedia(dir.filePath("clips/still.mp4"),
                                               {"-f", "lavfi", "-i", "color=c=gray:s=320x240:r=25:d=30", "-f", "lavfi", "-i",
                                                "aevalsrc='0.01*sin(2*PI*100*t)':d=30:s=48000", "-c:v", "libx264",
                                                "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QJsonObject calm = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{still}},
                                                {"options", QJsonObject{{"length_s", 8}}}});
        CHECK(std::abs(calm["session"]["length_s"].toDouble() - 8) < 0.2);
        Q_UNUSED(longer);
    }

    // ---- Recipe talk: the common pauses of two microphones; apply = a copy of the timeline without them
    {
        const QString talk = twoMics(dir.path());
        const QJsonObject r = run("direct", {{"recipe", "talk"}, {"project", talk}, {"timeline", "@current"},
                                             {"options", QJsonObject{{"min_pause_s", 0.6}, {"pad_s", 0.15}, {"tracks", "A1,A2"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        bool common = false;
        for (const QJsonValue& v : list) {
            CHECK_EQ(v["kind"].toString(), QString("pause"));
            CHECK_EQ(v["reason"]["key"].toString(), QString("pause"));
            common |= v["from_s"].toDouble() > 6.4 && v["to_s"].toDouble() < 9.2;
            CHECK(!(v["from_s"].toDouble() < 5.2 && v["to_s"].toDouble() > 3.2)); // B talks there
        }
        if (!CHECK(common)) std::printf("       %s\n", QJsonDocument(list).toJson().constData());
        const double before = run("info", {{"project", talk}, {"summary", true}})["timeline"].toObject()["length_s"].toDouble();
        const QJsonObject cut = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK(!cut.contains("error"));
        const QJsonObject after = run("info", {{"project", talk}, {"timeline", cut["timeline"]}, {"summary", true}});
        CHECK(after["timeline"]["length_s"].toDouble() < before - 1.5);
        CHECK(std::abs(run("info", {{"project", talk}, {"summary", true}})["timeline"].toObject()["length_s"].toDouble() - before) < 0.05);
        // The timeline changed since: stale
        const QJsonObject again = run("direct", {{"recipe", "talk"}, {"project", talk}, {"options", QJsonObject{{"tracks", "A1,A2"}}}});
        run("edit", {{"project", talk}, {"ops", QJsonArray{QJsonObject{{"op", "marker"}, {"at", "1s"}}}}});
        run("edit", {{"project", talk}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "1s"}}}}});
        CHECK_EQ(run("direct", {{"session", again["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}})["error"].toString(),
                 QString("SESSION_STALE"));
    }
    // ---- Review of part 3: talk on files is refused, tracks as a list, edges of a pause, two applies of one session
    {
        const QString talk2 = twoMics(dir.path());
        CHECK_EQ(run("direct", {{"recipe", "talk"}, {"project", talk2}, {"files", QJsonArray{clip}}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        const QJsonObject r = run("direct", {{"recipe", "talk"}, {"project", talk2},
                                             {"options", QJsonObject{{"tracks", QJsonArray{"A1", "A2"}}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        if (CHECK(!list.isEmpty())) {
            CHECK(list[0]["track"].toString().contains("A2")); // the page's list reached cut_pauses
            const int n = list[0]["n"].toInt();
            const double from = list[0]["from_s"].toDouble(), to = list[0]["to_s"].toDouble();
            const QJsonObject one = run("direct", {{"session", r["session"]["id"]},
                                                   {"apply", QJsonObject{{"on", QJsonArray{n}},
                                                                         {"adjust", QJsonArray{QJsonObject{{"n", n}, {"from_s", from + 0.1}, {"to_s", to}}}}}}});
            CHECK(!one.contains("error"));
            CHECK(std::abs(one["removed_s"].toDouble() - (to - from - 0.1)) < 0.05);
            // The same session again with all of them: the source timeline is the same one (by its id)
            const QJsonObject all = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
            CHECK(!all.contains("error"));
            CHECK(all["timeline"].toString() != one["timeline"].toString());
        }
    }
    // ---- finish in two steps (as the app runs it): measured, then refused when the timeline changed meanwhile
    {
        const QJsonObject made = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                {"project", dir.filePath("out/finish.schneidi")}});
        const QString fp = made["project"].toString();
        const QJsonObject prepared = run("direct", {{"project", fp},
                                                    {"finish", QJsonObject{{"timeline", made["timeline"]}, {"sfx", "mild"},
                                                                           {"captions", "off"}, {"session", session}, {"prepare", true}}}});
        CHECK(!prepared["ops"].toArray().isEmpty());
        CHECK(!prepared["stamp"].toString().isEmpty());
        run("edit", {{"project", fp}, {"timeline", made["timeline"]}, {"ops", QJsonArray{QJsonObject{{"op", "split"}, {"at", "2s"}}}}});
        CHECK_EQ(run("direct", {{"project", fp}, {"finish", QJsonObject{{"timeline", made["timeline"]}, {"ops", prepared["ops"]},
                                                                         {"stamp", prepared["stamp"]}}}})["error"].toString(),
                 QString("FINISH_STALE"));
    }
    // ---- Recipe script with a (fake) whisper: the takes of the sentence, placed as rough_cut would; whisper once
    {
        installFakeWhisper(dir.path());
        const QString sp = dir.filePath("out/script.schneidi");
        run("new", {{"project", sp}});
        const QJsonObject r = run("direct", {{"recipe", "script"}, {"project", sp}, {"files", QJsonArray{beach}},
                                             {"options", QJsonObject{{"script", "Hallo."}}}});
        const QJsonArray takes = r["session"]["suggestions"].toArray();
        if (CHECK(!takes.isEmpty())) {
            CHECK_EQ(takes[0]["kind"].toString(), QString("take"));
            CHECK_EQ(takes[0]["sentence"].toInt(), 1);
            CHECK(takes[0]["from_s"].toDouble() >= 0.0 && takes[0]["to_s"].toDouble() > takes[0]["from_s"].toDouble());
        }
        // A second suggestion takes the words kept from the first (no second whisper run: the fake is gone now)
        QFile::remove(dir.filePath("bin/whisper-cli"));
        const QJsonObject again = run("direct", {{"recipe", "script"}, {"project", sp}, {"files", QJsonArray{beach}},
                                                 {"options", QJsonObject{{"script", "Hallo."}}}});
        CHECK(!again["session"]["suggestions"].toArray().isEmpty());
        // Files alone (no project): the takes are found as well, the apply makes a project in the folder for projects
        // (videotest 21.11.: NO_MATERIAL although the help said files work)
        const QJsonObject alone = run("direct", {{"recipe", "script"}, {"files", QJsonArray{beach}},
                                                 {"options", QJsonObject{{"script", "Hallo."}}}});
        CHECK(!alone.contains("error"));
        CHECK(!alone["session"]["suggestions"].toArray().isEmpty());
        const QJsonObject aloneCut = run("direct", {{"session", alone["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK(!aloneCut.contains("error"));
        CHECK_EQ(QFileInfo(aloneCut["project"].toString()).absolutePath(), QFileInfo(AppPaths::projectFolder()).absoluteFilePath());
        CHECK(aloneCut["timeline"].toString().startsWith("Rohschnitt"));
        const QJsonObject cut = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        CHECK(!cut.contains("error"));
        CHECK(cut["timeline"].toString().startsWith("Rohschnitt"));
        // The transcript is the project's now (the Transcript panel shows it, the next rough_cut uses it)
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(sp, &d, &error));
        bool kept = false;
        for (const MediaInfo& m : d.media) kept |= m.path == beach && !m.transcripts.isEmpty();
        CHECK(kept);
    }
    // ---- Recipe script without a script: a clear error; template without a project or template: NO_MATERIAL
    CHECK_EQ(run("direct", {{"recipe", "script"}, {"project", proj}})["error"].toString(), QString("BAD_ARGUMENT"));
    CHECK_EQ(run("direct", {{"recipe", "template"}, {"files", QJsonArray{clip}}})["error"].toString(), QString("NO_MATERIAL"));

    // ---- Recipe template: the built-in short-hook template, its clip fields filled with shots of the material
    {
        const QJsonObject r = run("direct", {{"recipe", "template"}, {"files", QJsonArray{clip, beach}},
                                             {"options", QJsonObject{{"template", "short-hook"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        if (CHECK(!list.isEmpty())) {
            CHECK_EQ(list[0]["kind"].toString(), QString("slot"));
            CHECK(!list[0]["slot"].toString().isEmpty());
            CHECK_EQ(list[0]["reason"]["key"].toString(), QString("slot_match"));
        }
        const QString filled = dir.filePath("out/from-template.schneidi");
        const QJsonObject made = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                {"project", filled}});
        CHECK(!made.contains("error"));
        CHECK(QFileInfo::exists(filled));
        // project_dir (the app: its folder for projects): the new project and its media folder go there, never next
        // to the recordings (a recorder's folder)
        const QString folder = dir.filePath("my projects");
        const QJsonObject there = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                 {"project_dir", folder}});
        CHECK(!there.contains("error"));
        CHECK_EQ(QFileInfo(there["project"].toString()).absolutePath(), QFileInfo(folder).absoluteFilePath());
        CHECK(QFileInfo::exists(there["project"].toString()));
        CHECK(QDir(dir.filePath("clips/cs2")).entryList({"*.schneidi"}).isEmpty());
        CHECK(QDir(QFileInfo(beach).absolutePath()).entryList({"short-hook*"}).isEmpty());
    }
    // ---- Highlights with far less found than wanted: a note with fixed key and values, the end of the recording
    // offered switched off (the plan's fallback "end of every recording")
    {
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{beach}},
                                             {"options", QJsonObject{{"length_s", 300}}}});
        const QJsonObject s = r["session"].toObject();
        bool few = false;
        for (const QJsonValue& n : s["notes"].toArray())
            if (n["key"].toString() == "few_moments") {
                few = true;
                CHECK_EQ(n["values"]["target"].toDouble(), 300.0);
                CHECK(n["values"]["s"].toDouble() > 0 && n["values"]["s"].toDouble() < 150);
            }
        CHECK(few);
        // Before the fillers the moments themselves grew (more of their scene: up to 6 s before, 4 s after)
        bool grown = false;
        for (const QJsonValue& n : s["notes"].toArray())
            if (n["key"].toString() == "moments_grown") {
                grown = true;
                CHECK(n["values"]["s"].toDouble() > n["values"]["found"].toDouble());
                CHECK_EQ(n["values"]["target"].toDouble(), 300.0);
            }
        if (!CHECK(grown)) std::printf("       %s\n", QJsonDocument(s["notes"].toArray()).toJson().constData());
        int ends = 0;
        for (const QJsonValue& x : s["suggestions"].toArray())
            if (x["reason"]["key"].toString() == "fallback_end") {
                ++ends;
                CHECK(!x["on"].toBool());
                CHECK(x["to_s"].toDouble() > x["from_s"].toDouble() + 0.9);
            }
        if (!CHECK(ends >= 1)) std::printf("       %s\n", QJsonDocument(s).toJson().constData());
        // One 30 s recording, two loud moments too close for the spacing: the one not picked stays offered (switched
        // off) or lies inside the grown pick, and the fallback is only the tail nothing covers - never in its way
        const QString one = Check::makeMedia(
            dir.filePath("tail.mp4"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=30", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,7,7.6),0.5*sin(2*PI*440*t),0)+if(between(t,13,13.6),0.9*sin(2*PI*440*t),0)':d=30:s=48000",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!one.isEmpty())) {
            const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{one}},
                                                 {"options", QJsonObject{{"length_s", 300}, {"game", "none"}}}});
            const QJsonArray list = r["session"].toObject()["suggestions"].toArray();
            bool extraAfter = false, tail = false;
            for (const QJsonValue& x : list) {
                const bool end = x["reason"]["key"].toString() == "fallback_end";
                if (!end && !x["on"].toBool()) extraAfter = true; // the other moment, offered
                // ... or inside the picked one: it grew (300 s wanted, 4 s found) and took in the scene at 7 s
                if (!end && x["on"].toBool() && x["from_s"].toDouble() <= 7 && x["to_s"].toDouble() >= 7.6) extraAfter = true;
                if (!end) continue;
                tail = true;
                CHECK(!x["on"].toBool());
                CHECK(x["to_s"].toDouble() > 29.9);
                for (const QJsonValue& y : list) // never over another suggestion
                    if (y != x) CHECK(!(y["from_s"].toDouble() < x["to_s"].toDouble() - 0.01 && x["from_s"].toDouble() < y["to_s"].toDouble() - 0.01));
            }
            if (!CHECK(extraAfter && tail)) std::printf("       %s\n", QJsonDocument(list).toJson().constData());
        }
        // A weaker loud moment just after the picked one (too close for the spacing): the pick grows towards it, the
        // weaker one stays offered (switched off), cut to what the grown pick leaves free - never the same footage twice
        const QString near = Check::makeMedia(
            dir.filePath("near.mp4"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=30", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,13,13.6),0.9*sin(2*PI*440*t),0)+if(between(t,19.5,20.1),0.5*sin(2*PI*440*t),0)':d=30:s=48000",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!near.isEmpty())) {
            const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{near}},
                                                 {"options", QJsonObject{{"length_s", 300}, {"game", "none"}}}});
            const QJsonArray list = r["session"].toObject()["suggestions"].toArray();
            bool offered = false;
            for (const QJsonValue& x : list) {
                if (x["reason"]["key"].toString() == "fallback_end") continue;
                if (!x["on"].toBool() && x["from_s"].toDouble() <= 19.6 && x["to_s"].toDouble() >= 19.6) offered = true;
                for (const QJsonValue& y : list) // no two suggestions share footage
                    if (y != x) CHECK(!(y["from_s"].toDouble() < x["to_s"].toDouble() - 0.01 && x["from_s"].toDouble() < y["to_s"].toDouble() - 0.01));
            }
            if (!CHECK(offered)) std::printf("       %s\n", QJsonDocument(list).toJson().constData());
        }
        // Translated on the page: the key's German text with the values
        CHECK(Direction::noteText("few_moments", {{"s", 4}, {"target", 60}}, Direction::Lang::Ui).contains("60"));
    }
    // ---- s264's speed stage: `render` speed fast (only for s264)
    {
        const QString sp = dir.filePath("speed.schneidi");
        run("new", {{"project", sp}, {"width", 320}, {"height", 180}, {"fps", 30}});
        run("edit", {{"project", sp}, {"ops", QJsonArray{QJsonObject{{"op", "place"}, {"media", clip}, {"at", 0}}}}});
        CHECK_EQ(run("render", {{"project", sp}, {"out", dir.filePath("x.mp4")}, {"format", "h264"}, {"speed", "fast"}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        CHECK_EQ(run("render", {{"project", sp}, {"out", dir.filePath("x.mp4")}, {"format", "s264"}, {"speed", "turbo"}})["error"].toString(),
                 QString("BAD_ARGUMENT"));
        const QJsonObject fast = run("render", {{"project", sp}, {"out", dir.filePath("fast.mp4")}, {"format", "s264"},
                                                {"speed", "fast"}, {"from", "0s"}, {"to", "1s"}});
        CHECK(!fast.contains("error"));
        CHECK(QFileInfo(dir.filePath("fast.mp4")).size() > 0);
        CHECK_EQ(fast["s264_speed"].toString(), QString("fast")); // reached the encoder's configuration
        const QJsonObject plain = run("render", {{"project", sp}, {"out", dir.filePath("plain.mp4")}, {"format", "s264"},
                                                 {"from", "0s"}, {"to", "1s"}});
        CHECK_EQ(plain["s264_speed"].toString(), QString("default"));
        CHECK(!run("render", {{"project", sp}, {"out", dir.filePath("h.mp4")}, {"format", "h264"}, {"from", "0s"}, {"to", "1s"}})
                   .contains("s264_speed"));
    }

    // ---- Cancel: a running suggest stops with CANCELED; an apply canceled before it starts leaves no project
    {
        const QString fresh = holiday(dir.path(), "fresh", 30, 9);
        std::atomic<bool> stop{false};
        std::thread canceler([&stop] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop = true;
        });
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{fresh}}}, &stop);
        canceler.join();
        CHECK_EQ(r["error"].toString(), QString("CANCELED"));
        const QJsonObject s = run("direct", {{"recipe", "short"}, {"files", QJsonArray{beach}}})["session"].toObject();
        std::atomic<bool> already{true};
        const QString target = dir.filePath("out/never.schneidi");
        const QJsonObject a = run("direct", {{"session", s["id"]}, {"apply", QJsonObject{{"on", "all"}}}}, &already);
        CHECK_EQ(a["error"].toString(), QString("CANCELED"));
        CHECK(!QFileInfo::exists(AppPaths::projectFolder() + "/beach-short.schneidi"));
        Q_UNUSED(target);
    }

    // ---- Tempo: suggestions for 10 clips, cold and warm
    {
        const double len = qEnvironmentVariableIntValue("DIRECT_MEASURE") ? 30 : 12;
        QJsonArray ten;
        for (int i = 0; i < 10; ++i) ten << holiday(dir.path(), QString("ten%1").arg(i), len, 20 + i);
        QElapsedTimer t;
        t.start();
        const QJsonObject cold = run("direct", {{"recipe", "highlights"}, {"files", ten}});
        const qint64 coldMs = t.restart();
        const QJsonObject warm = run("direct", {{"recipe", "highlights"}, {"files", ten}});
        const qint64 warmMs = t.elapsed();
        CHECK(!cold.contains("error") && !warm.contains("error"));
        CHECK(warmMs * 3 < coldMs + 300); // the caches make it much quicker
        std::printf("       10 clips of %.0f s: cold %lld ms, warm %lld ms\n", len, coldMs, warmMs);
    }
    return Check::result();
}
