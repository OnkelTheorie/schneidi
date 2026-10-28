// SPDX-License-Identifier: AGPL-3.0-only
// Test Media Pool: bins (create, rename, move, remove), clip colors, flags - undo, save/load,
// broken files (missing bins, cycles, unknown colors); Media Pool widget offscreen (bin switch, import into bin)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include <QEventLoop>
#include <QThread>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimer>
#include <QFile>
#include <QDir>
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"

#include <QApplication>
#include <QListWidget>
#include <QTreeWidget>
#include <QUndoStack>

namespace {

MediaInfo media(const QString& name)
{
    return MediaInfo{"/x/" + name, name, 100, true, true, false};
}

int binOf(const Project& p, const QString& name) { return p.mediaInfo("/x/" + name)->bin; }

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("mediapool");

    Project p;
    p.addMedia(media("a.mp4"));
    p.addMedia(media("b.mp4"));
    p.addMedia(media("c.mp4"));
    QUndoStack* undo = p.undoStack();

    // --- Create bins: default names "Bin 1", "Bin 2"; sub-bin; sorted by name
    const int b1 = p.addBin(0);
    const int b2 = p.addBin(0, "  Zeug  ");
    const int sub = p.addBin(b1, "Unter");
    CHECK_EQ(p.binName(b1), QString("Bin 1"));
    CHECK_EQ(p.binName(b2), QString("Zeug"));
    CHECK_EQ(p.binName(0), QString("Master"));
    CHECK_EQ(p.childBins(0), (QVector<int>{b1, b2}));
    CHECK_EQ(p.childBins(b1), QVector<int>{sub});
    CHECK(p.binInside(sub, b1) && p.binInside(sub, 0) && !p.binInside(b1, sub));
    CHECK_EQ(p.binName(p.addBin(0)), QString("Bin 2")); // first free number
    undo->undo();
    CHECK_EQ(p.bins().size(), 3);

