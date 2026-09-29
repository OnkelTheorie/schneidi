// Test Media Pool: Bins (anlegen, umbenennen, verschieben, entfernen), Clipfarben, Flags – Undo, Speichern/Laden,
// kaputte Dateien (fehlende Bins, Zyklen, unbekannte Farben); Media-Pool-Widget offscreen (Bin-Wechsel, Import in Bin)
#include "check.h"

#include "core/Project.h"
#include "core/ProjectFile.h"
#include "engine/Engine.h"
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "ui/MediaPool.h"

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

    // --- Bins anlegen: Standardnamen „Bin 1“, „Bin 2“; Unter-Bin; sortiert nach Name
    const int b1 = p.addBin(0);
    const int b2 = p.addBin(0, "  Zeug  ");
    const int sub = p.addBin(b1, "Unter");
    CHECK_EQ(p.binName(b1), QString("Bin 1"));
    CHECK_EQ(p.binName(b2), QString("Zeug"));
    CHECK_EQ(p.binName(0), QString("Master"));
    CHECK_EQ(p.childBins(0), (QVector<int>{b1, b2}));
    CHECK_EQ(p.childBins(b1), QVector<int>{sub});
    CHECK(p.binInside(sub, b1) && p.binInside(sub, 0) && !p.binInside(b1, sub));
    CHECK_EQ(p.binName(p.addBin(0)), QString("Bin 2")); // erste freie Nummer
    undo->undo();
    CHECK_EQ(p.bins().size(), 3);

    // --- Clips verschieben, Farbe, Flags (jeweils ein Undo-Schritt)
    int n = undo->index();
    p.moveMediaToBin({"/x/a.mp4", "/x/b.mp4"}, sub);
    CHECK(binOf(p, "a.mp4") == sub && binOf(p, "b.mp4") == sub && binOf(p, "c.mp4") == 0);
    p.setClipColor({"/x/a.mp4"}, "orange");
    p.setClipColor({"/x/b.mp4"}, "gibtsnicht"); // unbekannt: nichts
    p.setFlag({"/x/a.mp4", "/x/c.mp4"}, "red", true);
    p.setFlag({"/x/a.mp4"}, "blue", true); // Reihenfolge wie in der Farbliste: blue vor red
    p.setFlag({"/x/a.mp4"}, "blue", true); // schon gesetzt: kein Schritt
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

    // --- Bin verschieben: nicht in sich selbst/darunter
    p.moveBin(b1, sub);
    CHECK_EQ(p.bin(b1)->parent, 0);
    p.moveBin(sub, b2);
    CHECK_EQ(p.bin(sub)->parent, b2);
    undo->undo();
    CHECK_EQ(p.bin(sub)->parent, b1);
    // Umbenennen (leer = nichts)
    p.renameBin(b2, "Musik");
    p.renameBin(b2, "   ");
    CHECK_EQ(p.binName(b2), QString("Musik"));

    // --- Bin entfernen: Inhalt und Unter-Bins wandern eine Ebene hoch; Undo stellt alles wieder her
    p.removeBin(b1);
    CHECK(!p.bin(b1) && p.bin(sub) && p.bin(sub)->parent == 0 && binOf(p, "a.mp4") == sub);
    p.removeBin(sub);
    CHECK(binOf(p, "a.mp4") == 0 && binOf(p, "b.mp4") == 0);
    undo->undo();
    undo->undo();
    CHECK(p.bin(b1) && p.bin(sub)->parent == b1 && binOf(p, "a.mp4") == sub);

    // --- Später importierte Medien: Undo der Organisation lässt sie in Ruhe; Import in einen fehlenden Bin -> Master
    MediaInfo d = media("d.mp4");
    d.bin = 999;
    p.addMedia(d);
    CHECK_EQ(binOf(p, "d.mp4"), 0);

    // --- Speichern -> Laden: alles gleich, neue Bin-ID nach dem Laden nicht doppelt
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

    // --- Kaputte Datei: Bin mit fehlendem Eltern-Bin, Eltern-Zyklus, doppelte ID, unbekannte Farbe/Flags
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
        CHECK(!q.binInside(q.bin(2)->parent, 2) || q.bin(2)->parent == 0); // Zyklus aufgebrochen
        CHECK(q.binInside(2, 0) && q.binInside(3, 0));
        CHECK_EQ(q.mediaInfo("/x/a.mp4")->bin, 0); // Bin 7 gibt es nicht
        CHECK(q.mediaInfo("/x/a.mp4")->clipColor.isEmpty());
        CHECK_EQ(q.mediaInfo("/x/a.mp4")->flags, (QStringList{"red", "blue"}));
        CHECK_EQ(q.mediaInfo("/x/b.mp4")->bin, 2);
        // alte Datei ohne Bins: alles im Master, Datei bleibt ohne neue Felder
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,
            "media":[{"path":"/x/a.mp4","name":"a.mp4","length":10,"hasVideo":true}],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &data, &err));
        CHECK(data.bins.isEmpty() && data.media[0].bin == 0);
        const QByteArray json = ProjectFile::toJson(data, "/x/p.schneidi");
        CHECK(!json.contains("\"bin\"") && !json.contains("clipColor") && !json.contains("flags"));
    }

    // --- Widget: Bins in der Liste, Bin-Wechsel zeigt nur dessen Clips, Undo aktualisiert die Ansicht
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
        CHECK_EQ(list->count(), 1 + 2); // Master: Titel + c, d
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
        // Suche: im Master auch Unter-Bins, ohne Titel-Eintrag; groß/klein egal; leer = wieder normal
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
        // Aktueller Bin wird entfernt (Undo des Anlegens): Ansicht springt auf Master
        const int tmpBin = p.addBin(0, "Weg");
        pool.setCurrentBin(tmpBin);
        undo->undo();
        app.processEvents();
        CHECK_EQ(pool.currentBin(), 0);
        CHECK(!pool.grab().isNull());
    }

    // --- Ordner samt Unterordnern als Bins (Media Storage/Drop): leere und medienlose Ordner ohne Bin, ein Undo-Schritt
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
        }
    }
    return Check::result();
}
