// SPDX-License-Identifier: AGPL-3.0-only
// Cancelling analyses and running them in the background (Context::cancel, Live::runInBackground) and the per-file
// analysis cache (engine/AnalysisCache): highlights of two files, silence, index and transcribe stop with CANCELED soon
// after cancel() and leave nothing half done (no cache entry, no thread); the event loop keeps running meanwhile and
// onProgress/onDone arrive on the UI thread; one job per file at a time; refusals; "@app" from a copy; a second run
// comes from the cache in a fraction of the time and a changed file is measured again.
// whisper itself is not needed: this program copies itself as "whisper-cli" and then works (sleeps) like a long run.
#include "check.h"

#include "cli/AnalysisDetail.h"
#include "cli/Commands.h"
#include "cli/Live.h"
#include "core/AppPaths.h"
#include "core/ProjectFile.h"
#include "engine/AnalysisCache.h"
#include "engine/MediaIndex.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <thread>

namespace {

// --- The fake whisper-cli: works FAKE_WHISPER_MS, then writes one word
int fakeWhisper(int argc, char** argv)
{
    QString out;
    for (int i = 1; i + 1 < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == "-of") out = QString::fromLocal8Bit(argv[i + 1]);
    QThread::msleep(qEnvironmentVariableIntValue("FAKE_WHISPER_MS"));
    const QJsonObject json{{"result", QJsonObject{{"language", "de"}}},
                           {"transcription", QJsonArray{QJsonObject{{"text", " hallo"},
                                                                    {"offsets", QJsonObject{{"from", 100}, {"to", 900}}}}}}};
    QFile o(out + ".json");
    if (!o.open(QIODevice::WriteOnly)) return 4;
    o.write(QJsonDocument(json).toJson());
    return 0;
}

QJsonObject run(const QString& name, const QJsonObject& raw, const std::atomic<bool>* cancel = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    ctx.cancel = cancel;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return {{"error", e.code}, {"message", e.message}};
    }
}

// A background job and what came back
struct Job {
    Cli::Live::JobHandle handle;
    QJsonObject reply;
    bool done = false;
    bool doneOnUiThread = false, progressOnUiThread = true;
    int progressCalls = 0;
    double lastProgress = 0;
    qint64 canceledAt = -1, doneAt = -1; // ms of `clock`
    bool doneInsideCall = false;
    int doneCalls = 0;
};

QElapsedTimer g_clock;

// Starts `command` in the background; cancelAt >= 0: cancel() once the progress reaches it
std::shared_ptr<Job> start(const QString& command, const QJsonObject& args, double cancelAt = -1,
                           LiveHost* host = nullptr, QStringList* events = nullptr, const QString& name = {})
{
    auto job = std::make_shared<Job>();
    bool inCall = true;
    QThread* ui = QThread::currentThread();
    job->handle = Cli::Live::runInBackground(
        command, args,
        [job, cancelAt, ui, events, name](double p, const QString&) {
            ++job->progressCalls;
            job->lastProgress = p;
            if (QThread::currentThread() != ui) job->progressOnUiThread = false;
            if (events) *events << name + ":progress";
            if (cancelAt >= 0 && p >= cancelAt && job->canceledAt < 0) {
                job->canceledAt = g_clock.elapsed();
                job->handle.cancel();
            }
        },
        [job, ui, &inCall, events, name](const QJsonObject& reply) {
            job->reply = reply;
            job->done = true;
            ++job->doneCalls;
            job->doneAt = g_clock.elapsed();
            job->doneOnUiThread = QThread::currentThread() == ui;
            job->doneInsideCall = inCall;
            if (events) *events << name + ":done";
        },
        host);
    inCall = false;
    return job;
}

