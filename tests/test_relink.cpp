// SPDX-License-Identifier: AGPL-3.0-only
// Test finding moved media again (core/Relink.h, Project::relinkMedia, app/MainWindowRelink.cpp):
// - the folders near the old places (old folder, its parent) and the bounded walk: sub-folders, sibling folders of the
//   parent (clipi/cs2 -> clipi/Alt/cs2), depth limit, no hidden folders, no endless loop through a linked folder
// - pairing: the same size decides between camera files of one name, a file of another size or length is not taken by
//   itself (a folder the user picked still takes it), never another kind of media, unreadable ones not by themselves,
//   no second pool entry of a file, equally good candidates leave the file offline
// - the open project: one undo step for all files, undo/redo, what is outside the history (source marks) stays
// - the app: opening a project with moved files relinks them by itself (no question), the rest is asked about, the
//   menu entry (File, Media Pool) is enabled only while files are missing, a search at any time is one undo step
#include "check.h"

#include "app/MainWindow.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Relink.h"
#include "engine/Engine.h"
#include "ui/MediaPool.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QtEndian>
#include <clocale>
#include <functional>

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

// A file of `bytes` bytes (contents do not matter where nothing probes it)
QString makeFile(const QString& path, int bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return {};
    f.write(QByteArray(bytes, 'x'));
    return path;
}

// A real sound file (16-bit stereo PCM WAV, silence) of `ms` milliseconds: probed as sound
QString makeWav(const QString& path, int ms)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    const quint32 rate = 48000, frames = rate * ms / 1000, data = frames * 4;
    QByteArray b;
    const auto u32 = [&](quint32 v) { v = qToLittleEndian(v); b.append(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](quint16 v) { v = qToLittleEndian(v); b.append(reinterpret_cast<const char*>(&v), 2); };
    b.append("RIFF");
    u32(36 + data);
    b.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    b.append("data");
    u32(data);
    b.append(QByteArray(int(data), '\0'));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size()) return {};
    return path;
}

MediaInfo missingVideo(const QString& path, qint64 size, int length = 100)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = true;
    m.hasAudio = true;
    m.fileSize = size;
    return m;
}

