// Watch folders (core/WatchFolders.h): which files count as new and complete (temporary, hidden, other kinds,
// subfolders, already in the pool, taken back, still being written, clock of a network share), one undo step per
// batch and its undo (the files stay out), the project file (round trip, old files without the field), the `watch`
// command (add/list/remove, a pass with its answer, `since`), schneidi-cli as a real process (one pass; --follow
// prints a file that is written in chunks once, when it is complete), the project open in a (fake) app: the pass runs
// there and follow does not keep the app, and the app's watcher (catch-up on open, waiting while busy, undo).
// WATCH_SHOT=<folder>: screenshots of the dialogs (German and English).
#include "app/FolderWatch.h"
#include "app/WatchFoldersDialog.h"
#include "cli/Commands.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/WatchFolders.h"
#include "engine/Engine.h"

#include "check.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QTemporaryDir>
#include <QThread>
#include <QUndoStack>

namespace {

using WatchFolders::Scanner;

// A file with `bytes` bytes and the given modification time (the scanner does not open files)
void write(const QString& path, const QDateTime& modified, int bytes = 10)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(QByteArray(bytes, 'x'));
    f.flush(); // else the buffered bytes are written on close, after the time was set, and the time is "now" again
    f.setFileTime(modified, QFileDevice::FileModificationTime);
}

QStringList names(const QStringList& paths)
{
    QStringList out;
    for (const QString& p : paths) out << QFileInfo(p).fileName();
    out.sort();
    return out;
}

QStringList names(const QVector<Scanner::Found>& found)
{
    QStringList paths;
    for (const Scanner::Found& f : found) paths << f.path;
    return names(paths);
}

// A copy that counts as written now (QFile::copy may keep the original's modification time)
void copyNow(const QString& from, const QString& to)
{
    QFile::copy(from, to);
    QFile f(to);
    f.open(QIODevice::ReadWrite);
    f.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
}

QJsonObject item(const QJsonObject& o, const char* list, int i) { return o[list].toArray().at(i).toObject(); }

MediaInfo media(const QString& path) { return MediaInfo{path, QFileInfo(path).fileName(), 100, true, true, false}; }

void emptyProject(Project& p)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
}

// Waits (events keep running: the app's watcher, the live server) until done() or the time is up
bool waitFor(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.hasExpired(ms)) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return true;
}

void testRules()
{
    for (const char* name : {".clip.mp4", "~clip.mp4", "clip.mp4~", "$clip.mp4", "clip.mp4.part", "clip.crdownload",
                             "clip.part.mp4", "clip.tmp.mkv", "clip.mkv.download", ".syncthing.clip.mp4.tmp"})
        Check::report(WatchFolders::isTemporary(name), name, __FILE__, __LINE__);
    for (const char* name : {"clip.mp4", "Replay 2026-10-26 21-03-11.mkv", "my.final.cut.mov", "partner.mp4"})
        Check::report(!WatchFolders::isTemporary(name), name, __FILE__, __LINE__);
    CHECK(WatchFolders::kindOf("/a/b.MKV") == WatchFolders::Kind::Video);
    CHECK(WatchFolders::kindOf("/a/b.wav") == WatchFolders::Kind::Audio);
    CHECK(WatchFolders::kindOf("/a/b.jpeg") == WatchFolders::Kind::Image);
    CHECK(WatchFolders::kindOf("/a/b.txt") == WatchFolders::Kind::Other);
    WatchFolder w;
    CHECK(WatchFolders::setTypes(w, {"video", " Images "}));
    CHECK(w.video && !w.audio && w.images);
    CHECK(WatchFolders::takes(w, "x.png") && !WatchFolders::takes(w, "x.mp3") && !WatchFolders::takes(w, "x.srt"));
    CHECK(!WatchFolders::setTypes(w, {"video", "film"})); // unknown name: unchanged
    CHECK(!WatchFolders::setTypes(w, {}));                 // nothing at all: unchanged
    CHECK_EQ(WatchFolders::typeNames(w), (QStringList{"video", "image"}));
    CHECK(WatchFolders::samePath("/a/b/../c/", "/a/c"));
}