    // --- Move clips, color, flags (one undo step each)
    int n = undo->index();
    p.moveMediaToBin({"/x/a.mp4", "/x/b.mp4"}, sub);
    CHECK(binOf(p, "a.mp4") == sub && binOf(p, "b.mp4") == sub && binOf(p, "c.mp4") == 0);
    p.setClipColor({"/x/a.mp4"}, "orange");
    p.setClipColor({"/x/b.mp4"}, "gibtsnicht"); // unknown: nothing
    p.setFlag({"/x/a.mp4", "/x/c.mp4"}, "red", true);
    p.setFlag({"/x/a.mp4"}, "blue", true); // order as in the color list: blue before red
    p.setFlag({"/x/a.mp4"}, "blue", true); // already set: no step
    CHECK_EQ(undo->index(), n + 4);
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->clipColor, QString("orange"));
    CHECK(p.mediaInfo("/x/b.mp4")->clipColor.isEmpty());
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->flags, (QStringList{"blue", "red"}));
    CHECK_EQ(p.mediaInfo("/x/c.mp4")->flags, QStringList{"red"});
    p.setFlag({"/x/a.mp4"}, "red", false);
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->flags, QStringList{"blue"});
    p.clearFlags({"/x/a.mp4", "/x/c.mp4"});
    CHECK(p.mediaInfo("/x/a.mp4")->flags.isEmpty() && p.mediaInfo("/x/c.mp4")->flags.isEmpty());
    undo->undo();
    undo->undo();
    CHECK_EQ(p.mediaInfo("/x/a.mp4")->flags, (QStringList{"blue", "red"}));
    undo->redo();
    undo->redo();

    // --- Move bin: not into itself/below
    p.moveBin(b1, sub);
    CHECK_EQ(p.bin(b1)->parent, 0);
    p.moveBin(sub, b2);
    CHECK_EQ(p.bin(sub)->parent, b2);
    undo->undo();
    CHECK_EQ(p.bin(sub)->parent, b1);
    // Rename (empty = nothing)
    p.renameBin(b2, "Musik");
    p.renameBin(b2, "   ");
    CHECK_EQ(p.binName(b2), QString("Musik"));

    // --- Remove bin: content and sub-bins move up one level; undo restores everything
    p.removeBin(b1);
    CHECK(!p.bin(b1) && p.bin(sub) && p.bin(sub)->parent == 0 && binOf(p, "a.mp4") == sub);
    p.removeBin(sub);
    CHECK(binOf(p, "a.mp4") == 0 && binOf(p, "b.mp4") == 0);
    undo->undo();
    undo->undo();
    CHECK(p.bin(b1) && p.bin(sub)->parent == b1 && binOf(p, "a.mp4") == sub);

    // --- Media imported later: undo of the organization leaves them alone; import into a missing bin -> Master
    MediaInfo d = media("d.mp4");
    d.bin = 999;
    p.addMedia(d);
    CHECK_EQ(binOf(p, "d.mp4"), 0);

    // --- Save -> load: everything the same, new bin ID after loading not duplicated
    {
        ProjectData data = p.data();
        QString err;
        ProjectData back;
        const QByteArray json = ProjectFile::toJson(data, "/x/p.schneidi");
        CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
        CHECK(back.bins == data.bins);
        for (int i = 0; i < data.media.size(); ++i)
            CHECK(back.media[i].bin == data.media[i].bin && back.media[i].clipColor == data.media[i].clipColor
                  && back.media[i].flags == data.media[i].flags);
        CHECK(ProjectFile::toJson(back, "/x/p.schneidi") == json);
        Project q;
        q.load(back);
        const int fresh = q.addBin(0);
        CHECK(fresh > b1 && fresh > b2 && fresh > sub);
        CHECK(!q.undoStack()->canRedo() && q.undoStack()->count() == 1);
    }

    // --- Broken file: bin with missing parent bin, parent cycle, duplicate ID, unknown color/flags
    {
        ProjectData data;
        QString err;
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,
            "media":[{"path":"/x/a.mp4","name":"a.mp4","length":10,"hasVideo":true,"bin":7,"clipColor":"quatsch",
                      "flags":["red","red","nix","blue"]},
                     {"path":"/x/b.mp4","name":"b.mp4","length":10,"hasVideo":true,"bin":2}],
            "bins":[{"id":2,"parent":3,"name":"A"},{"id":3,"parent":2,"name":"B"},{"id":3,"parent":0,"name":"doppelt"},
                    {"id":4,"parent":42,"name":"Waise"},{"id":0,"parent":0,"name":"Master?"},{"id":5,"name":""}],
            "timeline":{"video":[],"audio":[]}})", "/x/p.schneidi", &data, &err));
        Project q;
        q.load(data);
        CHECK_EQ(q.bins().size(), 3);
        CHECK(q.bin(4) && q.bin(4)->parent == 0);
        CHECK(!q.binInside(q.bin(2)->parent, 2) || q.bin(2)->parent == 0); // cycle broken up
        CHECK(q.binInside(2, 0) && q.binInside(3, 0));
        CHECK_EQ(q.mediaInfo("/x/a.mp4")->bin, 0); // bin 7 does not exist
        CHECK(q.mediaInfo("/x/a.mp4")->clipColor.isEmpty());
        CHECK_EQ(q.mediaInfo("/x/a.mp4")->flags, (QStringList{"red", "blue"}));
        CHECK_EQ(q.mediaInfo("/x/b.mp4")->bin, 2);
        // old file without bins: everything in Master, file stays without new fields
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,
            "media":[{"path":"/x/a.mp4","name":"a.mp4","length":10,"hasVideo":true}],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &data, &err));
        CHECK(data.bins.isEmpty() && data.media[0].bin == 0);
        const QByteArray json = ProjectFile::toJson(data, "/x/p.schneidi");
        CHECK(!json.contains("\"bin\"") && !json.contains("clipColor") && !json.contains("flags"));
    }

    // --- Widget: bins in the list, switching bins shows only its clips, undo updates the view
    {
        Engine engine;
        MediaPool pool(&p, &engine);
        pool.setSaveSettings(false);
        pool.resize(600, 300);
        pool.show();
        app.processEvents();
        auto* tree = pool.findChild<QTreeWidget*>();
        auto* list = pool.findChild<QListWidget*>();
        if (!CHECK(tree && list)) return Check::result();
        int items = 0;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) ++items;
        CHECK_EQ(items, 1 + int(p.bins().size()));
        CHECK_EQ(list->count(), 1 + 1 + 2); // Master: Titel + Timeline 1 + c, d
        pool.setCurrentBin(sub);
        app.processEvents();
        CHECK_EQ(list->count(), 2); // a, b
        CHECK_EQ(tree->currentItem()->data(0, Qt::UserRole).toInt(), sub);
        p.moveMediaToBin({"/x/b.mp4"}, 0);
        app.processEvents();
        CHECK_EQ(list->count(), 1);
        undo->undo();
        app.processEvents();
        CHECK_EQ(list->count(), 2);
        // Search: in Master also sub-bins, without title entry; case does not matter; empty = normal again
        pool.setCurrentBin(0);
        pool.setSearch("A.MP4");
        CHECK_EQ(list->count(), 1);
        CHECK_EQ(list->item(0)->data(Qt::UserRole).toString(), QString("/x/a.mp4"));
        pool.setSearch(".mp4");
        CHECK_EQ(list->count(), 4);
        pool.setCurrentBin(sub);
        CHECK_EQ(list->count(), 2);
        pool.setSearch("zzz");
        CHECK_EQ(list->count(), 0);
        pool.setSearch("");
        CHECK_EQ(list->count(), 2);
        // Current bin is removed (undo of creating): view jumps to Master
        const int tmpBin = p.addBin(0, "Weg");
        pool.setCurrentBin(tmpBin);
        undo->undo();
        app.processEvents();
        CHECK_EQ(pool.currentBin(), 0);
        CHECK(!pool.grab().isNull());
    }

    // --- Folder including subfolders as bins (Media Storage/drop): empty and media-less folders without bin, one undo step
    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        QDir root(tmp.path());
        root.mkpath("Dreh/Tag1");
        root.mkpath("Dreh/Leer");
        root.mkpath("Dreh/NurText");
        QFile txt(root.filePath("Dreh/NurText/notiz.txt"));
        txt.open(QIODevice::WriteOnly);
        txt.write("x");
        txt.close();
        const QStringList clipArgs{"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1", "-c:v", "libx264",
                                   "-preset", "ultrafast", "-pix_fmt", "yuv420p"};
        const QString a = Check::makeMedia(root.filePath("Dreh/a.mp4"), clipArgs);
        const QString b = Check::makeMedia(root.filePath("Dreh/Tag1/b.mp4"), clipArgs);
        if (CHECK(!a.isEmpty() && !b.isEmpty())) {
            Project q;
            Engine engine;
            QString err;
            if (!engine.init(&err)) return Check::skip(qPrintable("MLT: " + err));
            MediaPool pool(&q, &engine);
            pool.setSaveSettings(false);
            const int before = q.undoStack()->count();
            pool.importFolder(root.filePath("Dreh"));
            CHECK_EQ(q.undoStack()->count(), before + 1);
            CHECK_EQ(int(q.bins().size()), 2);
            const MediaInfo* ma = q.mediaInfo(QFileInfo(a).absoluteFilePath());
            const MediaInfo* mb = q.mediaInfo(QFileInfo(b).absoluteFilePath());
            if (CHECK(ma && mb)) {
                CHECK_EQ(q.binName(ma->bin), QString("Dreh"));
                CHECK_EQ(q.binName(mb->bin), QString("Tag1"));
                CHECK_EQ(q.bin(mb->bin)->parent, ma->bin);
                CHECK_EQ(q.bin(ma->bin)->parent, 0);
            }
            q.undoStack()->undo();
            CHECK(q.bins().isEmpty());
            // Folder without anything usable (only text, empty subfolders): no empty undo step, no change
            const int idx = q.undoStack()->index();
            for (const char* sub : {"Dreh/NurText", "Dreh/Leer"}) {
                pool.importFolder(root.filePath(sub));
                CHECK_EQ(q.undoStack()->index(), idx);
            }
            root.mkpath("Leer2/Unten");
            pool.importFolder(root.filePath("Leer2"));
            CHECK_EQ(q.undoStack()->index(), idx);
            CHECK(q.bins().isEmpty());
        }
    }

    // --- Media Storage: thumbnails of what is shown come first. Scrolled to the end right away, the clips at the
    // bottom get theirs while the ones above (out of view) still wait.
    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        const QString first = Check::makeMedia(tmp.filePath("clip00.mp4"),
                                               {"-f", "lavfi", "-i", "testsrc2=size=160x90:rate=10:duration=1.5", "-c:v", "libx264"});
        constexpr int kClips = 40;
        for (int i = 1; i < kClips; ++i) QFile::copy(first, tmp.filePath(QString("clip%1.mp4").arg(i, 2, 10, QChar('0'))));
        MediaStorage storage;
        storage.setPersistent(false);
        storage.resize(500, 300);
        storage.show();
        storage.setFolder(tmp.path());
        auto* list = storage.findChild<QListWidget*>();
        if (CHECK(list) && CHECK_EQ(list->count(), kClips)) {
            list->scrollToBottom();
            QVector<qint64> before;
            for (int i = 0; i < kClips; ++i) before << list->item(i)->icon().cacheKey();
            QListWidgetItem* last = list->item(kClips - 1);
            QEventLoop loop;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (last->icon().cacheKey() != before.last()) loop.quit();
            });
            poll.start(20);
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            loop.exec();
            CHECK(last->icon().cacheKey() != before.last()); // the last one got its thumbnail
            int waiting = 0; // out of view at the top, still without one (the first one may have been under way)
            for (int i = 1; i < 10; ++i)
                if (list->item(i)->icon().cacheKey() == before[i]) ++waiting;
            CHECK_EQ(waiting, 9);
        }
    }

    // --- Proxy: does not start ffmpeg (broken file at the front of PATH), reports that instead of waiting forever
    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        const QString a = Check::makeMedia(tmp.filePath("a.mp4"),
                                           {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1", "-c:v", "libx264"});
        const QString bin = tmp.filePath("bin");
        QDir().mkpath(bin);
#ifdef Q_OS_WIN
        QFile fake(QDir(bin).filePath("ffmpeg.exe"));
#else
        QFile fake(QDir(bin).filePath("ffmpeg"));
#endif
        if (CHECK(!a.isEmpty() && fake.open(QIODevice::WriteOnly))) {
            fake.write("\x7f" "ELF kaputt");
            fake.close();
            fake.setPermissions(fake.permissions() | QFileDevice::ExeOwner);
            const QByteArray path = qgetenv("PATH");
            qputenv("PATH", QDir::toNativeSeparators(bin).toUtf8() + QDir::listSeparator().toLatin1() + path);
            ProxyManager pm;
            QString message;
            QObject::connect(&pm, &ProxyManager::failed, [&](const QString&, const QString& m) { message = m; });
            pm.generate({a});
            QEventLoop loop;
            QTimer poll;
            QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
                if (!pm.isPending(a)) loop.quit();
            });
            poll.start(50);
            QTimer::singleShot(20000, &loop, &QEventLoop::quit);
            loop.exec();
            CHECK(!pm.isPending(a));
            CHECK(!message.isEmpty());
            CHECK(!pm.hasProxy(a));
            qputenv("PATH", path);
        }
    }
    // Media Pool thumbnails show a clip in its own shape: a landscape clip fills its 16:9 tile also in a 9:16 project
    // (it used to be letterboxed into 9:16 and that again into the tile), a portrait clip stands in the middle
    if (Check::haveFfmpeg()) {
        QTemporaryDir tmp;
        const QString wide = Check::makeMedia(tmp.filePath("wide.mp4"), {"-f", "lavfi", "-i", "color=c=red:size=320x180:rate=25:duration=1",
                                                                       "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        const QString tall = Check::makeMedia(tmp.filePath("tall.mp4"), {"-f", "lavfi", "-i", "color=c=red:size=180x320:rate=25:duration=1",
                                                                       "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        Engine engine;
        QString err;
        if (CHECK(!wide.isEmpty() && !tall.isEmpty()) && CHECK(engine.init(&err))) {
            ProjectFormat vertical;
            vertical.width = 1080;
            vertical.height = 1920;
            const QSize tile(144, 81);
            const auto red = [](const QImage& img, int x, int y) { return QColor(img.pixel(x, y)).red() > 150; };
            const QImage own = Engine::thumbnail(vertical, wide, 0, tile, true);
            if (CHECK_EQ(own.size(), tile)) {
                CHECK(red(own, 3, 40) && red(own, 140, 40) && red(own, 72, 3) && red(own, 72, 78)); // fills it
            }
            const QImage project = Engine::thumbnail(vertical, wide, 0, tile); // as the 9:16 timeline shows it
            if (CHECK_EQ(project.size(), tile)) CHECK(!red(project, 3, 40) && !red(project, 72, 10) && red(project, 72, 40));
            const QImage portrait = Engine::thumbnail(vertical, tall, 0, tile, true);
            if (CHECK_EQ(portrait.size(), tile)) CHECK(!red(portrait, 3, 40) && red(portrait, 72, 3) && red(portrait, 72, 78));
            // The pool asks for them like that
            Project q;
            q.setFormat(vertical);
            q.addMedia(Engine::probe(vertical, wide));
            MediaPool pool(&q, &engine);
            pool.setSaveSettings(false);
            pool.resize(600, 300);
            pool.show();
            auto* list = pool.findChild<QListWidget*>();
            QListWidgetItem* item = nullptr;
            for (int i = 0; list && i < list->count(); ++i)
                if (list->item(i)->data(Qt::UserRole).toString() == wide) item = list->item(i);
            if (CHECK(item)) {
                QElapsedTimer t;
                t.start();
                QImage icon;
                while (t.elapsed() < 10000) {
                    app.processEvents();
                    icon = item->icon().pixmap(tile).toImage();
                    if (!icon.isNull() && red(icon, 72, 20)) break;
                    QThread::msleep(20);
                }
                CHECK(!icon.isNull() && red(icon, 3, 30) && red(icon, 140, 30));
            }
        }
    }

    // New frame rate (empty timelines): lengths, source in/out and source markers counted anew; a file that cannot
    // be read again is scaled
    {
        Project q;
        MediaInfo m = media("c.mp4"); // /x/c.mp4 does not exist
        m.length = 100;
        m.markIn = 10;
        m.markOut = 50;
        m.markers = {20, 99};
        q.addMedia(m);
        ProjectFormat f = q.format();
        const FrameRate old = f.rate;
        f.rate = FrameRate{50, 1};
        q.setFormat(f);
        q.recountMedia(old, [](const QString&) { return 0; });
        const MediaInfo* r = q.mediaInfo("/x/c.mp4");
        if (CHECK(r)) {
            CHECK_EQ(r->length, 200);
            CHECK_EQ(r->markIn, 20);
            CHECK_EQ(r->markOut, 100);
            CHECK_EQ(r->markers, (QVector<int>{40, 198}));
        }
    }
    return Check::result();
}
