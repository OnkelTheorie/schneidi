// schneidi-cli `watch`: the watch folders of a project (core/WatchFolders.h). Sets them up (add, remove, list), looks
// for new complete files and imports them (one pass, the default), or keeps watching (follow, command line only) and
// prints every new file as one JSON line when it is there - for scripts and AI agents that react to new recordings.
// A pass changes the project: with the project open in the app it runs there (the app's project, one undo step), else
// on the file (write lock, backup) - like `import`.
#include "cli/CommandsDetail.h"
#include "cli/Live.h"

#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/WatchFolders.h"
#include "engine/Background.h"
#include "engine/Engine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <cstdio>

namespace Cli::detail {

namespace {

QJsonObject folderJson(const Project& p, const WatchFolder& w)
{
    QJsonObject o{{"folder", w.path}, {"types", QJsonArray::fromStringList(WatchFolders::typeNames(w))}};
    if (w.subfolders) o["subfolders"] = true;
    if (w.bin != 0 && p.bin(w.bin)) o["bin"] = binPath(p, w.bin);
    if (w.since.isValid()) o["since"] = w.since.toUTC().toString(Qt::ISODate);
    if (!QFileInfo(w.path).isDir()) o["offline"] = true;
    if (!w.ignored.isEmpty()) o["ignored"] = int(w.ignored.size()); // imports the user took back
    return o;
}

QJsonArray foldersJson(const Project& p)
{
    QJsonArray out;
    for (const WatchFolder& w : p.watchFolders()) out << folderJson(p, w);
    return out;
}

// A file a watch folder brought in, with what `info` shows of it in the media pool
QJsonObject arrivalJson(const Project& p, const WatchArrival& a)
{
    QJsonObject o{{"file", a.path}, {"revision", a.revision}, {"folder", a.folder},
                  {"time", a.time.toUTC().toString(Qt::ISODate)}};
    const MediaInfo* m = p.mediaInfo(a.path);
    if (!m) {
        o["in_pool"] = false; // the user took the import back (undo)
        return o;
    }
    o["frames"] = m->length;
    o["seconds"] = seconds(m->length, p.format());
    o["video"] = m->hasVideo;
    o["audio"] = m->hasAudio;
    if (m->isImage) o["image"] = true;
    if (m->bin) o["bin"] = binPath(p, m->bin);
    return o;
}

// Bin by path ("Clips" or "Recordings/Raw"), created if missing; "" or "Master" = Master
int binByPath(Project& p, const QString& path)
{
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty() || (parts.size() == 1 && parts[0].trimmed().compare("Master", Qt::CaseInsensitive) == 0))
        return 0;
    int bin = 0;
    for (const QString& part : parts) {
        int found = -1;
        for (int child : p.childBins(bin))
            if (p.binName(child).compare(part.trimmed(), Qt::CaseInsensitive) == 0) found = child;
        bin = found >= 0 ? found : p.addBin(bin, part.trimmed());
    }
    return bin;
}

// Probes files side by side (a recording with several audio streams takes ~0.5 s)
QVector<MediaInfo> probeAll(const ProjectFormat& format, const QStringList& paths)
{
    QVector<MediaInfo> infos(paths.size());
    if (paths.size() == 1) infos[0] = Engine::probe(format, paths[0]);
    if (paths.size() <= 1) return infos;
    QThreadPool pool;
    pool.setMaxThreadCount(std::clamp(Background::threads(QThread::idealThreadCount() / 2), 1, int(paths.size())));
    MediaInfo* out = infos.data(); // detached once here, the threads only write their own element
    for (int i = 0; i < paths.size(); ++i)
        pool.start([out, &format, &paths, i] { out[i] = Engine::probe(format, paths[i]); });
    pool.waitForDone();
    return infos;
}

int settleMs(const QJsonObject& a)
{
    const double s = a.contains("settle_s") ? a["settle_s"].toDouble() : WatchFolders::kSettleMs / 1000.0;
    if (s < 0 || s > 3600) fail("BAD_ARGUMENT", "settle_s must be between 0 and 3600 seconds");
    return int(s * 1000);
}

// One pass: the new complete files of every watch folder into the project (one undo step in the app)
struct Pass {
    QVector<WatchArrival> arrivals;
    QStringList pending, unreadable;
};

Pass scanOnce(Project& p, int settle)
{
    const QVector<WatchFolder> watches = p.watchFolders();
    QSet<QString> pool;
    for (const MediaInfo& m : p.media()) pool.insert(WatchFolders::pathKey(m.path));
    WatchFolders::Scanner scanner(settle);
    const WatchFolders::Scanner::Result found = scanner.scan(watches, [&](const QString& key) { return pool.contains(key); });
    QStringList paths;
    for (const WatchFolders::Scanner::Found& f : found.ready) paths << f.path;
    const QVector<MediaInfo> infos = probeAll(p.format(), paths);
    Pass pass;
    pass.pending = found.pending;
    QVector<WatchImport> imports;
    for (int i = 0; i < infos.size(); ++i) {
        // Not readable (yet): an MP4 whose recorder writes the index last and paused, or a broken file
        if (infos[i].length <= 0 || (!infos[i].hasVideo && !infos[i].hasAudio)) pass.unreadable << paths[i];
        else imports << WatchImport{infos[i], watches[found.ready[i].watch].path};
    }
    pass.arrivals = p.addWatchedMedia(imports);
    return pass;
}

QJsonObject watchOnce(const QJsonObject& a)
{
    const bool add = a.contains("add"), remove = a.contains("remove");
    if (add && remove) fail("BAD_ARGUMENT", "give either 'add' or 'remove'");
    for (const char* k : {"bin", "subfolders", "types", "existing"})
        if (a.contains(k) && !add) fail("BAD_ARGUMENT", QString("'%1' goes with 'add' (the folder to watch)").arg(k));
    const bool scan = !add && !remove && !a["list"].toBool();
    const int settle = settleMs(a);
    Session s(a["project"].toString(), add || remove || scan ? Session::Access::Write : Session::Access::Read);
    Project& p = s.project;
    QJsonObject o{{"project", s.path}};
    bool changed = false;
    if (add) {
        const QString folder = QDir::cleanPath(a["add"].toString());
        if (!QFileInfo(folder).isDir()) fail("NOT_FOUND", "folder not found: " + folder);
        QVector<WatchFolder> list = p.watchFolders();
        const int i = WatchFolders::indexOf(list, folder);
        WatchFolder w = i >= 0 ? list[i] : WatchFolder{};
        if (i < 0) {
            w.path = folder;
            w.since = QDateTime::currentDateTimeUtc(); // from now on
        }
        if (a.contains("subfolders")) w.subfolders = a["subfolders"].toBool();
        if (a.contains("types") && !WatchFolders::setTypes(w, a["types"].toString().split(',')))
            fail("BAD_ARGUMENT", "types: a comma list of video, audio, image");
        if (a.contains("bin")) w.bin = binByPath(p, a["bin"].toString());
        if (a["existing"].toBool()) w.since = QDateTime(); // also the files that are there already
        if (i < 0) list << w;
        else list[i] = w;
        changed = list != p.watchFolders();
        p.setWatchFolders(list);
    } else if (remove) {
        QVector<WatchFolder> list = p.watchFolders();
        const int i = WatchFolders::indexOf(list, a["remove"].toString());
        if (i < 0) {
            QStringList watched;
            for (const WatchFolder& w : list) watched << w.path;
            fail("NOT_FOUND", QString("the project does not watch %1 (watched: %2)")
                                  .arg(a["remove"].toString(), watched.isEmpty() ? "none" : watched.join(", ")));
        }
        list.removeAt(i);
        p.setWatchFolders(list);
        changed = true;
    }
    Pass pass;
    if (scan) {
        if (p.watchFolders().isEmpty())
            o["notes"] = QJsonArray{"the project watches no folder: add one with 'add' (its new media files then come "
                                    "into the media pool on their own)"};
        else
            pass = scanOnce(p, settle);
    }
    o["revision"] = p.watchRevision();
    o["folders"] = foldersJson(p);
    if (scan) {
        // `since`: everything after that revision (also what the app's own watching brought in meanwhile), else
        // what this pass imported
        const bool since = a.contains("since");
        QSet<int> now;
        for (const WatchArrival& x : pass.arrivals) now.insert(x.revision);
        QJsonArray news;
        for (const WatchArrival& x : p.watchLog())
            if (since ? x.revision > a["since"].toInt() : now.contains(x.revision)) news << arrivalJson(p, x);
        o["new"] = news;
        o["imported"] = int(pass.arrivals.size());
        if (!pass.pending.isEmpty()) o["pending"] = QJsonArray::fromStringList(pass.pending);
        if (!pass.unreadable.isEmpty()) o["unreadable"] = QJsonArray::fromStringList(pass.unreadable);
        if (since && !p.watchLog().isEmpty() && a["since"].toInt() < p.watchLog().first().revision - 1)
            o["notes"] = QJsonArray{QString("only the newest %1 files are remembered: some after revision %2 are not "
                                            "listed")
                                        .arg(WatchFolders::kLogSize)
                                        .arg(a["since"].toInt())};
    }
    if (changed || !pass.arrivals.isEmpty()) {
        const QString backup = s.save();
        if (!backup.isEmpty()) o["backup"] = backup;
    }
    if (s.live) o["live"] = true;
    return o;
}

// ---------- follow ----------

// Size and time of every file the watch folders list now (by path key)
QHash<QString, QPair<qint64, qint64>> listing(const QVector<WatchFolder>& watches)
{
    QHash<QString, QPair<qint64, qint64>> out;
    for (const WatchFolder& w : watches)
        for (const WatchFolders::Listed& f : WatchFolders::list(w)) out.insert(WatchFolders::pathKey(f.path), {f.size, f.modified});
    return out;
}

// The watch folders as a pass answered them (what to list between passes)
QVector<WatchFolder> watchesOf(const QJsonArray& folders)
{
    QVector<WatchFolder> out;
    for (const QJsonValue& v : folders) {
        WatchFolder w;
        w.path = v["folder"].toString();
        w.subfolders = v["subfolders"].toBool();
        QStringList types;
        for (const QJsonValue& t : v["types"].toArray()) types << t.toString();
        WatchFolders::setTypes(w, types);
        out << w;
    }
    return out;
}

void warn(const Error& e)
{
    const QByteArray line = QJsonDocument(QJsonObject{{"warning", e.message}, {"code", e.code}}).toJson(QJsonDocument::Compact);
    std::fprintf(stderr, "%s\n", line.constData());
    std::fflush(stderr);
}

// Keeps watching: a pass whenever a file in a watched folder came, changed or went and then stayed unchanged for the
// settle time (a recording is looked at when it is done, not every second while it grows), or the project file
// changed (someone added a folder). Each pass goes through execute(): into the app if it has the project open (its
// connection closed again right after, so the app is free for other AIs), else on the file. Never returns.
QJsonObject follow(const QJsonObject& a, Context& ctx)
{
    const Command* self = Cli::find("watch");
    QJsonObject args = a;
    args.remove("follow");
    const int settle = settleMs(a);
    int revision = a.contains("since") ? a["since"].toInt() : -1; // -1: the first pass shows what it imports
    QVector<WatchFolder> watches;
    QHash<QString, QPair<qint64, qint64>> last;
    QHash<QString, qint64> dirty; // files that came or changed since the last pass -> when they last changed (ms)
    QString projectFile;
    QDateTime projectTime;
    for (bool pass = true;; QThread::msleep(1000)) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!pass) {
            const auto now = listing(watches);
            for (auto it = now.begin(); it != now.end(); ++it)
                if (last.value(it.key(), {-1, -1}) != it.value()) dirty[it.key()] = nowMs;
            for (auto it = dirty.begin(); it != dirty.end();)
                it = now.contains(it.key()) ? std::next(it) : dirty.erase(it);
            last = now;
            pass = std::any_of(dirty.cbegin(), dirty.cend(), [&](qint64 t) { return nowMs - t >= settle; })
                   || (!projectFile.isEmpty() && QFileInfo(projectFile).lastModified() != projectTime);
            if (!pass) continue;
        }
        QJsonObject passArgs = args;
        if (revision >= 0) passArgs["since"] = revision;
        QJsonObject r;
        try {
            r = Cli::execute(*self, passArgs, ctx);
            Live::release();
        } catch (const Error& e) {
            Live::release();
            // The app is busy with another AI, waits for the user or was closed; the file is being saved: again soon
            static const QStringList transient{"APP_BUSY", "APP_WAITING", "APP_DISCONNECTED", "PROJECT_LOCKED"};
            if (!transient.contains(e.code)) throw;
            warn(e);
            QThread::msleep(1000);
            continue;
        }
        for (const QJsonValue& v : r["new"].toArray()) ctx.line(v.toObject());
        revision = r["revision"].toInt();
        watches = watchesOf(r["folders"].toArray());
        if (watches.isEmpty())
            fail("NOT_FOUND", "the project watches no folder: add one first (watch <project> --add <folder>)");
        last = listing(watches);
        dirty.clear();
        for (const QJsonValue& v : r["pending"].toArray()) dirty.insert(WatchFolders::pathKey(v.toString()), nowMs);
        projectFile = r["project"].toString();
        projectTime = QFileInfo(projectFile).lastModified();
        pass = false;
    }
}

} // namespace

QJsonObject cmdWatch(const QJsonObject& a, Context& ctx)
{
    if (!a["follow"].toBool()) return watchOnce(a);
    if (!ctx.line || Live::host())
        fail("BAD_ARGUMENT", "follow keeps running and prints a line per new file: command line only (schneidi-cli "
                             "watch <project> --follow); here call `watch` again with 'since' (the revision of the "
                             "last answer) to see what arrived");
    if (a.contains("add") || a.contains("remove") || a["list"].toBool())
        fail("BAD_ARGUMENT", "follow watches; set folders up with a call of their own ('add', 'remove', 'list')");
    return follow(a, ctx);
}

} // namespace Cli::detail