MediaInfo probedAs(const QString& path, bool video, bool audio, int length = 100)
{
    MediaInfo m;
    m.path = path;
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

void testRootsAndWalk(const QTemporaryDir& tmp)
{
    const QString clipi = tmp.filePath("walk/clipi");
    // Moved from clipi/cs2 into clipi/Alt/cs2 (sibling of the old folder's parent) and clipi/Alt (one level up)
    makeFile(clipi + "/Alt/cs2/a.mkv", 100);
    makeFile(clipi + "/Alt/b.mkv", 200);
    // Too deep below the parent, in a hidden folder: not found
    makeFile(clipi + "/1/2/3/4/5/deep.mkv", 10);
    makeFile(clipi + "/.versteckt/hidden.mkv", 10);
    const QStringList missing{clipi + "/cs2/a.mkv", clipi + "/cs2/b.mkv", clipi + "/cs2/deep.mkv", clipi + "/cs2/hidden.mkv"};

    // cs2 is gone: only its parent is walked (each once)
    QVector<Relink::Root> roots = Relink::nearRoots(missing);
    CHECK_EQ(roots.size(), 1);
    CHECK_EQ(QFileInfo(roots.value(0).dir).canonicalFilePath(), QFileInfo(clipi).canonicalFilePath());
    CHECK_EQ(roots.value(0).depth, Relink::kNearDepth);
    // An old folder that is there comes first (moved into a sub-folder of it)
    QDir().mkpath(clipi + "/cs2");
    roots = Relink::nearRoots(missing);
    CHECK_EQ(roots.size(), 2);
    CHECK(roots.value(0).dir.endsWith("/cs2"));
    QDir(clipi + "/cs2").removeRecursively();
    // Neither the filesystem root nor the home folder itself is walked
    CHECK(Relink::nearRoots({QDir::rootPath() + "gone.mkv"}).isEmpty());
    CHECK(Relink::nearRoots({QDir::homePath() + "/gone-folder-xyz/a.mkv"}).isEmpty());

    // A linked folder that leads back up (a loop): not followed, the walk ends, nothing found twice
#ifndef Q_OS_WIN
    CHECK(QFile::link(clipi, clipi + "/Alt/schleife"));
#endif
    bool truncated = true;
    Relink::Progress progress;
    const QVector<Relink::Found> found =
        Relink::find(roots, {"a.mkv", "b.mkv", "deep.mkv", "hidden.mkv"}, Relink::kNearLimits, &progress, &truncated);
    CHECK(!truncated);
    CHECK(progress.dirs.load() > 0);
    QStringList names;
    for (const Relink::Found& f : found) names << QFileInfo(f.path).fileName();
    names.sort();
    CHECK_EQ(names, QStringList({"a.mkv", "b.mkv"}));
    for (const Relink::Found& f : found)
        if (f.path.endsWith("a.mkv")) CHECK_EQ(f.size, qint64(100));

    // The limits: a folder count stops the walk (said as truncated); a cancel likewise
    Relink::find({Relink::Root{clipi, 8}}, {"a.mkv"}, Relink::Limits{1, 0}, nullptr, &truncated);
    CHECK(truncated);
    Relink::Progress cancelled;
    cancelled.cancel = true;
    CHECK(Relink::find({Relink::Root{clipi, 8}}, {"a.mkv"}, Relink::kNearLimits, &cancelled, &truncated).isEmpty());
    CHECK(truncated);

    // The whole thing on a project: found by itself (the sizes known), the others stay offline
    ProjectData d;
    for (const QString& p : missing) d.media << missingVideo(p, p.endsWith("a.mkv") ? 100 : p.endsWith("b.mkv") ? 200 : 10);
    Clip c;
    c.id = 1;
    c.mediaPath = clipi + "/cs2/a.mkv";
    c.out = 9;
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << c;
    CHECK_EQ(ProjectFile::relinkNear(&d), 2);
    CHECK_EQ(d.media.value(0).path, QFileInfo(clipi + "/Alt/cs2/a.mkv").absoluteFilePath());
    CHECK_EQ(d.media.value(1).path, QFileInfo(clipi + "/Alt/b.mkv").absoluteFilePath());
    CHECK_EQ(d.timeline.video[0].clips[0].mediaPath, d.media.value(0).path);
    CHECK_EQ(ProjectFile::missingMedia(d).size(), 2);
}

void testMatching(const QTemporaryDir& tmp)
{
    using Relink::Found;
    using Relink::Mode;
    const QString root = tmp.filePath("match");
    // Camera files of one name, moved into folders whose names no longer tell them apart: the size does
    const QString one = makeFile(root + "/Alt/eins/C0001.MP4", 200), two = makeFile(root + "/Alt/zwei/C0001.MP4", 100);
    const QVector<Found> cards{{one, 200}, {two, 100}};
    QVector<MediaInfo> missing{missingVideo("/karte/camA/C0001.MP4", 100), missingVideo("/karte/camB/C0001.MP4", 200)};
    QHash<QString, QString> moved = Relink::match(missing, cards, Mode::Near, {});
    CHECK_EQ(moved.size(), 2);
    CHECK_EQ(moved.value("/karte/camA/C0001.MP4"), two);
    CHECK_EQ(moved.value("/karte/camB/C0001.MP4"), one);
    // Sizes not known (an older project): nothing tells them apart - both stay offline, in either mode
    missing[0].fileSize = missing[1].fileSize = 0;
    CHECK(Relink::match(missing, cards, Mode::Near, {}).isEmpty());
    CHECK(Relink::match(missing, cards, Mode::Folder, {}).isEmpty());
    // ... a folder name at the end that matches still decides (as before)
    const QVector<Found> named{{root + "/neu/camA/C0001.MP4", 1}, {root + "/neu/camB/C0001.MP4", 2}};
    moved = Relink::match(missing, named, Mode::Near, {});
    CHECK_EQ(moved.value("/karte/camA/C0001.MP4"), root + "/neu/camA/C0001.MP4");
    CHECK_EQ(moved.value("/karte/camB/C0001.MP4"), root + "/neu/camB/C0001.MP4");
    // Two files of the same size and folder score for one missing file: offline (no guessing)
    QVector<MediaInfo> single{missingVideo("/karte/x/C0001.MP4", 100)};
    CHECK(Relink::match(single, {{root + "/a/C0001.MP4", 100}, {root + "/b/C0001.MP4", 100}}, Mode::Near, {}).isEmpty());
    // ... one of them the same size: that one
    CHECK_EQ(Relink::match(single, {{root + "/a/C0001.MP4", 300}, {root + "/b/C0001.MP4", 100}}, Mode::Near, {})
                 .value("/karte/x/C0001.MP4"),
             root + "/b/C0001.MP4");

    // Another size: by itself never (another file of that name); a folder the user picked takes it (re-encoded)
    CHECK(Relink::match(single, {{root + "/a/C0001.MP4", 999}}, Mode::Near, {}).isEmpty());
    CHECK_EQ(Relink::match(single, {{root + "/a/C0001.MP4", 999}}, Mode::Folder, {}).size(), 1);

    // Size not known: the length decides (probed); a different length not by itself, the user's folder prefers the
    // same length
    QVector<MediaInfo> noSize{missingVideo("/alt/clip.mp4", 0, 250)};
    const QString same = root + "/s/clip.mp4", other = root + "/o/clip.mp4";
    QHash<QString, MediaInfo> probed{{same, probedAs(same, true, true, 250)}, {other, probedAs(other, true, true, 900)}};
    CHECK(Relink::match(noSize, {{other, 5}}, Mode::Near, probed).isEmpty());
    CHECK_EQ(Relink::match(noSize, {{other, 5}, {same, 5}}, Mode::Near, probed).value("/alt/clip.mp4"), same);
    CHECK_EQ(Relink::match(noSize, {{other, 5}, {same, 5}}, Mode::Folder, probed).value("/alt/clip.mp4"), same);
    // A rounding of a frame or two is the same length
    probed[other].length = 251;
    CHECK_EQ(Relink::match(noSize, {{other, 5}}, Mode::Near, probed).size(), 1);

    // Never another kind of media: a sound file of that name for a video (even the same size), a still for a video
    const QString sound = root + "/s/clip.mp4";
    probed[sound] = probedAs(sound, false, true, 250);
    CHECK(Relink::match({missingVideo("/alt/clip.mp4", 5, 250)}, {{sound, 5}}, Mode::Near, probed).isEmpty());
    CHECK(Relink::match({missingVideo("/alt/clip.mp4", 5, 250)}, {{sound, 5}}, Mode::Folder, probed).isEmpty());
    MediaInfo still = probedAs(sound, true, false, 125);
    still.isImage = true;
    probed[sound] = still;
    CHECK(Relink::match({missingVideo("/alt/clip.mp4", 5, 250)}, {{sound, 5}}, Mode::Folder, probed).isEmpty());
    // Unreadable (being copied, broken): not by itself; the user's folder as before
    probed[sound] = MediaInfo{};
    CHECK(Relink::match({missingVideo("/alt/clip.mp4", 5, 250)}, {{sound, 5}}, Mode::Near, probed).isEmpty());
    CHECK_EQ(Relink::match({missingVideo("/alt/clip.mp4", 5, 250)}, {{sound, 5}}, Mode::Folder, probed).size(), 1);
    // A file already in the Media Pool is never taken (no second entry of one file)
    CHECK(Relink::match(single, {{root + "/b/C0001.MP4", 100}}, Mode::Folder, {}, {root + "/b/C0001.MP4"}).isEmpty());

    // The folder search of the dialog (ProjectFile::relink) follows the same rules: the size picks the card
    ProjectData d;
    d.media << missingVideo("/karte/camA/C0001.MP4", 100) << missingVideo("/karte/camB/C0001.MP4", 200);
    CHECK_EQ(ProjectFile::relink(&d, root + "/Alt"), 2);
    CHECK_EQ(d.media.value(0).path, QFileInfo(two).absoluteFilePath());
    CHECK_EQ(d.media.value(1).path, QFileInfo(one).absoluteFilePath());

    // The size is saved with the project (and read back): next time a moved file is told apart
    {
        ProjectData s;
        s.media << missingVideo(one, 0) << missingVideo("/gone/x.mp4", 4242);
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(s, tmp.filePath("s.schneidi")), tmp.filePath("s.schneidi"), &back, &err));
        CHECK_EQ(back.media.value(0).fileSize, qint64(200)); // as it is on disk now
        CHECK_EQ(back.media.value(1).fileSize, qint64(4242)); // missing: as last seen
    }
}

