// SPDX-License-Identifier: AGPL-3.0-only
// Media Pool material search (ui/MediaPool + engine/MediaIndex): the search field also searches the material index
// - without an index it says how to make one, buildIndex measures in the background, a hit by description or by a
// (stubbed) hearable event shows file, time and reason, a double-click asks for the source viewer at the hit's
// source frame, an empty search hides the hits. POOL_INDEX_DUMP=<file.png>: picture of the pool with hits.
#include "check.h"

#include "core/I18n.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "engine/MediaIndex.h"
#include "ui/MediaPool.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QMessageBox>
#include <QTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTemporaryDir>
#include <QThread>

#include <functional>

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 60000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("pool_index");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg missing");
    Check::initEngine();
    QTemporaryDir dir;
    QDir(MediaIndex::cacheDir()).removeRecursively();
    QDir(MediaIndex::notesDir()).removeRecursively();
    // a.mp4: two shots (cut at 4 s), b.mp4: one
    const QString a = Check::makeMedia(
        dir.filePath("a.mp4"), {"-f", "lavfi", "-i", "testsrc=s=320x180:r=25:d=4", "-f", "lavfi", "-i",
                                "testsrc=s=320x180:r=25:d=4", "-filter_complex", "[1]negate[n];[0][n]concat=n=2:v=1[v]",
                                "-map", "[v]", "-c:v", "libx264", "-pix_fmt", "yuv420p"});
    const QString b = Check::makeMedia(dir.filePath("b.mp4"), {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=3", "-c:v",
                                                               "libx264", "-pix_fmt", "yuv420p"});
    if (!CHECK(!a.isEmpty() && !b.isEmpty())) return Check::result();
    MediaIndex::setEventSource([b](const QString& file) {
        return file == b ? QVector<AudioEvents::Event>{{"laughter", 1.0, 2.0, 0.9}} : QVector<AudioEvents::Event>{};
    });

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    Project p;
    p.addMedia(Engine::probe(p.format(), a));
    p.addMedia(Engine::probe(p.format(), b));
    MediaPool pool(&p, &engine);
    pool.setSaveSettings(false);
    pool.resize(420, 420);
    pool.show();
    auto* panel = pool.findChild<QWidget*>("pool_index_panel");
    auto* title = pool.findChild<QLabel*>("pool_index_title");
    auto* hits = pool.findChild<QListWidget*>("pool_index_hits");
    auto* field = pool.findChild<QLineEdit*>("pool_search");
    if (!CHECK(panel && title && hits && field)) return Check::result();
    CHECK(!field->placeholderText().isEmpty());
    CHECK(panel->isHidden());

    // No index yet: the panel says how to make one
    pool.setSearch("Totale");
    CHECK(waitFor([&] { return panel->isVisible() && !title->text().isEmpty(); }, 3000));
    CHECK_EQ(hits->count(), 0);
    const QString noIndex = title->text();

    int built = 0;
    QObject::connect(&pool, &MediaPool::indexBuilt, [&built](const QString&) { ++built; });
    pool.buildIndex({a, b});
    CHECK(waitFor([&] { return built == 2; }));
    std::optional<MediaIndex::Entry> e = MediaIndex::cached(a);
    if (!CHECK(e && e->shots.size() == 2)) return Check::result();
    CHECK(MediaIndex::annotate(*e, 2, QString("Totale vom Laden"), QStringList{"laden"}));
    // "Beschreibung bearbeiten…": a note that cannot be written, a shot that is gone - said in the user's language
    // (it showed the English error of MediaIndex::annotate, "cannot write …")
    {
        const auto answer = [](QString* text, const QString& dump) {
            QTimer::singleShot(0, [text, dump] {
                waitFor([] { return QApplication::activeModalWidget() != nullptr; }, 5000);
                if (auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    dlg && dlg->objectName() == "index_note_dialog") {
                    if (auto* desc = dlg->findChild<QLineEdit*>("index_note_desc")) desc->setText("Halbtotale");
                    if (!dump.isEmpty()) dlg->grab().save(dump);
                    QTimer::singleShot(0, [text] {
                        waitFor([] { return qobject_cast<QMessageBox*>(QApplication::activeModalWidget()) != nullptr; }, 5000);
                        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                            *text = box->text();
                            box->accept();
                        }
                    });
                    dlg->accept();
                } else if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    *text = box->text();
                    box->accept();
                }
            });
        };
        const QString notes = MediaIndex::notesDir();
        QDir(notes).removeRecursively();
        QFile blocker(notes); // a file where the folder should be: the note cannot be written
        CHECK(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QString text;
        answer(&text, qEnvironmentVariable("POOL_NOTE_DUMP"));
        pool.editHitNote(a, 1);
        CHECK_EQ(text, T("Die Beschreibung ließ sich nicht speichern: in den Ordner %1 kann schneidi nicht schreiben.")
                           .arg(QDir::toNativeSeparators(notes)));
        CHECK(!text.contains("cannot write"));
        QFile::remove(notes);
        text.clear();
        answer(&text, {});
        pool.editHitNote(a, 9);
        CHECK_EQ(text, T("Die Einstellung %1 gibt es in %2 nicht mehr (die Datei wurde neu untersucht).").arg(9).arg("a.mp4"));
        // Written again (the folder is back): the earlier note of shot 2 is gone with the folder, write it anew
        e = MediaIndex::cached(a);
        if (!CHECK(e)) return Check::result();
        CHECK(MediaIndex::annotate(*e, 2, QString("Totale vom Laden"), QStringList{"laden"}));
    }

    // By description: the second shot of a.mp4, at its source frame
    pool.setSearch("totale");
    CHECK(waitFor([&] { return hits->count() == 1; }, 3000));
    CHECK(title->text() != noIndex);
    if (hits->count() == 1) {
        QListWidgetItem* it = hits->item(0);
        CHECK_EQ(it->data(Qt::UserRole).toString(), a);
        CHECK_EQ(it->data(Qt::UserRole + 1).toInt(), 100); // 4 s at 25 fps
        CHECK(it->text().contains("a.mp4") && it->text().contains("0:04"));
        CHECK(waitFor([&] { return !it->icon().isNull(); }, 10000)); // its picture
        QString gotPath;
        int gotFrame = -1;
        QObject::connect(&pool, &MediaPool::sourceRequestedAt, [&](const QString& path, int frame) {
            gotPath = path;
            gotFrame = frame;
        });
        emit hits->itemDoubleClicked(it);
        CHECK_EQ(gotPath, a);
        CHECK_EQ(gotFrame, 100);
    }
    // By a hearable event, with a German word
    pool.setSearch("wo lacht er");
    CHECK(waitFor([&] { return hits->count() == 1 && hits->item(0)->data(Qt::UserRole).toString() == b; }, 3000));
    if (hits->count() == 1) {
        CHECK_EQ(hits->item(0)->data(Qt::UserRole).toString(), b);
        CHECK_EQ(hits->item(0)->data(Qt::UserRole + 1).toInt(), 25); // the laugh at 1 s
    }
    // Another frame rate: the open hits count in it (a double-click opened the source at half the time)
    {
        ProjectFormat f = p.format();
        f.rate = {50, 1};
        p.setFormat(f);
        CHECK(waitFor([&] { return hits->count() == 1 && hits->item(0)->data(Qt::UserRole + 1).toInt() == 50; }, 3000));
        // Another project: its hits, not the old ones (none: nothing of it is indexed)
        Project other;
        p.load(other.data());
        CHECK(waitFor([&] { return hits->count() == 0; }, 3000));
    }
    if (const QString dump = qEnvironmentVariable("POOL_INDEX_DUMP"); !dump.isEmpty()) pool.grab().save(dump);
    // A project that holds the file in another spelling (".", on Windows a lower-case drive letter): its hits show
    // (they were counted in the title but left out of the list)
    {
        const QString spelled = QFileInfo(a).absolutePath() + "/./" + QFileInfo(a).fileName();
        Project q;
        MediaInfo m = Engine::probe(q.format(), a);
        m.path = spelled;
        q.addMedia(m);
        MediaPool other(&q, &engine);
        other.setSaveSettings(false);
        other.show();
        auto* list = other.findChild<QListWidget*>("pool_index_hits");
        other.setSearch("totale");
        CHECK(waitFor([&] { return list && list->count() == 1; }, 3000));
        if (list && list->count() == 1) CHECK_EQ(list->item(0)->data(Qt::UserRole).toString(), spelled);
    }
    pool.setSearch("");
    CHECK(waitFor([&] { return panel->isHidden(); }, 3000));
    MediaIndex::setEventSource({});
    return Check::result();
}