// Runs the event loop until every job is done (at most `ms`)
bool wait(const QVector<std::shared_ptr<Job>>& jobs, int ms = 120000)
{
    QElapsedTimer t;
    t.start();
    const auto all = [&] {
        return std::all_of(jobs.begin(), jobs.end(), [](const auto& j) { return j->done; });
    };
    while (!all() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
    return all();
}

QString code(const QJsonObject& reply) { return reply["error"]["code"].toString(); }
// CANCELED, but the command had got to its end anyway (a loaded machine): what it stores may be there
bool late(const QJsonObject& reply) { return reply["error"]["late"].toBool(); }

int filesIn(const QString& dir)
{
    int n = 0;
    for (QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories); it.hasNext(); it.next()) ++n;
    return n;
}

} // namespace

int main(int argc, char** argv)
{
    if (QFileInfo(QString::fromLocal8Bit(argv[0])).baseName() == QLatin1String("whisper-cli")) return fakeWhisper(argc, argv);
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("cli_cancel");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    Check::initEngine();
    g_clock.start();

    // --- AnalysisCache on its own: stored, read back, a changed file is a miss
    QTemporaryDir dir;
    {
        QDir(AnalysisCache::folder()).removeRecursively(); // the test's own cache folder: from a clean start
        // An audio events cache of an older version: removed by the first use of the analysis cache
        const QString oldEvents = AppPaths::writable(QStandardPaths::CacheLocation) + "/audio-events/x.json";
        QDir().mkpath(QFileInfo(oldEvents).absolutePath());
        QFile(oldEvents).open(QIODevice::WriteOnly);
        CHECK(QFileInfo::exists(oldEvents));
        const QString f = dir.filePath("plain.bin");
        QFile w(f);
        CHECK(w.open(QIODevice::WriteOnly) && w.write("abc") == 3);
        w.close();
        const QJsonObject options{{"x", 1}};
        const auto keyOf = [](const QString& kind, const QString& file, const QJsonObject& o, int v) {
            return AnalysisCache::key(kind, file, o, v);
        };
        const auto get = [&](const QString& kind, const QString& file, const QJsonObject& o, int v) {
            return AnalysisCache::get(keyOf(kind, file, o, v));
        };
        CHECK(!get("test", f, options, 1));
        const int hitsBefore = AnalysisCache::hits();
        AnalysisCache::put(keyOf("test", f, options, 1), {{"v", 42}});
        CHECK_EQ((*get("test", f, options, 1))["v"].toInt(), 42);
        CHECK_EQ(AnalysisCache::hits(), hitsBefore + 1);
        CHECK(!get("test", f, {{"x", 2}}, 1)); // other options
        CHECK(!get("test", f, options, 2));    // other version
        CHECK(!get("other", f, options, 1));   // other kind
        QFile t(f);
        CHECK(t.open(QIODevice::ReadWrite));
        CHECK(t.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        t.close();
        CHECK(!get("test", f, options, 1)); // the file changed (modification time)
        CHECK(!get("test", dir.filePath("missing.bin"), options, 1));
        AnalysisCache::put(keyOf("test", dir.filePath("missing.bin"), options, 1), {{"v", 1}}); // nothing, no crash
        // The file overwritten between the key (a miss, measuring starts) and put: nothing is stored, neither under
        // the old nor under the new stamp
        {
            const AnalysisCache::Key before = keyOf("test", f, {{"y", 1}}, 1);
            CHECK(!AnalysisCache::get(before));
            QFile o(f);
            CHECK(o.open(QIODevice::WriteOnly | QIODevice::Truncate) && o.write("other content") == 13);
            o.close();
            AnalysisCache::put(before, {{"v", 1}}); // measured the old content
            CHECK(!AnalysisCache::get(before));
            CHECK(!get("test", f, {{"y", 1}}, 1));
            CHECK(!QFileInfo::exists(before.entry));
        }
        // Number lists packed exactly
        const std::vector<float> fl{0.f, 1e-7f, 0.123456789f, -3.5f};
        const std::vector<double> db{0.0, 1.0 / 3.0, 1e300, -2.25};
        CHECK(AnalysisCache::unpackFloats(AnalysisCache::pack(fl)) == fl);
        CHECK(AnalysisCache::unpackDoubles(AnalysisCache::pack(db)) == db);
        CHECK(AnalysisCache::unpackDoubles(QJsonValue("QUJD")).empty()); // 3 bytes: no whole double
        CHECK(AnalysisCache::unpackFloats(QJsonValue(5)).empty());
        // What storing costs on the cold path: the picture scores of a 40 min recording at 60 fps (3 values a picture)
        std::vector<double> big(size_t(40 * 60 * 60) * 3, 0.5);
        QElapsedTimer clock;
        clock.start();
        AnalysisCache::put(AnalysisCache::key("test", f, {{"big", true}}, 1), {{"scores", AnalysisCache::pack(big)}});
        const double putMs = clock.nsecsElapsed() / 1e6;
        clock.restart();
        const auto back = AnalysisCache::get(AnalysisCache::key("test", f, {{"big", true}}, 1));
        const double getMs = clock.nsecsElapsed() / 1e6;
        CHECK(back && AnalysisCache::unpackDoubles((*back)["scores"]).size() == big.size());
        std::printf("cache of 144000 pictures: put %.1f ms, get %.1f ms\n", putMs, getMs);
        // Pruning: below the cap nothing goes; above it the entries used longest ago go first
        CHECK_EQ(AnalysisCache::prune(), 0);
        const AnalysisCache::Key oldKey = AnalysisCache::key("test", f, {{"n", 1}}, 1);
        const AnalysisCache::Key newKey = AnalysisCache::key("test", f, {{"n", 2}}, 1);
        AnalysisCache::put(oldKey, {{"v", 1}});
        AnalysisCache::put(newKey, {{"v", 2}});
        {
            QFile e(oldKey.entry);
            CHECK(e.open(QIODevice::ReadWrite));
            CHECK(e.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
        }
        const QString bigEntry = AnalysisCache::key("test", f, {{"big", true}}, 1).entry;
        {
            QFile e(bigEntry);
            CHECK(e.open(QIODevice::ReadWrite));
            CHECK(e.setFileTime(QDateTime::currentDateTime().addDays(-10), QFileDevice::FileModificationTime));
        }
        // Over the cap: the oldest go (a month, ten days) until below 80 % of it, the one used now stays
        CHECK_EQ(AnalysisCache::prune(QFileInfo(bigEntry).size()), 2);
        CHECK(!QFileInfo::exists(oldKey.entry));
        CHECK(!QFileInfo::exists(bigEntry));
        CHECK(QFileInfo::exists(newKey.entry));
        // A hit marks an entry as used (touched when older than a day)
        {
            QFile e(newKey.entry);
            CHECK(e.open(QIODevice::ReadWrite));
            CHECK(e.setFileTime(QDateTime::currentDateTime().addDays(-30), QFileDevice::FileModificationTime));
        }
        CHECK(AnalysisCache::get(newKey));
        CHECK(QFileInfo(newKey.entry).lastModified().secsTo(QDateTime::currentDateTime()) < 3600);
        // The old audio events folder (before 0.25) is gone after the first use
        QThreadPool::globalInstance()->waitForDone(10000);
        CHECK(!QFileInfo::exists(oldEvents));
    }

    // --- Material: two recordings with loud bursts, a long sound file (silence), the fake whisper on the PATH
    const auto recording = [&](const QString& name, int hz) {
        return Check::makeMedia(
            dir.filePath(name),
            {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=90", "-f", "lavfi", "-i",
             QString("sine=f=%1:d=90:sample_rate=48000,volume='if(lt(mod(t,5),0.4),1,0.03)':eval=frame").arg(hz),
             "-c:v", "libx264", "-preset", "ultrafast", "-g", "30", "-c:a", "aac", "-shortest"});
    };
    const QString a = recording("a.mkv", 440), b = recording("b.mkv", 660);
    const QString talk = Check::makeMedia(
        dir.filePath("talk.m4a"), {"-f", "lavfi", "-i",
                                   "sine=f=300:d=900:sample_rate=48000,volume='if(lt(mod(t,7),5),1,0)':eval=frame",
                                   "-ac", "1", "-c:a", "aac", "-b:a", "48k"});
    if (!CHECK(!a.isEmpty() && !b.isEmpty() && !talk.isEmpty())) return Check::result();

    // --- Cancel on the calling thread through Context::cancel (execute installs the scope; one mechanism)
    {
        std::atomic<bool> stop{false};
        std::thread later([&stop] {
            QThread::msleep(30);
            stop = true;
        });
        QElapsedTimer t;
        t.start();
        const QJsonObject r = run("silence", {{"file", talk}}, &stop);
        later.join();
        CHECK_EQ(r["error"].toString(), QString("CANCELED"));
        CHECK(t.elapsed() < 5000);
        // Already set: nothing runs at all
        const QJsonObject r2 = run("probe", {{"file", a}}, &stop);
        CHECK_EQ(r2["error"].toString(), QString("CANCELED"));
    }

    const QString cacheDir = AnalysisCache::folder();

    // --- Background: highlights of two files, canceled at ~10 %
    {
        Cli::Live::setBackgroundLimit(2);
        // With a kill feed to look for (region + color): the pictures are read, it takes a moment
        const QJsonObject args{{"files", QJsonArray{a, b}}, {"region", "0,0,0.5,0.3"}, {"color", "#ff0000"}};
        const auto job = start("highlights", args, 0.1);
        CHECK(!job->done); // never inside the call
        CHECK(job->handle.running());
        CHECK(wait({job}));
        CHECK_EQ(code(job->reply), QString("CANCELED"));
        CHECK(job->doneOnUiThread);
        CHECK(job->progressOnUiThread);
        CHECK(!job->doneInsideCall);
        CHECK(!job->handle.running());
        CHECK(job->canceledAt >= 0 && job->doneAt - job->canceledAt < 10000);
        CHECK_EQ(Cli::Live::backgroundJobs(), 0);
        // It really stopped: not both files were measured to the end (a finished file is in the highlights cache)
        const QJsonObject again = run("highlights", args);
        int cached = 0;
        for (const QJsonValue& f : again["files"].toArray()) cached += f["cached"].toBool() ? 1 : 0;
        if (!late(job->reply)) CHECK(cached < 2);
    }

    // --- silence canceled: no cache entry of its peaks
    {
        const int before = filesIn(cacheDir + "/frame-peaks");
        const auto job = start("silence", {{"file", talk}}, 0.1);
        CHECK(wait({job}));
        CHECK_EQ(code(job->reply), QString("CANCELED"));
        CHECK(job->doneOnUiThread);
        if (!late(job->reply)) CHECK_EQ(filesIn(cacheDir + "/frame-peaks"), before);
    }

    // --- index canceled: not in the index
    {
        const auto job = start("index", {{"file", a}, {"sheet", false}}, 0.1);
        CHECK(wait({job}));
        CHECK_EQ(code(job->reply), QString("CANCELED"));
        if (!late(job->reply)) CHECK(!MediaIndex::cached(a));
    }

    // --- transcribe with a whisper that works long: canceled while it runs; the event loop keeps running
    {
        const QString suffix = QFileInfo(QCoreApplication::applicationFilePath()).suffix();
        const QString fake = dir.filePath(suffix.isEmpty() ? "bin/whisper-cli" : "bin/whisper-cli." + suffix);
        QDir().mkpath(dir.filePath("bin"));
        CHECK(QFile::copy(QCoreApplication::applicationFilePath(), fake));
        QFile::setPermissions(fake, QFile::permissions(fake) | QFileDevice::ExeOwner);
        qputenv("PATH", (QDir::toNativeSeparators(dir.filePath("bin")) + QDir::listSeparator() + qEnvironmentVariable("PATH")).toUtf8());
        qputenv("FAKE_WHISPER_MS", "30000");
        const QString model = dir.filePath("ggml-test.bin");
        QFile m(model);
        CHECK(m.open(QIODevice::WriteOnly) && m.write("model") == 5);
        m.close();
        int ticks = 0;
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, [&ticks] { ++ticks; });
        tick.start(10);
        const auto job = start("transcribe", {{"source", a}, {"model", model}});
        QTimer::singleShot(800, [job] {
            job->canceledAt = g_clock.elapsed();
            job->handle.cancel();
        });
        CHECK(wait({job}, 30000));
        tick.stop();
        CHECK_EQ(code(job->reply), QString("CANCELED"));
        CHECK(job->doneAt - job->canceledAt < 10000); // whisper was stopped, not waited for (it works 30 s)
        CHECK(ticks >= 30); // ~80 in 800 ms: the UI thread was free
        CHECK(job->doneOnUiThread);
        qputenv("FAKE_WHISPER_MS", "0");
    }

#ifndef Q_OS_WIN
    // --- schneidi-cli as a process, ended by SIGTERM (`kill`, `timeout`) while whisper and a render run: it answers
    // CANCELED with exit code 130 at once, stops whisper and leaves no temporary folder and no part file
    {
        const QString tmp = dir.filePath("cli-tmp");
        QDir().mkpath(tmp);
        const auto stopped = [&](const QStringList& args, int afterMs, QByteArray* out) {
            QProcess p;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("TMPDIR", tmp);
            env.insert("FAKE_WHISPER_MS", "30000");
            p.setProcessEnvironment(env);
            p.start(Check::cliPath(), args);
            if (!p.waitForStarted(10000)) return -1;
            QThread::msleep(afterMs);
            QElapsedTimer t;
            t.start();
            p.terminate(); // SIGTERM
            p.waitForFinished(20000);
            if (!CHECK(t.elapsed() < 4000)) std::printf("       took %lld ms to stop\n", qint64(t.elapsed()));
            *out = p.readAllStandardOutput();
            return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -2;
        };
        const QString model = dir.filePath("ggml-test.bin");
        QByteArray out;
        CHECK_EQ(stopped({"transcribe", a, "--model", model}, 2500, &out), 130);
        CHECK(out.contains("\"CANCELED\""));
        CHECK_EQ(QDir(tmp).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size(), 0);

        const QString clip = Check::makeMedia(dir.filePath("long.mkv"), {"-f", "lavfi", "-i", "testsrc2=s=1280x720:r=30:d=60",
                                                                          "-c:v", "libx264", "-preset", "ultrafast"});
        const QString project = dir.filePath("long.schneidi");
        run("new", {{"project", project}});
        run("edit", {{"project", project}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(clip)}});
        const QString renders = dir.filePath("renders");
        QDir().mkpath(renders);
        CHECK_EQ(stopped({"render", project, "--out", renders + "/long.mp4", "--height", "360", "--encoder", "software"},
                         2500, &out), 130);
        CHECK(out.contains("\"CANCELED\""));
        CHECK_EQ(QDir(renders).entryList(QDir::Files | QDir::Hidden).size(), 0); // no long.<id>.part.mp4
    }
#endif

    // --- Refusals: changing commands, changing forms, "@app" without the app; onDone comes later, never inside
    {
        const auto edit = start("edit", {{"project", dir.filePath("x.schneidi")}, {"ops", QJsonArray{}}});
        const auto beats = start("beats", {{"file", "@app"}, {"apply", "markers"}});
        const auto noApp = start("silence", {{"file", "@app"}});
        const auto unknown = start("no-such-command", {});
        CHECK(!edit->done && !beats->done && !noApp->done && !unknown->done);
        CHECK(edit->handle.running()); // not delivered yet
        CHECK(wait({edit, beats, noApp, unknown}));
        CHECK_EQ(code(edit->reply), QString("NOT_IN_BACKGROUND"));
        CHECK_EQ(code(beats->reply), QString("NOT_IN_BACKGROUND"));
        CHECK_EQ(code(noApp->reply), QString("NO_APP"));
        CHECK_EQ(code(unknown->reply), QString("BAD_REQUEST"));
        CHECK(!edit->doneInsideCall && !noApp->doneInsideCall);
        CHECK(Cli::Live::runsInBackground("peaks", {{"file", a}}));
        CHECK(!Cli::Live::runsInBackground("transcribe", {{"source", a}, {"apply", true}}));
        CHECK(!Cli::Live::runsInBackground("index", {{"file", a}, {"annotate", QJsonArray{QJsonObject{{"shot", 1}}}}}));
        CHECK(!Cli::Live::runsInBackground("render", {}));
    }

    // --- A queued job canceled before it starts: CANCELED, it never runs
    {
        Cli::Live::setBackgroundLimit(1);
        const auto first = start("peaks", {{"file", a}});
        const auto second = start("peaks", {{"file", b}});
        second->handle.cancel();
        CHECK(wait({first, second}));
        CHECK(first->reply["ok"].toBool());
        CHECK_EQ(code(second->reply), QString("CANCELED"));
        CHECK_EQ(second->progressCalls, 0);
    }

    // --- cancelAllBackground with running and queued jobs (more than the limit, two on one file): every onDone once,
    // CANCELED, nothing left
    {
        Cli::Live::setBackgroundLimit(2);
        QVector<std::shared_ptr<Job>> jobs;
        for (const QJsonObject& args : {QJsonObject{{"file", a}}, QJsonObject{{"file", a}}, QJsonObject{{"file", b}},
                                        QJsonObject{{"file", talk}}, QJsonObject{{"file", b}}})
            jobs << start("scenes", args);
        CHECK_EQ(Cli::Live::backgroundJobs(), 5);
        Cli::Live::cancelAllBackground();
        CHECK(wait(jobs));
        QCoreApplication::processEvents(); // nothing comes twice
        for (const auto& j : jobs) {
            CHECK_EQ(j->doneCalls, 1);
            CHECK_EQ(code(j->reply), QString("CANCELED"));
        }
        CHECK_EQ(Cli::Live::backgroundJobs(), 0);
        // Canceled after a refusal: CANCELED too (whatever came of it)
        const auto refused = start("render", {});
        refused->handle.cancel();
        CHECK(wait({refused}));
        CHECK_EQ(code(refused->reply), QString("CANCELED"));
    }

    // --- One job per file: the second on the same file starts when the first is done (another file does not wait)
    {
        Cli::Live::setBackgroundLimit(4);
        QStringList events;
        const auto one = start("scenes", {{"file", b}}, -1, nullptr, &events, "one");
        const auto same = start("silence", {{"file", b}}, -1, nullptr, &events, "same");
        CHECK(wait({one, same}));
        CHECK(one->reply["ok"].toBool() && same->reply["ok"].toBool());
        const qsizetype oneDone = events.indexOf("one:done");
        const qsizetype sameFirst = events.indexOf(QRegularExpression("same:.*"));
        if (!CHECK(oneDone >= 0 && sameFirst > oneDone)) std::printf("       %s\n", qPrintable(events.join(' ')));
        Cli::Live::setBackgroundLimit(0);
        CHECK(Cli::Live::backgroundLimit() >= 1 && Cli::Live::backgroundLimit() <= 4);
    }

    // --- "@app" from a copy of the app's project on the job's thread (what runInBackground hands it)
    {
        const QString project = dir.filePath("p.schneidi");
        CHECK(!run("new", {{"project", project}, {"fps", 25}}).contains("error"));
        CHECK(!run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", a}}}}})
                   .contains("error"));
        ProjectData d;
        QString error;
        CHECK(ProjectFile::load(project, &d, &error));
        Cli::Live::AppCopy copy;
        copy.path = project;
        copy.data = std::make_shared<const ProjectData>(d);
        copy.app = QJsonObject{{"playhead", 7}};
        QJsonObject fromCopy;
        std::thread worker([&] {
            const Cli::Live::AppCopyScope scope(&copy);
            fromCopy = run("silence", {{"file", "@app"}});
        });
        worker.join();
        CHECK(!fromCopy.contains("error"));
        CHECK_EQ(fromCopy["project"].toString(), project);
        CHECK(fromCopy["silence"].toArray().size() > 5);
        // Several files with a project: every thread counts in the project's frame rate
        const QJsonObject both = run("peaks", {{"files", QJsonArray{a, b}}, {"project", project}});
        for (const QJsonValue& f : both["files"].toArray()) CHECK_EQ(f["fps"].toDouble(), 25.0);
    }

    // --- Cache: the second run of silence and scenes reads the measurement, a changed file is measured again
    {
        const auto timed = [&](const QString& command, const QJsonObject& args, QJsonObject* out) {
            QElapsedTimer t;
            t.start();
            *out = run(command, args);
            return t.nsecsElapsed() / 1e6;
        };
        // A copy no earlier job has measured (a cancel that came late may have stored the original's)
        const QString fresh = dir.filePath("talk2.m4a");
        CHECK(QFile::copy(talk, fresh));
        // Hits counted by the cache itself; the time only as a loose check (a loaded machine)
        QJsonObject first, second;
        const int hits0 = AnalysisCache::hits();
        const double cold = timed("silence", {{"file", fresh}}, &first);
        CHECK_EQ(AnalysisCache::hits(), hits0);
        const double warm = timed("silence", {{"file", fresh}}, &second);
        CHECK_EQ(AnalysisCache::hits(), hits0 + 1);
        CHECK(!first.contains("error"));
        CHECK_EQ(QJsonDocument(first).toJson(), QJsonDocument(second).toJson()); // the same answer
        if (!CHECK(warm < cold * 0.25)) std::printf("       silence cold %.1f ms, warm %.1f ms\n", cold, warm);
        else std::printf("silence cold %.1f ms, warm %.1f ms\n", cold, warm);
        // Other settings, the same measurement: from the cache too
        QJsonObject other;
        const double otherMs = timed("silence", {{"file", fresh}, {"min", "1s"}}, &other);
        CHECK_EQ(AnalysisCache::hits(), hits0 + 2);
        CHECK(otherMs < cold * 0.25);
        CHECK(other["silence"].toArray().size() <= first["silence"].toArray().size());

        // A picture as large as a recording's (opening the file stays: the cache saves the decoding)
        const QString hd = Check::makeMedia(dir.filePath("hd.mkv"), {"-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=40",
                                                                     "-c:v", "libx264", "-preset", "ultrafast"});
        CHECK(!hd.isEmpty());
        QJsonObject s1, s2;
        const double sceneCold = timed("scenes", {{"file", hd}}, &s1);
        const int hits1 = AnalysisCache::hits();
        const double sceneWarm = timed("scenes", {{"file", hd}}, &s2);
        CHECK_EQ(AnalysisCache::hits(), hits1 + 1);
        CHECK(!s1.contains("error"));
        CHECK_EQ(QJsonDocument(s1).toJson(), QJsonDocument(s2).toJson());
        if (!CHECK(sceneWarm < sceneCold * 0.25))
            std::printf("       scenes cold %.1f ms, warm %.1f ms\n", sceneCold, sceneWarm);
        else std::printf("scenes cold %.1f ms, warm %.1f ms\n", sceneCold, sceneWarm);

        // The file overwritten with other sound (no pauses now): measured again, another answer
        QFile::remove(fresh);
        const QString again = Check::makeMedia(fresh, {"-f", "lavfi", "-i", "sine=f=300:d=900:sample_rate=48000", "-ac",
                                                      "1", "-c:a", "aac", "-b:a", "48k"});
        CHECK(!again.isEmpty());
        QJsonObject changed;
        timed("silence", {{"file", fresh}}, &changed);
        CHECK(!changed.contains("error"));
        CHECK(first["silence"].toArray().size() > 10);
        CHECK(changed["silence"].toArray().isEmpty());
    }

    Cli::Live::cancelAllBackground();
    CHECK_EQ(Cli::Live::backgroundJobs(), 0);
    return Check::result();
}