// The scanner with a simulated clock: `since` lies a minute in the future, so the real creation and change times of the
// files made here count as "before the watch", their modification times (set) decide
void testScanner(const QString& root)
{
    const QString dir = root + "/scan";
    const QDateTime t0 = QDateTime::currentDateTimeUtc();
    const QDateTime later = t0.addSecs(65);
    write(dir + "/old.mp4", t0.addSecs(-3600)); // was there before the watch
    for (const char* n : {"new.mp4", ".hidden.mp4", "x.mp4.part", "clip.tmp.mkv", "~lock.mp4", "song.mp3", "notes.txt",
                          "sub/deep.mp4", ".cache/inside.mp4"})
        write(dir + "/" + n, later);
    write(dir + "/empty.mp4", later, 0); // created, nothing written yet
    WatchFolder w;
    w.path = dir;
    w.since = t0.addSecs(60);
    w.audio = false;
    const auto none = [](const QString&) { return false; };

    Scanner sc(3000);
    Scanner::Result r = sc.scan({w}, none, t0.addSecs(66)); // new.mp4 written a second ago: not done yet
    CHECK(r.ready.isEmpty());
    CHECK_EQ(names(r.pending), QStringList{"new.mp4"});
    CHECK_EQ(r.folders, QStringList{dir}); // no subfolders
    r = sc.scan({w}, none, t0.addSecs(69));
    CHECK_EQ(names(r.ready), QStringList{"new.mp4"});
    CHECK(r.pending.isEmpty());

    // Subfolders (not hidden ones), audio too
    w.subfolders = true;
    w.audio = true;
    r = sc.scan({w}, none, t0.addSecs(70));
    CHECK_EQ(names(r.ready), (QStringList{"deep.mp4", "new.mp4", "song.mp3"}));
    CHECK(r.folders.contains(dir + "/sub") && !r.folders.contains(dir + "/.cache"));

    // Already in the pool, or taken back by the user: not again
    const QString key = WatchFolders::pathKey(dir + "/new.mp4");
    r = sc.scan({w}, [&](const QString& k) { return k == key; }, t0.addSecs(70));
    CHECK(!names(r.ready).contains("new.mp4"));
    w.ignored << dir + "/song.mp3";
    r = sc.scan({w}, none, t0.addSecs(70));
    CHECK(!names(r.ready).contains("song.mp3"));
    w.ignored.clear();

    // Could not be read: left alone until it changes, then taken once it is complete again
    sc.rejected(dir + "/sub/deep.mp4");
    r = sc.scan({w}, none, t0.addSecs(71));
    CHECK(!names(r.ready).contains("deep.mp4"));
    write(dir + "/sub/deep.mp4", t0.addSecs(71), 20);
    r = sc.scan({w}, none, t0.addSecs(72));
    CHECK(names(r.pending).contains("deep.mp4"));
    r = sc.scan({w}, none, t0.addSecs(75));
    CHECK(names(r.ready).contains("deep.mp4"));

    // Appeared while watching, with an old modification time (a copy that kept it): taken
    write(dir + "/copied.mp4", t0.addSecs(-7200));
    r = sc.scan({w}, none, t0.addSecs(80));
    CHECK(names(r.ready).contains("copied.mp4"));
    CHECK(!names(r.ready).contains("old.mp4"));

    // A folder watched later: what is in it when it is first listed stays out
    const QString dir2 = root + "/scan2";
    write(dir2 + "/before.mp4", t0.addSecs(-3600));
    WatchFolder w2;
    w2.path = dir2;
    w2.since = t0.addSecs(60);
    r = sc.scan({w, w2}, none, t0.addSecs(81));
    CHECK(!names(r.ready).contains("before.mp4"));
    // ... unless it is to take the files there already
    w2.since = QDateTime();
    r = sc.scan({w, w2}, none, t0.addSecs(82));
    CHECK(names(r.ready).contains("before.mp4"));

    // A network share whose clock is ahead: the modification time is in the future, the size stays -> taken once it
    // stayed unchanged for the settle time as this scanner saw it
    write(dir + "/skew.mp4", t0.addSecs(3600), 50);
    r = sc.scan({w}, none, t0.addSecs(90));
    CHECK(names(r.pending).contains("skew.mp4"));
    r = sc.scan({w}, none, t0.addSecs(92));
    CHECK(names(r.pending).contains("skew.mp4"));
    r = sc.scan({w}, none, t0.addSecs(94));
    CHECK(names(r.ready).contains("skew.mp4"));

    // Without an earlier scan (`watch` from the command line) the modification time alone decides
    Scanner once(3000);
    r = once.scan({w}, none, t0.addSecs(95));
    CHECK(names(r.ready).contains("new.mp4") && !names(r.ready).contains("skew.mp4"));
    // A folder that is not there: nothing, no error
    WatchFolder gone;
    gone.path = root + "/not-there";
    CHECK(once.scan({gone}, none).folders.isEmpty());
}