void testProjectUndo(const QTemporaryDir& tmp)
{
    const QString now = makeFile(tmp.filePath("undo/neu/a.mp4"), 50);
    const QString old = tmp.filePath("undo/alt/a.mp4");
    ProjectData d;
    MediaInfo m = missingVideo(old, 50);
    m.clipColor = "orange";
    d.media << m << missingVideo(tmp.filePath("undo/alt/b.mp4"), 7);
    Clip c;
    c.id = 1;
    c.mediaPath = old;
    c.out = 9;
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << c;
    d.timeline.audio.resize(1);
    d.lastClipId = 1;
    Project p;
    p.load(d);
    // A second timeline that uses the file too: it follows in the same step
    const int second = p.addTimeline();
    p.edit("clip", [&](Timeline& tl) {
        Clip k = c;
        k.id = p.newClipId();
        tl.video[0].clips << k;
    });
    const int before = p.undoStack()->count();
    CHECK_EQ(p.missingMedia().size(), 2);
    // Stale pairs are left out: an old path not in the pool, a target already in it
    CHECK_EQ(p.relinkMedia({{"/nicht/im/pool.mp4", now}}, {}, "x"), 0);
    CHECK_EQ(p.undoStack()->count(), before);
    CHECK_EQ(p.relinkMedia({{old, now}}, {}, "Fehlende Medien wiedergefunden"), 1);
    CHECK_EQ(p.undoStack()->count(), before + 1); // one step
    CHECK(p.mediaInfo(now) && !p.mediaInfo(old));
    CHECK_EQ(p.mediaInfo(now)->clipColor, QString("orange")); // the pool entry keeps what the user set
    CHECK_EQ(p.mediaInfo(now)->fileSize, qint64(50));
    CHECK_EQ(p.missingMedia().size(), 1);
    const auto clipPath = [&](int seq) {
        const Sequence* s = p.sequence(seq);
        return s && !s->timeline.video.isEmpty() && !s->timeline.video[0].clips.isEmpty()
                   ? s->timeline.video[0].clips[0].mediaPath
                   : QString();
    };
    const int first = p.sequences().value(0).id;
    CHECK_EQ(clipPath(first), now);
    CHECK_EQ(clipPath(second), now);
    // Outside the history (source marks) after the step: undo leaves them
    p.setMediaMarks(now, 3, 8);
    p.undoStack()->undo();
    CHECK(p.mediaInfo(old) && !p.mediaInfo(now));
    CHECK_EQ(clipPath(first), old);
    CHECK_EQ(clipPath(second), old);
    CHECK_EQ(p.mediaInfo(old)->markIn, 3);
    CHECK_EQ(p.missingMedia().size(), 2);
    p.undoStack()->redo();
    CHECK(p.mediaInfo(now) && clipPath(second) == now && p.mediaInfo(now)->markOut == 8);
}

