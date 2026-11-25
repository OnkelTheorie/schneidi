// SPDX-License-Identifier: AGPL-3.0-only
// `direct` (0.25): recipe short and highlights suggest -> apply on synthetic recordings (lavfi picture, synthetic
// sounds): suggestions change nothing, apply is one edit with the project's "direction" set; material that is no game
// gives moments from laughter, cheering and movement; sessions by id or object, expired and stale ones refused; a
// canceled suggest or apply fails with CANCELED and leaves no half project. Prints the suggest time of 10 clips cold
// and warm (DIRECT_MEASURE=1 makes them 30 s long as in the plan's target).
#include "cli/Commands.h"
#include "cli/DirectDetail.h"
#include "engine/AnalysisCache.h"
#include "core/AppPaths.h"
#include "core/Direction.h"
#include "core/Vocabulary.h"
#include "engine/Extensions.h"
#include "cli/Live.h"
#include "cli/Sandbox.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TranscriptCut.h"

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
#include <QPainter>
#include <QSet>
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
        return {{"error", e.code}, {"message", e.message}};
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
    {   // The id carries a tag: the number alone (another folder's "d1") or another tag finds nothing
        const QString id = session["id"].toString();
        CHECK(QRegularExpression("^d[1-9][0-9]*-[0-9a-f]{4}$").match(id).hasMatch());
        const QString number = id.section('-', 0, 0);
        const QString other = number + (id.endsWith("0000") ? "-0001" : "-0000");
        for (const QString& wrong : {number, other})
            CHECK_EQ(run("direct", {{"session", wrong}, {"apply", QJsonObject{{"on", "all"}}}})["error"].toString(),
                     QString("SESSION_EXPIRED"));
    }
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
            // A replay clip of up to 60 s: its end is a moment of its own (25.11.; was the fallback of nothing found)
            CHECK(gs[0]["reason"]["key"].toString().startsWith("clip_end"));
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

    // ---- A game recording with voice chat (Mix, System Output, Discord): the cut keeps the game sound and says on
    // its own which streams it left out (videotest 21.11.: only inside one long `done` line)
    {
        const QString burst = "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,6,6.6),0.9*sin(2*PI*440*t),0)':d=12:s=48000";
        const QString chat = Check::makeMedia(
            dir.filePath("clips/chat.mkv"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=12", "-f", "lavfi", "-i", burst, "-f", "lavfi", "-i", burst,
             "-f", "lavfi", "-i", burst, "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:a",
             "-metadata:s:a:0", "title=Mix", "-metadata:s:a:1", "title=System Output", "-metadata:s:a:2", "title=Discord",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QString quiet = Check::makeMedia(dir.filePath("clips/quiet.mp4"),
                                               {"-f", "lavfi", "-i", "color=c=gray:s=320x180:r=30:d=6", "-f", "lavfi", "-i",
                                                "aevalsrc='0.01*sin(2*PI*100*t)':d=6:s=48000", "-c:v", "libx264",
                                                "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QJsonObject r = run("direct", {{"recipe", "highlights"}, {"files", QJsonArray{chat, quiet}},
                                             {"options", QJsonObject{{"game", "none"}, {"length_s", 8}}}});
        const QJsonObject made = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                {"project_dir", dir.filePath("out/chat")}});
        CHECK(!made.contains("error"));
        // The new project holds the recordings of the cut only (the quiet one has no moment in it; videotest 21.11.)
        {
            ProjectData d;
            QString error;
            CHECK(ProjectFile::load(made["project"].toString(), &d, &error));
            QStringList paths;
            for (const MediaInfo& m : d.media) paths << m.path;
            CHECK(paths.contains(chat));
            CHECK(!paths.contains(quiet));
        }
        bool said = false;
        for (const QJsonValue& n : made["notes"].toArray())
            said |= n.toString().startsWith("left out of the sound") && n.toString().section(" - ", 0, 0).endsWith(": Discord (chat.mkv)");
        if (!CHECK(said)) std::printf("       %s\n", QJsonDocument(made).toJson().constData());
    }

    // ---- CS2's death screen (25.11.: a short showed the death and 5 s of spectating after the kills): the red tint
    // over the whole picture that fades into a black screen is a death, a black screen alone or a red picture is not
    {
        std::vector<std::array<double, 3>> rgb;
        const auto add = [&](double seconds, std::array<double, 3> c) {
            for (int i = 0; i < int(seconds * 10); ++i) rgb.push_back(c);
        };
        add(5, {130, 120, 110});  // game
        add(1.5, {120, 60, 60});  // tint at 5.0
        add(2, {2, 2, 2});        // black at 6.5
        add(3, {130, 120, 110});  // spectating
        add(2, {2, 2, 2});        // a black screen without a tint (a loading screen) at 11.5
        add(1, {130, 120, 110});
        add(1.2, {200, 60, 60});  // a red picture at 14.5 ... up to the end: the clip saved while the tint shows
        const QJsonArray deaths = Cli::detail::deathScreensOf(rgb, 10);
        if (CHECK_EQ(deaths.size(), 2)) {
            CHECK(std::abs(deaths[0]["s"].toDouble() - 5.0) < 0.05);
            CHECK(std::abs(deaths[0]["end_s"].toDouble() - 5.7) < 0.05); // 0.7 s after it, before the black
            CHECK(std::abs(deaths[1]["s"].toDouble() - 14.5) < 0.05);
        }
        rgb.resize(150); // without the tint at the end: only the first
        CHECK_EQ(Cli::detail::deathScreensOf(rgb, 10).size(), 1);
    }

    // ---- Where the game picture ends (the recorder stopped from the task bar: the desktop in the last seconds), in
    // pictures 10 a second over the last 5 s: a hard change to a still picture; the recorder's stop moving it once
    // more at the very end; the task bar coming up just before; a game going on (or cutting to another moving scene)
    // has none
    {
        const auto game = [](int i) { // a moving picture: a bright square crossing a gradient
            QImage img(160, 90, QImage::Format_RGB32);
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) img.setPixel(x, y, qRgb((x * 3 + i * 7) % 256, (y * 2 + i * 5) % 256, 90));
            QPainter(&img).fillRect((i * 9) % 140, 30, 20, 20, Qt::white);
            return img;
        };
        const auto desktop = [](int i) {
            QImage img(160, 90, QImage::Format_RGB32);
            img.fill(QColor(40, 44, 52));
            QPainter p(&img);
            p.fillRect(20, 10, 120, 70, QColor(200, 200, 205));
            p.fillRect(80 + i % 2, 50, 2, 3, Qt::black); // a blinking cursor
            return img;
        };
        const auto withBar = [](QImage img) {
            QPainter(&img).fillRect(0, 86, 160, 4, QColor(20, 20, 24));
            return img;
        };
        QVector<QImage> p;
        for (int i = 0; i < 50; ++i) p << (i < 32 ? game(i) : desktop(i));
        CHECK(std::abs(Cli::detail::gameEndOf(p, 25, 10) - 28.2) < 1e-6);
        // the recorder's stop changes the picture once more in the last 0.4 s
        for (int i = 46; i < 50; ++i) p[i] = game(i * 3);
        CHECK(std::abs(Cli::detail::gameEndOf(p, 25, 10) - 28.2) < 1e-6);
        // the task bar comes up 0.2 s before the window
        p[30] = withBar(game(30)), p[31] = withBar(game(31));
        CHECK(std::abs(Cli::detail::gameEndOf(p, 25, 10) - 28.0) < 1e-6);
        // only the last 0.3 s show the desktop
        p.clear();
        for (int i = 0; i < 50; ++i) p << (i < 47 ? game(i) : desktop(i));
        CHECK(std::abs(Cli::detail::gameEndOf(p, 25, 10) - 29.7) < 1e-6);
        // the game to the end, or a cut to another moving scene: none
        p.clear();
        for (int i = 0; i < 50; ++i) p << game(i);
        CHECK_EQ(Cli::detail::gameEndOf(p, 25, 10), -1.0);
        for (int i = 30; i < 50; ++i) p[i] = game(i).mirrored(true, true);
        CHECK_EQ(Cli::detail::gameEndOf(p, 25, 10), -1.0);
    }

    // ... and in a recording: no moment reaches into the desktop at its end (a loud moment right before it)
    {
        const QString tail = Check::makeMedia(
            dir.filePath("clips/cs2/cs2_clip_tail.mkv"),
            {"-f", "lavfi", "-i",
             "testsrc2=s=640x360:r=30:d=16[a];color=c=0x30343c:s=640x360:r=30:d=16,drawbox=x=100:y=40:w=440:h=280:"
             "c=0xc8c8cc:t=fill[d];[a][d]overlay=enable='gte(t,13.5)'",
             "-f", "lavfi", "-i", "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,12.6,13.0),0.8*sin(2*PI*440*t),0)':d=16:s=48000",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!tail.isEmpty())) {
            const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{tail}}});
            const QJsonObject session = r["session"].toObject();
            const double end = session["material"][0]["analysis"]["game_end_s"].toDouble(-1);
            if (!CHECK(std::abs(end - 13.5) < 0.15)) std::printf("       game end %.2f\n", end);
            const QJsonArray list = session["suggestions"].toArray();
            CHECK(!list.isEmpty());
            for (const QJsonValue& x : list)
                if (!CHECK(x["to_s"].toDouble() <= 13.5 + 0.15)) std::printf("       %s\n", QJsonDocument(x.toObject()).toJson().constData());
        }
    }

    // ---- A CS2 replay clip without a kill (25.11.: the second clip of a short was dropped as "no moment"): its end,
    // cut at the player's death (the tint, then the black screen; spectating after it); the microphone's laughter a
    // moment of its own from the sentence before it; voices heard in the short's moments are placed, the game ducked
    {
        const int rate = 48000;
        Synth::Builder mic(rate, 20.0, 7), discord(rate, 20.0, 8);
        mic.floor();
        Synth::speech(mic, 0.5, 1.5); // the sentence ...
        Synth::laughter(mic, 2.2);    // ... and the laughter after it
        discord.floor();
        const QString micWav = dir.filePath("clips/mic.wav"), discordWav = dir.filePath("clips/discord.wav");
        Synth::writeWav(micWav, mic.s, rate);
        Synth::writeWav(discordWav, discord.s, rate);
        const QString died = Check::makeMedia(
            dir.filePath("clips/cs2/cs2_clip_died.mkv"),
            {"-f", "lavfi", "-i",
             "testsrc2=s=640x360:r=30:d=20[a];color=c=0xC03030:s=640x360:r=30:d=20[r];color=c=black:s=640x360:r=30:d=20[k];"
             "[a][r]overlay=enable='between(t,14,15.5)'[x];[x][k]overlay=enable='between(t,15.5,18)'",
             "-f", "lavfi", "-i", "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,13,13.4),0.8*sin(2*PI*440*t),0)':d=20:s=48000",
             "-i", micWav, "-i", discordWav, "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:a",
             "-metadata:s:a:0", "title=System Output", "-metadata:s:a:1", "title=Mikrofon", "-metadata:s:a:2",
             "title=Discord", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!died.isEmpty())) {
            // (min_s 5: the two moments are not grown into one)
            const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{died}},
                                                 {"options", QJsonObject{{"min_s", 5}}}});
            const QJsonObject session = r["session"].toObject();
            const QJsonArray list = session["suggestions"].toArray();
            QJsonObject end, laugh;
            for (const QJsonValue& x : list) {
                if (x["reason"]["key"].toString() == QLatin1String("clip_end_death")) end = x.toObject();
                if (x["reason"]["key"].toString() == QLatin1String("voice_laughter")) laugh = x.toObject();
            }
            // The end: the fight before the death, up to 0.7 s after the tint began (14 s) - not the black screen
            if (!CHECK(!end.isEmpty())) std::printf("       %s\n", QJsonDocument(session).toJson().constData());
            CHECK(std::abs(end["to_s"].toDouble() - 14.7) < 0.15);
            CHECK(end["from_s"].toDouble() >= 8.9 && end["from_s"].toDouble() < 12);
            CHECK(end["labels"].toArray().contains("death"));
            CHECK(end["reason"].toObject()["text"].toString().contains("death"));
            // The laughter: from the sentence that leads into it (0.5 s), scored over the clip's end
            if (CHECK(!laugh.isEmpty())) {
                if (!CHECK(laugh["from_s"].toDouble() >= 0.3 && laugh["from_s"].toDouble() <= 0.8))
                    std::printf("       laughter from %.2f\n", laugh["from_s"].toDouble());
                CHECK(laugh["to_s"].toDouble() >= 3.4);
                CHECK(laugh["reason"].toObject()["text"].toString().contains("Mikrofon"));
                CHECK(laugh["score"].toDouble() > end["score"].toDouble());
                CHECK(laugh["on"].toBool());
            }
            // Where each voice stream is heard: the microphone, not the quiet Discord
            const QJsonArray voice = session["material"][0]["voice"].toArray();
            QStringList voiced;
            for (const QJsonValue& v : voice) voiced << v["name"].toString();
            CHECK(voiced.contains("Mikrofon") && !voiced.contains("Discord"));
            CHECK_EQ(session["material"][0]["analysis"]["death"].toString(), QString("done"));
            // Applied (audio_stream auto): the game sound and the microphone heard in the moments, the game ducked
            // under it; Discord left out (said)
            const QJsonObject made = run("direct", {{"session", session["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                    {"project_dir", dir.filePath("out/died")}});
            const QString doneText = QJsonDocument(made["done"].toArray()).toJson();
            if (!CHECK(doneText.contains("voices heard in the moments placed too") && doneText.contains("Mikrofon")))
                std::printf("       %s\n", qPrintable(doneText));
            const QString notesText = QJsonDocument(made["notes"].toArray()).toJson();
            CHECK(notesText.contains("left out of the sound") && notesText.contains("Discord"));
            {
                ProjectData d;
                QString error;
                CHECK(ProjectFile::load(made["project"].toString(), &d, &error));
                QStringList streams;
                bool ducked = false;
                for (const Track& t : d.timeline.audio)
                    for (const Clip& c : t.clips) {
                        const auto m = std::find_if(d.media.begin(), d.media.end(),
                                                    [&](const MediaInfo& x) { return x.path == c.mediaPath; });
                        if (m != d.media.end()) streams << m->audioStreamName(c.audioStream);
                        ducked |= m != d.media.end() && m->audioStreamName(c.audioStream) == QLatin1String("System Output")
                                  && !c.keys.value(AnimParam::Volume).isEmpty();
                    }
                CHECK(streams.contains("System Output") && streams.contains("Mikrofon") && !streams.contains("Discord"));
                CHECK(ducked);
            }
            // The session's stream names differ from the file's (an untitled stream's "Spur 2" made in another UI
            // language): the duck finds the tracks by the streams placed, not by their names - and never fails the cut
            {
                QJsonObject renamed = session;
                QJsonArray material = renamed["material"].toArray();
                QJsonObject m0 = material[0].toObject();
                QJsonArray names = m0["audio_streams"].toArray();
                if (CHECK(names.size() == 3)) names[1] = "Microphone";
                m0["audio_streams"] = names;
                QJsonArray voices = m0["voice"].toArray();
                for (int i = 0; i < voices.size(); ++i) {
                    QJsonObject v = voices[i].toObject();
                    if (v["stream"].toInt() == 1) v["name"] = "Microphone";
                    voices[i] = v;
                }
                m0["voice"] = voices;
                material[0] = m0;
                renamed["material"] = material;
                const QJsonObject r2 = run("direct", {{"session", renamed}, {"apply", QJsonObject{{"on", "all"}}},
                                                      {"project_dir", dir.filePath("out/died-renamed")}});
                CHECK(!r2.contains("error"));
                ProjectData d;
                QString error;
                bool ducked = false;
                if (CHECK(ProjectFile::load(r2["project"].toString(), &d, &error)))
                    for (const Track& t : d.timeline.audio)
                        for (const Clip& c : t.clips)
                            ducked |= c.audioStream == 0 && !c.keys.value(AnimParam::Volume).isEmpty();
                if (!CHECK(ducked)) {
                    std::printf("       %s\n", QJsonDocument(r2).toJson().constData());
                    for (const Track& t : d.timeline.audio)
                        for (const Clip& c : t.clips)
                            std::printf("       track '%s' stream %d keys %d\n", qPrintable(t.name), c.audioStream,
                                        int(c.keys.value(AnimParam::Volume).size()));
                }
            }
            // audio_stream 'game': only the game sound, as before
            const QJsonObject gameOnly = run("direct", {{"recipe", "short"}, {"files", QJsonArray{died}},
                                                        {"options", QJsonObject{{"audio_stream", "game"}}}});
            const QJsonObject madeGame = run("direct", {{"session", gameOnly["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                        {"project_dir", dir.filePath("out/died-game")}});
            CHECK(!QJsonDocument(madeGame["done"].toArray()).toJson().contains("voices heard"));
            CHECK(QJsonDocument(madeGame["notes"].toArray()).toJson().contains("Mikrofon"));

            // Accents: a mild zoom punch on each hit of the short's moments (the session's hit_s), none with 'off'
            const auto zoomKeys = [](const QString& project) {
                ProjectData d;
                QString error;
                ProjectFile::load(project, &d, &error);
                int n = 0;
                for (const Track& t : d.timeline.video)
                    for (const Clip& c : t.clips) n += int(c.keys.value(AnimParam::ZoomX).size());
                return n;
            };
            QJsonObject withHits = session;
            QJsonArray sug = withHits["suggestions"].toArray();
            for (int i = 0; i < sug.size(); ++i) {
                QJsonObject x = sug[i].toObject();
                if (x["on"].toBool()) x["hit_s"] = QJsonArray{x["from_s"].toDouble() + 1.0};
                sug[i] = x;
            }
            withHits["suggestions"] = sug;
            const QJsonObject punched = run("direct", {{"session", withHits}, {"apply", QJsonObject{{"on", "all"}}},
                                                       {"project_dir", dir.filePath("out/punch")}});
            CHECK(QJsonDocument(punched["done"].toArray()).toJson().contains("accents: a mild zoom punch on"));
            // Two moments moved into each other become one: the hits of both keep their punch
            {
                QJsonArray on;
                for (const QJsonValue& x : sug)
                    if (x["on"].toBool()) on << x;
                if (CHECK(on.size() >= 2)) {
                    const QJsonObject first = on[0].toObject(), second = on[1].toObject();
                    const double from = std::min(first["from_s"].toDouble(), second["from_s"].toDouble());
                    const double to = std::max(first["to_s"].toDouble(), second["to_s"].toDouble());
                    const QJsonObject merged = run(
                        "direct", {{"session", withHits},
                                   {"apply", QJsonObject{{"on", "all"},
                                                         {"adjust", QJsonArray{QJsonObject{{"n", first["n"]}, {"from_s", from},
                                                                                           {"to_s", to}}}}}},
                                   {"project_dir", dir.filePath("out/punch-merged")}});
                    const QString text = QJsonDocument(merged["done"].toArray()).toJson();
                    if (!CHECK(text.contains(QString("zoom punch on %1 hits").arg(on.size()))))
                        std::printf("       %s\n", qPrintable(text));
                }
            }
            QJsonObject options = withHits["options"].toObject();
            options["accents"] = "off";
            withHits["options"] = options;
            const QJsonObject calm = run("direct", {{"session", withHits}, {"apply", QJsonObject{{"on", "all"}}},
                                                    {"project_dir", dir.filePath("out/calm")}});
            CHECK(!QJsonDocument(calm["done"].toArray()).toJson().contains("accents"));
            const int with = zoomKeys(punched["project"].toString()), without = zoomKeys(calm["project"].toString());
            if (!CHECK(with > without)) std::printf("       zoom keys %d vs %d\n", with, without);
            CHECK_EQ(run("direct", {{"recipe", "short"}, {"files", QJsonArray{died}},
                                    {"options", QJsonObject{{"accents", "flash"}}}})["error"].toString(),
                     QString("BAD_ARGUMENT"));
        }
    }

    // ---- Two voices in a game recording, the voice chat partner 9 dB under the microphone: brought to one level (the
    // quieter one raised, said in done) before the game sound is ducked under them
    {
        const int rate = 48000;
        Synth::Builder mic(rate, 12.0, 21), chat(rate, 12.0, 22);
        mic.floor();
        chat.floor();
        Synth::speech(mic, 3.0, 3.0);
        Synth::speech(chat, 4.5, 3.0, 160);
        for (float& v : chat.s) v *= 0.355f; // -9 dB
        const QString micWav = dir.filePath("clips/level-mic.wav"), chatWav = dir.filePath("clips/level-chat.wav");
        Synth::writeWav(micWav, mic.s, rate);
        Synth::writeWav(chatWav, chat.s, rate);
        const QString two = Check::makeMedia(
            dir.filePath("clips/cs2/cs2_clip_two.mkv"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=12", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,5,5.4),0.8*sin(2*PI*440*t),0)':d=12:s=48000", "-i", micWav, "-i",
             chatWav, "-map", "0:v", "-map", "1:a", "-map", "2:a", "-map", "3:a", "-metadata:s:a:0", "title=System Output",
             "-metadata:s:a:1", "title=Mikrofon", "-metadata:s:a:2", "title=Discord", "-c:v", "libx264", "-preset",
             "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!two.isEmpty())) {
            const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{two}}});
            const QJsonObject made = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}},
                                                    {"project_dir", dir.filePath("out/two-voices")}});
            const QString doneText = QJsonDocument(made["done"].toArray()).toJson();
            if (!CHECK(doneText.contains("voices leveled") && doneText.contains("Discord +")))
                std::printf("       %s\n", qPrintable(doneText));
            ProjectData d;
            QString error;
            double micDb = -100, chatDb = -100;
            if (CHECK(ProjectFile::load(made["project"].toString(), &d, &error)))
                for (const Track& t : d.timeline.audio)
                    for (const Clip& c : t.clips) {
                        if (c.mediaPath != two) continue;
                        if (c.audioStream == 1) micDb = c.volumeDb;
                        if (c.audioStream == 2) chatDb = c.volumeDb;
                    }
            std::printf("       voices: microphone %+.1f dB, Discord %+.1f dB\n", micDb, chatDb);
            CHECK(std::abs(micDb) < 0.01);
            CHECK(chatDb > 6 && chatDb < 12.01);
            // The captions of the short: each voice transcribed on its own (here the transcripts are in the cache
            // already, as whisper would have left them), both in one list in time order, a speaker per word
            // (per part of the recording the short plays, keyed with the model that hears it: the default one)
            Cli::detail::Session shortView(made["project"].toString());
            shortView.selectTimeline(made["timeline"].toString());
            const Timeline shortTl = shortView.project.timeline();
            const double shortFps = shortView.project.frameRate();
            const auto inParts = [&](int stream, const QJsonArray& words, const auto& putOne) {
                for (const auto& [from, to] : TranscriptCut::playedRangesMs(shortTl, shortFps, -1, two, stream)) {
                    QJsonArray part;
                    for (const QJsonValue& w : words)
                        if (w[0].toDouble() >= from && w[0].toDouble() < to) part << w;
                    putOne(from, to, part);
                }
            };
            const auto put = [&](int stream, const QJsonArray& words) {
                inParts(stream, words, [&](qint64 from, qint64 to, const QJsonArray& part) {
                    AnalysisCache::put(Cli::detail::streamTranscriptKey(two, stream, "", false, {}, {}, from, to),
                                       QJsonObject{{"stream", stream}, {"language", "de"}, {"words", part}});
                });
            };
            put(1, QJsonArray{QJsonArray{4100, 4400, "Einer"}, QJsonArray{4450, 4800, "soll?"}});
            put(2, QJsonArray{QJsonArray{5600, 6000, "Heilige"}, QJsonArray{6050, 6500, "Scheiße."},
                              QJsonArray{7000, 7300, "Crazy."}});
            Cli::Context cctx;
            QStringList voices;
            const QJsonArray words = Cli::detail::voiceCaptionWords(made["project"].toString(), made["timeline"].toString(),
                                                                    "", cctx, &voices);
            QStringList said;
            QSet<int> speakers;
            for (const QJsonValue& w : words) {
                said << w["text"].toString();
                speakers << w["speaker"].toInt();
            }
            if (!CHECK_EQ(said.join(' '), QString("Einer soll? Heilige Scheiße. Crazy.")))
                std::printf("       voices %s\n", qPrintable(voices.join(", ")));
            CHECK(voices.contains("Mikrofon") && voices.contains("Discord") && speakers.size() == 2);
            for (int i = 1; i < words.size(); ++i) CHECK(words[i]["from"].toInt() >= words[i - 1]["from"].toInt());
            // With a game: its terms go to whisper, so the transcripts are other ones (another cache key; the ones
            // without a word list stay where they are)
            const QString id = Vocabulary::cacheId(Vocabulary::gameTerms("cs2"));
            CHECK(!id.isEmpty());
            const auto putTerms = [&](int stream, const QJsonArray& w) {
                inParts(stream, w, [&](qint64 from, qint64 to, const QJsonArray& part) {
                    AnalysisCache::put(Cli::detail::streamTranscriptKey(two, stream, "", false, Vocabulary::gameTerms("cs2"), {}, from, to),
                                       QJsonObject{{"stream", stream}, {"language", "de"}, {"words", part}});
                });
            };
            putTerms(1, QJsonArray{QJsonArray{4100, 4400, "Einer"}, QJsonArray{4450, 4800, "Smoke?"}});
            putTerms(2, QJsonArray{QJsonArray{5600, 6000, "Headshot."}});
            // The game's terms are off by default (setting transcribe/gameTerms): the old key, the old transcripts;
            // switched on (setting or the call's game_terms) another key
            const auto said2 = [&](bool given, const QJsonValue& call) {
                Vocabulary::setGameTermsOn(given);
                Cli::detail::ShortFinish f;
                f.game = "cs2";
                f.gameTerms = Vocabulary::useGameTerms(call);
                QStringList gameVoices, out;
                for (const QJsonValue& w : Cli::detail::voiceCaptionWords(made["project"].toString(), made["timeline"].toString(),
                                                                         "", cctx, &gameVoices, Cli::detail::captionTermsGame(f)))
                    out << w["text"].toString();
                return out.join(' ');
            };
            CHECK(!Vocabulary::gameTermsOn());
            CHECK_EQ(said2(false, {}), QString("Einer soll? Heilige Scheiße. Crazy."));
            CHECK_EQ(said2(true, {}), QString("Einer Smoke? Headshot."));
            CHECK_EQ(said2(true, false), QString("Einer soll? Heilige Scheiße. Crazy."));
            CHECK_EQ(said2(false, true), QString("Einer Smoke? Headshot."));
            Vocabulary::setGameTermsOn(false);
            // Two models must agree: the second model's transcripts in the cache too (its file name in the key, the
            // language the first one heard); a phrase heard differently is left out, with both readings
            const QString checkModel = "/nonexistent/ggml-check.bin";
            const auto putCheck = [&](int stream, const QJsonArray& w) {
                inParts(stream, w, [&](qint64 from, qint64 to, const QJsonArray& part) {
                    AnalysisCache::put(Cli::detail::streamTranscriptKey(two, stream, "de", false, {}, checkModel, from, to),
                                       QJsonObject{{"stream", stream}, {"language", "de"}, {"words", part}});
                });
            };
            putCheck(1, QJsonArray{QJsonArray{4120, 4800, "einer soll"}});                       // the same
            putCheck(2, QJsonArray{QJsonArray{5610, 6000, "Heilige"}, QJsonArray{6040, 6500, "Scheisse."}, // spelt otherwise
                                   QJsonArray{7010, 7300, "Lazy."}});                               // another word
            const auto checked = [&](const QString& mode, const QString& model, Cli::detail::VoiceCaptions* v) {
                v->check = mode;
                v->checkModel = model;
                QStringList out;
                for (const QJsonValue& w : Cli::detail::voiceCaptionWords(made["project"].toString(), made["timeline"].toString(), cctx, v))
                    out << w["text"].toString();
                return out.join(' ');
            };
            Cli::detail::VoiceCaptions on;
            CHECK_EQ(checked("on", checkModel, &on), QString("Einer soll? Heilige Scheiße."));
            CHECK_EQ(on.checkedWith, QString("ggml-check.bin"));
            if (CHECK_EQ(on.leftOut.size(), 1)) {
                const QJsonObject l = on.leftOut[0].toObject();
                CHECK_EQ(l["heard"].toString(), QString("Crazy."));
                CHECK_EQ(l["other"].toString(), QString("Lazy."));
                CHECK_EQ(l["speaker"].toString(), QString("Discord"));
                CHECK(l["from_s"].toDouble() > 0 && l["to_s"].toDouble() > l["from_s"].toDouble());
            }
            Cli::detail::VoiceCaptions off; // option off: every line, nothing checked
            CHECK_EQ(checked("off", checkModel, &off), QString("Einer soll? Heilige Scheiße. Crazy."));
            CHECK(off.checkedWith.isEmpty() && off.leftOut.isEmpty() && off.skipped.isEmpty());
            if (Extensions::installedWhisperModels().size() < 2) { // no second model installed: no check, said why
                Cli::detail::VoiceCaptions none;
                CHECK_EQ(checked("auto", {}, &none), QString("Einer soll? Heilige Scheiße. Crazy."));
                CHECK_EQ(none.skipped, QString("no_model"));
            }
            // The notes the short and direct finish give
            CHECK(Direction::noteText("captions_left_out").contains("captions_left_out"));
            CHECK(!Direction::noteText("captions_check_no_model").isEmpty());
            CHECK(!Direction::noteText("captions_check_slow").isEmpty());
            // The setting: on (auto) by default, the call's value wins
            CHECK_EQ(Vocabulary::captionsCheck({}), QString("auto"));
            CHECK_EQ(Vocabulary::captionsCheck("on"), QString("on"));
            Vocabulary::setCaptionsCheckOn(false);
            CHECK_EQ(Vocabulary::captionsCheck({}), QString("off"));
            CHECK_EQ(Vocabulary::captionsCheck("auto"), QString("auto"));
            Vocabulary::setCaptionsCheckOn(true);
        }
    }

    // ---- A voice getting loud in the voice chat (high and loud, not laughter): named as voices getting loud, not as
    // a scream - on a gated microphone (silence around every word) a loud exclamation and a scream look the same
    {
        const int rate = 48000;
        Synth::Builder mic(rate, 12.0, 11);
        mic.floor();
        Synth::scream(mic, 5.0, 1.2);
        const QString micWav = dir.filePath("clips/loud-mic.wav");
        Synth::writeWav(micWav, mic.s, rate);
        const QString loud = Check::makeMedia(
            dir.filePath("clips/cs2/cs2_clip_loud.mkv"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=12", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)':d=12:s=48000", "-i", micWav, "-map", "0:v", "-map", "1:a", "-map", "2:a",
             "-metadata:s:a:0", "title=System Output", "-metadata:s:a:1", "title=Mikrofon", "-c:v", "libx264",
             "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (CHECK(!loud.isEmpty())) {
            const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{loud}}});
            QJsonObject voice;
            for (const QJsonValue& x : r["session"]["suggestions"].toArray())
                if (x.toObject()["reason"].toObject()["key"].toString() == QLatin1String("voice_shout")) voice = x.toObject();
            if (CHECK(!voice.isEmpty())) {
                CHECK(voice["labels"].toArray().contains("loud") && !voice["labels"].toArray().contains("scream"));
                const QString text = voice["reason"].toObject()["text"].toString();
                if (!CHECK(text.contains("voices get loud"))) std::printf("       %s\n", qPrintable(text));
            } else {
                std::printf("       %s\n", QJsonDocument(r["session"]["suggestions"].toArray()).toJson().constData());
            }
        }
    }

    // ---- A game recording that does not name its game ("Desktop_clip_…"): CS2 recognised in the picture (kill feed
    // lines staying, the player's own kills in their red frame) and said; the same boxes without a kill's frame
    // (a racing game's fixed HUD boxes at the top right) are no game
    {
        const auto line = [](int y, const QString& when, bool own) {
            QString s = QString("drawbox=x=1640:y=%1:w=270:h=31:c=0x181818@0.7:t=fill:enable='%2'").arg(y).arg(when);
            if (own) s += QString(",drawbox=x=1640:y=%1:w=270:h=31:c=0xd8444a:t=1:enable='%2'").arg(y).arg(when);
            for (int x = 1660; x < 1890; x += 40)
                s += QString(",drawbox=x=%1:y=%2:w=25:h=12:c=white:t=fill:enable='%3'").arg(x).arg(y + 10).arg(when);
            return s;
        };
        const QString back = "gradients=s=1920x1080:c0=0x587898:c1=0x403020:d=12:r=25:speed=0.01";
        const QString desktop = Check::makeMedia(
            dir.filePath("clips/Desktop_clip_cs.mkv"),
            {"-f", "lavfi", "-i", back + "," + line(75, "between(t,2,7.5)", true) + "," + line(109, "between(t,5,10.5)", true),
             "-f", "lavfi", "-i", "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,2,2.3)+between(t,5,5.3),0.8*sin(2*PI*440*t),0)':d=12:s=48000",
             "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "aac", "-shortest"});
        const QString racing = Check::makeMedia(
            dir.filePath("clips/Desktop_clip_race.mkv"),
            {"-f", "lavfi", "-i", back + "," + line(75, "gte(t,0)", false) + "," + line(109, "gte(t,0)", false), "-f",
             "lavfi", "-i", "aevalsrc='0.02*sin(2*PI*220*t)':d=12:s=48000", "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "aac",
             "-shortest"});
        if (CHECK(!desktop.isEmpty() && !racing.isEmpty())) {
            Cli::Context ctx;
            CHECK_EQ(Cli::detail::gameInPicture({desktop}, ctx), QString("cs2"));
            CHECK_EQ(Cli::detail::gameInPicture({racing}, ctx), QString());
            const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{desktop}}});
            CHECK_EQ(r["session"]["options"]["game"].toString(), QString("cs2"));
            if (!CHECK(QJsonDocument(r["session"]["notes"].toArray()).toJson().contains("recognised in the picture")))
                std::printf("       %s\n", QJsonDocument(r["session"]["notes"].toArray()).toJson().constData());
            const QJsonObject other = run("direct", {{"recipe", "short"}, {"files", QJsonArray{racing}}});
            CHECK(!other["session"]["options"].toObject().contains("game"));
        }
    }

    // ---- The short's hook starts on the action: a hard cut in its lead-in (CS2: the round restarts after the
    // round-end screen) moves its start there (videotest 21.11.: the hook began on "RUNDE VERLOREN")
    {
        const QString cutClip = Check::makeMedia(
            dir.filePath("clips/round.mp4"),
            {"-f", "lavfi", "-i",
             "color=c=red:s=320x180:r=30:d=20[a];testsrc2=s=320x180:r=30:d=20[b];[a][b]overlay=enable='gte(t,8.5)'",
             "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,9,9.6),0.9*sin(2*PI*440*t),0)':d=20:s=48000",
             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        const QJsonObject r = run("direct", {{"recipe", "short"}, {"files", QJsonArray{cutClip}},
                                             {"options", QJsonObject{{"game", "none"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        const QJsonObject hook = list.isEmpty() ? QJsonObject{} : list[0].toObject();
        // (the lead-in is 1.5 s before the first loud peak since 25.11.: the cut lies inside it at 8.5 s)
        if (!CHECK(std::abs(hook["from_s"].toDouble() - 8.5) < 0.1)) std::printf("       from %.3f\n", hook["from_s"].toDouble());
        CHECK(hook["labels"].toArray().contains("hook_trimmed"));
        CHECK(hook["to_s"].toDouble() > 9.6);
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
            // The reason names the pause as long as it is (with its pads), the cut part separately (videotest 21.11.)
            const double len = v["to_s"].toDouble() - v["from_s"].toDouble();
            CHECK(v["reason"]["values"]["s"].toDouble() >= len + 0.15 - 0.02);
            CHECK(std::abs(v["reason"]["values"]["cut"].toDouble() - len) < 0.02);
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
    // ---- Options typed (review 25.11.): seconds as text ("0.6s") mean what the number means (were 0: every 1-frame
    // pause cut), a misspelt option is refused naming the known ones, text that is no number is refused
    {
        const QString talk = twoMics(dir.path());
        const QJsonObject num = run("direct", {{"recipe", "talk"}, {"project", talk},
                                               {"options", QJsonObject{{"min_pause_s", 0.6}, {"pad_s", 0.15}, {"tracks", "A1,A2"}}}});
        const QJsonObject text = run("direct", {{"recipe", "talk"}, {"project", talk},
                                                {"options", QJsonObject{{"min_pause_s", "0.6s"}, {"pad_s", "0.15"}, {"tracks", "A1,A2"}}}});
        CHECK(!text.contains("error"));
        CHECK_EQ(text["session"]["suggestions"].toArray().size(), num["session"]["suggestions"].toArray().size());
        CHECK_EQ(text["session"]["options"]["min_pause_s"].toDouble(), 0.6);
        const QJsonObject typo = run("direct", {{"recipe", "talk"}, {"project", talk}, {"options", QJsonObject{{"min_pause", 0.6}}}});
        CHECK_EQ(typo["error"].toString(), QString("BAD_ARGUMENT"));
        CHECK(typo["message"].toString().contains("min_pause_s"));
        const QJsonObject bad = run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}}, {"options", QJsonObject{{"min_s", "fifteen"}}}});
        CHECK_EQ(bad["error"].toString(), QString("BAD_ARGUMENT"));
        CHECK(bad["message"].toString().contains("min_s"));
        CHECK_EQ(run("direct", {{"recipe", "short"}, {"files", QJsonArray{clip}}, {"options", QJsonObject{{"variants", "two"}}}})["error"]
                     .toString(), QString("BAD_ARGUMENT"));
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
        // Captions without whisper (not installed here): the note has its fixed key, the page shows it translated
        // (review 21.11.: English text in the German UI)
        const QJsonObject noCaptions = run("direct", {{"project", fp},
                                                      {"finish", QJsonObject{{"timeline", made["timeline"]}, {"captions", "karaoke"},
                                                                             {"session", session}, {"prepare", true}}}});
        if (CHECK_EQ(noCaptions["note_keys"].toArray().size(), noCaptions["notes"].toArray().size())) {
            CHECK_EQ(noCaptions["note_keys"][0].toString(), QString("no_captions_whisper"));
            CHECK_EQ(noCaptions["notes"][0].toString(), Direction::noteText("no_captions_whisper"));
            CHECK(Direction::noteText("no_captions_whisper", {}, Direction::Lang::Ui) != Direction::noteText("no_captions_whisper"));
        }
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
        // talk with fillers on recordings without a transcript: whisper writes one (verbatim) while suggesting, the
        // apply keeps it in the project (videotest 21.11.: "no transcript" after a `transcribe` of the project)
        {
            const QString talk3 = twoMics(dir.path());
            const QJsonObject f = run("direct", {{"recipe", "talk"}, {"project", talk3},
                                                 {"options", QJsonObject{{"fillers", true}, {"tracks", "A1,A2"}}}});
            bool none = false, some = false;
            for (const QJsonValue& n : f["session"]["notes"].toArray()) {
                none |= n["key"].toString() == "no_transcript_fillers";
                some |= n["key"].toString() == "fillers_transcript";
            }
            CHECK(!none && some);
            const QJsonObject applied = run("direct", {{"session", f["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
            CHECK(!applied.contains("error"));
            ProjectData d;
            QString error;
            CHECK(ProjectFile::load(talk3, &d, &error));
            bool kept = false;
            for (const MediaInfo& m : d.media) kept |= !m.transcripts.isEmpty();
            CHECK(kept);
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
        // The same in a work folder (`mcp --root`, review 21.11.): the scratch project lies inside the root (a system
        // temporary folder was OUTSIDE_ROOT) and is gone afterwards
        {
            Cli::Sandbox::configure({dir.path(), {}, false});
            const QJsonObject rooted = run("direct", {{"recipe", "script"}, {"files", QJsonArray{beach}},
                                                      {"options", QJsonObject{{"script", "Hallo."}}}});
            Cli::Sandbox::configure({});
            CHECK(!rooted.contains("error"));
            CHECK(!rooted["session"]["suggestions"].toArray().isEmpty());
            CHECK(QDir(dir.path()).entryList({"schneidi-script-*"}, QDir::AllEntries | QDir::Hidden).isEmpty());
        }
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
                                             {"options", QJsonObject{{"template", "short-hook"}, {"game", "none"}}}});
        const QJsonArray list = r["session"]["suggestions"].toArray();
        if (CHECK(!list.isEmpty())) {
            CHECK_EQ(list[0]["kind"].toString(), QString("slot"));
            CHECK_EQ(list[0]["slot"].toString(), QString("clip1"));
            // The fields get the material's moments, not the first seconds of a recording (videotest 21.11.): the
            // strongest moment ("the strongest moment first") the best one found, as long as the field (4 s)
            CHECK_EQ(list[0]["reason"]["key"].toString(), QString("loud"));
            bool burst = false; // the clip's loud burst at 29 s is in a field
            for (const QJsonValue& x : list) {
                CHECK(x["score"].toDouble() <= list[0]["score"].toDouble());
                burst |= x["file"].toString() == clip && x["from_s"].toDouble() <= 29.0 && x["to_s"].toDouble() >= 29.6;
            }
            CHECK(burst);
            for (const QJsonValue& x : list) {
                CHECK(x["to_s"].toDouble() - x["from_s"].toDouble() <= 4.05);
                CHECK(!x["reason"]["text"].toString().isEmpty());
                for (const QJsonValue& y : list) // never the same part of a recording twice
                    if (x["n"] != y["n"] && x["file"] == y["file"])
                        CHECK(x["to_s"].toDouble() <= y["from_s"].toDouble() || y["to_s"].toDouble() <= x["from_s"].toDouble());
            }
        }
        // The text field is named, not silently left (filled on apply with values)
        bool kept = false;
        for (const QJsonValue& n : r["session"]["notes"].toArray())
            kept |= n["key"].toString() == "fields_kept" && n["text"].toString().contains("hook");
        CHECK(kept);
        const QString filled = dir.filePath("out/from-template.schneidi");
        const QJsonObject made = run("direct", {{"session", r["session"]["id"]},
                                                {"apply", QJsonObject{{"on", "all"}, {"values", QJsonObject{{"hook", "Watch this"}}}}},
                                                {"project", filled}});
        CHECK(!made.contains("error"));
        CHECK(QFileInfo::exists(filled));
        CHECK(!made["result"]["unfilled"].toArray().contains("hook"));
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
        // A work folder (`mcp --root`, review 21.11.): without project/project_dir the new project goes into the root,
        // not into the folder for projects outside it
        Cli::Sandbox::configure({dir.path(), {}, false});
        const QJsonObject rooted = run("direct", {{"session", r["session"]["id"]}, {"apply", QJsonObject{{"on", "all"}}}});
        Cli::Sandbox::configure({});
        if (CHECK(!rooted.contains("error"))) {
            CHECK_EQ(QFileInfo(rooted["project"].toString()).absolutePath(), QFileInfo(dir.path()).absoluteFilePath());
            CHECK(QFileInfo::exists(rooted["project"].toString()));
        }
        // templates fill resolves out_project itself (called by direct without the parameter table): a relative
        // path starts in the root, a path outside it is refused
        Cli::Sandbox::configure({dir.filePath("clips"), {}, false});
        const Cli::Command* tc = Cli::find("templates");
        Cli::Context tctx;
        QString code;
        try {
            Cli::execute(*tc, {{"fill", "short-hook"}, {"out_project", dir.filePath("outside.schneidi")}, {"dry_run", true}}, tctx);
        } catch (const Cli::Error& e) {
            code = e.code;
        }
        Cli::Sandbox::configure({});
        CHECK_EQ(code, QString("OUTSIDE_ROOT"));
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
        // (clearly weaker: at 0.5 the true sound - lossless, or FFmpeg 9.1's AAC - scores it over 80 % of the loud
        // one, so it came in as a scene of its own; only 7.1's AAC overshoot at the end of the loud tone kept it out)
        const QString near = Check::makeMedia(
            dir.filePath("near.mp4"),
            {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=30:d=30", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,13,13.6),0.9*sin(2*PI*440*t),0)+if(between(t,19.5,20.1),0.3*sin(2*PI*440*t),0)':d=30:s=48000",
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