void testProject()
{
    Project p;
    emptyProject(p);
    QUndoStack* undo = p.undoStack();
    const int bin = p.addBin(0, "Aufnahmen");
    p.markSaved();
    WatchFolder w;
    w.path = "/rec";
    w.bin = bin;
    w.since = QDateTime::currentDateTimeUtc();
    const int index = undo->index();
    // Setting up: not in the undo history, but a change
    p.setWatchFolders({w});
    CHECK_EQ(undo->index(), index);
    CHECK(p.isModified());
    p.markSaved();

    // One undo step for the batch, into the watch's bin, revisions counting up
    const QVector<WatchArrival> a = p.addWatchedMedia({{media("/rec/a.mp4"), "/rec"}, {media("/rec/b.mp4"), "/rec"},
                                                       {media("/rec/a.mp4"), "/rec"}});
    CHECK_EQ(a.size(), 2);
    CHECK_EQ(p.watchRevision(), 2);
    CHECK_EQ(a.last().revision, 2);
    CHECK_EQ(undo->index(), index + 1);
    CHECK_EQ(undo->undoText(), T("Überwachter Ordner „%1“: %2 neue Dateien").arg("rec").arg(2));
    CHECK_EQ(p.media().size(), 2);
    CHECK_EQ(p.mediaInfo("/rec/b.mp4")->bin, bin);
    CHECK(p.isModified());
    // Already in the pool: nothing, no step
    CHECK(p.addWatchedMedia({{media("/rec/a.mp4"), "/rec"}}).isEmpty());
    CHECK_EQ(undo->index(), index + 1);
    p.markSaved();

    // Undo: out of the pool again and on the ignored list (also a change to save); redo brings them back
    p.setMediaMarks("/rec/a.mp4", 10, 20);
    undo->undo();
    CHECK(p.media().isEmpty());
    CHECK_EQ(p.watchFolders()[0].ignored, (QStringList{"/rec/a.mp4", "/rec/b.mp4"}));
    CHECK(p.isModified());
    undo->redo();
    CHECK_EQ(p.media().size(), 2);
    CHECK(p.watchFolders()[0].ignored.isEmpty());
    CHECK_EQ(p.mediaInfo("/rec/a.mp4")->markIn, 10); // what changed meanwhile comes back with it
    CHECK_EQ(p.watchLog().size(), 2);

    // A bin removed meanwhile: Master
    p.removeBin(bin);
    p.addWatchedMedia({{media("/rec/c.mp4"), "/rec"}});
    CHECK_EQ(p.mediaInfo("/rec/c.mp4")->bin, 0);
    CHECK_EQ(p.watchRevision(), 3);
    // One file: named, no "file(s)"
    CHECK_EQ(undo->undoText(), T("Überwachter Ordner „%1“: neue Datei „%2“").arg("rec", "c.mp4"));
    CHECK_EQ(WatchFolders::importedNote({"/rec/c.mp4"}, "/rec", "Master"),
             T("Neue Datei „%1“ aus „%2“ im Bin „%3“").arg("c.mp4", "rec", "Master"));
    CHECK_EQ(WatchFolders::importedNote({"/rec/a.mp4", "/rec/b.mp4"}, "/rec/", "Aufnahmen"),
             T("%1 neue Dateien aus „%2“ im Bin „%3“").arg("2", "rec", "Aufnahmen"));
}