// Clicks a button of the "Medien fehlen" box once it shows (it comes after the search, from the event loop)
void answerMissing(const char* button, int* asked)
{
    auto* timer = new QTimer;
    timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, timer, [timer, button, asked] {
        for (QWidget* w : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(w); box && box->isVisible() && box->objectName() == "missingMediaBox") {
                ++*asked;
                box->findChild<QPushButton*>(button)->click();
                timer->stop();
                timer->deleteLater();
                return;
            }
    });
    timer->start();
}

// The entry's state as the File menu shows it (updated when it opens)
bool fileMenuEnabled(MainWindow& w)
{
    for (QMenu* m : w.menuBar()->findChildren<QMenu*>())
        if (m->title() == QString("&Datei")) {
            emit m->aboutToShow();
            break;
        }
    return w.findMissingAction()->isEnabled();
}

// The Media Pool menu on the empty space: is the entry in it, enabled?
std::pair<bool, bool> poolMenuEntry(MainWindow& w)
{
    auto* pool = w.findChild<MediaPool*>();
    auto* list = pool->findChild<QListWidget*>();
    std::pair<bool, bool> out{false, false};
    QTimer::singleShot(0, [&] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            for (QAction* a : menu->actions())
                if (a == w.findMissingAction()) out = {true, a->isEnabled()};
            menu->close();
        }
    });
    list->clearSelection();
    QPoint empty(-1, -1); // a point of the list with no clip under it
    for (int y = list->viewport()->height() - 2; y > 0 && empty.x() < 0; y -= 8)
        for (int x = list->viewport()->width() - 2; x > 0 && empty.x() < 0; x -= 8)
            if (!list->itemAt(QPoint(x, y))) empty = QPoint(x, y);
    if (!CHECK(empty.x() >= 0)) return out;
    emit list->customContextMenuRequested(empty);
    return out;
}