void testProjectFile(const QString& root)
{
    ProjectData d;
    d.timeline = emptyTimeline();
    d.bins << MediaBin{3, 0, "Aufnahmen"};
    WatchFolder w;
    w.path = root + "/rec";
    w.subfolders = true;
    w.audio = false;
    w.bin = 3;
    w.since = QDateTime::fromString("2026-10-26T20:15:42.123Z", Qt::ISODateWithMs);
    w.ignored << root + "/rec/x.mp4";
    WatchFolder all;
    all.path = root + "/all"; // all kinds, every file (no `since`)
    d.watchFolders << w << all;
    d.watchRevision = 7;
    d.watchLog << WatchArrival{6, root + "/rec/a.mp4", w.path, w.since.addSecs(5)}
               << WatchArrival{7, root + "/rec/b.mp4", w.path, w.since.addSecs(9)};
    const QString path = root + "/proj/p.schneidi";
    const QByteArray json = ProjectFile::toJson(d, path);
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    CHECK_EQ(o["watchFolders"].toArray()[0].toObject()["relPath"].toString(), QString("../rec"));
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(json, path, &back, &error));
    CHECK(back.watchFolders == d.watchFolders);
    CHECK(back.watchLog == d.watchLog);
    CHECK_EQ(back.watchRevision, 7);
    CHECK(ProjectFile::toJson(back, path) == json); // stable

    // A newer version's kind of media this one does not know: the watch stays with the kinds it knows; only a watch
    // with none of them is left out
    {
        QJsonObject root = o;
        QJsonArray watches = root["watchFolders"].toArray();
        QJsonObject first = watches[0].toObject();
        first["types"] = QJsonArray{"video", "hologram"};
        watches[0] = first;
        QJsonObject second = watches[1].toObject();
        second["types"] = QJsonArray{"hologram"};
        watches[1] = second;
        root["watchFolders"] = watches;
        ProjectData newer;
        CHECK(ProjectFile::fromJson(QJsonDocument(root).toJson(), path, &newer, &error));
        if (CHECK_EQ(newer.watchFolders.size(), 1)) {
            CHECK(WatchFolders::samePath(newer.watchFolders[0].path, w.path));
            CHECK(newer.watchFolders[0].video && !newer.watchFolders[0].audio && !newer.watchFolders[0].images);
            CHECK_EQ(newer.watchFolders[0].bin, 3);
            CHECK(newer.watchFolders[0].since == w.since);
        }
    }

    // Old files (and projects without watch folders) have no field: nothing watched, nothing written
    ProjectData plain;
    plain.timeline = emptyTimeline();
    const QJsonObject po = QJsonDocument::fromJson(ProjectFile::toJson(plain, path)).object();
    CHECK(!po.contains("watchFolders") && !po.contains("watchLog"));
    ProjectData old;
    if (CHECK(ProjectFile::load(SCHNEIDI_TEST_DATA "/projects/v2-0.9.schneidi", &old, &error))) {
        CHECK(old.watchFolders.isEmpty() && old.watchLog.isEmpty() && old.watchRevision == 0);
    }

    // The project: a bin that is gone becomes Master, the revision never goes back
    d.bins.clear();
    Project p;
    p.load(d);
    CHECK_EQ(p.watchFolders()[0].bin, 0);
    CHECK_EQ(p.watchRevision(), 7);
}

// ---------- the command ----------

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
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

void testCommand(const QString& root, const QString& clip)
{
    const QString dir = root + "/cmd";
    const QString project = root + "/cmd.schneidi";
    QDir().mkpath(dir);
    QFile::copy(clip, dir + "/before.mp4"); // there before the watch: stays out
    CHECK(!run("new", {{"project", project}}).contains("error"));

    QJsonObject r = run("watch", {{"project", project}, {"add", dir}, {"bin", "Recordings/Raw"}, {"types", "video"}});
    CHECK_EQ(r["folders"].toArray().size(), 1);
    CHECK_EQ(item(r, "folders", 0)["bin"].toString(), QString("Recordings/Raw"));
    CHECK_EQ(item(r, "folders", 0)["types"].toArray(), QJsonArray{"video"});
    CHECK(item(r, "folders", 0)["since"].isString());
    ProjectData d;
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(d.watchFolders.size(), 1);
    CHECK_EQ(d.bins.size(), 2);

    // Errors: no such folder, options without 'add', a folder not watched, follow outside the command line
    CHECK_EQ(errorOf("watch", {{"project", project}, {"add", root + "/missing"}}), QString("NOT_FOUND"));
    CHECK_EQ(errorOf("watch", {{"project", project}, {"bin", "X"}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("watch", {{"project", project}, {"remove", root}}), QString("NOT_FOUND"));
    CHECK_EQ(errorOf("watch", {{"project", project}, {"follow", true}}), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf("watch", {{"project", project}, {"add", dir}, {"types", "film"}}), QString("BAD_ARGUMENT"));

    // A pass: the new complete file, not the one from before, not a broken one, not a sound file (video only)
    QThread::msleep(20);
    copyNow(clip, dir + "/new.mp4");
    write(dir + "/broken.mp4", QDateTime::currentDateTime().addSecs(-10), 5000);
    copyNow(clip, dir + "/sound.m4a");
    r = run("watch", {{"project", project}, {"settle_s", 0}});
    if (CHECK_EQ(r["new"].toArray().size(), 1)) {
        const QJsonObject n = item(r, "new", 0);
        CHECK_EQ(n["file"].toString(), dir + "/new.mp4");
        CHECK_EQ(n["revision"].toInt(), 1);
        CHECK_EQ(n["frames"].toInt(), 50); // 2 s at 25 fps
        CHECK_EQ(n["bin"].toString(), QString("Recordings/Raw"));
        CHECK(n["video"].toBool() && n["audio"].toBool());
    }
    CHECK_EQ(r["imported"].toInt(), 1);
    CHECK_EQ(r["unreadable"].toArray(), QJsonArray{dir + "/broken.mp4"});
    CHECK(r["backup"].isString());
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(d.media.size(), 1);
    CHECK_EQ(d.watchRevision, 1);
    // Nothing new: nothing imported, nothing saved; `since` lists what came after a revision
    r = run("watch", {{"project", project}, {"settle_s", 0}});
    CHECK(r["new"].toArray().isEmpty() && !r.contains("backup"));
    r = run("watch", {{"project", project}, {"since", 0}});
    CHECK_EQ(r["new"].toArray().size(), 1);
    CHECK_EQ(r["revision"].toInt(), 1);
    // Still fresh with the default settle time: pending, not imported
    copyNow(clip, dir + "/fresh.mp4");
    r = run("watch", {{"project", project}});
    CHECK_EQ(r["pending"].toArray(), QJsonArray{dir + "/fresh.mp4"});
    CHECK(r["new"].toArray().isEmpty());
    // list: no import; `info` shows the revision
    r = run("watch", {{"project", project}, {"list", true}, {"settle_s", 0}});
    CHECK(!r.contains("new") && r["folders"].toArray().size() == 1);
    r = run("info", {{"project", project}, {"summary", true}});
    CHECK_EQ(r["watch"].toObject()["revision"].toInt(), 1);
    CHECK_EQ(r["watch"].toObject()["folders"].toInt(), 1);
    // Remove: the imported file stays
    r = run("watch", {{"project", project}, {"remove", dir}});
    CHECK(r["folders"].toArray().isEmpty());
    ProjectFile::load(project, &d, nullptr);
    CHECK(d.watchFolders.isEmpty() && d.media.size() == 1);
    r = run("watch", {{"project", project}});
    CHECK(r["notes"].toArray().size() == 1 && r["new"].toArray().isEmpty());
}

// ---------- schneidi-cli as a process ----------

QJsonObject cliJson(const QStringList& args)
{
    QProcess p;
    p.start(SCHNEIDI_CLI, args);
    p.waitForFinished(120000);
    const QByteArray out = p.readAllStandardOutput().trimmed();
    const QJsonObject o = QJsonDocument::fromJson(out.split('\n').last()).object();
    if (o.isEmpty()) std::printf("       no JSON: %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

// One pass on the command line; --follow prints a file written in chunks (a recording) once, when it is complete
void testCliProcess(const QString& root, const QString& clip, const QString& longClip)
{
    const QString dir = root + "/proc";
    const QString project = root + "/proc.schneidi";
    QDir().mkpath(dir);
    cliJson({"new", project});
    QJsonObject r = cliJson({"watch", project, "--add", dir, "--subfolders"});
    CHECK(r["ok"].toBool());
    QThread::msleep(20);
    QDir().mkpath(dir + "/day1");
    copyNow(clip, dir + "/day1/one.mp4");
    r = cliJson({"watch", project, "--settle-s", "0"});
    CHECK(r["ok"].toBool());
    CHECK_EQ(r["new"].toArray().size(), 1);
    CHECK_EQ(item(r, "new", 0)["file"].toString(), dir + "/day1/one.mp4");

    QFile src(longClip);
    src.open(QIODevice::ReadOnly);
    const QByteArray bytes = src.readAll();
    QProcess p;
    p.start(SCHNEIDI_CLI, {"watch", project, "--follow", "--settle-s", "2"});
    CHECK(p.waitForStarted(10000));
    QThread::msleep(1500); // its first pass (nothing new) - not needed, but the file then really arrives while it runs
    const QString target = dir + "/recording.mkv";
    const int chunks = 8;
    bool early = false;
    for (int i = 0; i < chunks; ++i) {
        QFile f(target);
        f.open(QIODevice::WriteOnly | QIODevice::Append);
        f.write(bytes.mid(i * bytes.size() / chunks, (i + 1) * bytes.size() / chunks - i * bytes.size() / chunks));
        f.close();
        if (p.waitForReadyRead(400)) early = true; // a line while it is still being written: imported too early
    }
    CHECK(!early);
    QByteArray out = p.readAllStandardOutput();
    QElapsedTimer t;
    t.start();
    while (!out.contains('\n') && !t.hasExpired(30000))
        if (p.waitForReadyRead(500)) out += p.readAllStandardOutput();
    const QList<QByteArray> lines = out.trimmed().split('\n');
    if (CHECK_EQ(lines.size(), 1)) {
        const QJsonObject line = QJsonDocument::fromJson(lines[0]).object();
        CHECK_EQ(line["file"].toString(), target);
        // The whole 4 s, not a part of it (AAC in MKV: 4.02 s with some FFmpeg versions)
        CHECK(line["frames"].toInt() >= 100 && line["frames"].toInt() <= 101);
        CHECK_EQ(line["revision"].toInt(), 2);
    } else {
        std::printf("       output: %s\n       stderr: %s\n", out.constData(), p.readAllStandardError().constData());
    }
    // Nothing more for it (one line per file)
    CHECK(!p.waitForReadyRead(3500));
    p.kill();
    p.waitForFinished(5000);
    ProjectData d;
    ProjectFile::load(project, &d, nullptr);
    CHECK_EQ(d.media.size(), 2);
}

// ---------- the project open in the app ----------

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    QStringList events;
    QVector<LiveChange> changes;
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
    void liveConnectionChanged(bool connected, const QString& client) override
    {
        events << (connected ? "attach " + client : QString("detach"));
    }
    void liveChanged(const LiveChange& c) override { changes << c; }
    void liveActivity(const LiveActivity&) override {}
};

QJsonObject cliServing(const QStringList& args)
{
    QProcess p;
    p.start(SCHNEIDI_CLI, args);
    p.closeWriteChannel();
    waitFor([&] { return p.state() == QProcess::NotRunning; }, 120000);
    QCoreApplication::processEvents();
    const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput().trimmed().split('\n').last()).object();
    if (o.isEmpty()) std::printf("       no JSON: %s\n", p.readAllStandardError().constData());
    return o;
}

void testLive(const QString& root, const QString& clip)
{
    const QString dir = root + "/live";
    QDir().mkpath(dir);
    FakeApp fake;
    emptyProject(fake.project);
    fake.path = root + "/live.schneidi";
    WatchFolder w;
    w.path = dir;
    w.since = QDateTime::currentDateTimeUtc();
    fake.project.setWatchFolders({w});
    QString error;
    ProjectFile::save(fake.project.data(), fake.path, &error);
    fake.project.markSaved();
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return;

    // A pass runs in the app: its project, one undo step, the file untouched; logged as a step
    QThread::msleep(20);
    copyNow(clip, dir + "/a.mp4");
    const int index = fake.project.undoStack()->index();
    QJsonObject r = cliServing({"watch", "@app", "--settle-s", "0"});
    CHECK(r["ok"].toBool() && r["live"].toBool());
    CHECK_EQ(r["new"].toArray().size(), 1);
    CHECK_EQ(fake.project.media().size(), 1);
    CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    ProjectData d;
    ProjectFile::load(fake.path, &d, nullptr);
    CHECK(d.media.isEmpty());
    if (CHECK(!fake.changes.isEmpty())) CHECK_EQ(fake.changes.last().step, QString("watch: 1 new"));
    // Nothing new: not a step in the AI log
    const int logged = fake.changes.size();
    r = cliServing({"watch", fake.path, "--settle-s", "0"});
    CHECK(r["ok"].toBool() && r["new"].toArray().isEmpty());
    CHECK_EQ(fake.changes.size(), logged);
    // list reads what the app shows
    r = cliServing({"watch", "@app", "--list"});
    CHECK_EQ(r["revision"].toInt(), 1);

    // Follow: every pass goes into the app and lets it go again at once (another AI may attach meanwhile)
    fake.events.clear();
    QProcess p;
    p.start(SCHNEIDI_CLI, {"watch", fake.path, "--follow", "--settle-s", "1"});
    waitFor([&] { return fake.events.size() >= 2; }, 30000); // the first pass
    copyNow(clip, dir + "/b.mp4");
    QByteArray out;
    waitFor([&] {
        out += p.readAllStandardOutput();
        return out.contains('\n');
    }, 30000);
    const QJsonObject line = QJsonDocument::fromJson(out.trimmed().split('\n').first()).object();
    CHECK_EQ(line["file"].toString(), dir + "/b.mp4");
    CHECK_EQ(fake.project.media().size(), 2);
    waitFor([&] { return fake.events.size() >= 4 && fake.events.last() == "detach"; }, 5000);
    CHECK(fake.events.size() >= 4);
    CHECK_EQ(fake.events.value(0), QString("attach schneidi-cli"));
    CHECK_EQ(fake.events.last(), QString("detach"));
    p.kill();
    p.waitForFinished(5000);
}

// ---------- the app's watcher ----------

void testFolderWatch(const QString& root, const QString& clip)
{
    const QString dir = root + "/app";
    QDir().mkpath(dir);
    Project p;
    emptyProject(p);
    FolderWatch fw(&p);
    fw.setSettleMs(300);
    fw.setRescanMs(200);
    QStringList got;
    QObject::connect(&fw, &FolderWatch::imported, [&](const QStringList& paths, const QString&) { got << paths; });
    bool busy = true;
    fw.setBusy([&] { return busy; });
    WatchFolder w;
    w.path = dir;
    w.bin = p.addBin(0, "Auto");
    w.since = QDateTime::currentDateTimeUtc();
    const int index = p.undoStack()->index();
    p.setWatchFolders({w});
    CHECK(waitFor([&] { return fw.watchedDirectories().contains(dir); }, 5000));

    // A new file: found, but the app is busy - it waits; then one undo step into the bin
    QThread::msleep(20);
    copyNow(clip, dir + "/a.mp4");
    waitFor([] { return false; }, 3000);
    CHECK(p.media().isEmpty());
    busy = false;
    CHECK(waitFor([&] { return !p.media().isEmpty(); }, 10000));
    CHECK_EQ(got, QStringList{dir + "/a.mp4"});
    CHECK_EQ(p.undoStack()->index(), index + 1);
    if (CHECK(!p.media().isEmpty())) CHECK_EQ(p.media()[0].bin, w.bin);

    // Undone: stays out, also after more scans
    p.undoStack()->undo();
    CHECK(p.media().isEmpty());
    waitFor([] { return false; }, 1500);
    CHECK(p.media().isEmpty());
    CHECK_EQ(p.watchFolders()[0].ignored, QStringList{dir + "/a.mp4"});

    // Catch-up: a file that came while the project was closed is imported when it opens, one from before is not
    const QString dir2 = root + "/closed";
    QDir().mkpath(dir2);
    QFile::copy(clip, dir2 + "/old.mp4");
    QThread::msleep(50);
    ProjectData d;
    d.timeline = emptyTimeline();
    WatchFolder w2;
    w2.path = dir2;
    w2.since = QDateTime::currentDateTimeUtc();
    d.watchFolders << w2;
    QThread::msleep(50);
    QFile::copy(clip, dir2 + "/came.mp4");
    Project q;
    FolderWatch fw2(&q);
    fw2.setSettleMs(300);
    fw2.setRescanMs(200);
    q.load(d);
    CHECK(waitFor([&] { return q.media().size() == 1; }, 10000));
    waitFor([] { return false; }, 1000);
    CHECK_EQ(q.media().size(), 1);
    if (!q.media().isEmpty()) CHECK_EQ(q.media()[0].path, dir2 + "/came.mp4");
    // No watch folders: nothing watched at all
    q.setWatchFolders({});
    CHECK(waitFor([&] { return fw2.watchedDirectories().isEmpty() && fw2.idle(); }, 5000));
}

// The "Watch Folder" dialog is as tall as its text needs in every language (English had an empty gap above the intro:
// its height came from a narrower width than the dialog's)
void testDialogHeight()
{
    Project p;
    emptyProject(p);
    for (const char* lang : {"en", "de", "fr"}) {
        I18n::install(lang);
        WatchFolder n;
        n.path = QDir::tempPath();
        n.bin = WatchFolderDialog::kNewBin;
        WatchFolderDialog add(p, n, true);
        add.show();
        QCoreApplication::processEvents();
        auto* intro = add.findChild<QLabel*>("watchIntro");
        if (!CHECK(intro)) continue;
        Check::report(intro->height() <= intro->heightForWidth(intro->width()) + 4, "intro taller than its text",
                      __FILE__, __LINE__, QString("%1: %2 px for %3").arg(lang).arg(intro->height())
                                              .arg(intro->heightForWidth(intro->width())));
    }
    I18n::install("de");
}

void screenshots(const QString& folder)
{
    Project p;
    emptyProject(p);
    const int rec = p.addBin(0, "Aufnahmen");
    p.addBin(p.addBin(0, "Clips"), "Roh");
    WatchFolder a;
    a.path = QDir::homePath() + "/Videos/OBS";
    a.bin = rec;
    a.video = true;
    a.audio = a.images = false;
    WatchFolder b;
    b.path = QDir::homePath() + "/ComfyUI/output";
    b.subfolders = true;
    p.setWatchFolders({a, b});
    for (const char* lang : {"de", "en"}) {
        I18n::install(lang);
        WatchFolder n;
        n.path = QDir::tempPath();
        n.bin = WatchFolderDialog::kNewBin;
        WatchFolderDialog add(p, n, true);
        add.show();
        QCoreApplication::processEvents();
        add.grab().save(QString("%1/watch-add-%2.png").arg(folder, lang));
        WatchFoldersDialog list(p);
        list.show();
        QCoreApplication::processEvents();
        list.grab().save(QString("%1/watch-list-%2.png").arg(folder, lang));
    }
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir tmp;
    // Own registry of running apps, backups and settings: schneidi-cli (a child process) uses the same ones
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    QApplication::setApplicationName("schneidi");
    Check::initMlt();
    const QString root = QDir(tmp.path()).filePath("w");
    QDir().mkpath(root);

    testRules();
    testScanner(root);
    testProject();
    testProjectFile(root);
    const QString shot = qEnvironmentVariable("WATCH_SHOT");
    if (!Check::haveFfmpeg()) {
        std::printf("ffmpeg missing: the parts with real media are skipped\n");
        return Check::result();
    }
    const QString clip = Check::makeMedia(root + "/clip.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2",
                                                               "-f", "lavfi", "-i", "sine=frequency=440:duration=2",
                                                               "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                                               "-shortest"});
    const QString longClip = Check::makeMedia(root + "/long.mkv", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=4",
                                                                   "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
                                                                   "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                                                   "-shortest"});
    if (!CHECK(!clip.isEmpty() && !longClip.isEmpty())) return Check::result();
    testCommand(root, clip);
    testCliProcess(root, clip, longClip);
    testLive(root, clip);
    testFolderWatch(root, clip);
    testDialogHeight();
    if (!shot.isEmpty()) screenshots(shot); // last: it switches the language
    return Check::result();
}