void testApp(const QTemporaryDir& tmp)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show(); // offscreen: nothing visible; the Media Pool gets its size
    Project* p = w.liveProject();

    // A project whose recordings were moved from clipi/cs2 into clipi/Alt/cs2 (one is gone for good)
    const QString clipi = tmp.filePath("app/clipi");
    const QString a = makeWav(clipi + "/cs2/a.wav", 1000), b = makeWav(clipi + "/cs2/b.wav", 1500);
    const QString gone = makeWav(clipi + "/cs2/weg.wav", 500);
    ProjectData d;
    for (const QString& f : {a, b, gone}) d.media << Engine::probe(d.format, f);
    CHECK(d.media.value(0).hasAudio && d.media.value(0).length > 0);
    d.timeline.video.resize(1);
    d.timeline.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = a;
    c.out = 9;
    d.timeline.audio[0].clips << c;
    d.lastClipId = 1;
    const QString project = tmp.filePath("app/schnitt.schneidi");
    CHECK(ProjectFile::save(d, project, &err));
    QDir().mkpath(clipi + "/Alt/cs2");
    CHECK(QFile::rename(a, clipi + "/Alt/cs2/a.wav"));
    CHECK(QFile::rename(b, clipi + "/Alt/cs2/b.wav"));
    CHECK(QFile::remove(gone));

    // Opening: usable at once (the search runs in the background), the two found by themselves as one step, only the
    // lost one asked about
    int asked = 0;
    answerMissing("relinkLeaveOffline", &asked);
    CHECK(w.openProject(project));
    CHECK(waitFor([&] { return !w.relinkSearching() && asked > 0; }));
    CHECK_EQ(asked, 1);
    const QString movedA = QFileInfo(clipi + "/Alt/cs2/a.wav").absoluteFilePath();
    CHECK(p->mediaInfo(movedA));
    CHECK(p->mediaInfo(QFileInfo(clipi + "/Alt/cs2/b.wav").absoluteFilePath()));
    CHECK_EQ(p->timeline().audio.value(0).clips.value(0).mediaPath, movedA);
    CHECK_EQ(p->missingMedia(), QStringList{gone});
    CHECK_EQ(p->undoStack()->count(), 1);
    CHECK(w.statusBar()->currentMessage().contains("2 verschobene Dateien automatisch wiedergefunden"));
    // Undo: offline again, the entry enabled; redo
    p->undoStack()->undo();
    CHECK_EQ(p->missingMedia().size(), 3);
    CHECK_EQ(p->timeline().audio.value(0).clips.value(0).mediaPath, a);
    p->undoStack()->redo();
    CHECK_EQ(p->missingMedia().size(), 1);

    // The menu entry: enabled while something is missing (File menu, Media Pool menu on the empty space)
    CHECK(fileMenuEnabled(w));
    auto [inMenu, enabled] = poolMenuEntry(w);
    CHECK(inMenu && enabled);
    // Nothing missing (the lost file removed from the pool): disabled
    CHECK(p->removeMedia({gone}, true));
    CHECK(p->missingMedia().isEmpty());
    CHECK(!fileMenuEnabled(w));
    std::tie(inMenu, enabled) = poolMenuEntry(w);
    CHECK(inMenu && !enabled);

    // At any time, without reopening: a file moved now (into a sub-folder of its folder) is found by the entry, one
    // undo step, nothing asked (none left missing)
    QDir().mkpath(clipi + "/Alt/cs2/sortiert");
    CHECK(QFile::rename(movedA, clipi + "/Alt/cs2/sortiert/a.wav"));
    CHECK(fileMenuEnabled(w)); // noticed when the menu opens
    const int steps = p->undoStack()->count();
    asked = 0;
    w.findMissingAction()->trigger();
    CHECK(w.relinkSearching());
    CHECK(!w.findMissingAction()->isEnabled()); // not twice at once
    CHECK(waitFor([&] { return !w.relinkSearching(); }));
    QApplication::processEvents();
    const QString sorted = QFileInfo(clipi + "/Alt/cs2/sortiert/a.wav").absoluteFilePath();
    CHECK_EQ(p->timeline().audio.value(0).clips.value(0).mediaPath, sorted);
    CHECK_EQ(p->undoStack()->count(), steps + 1);
    CHECK_EQ(asked, 0);
    CHECK(w.statusBar()->currentMessage().contains("1 verschobene Datei automatisch wiedergefunden"));
    CHECK(!w.findMissingAction()->isEnabled());
    p->undoStack()->undo();
    CHECK_EQ(p->timeline().audio.value(0).clips.value(0).mediaPath, movedA);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("relink");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir tmp;
    testRootsAndWalk(tmp);
    testMatching(tmp);
    testProjectUndo(tmp);
    testApp(tmp);
    return Check::result();
}
