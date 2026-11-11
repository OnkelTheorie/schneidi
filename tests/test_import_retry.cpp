// SPDX-License-Identifier: AGPL-3.0-only
// Importing a file that cannot be read yet (a recording, download or copy still being written, a file another program
// holds): the message names the file, says it may still be being written or locked and offers to try again; trying
// again once the file is complete imports it (a user on Windows: "no picture and no sound" for a clip whose thumbnail
// showed, and only "Not readable: <name>" said so, with no way but importing again by hand).

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "engine/MediaProbe.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QUrl>
#include <QFile>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <clocale>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("import-retry");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir dir;
    const QString complete = Check::makeMedia(dir.filePath("complete.mp4"),
                                              {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=2", "-c:v",
                                               "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!complete.isEmpty())) return Check::result();
    // The recording as it is while being written: the header not there yet
    const QString file = dir.filePath("recording.mp4");
    {
        QFile f(file);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(QByteArray(64 * 1024, '\0')) == 64 * 1024);
    }

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    Project* project = w.liveProject();

    int boxes = 0;
    QString text, informative;
    bool retried = false;
    // Answers the message box(es) the import opens (modal: from a timer while it waits)
    std::function<void()> answer = [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            QTimer::singleShot(20, answer);
            return;
        }
        ++boxes;
        text = box->text();
        informative = box->informativeText();
        QAbstractButton* retry = nullptr;
        QAbstractButton* close = nullptr;
        for (QAbstractButton* b : box->buttons()) { // (not "Show Details…")
            if (box->buttonRole(b) == QMessageBox::AcceptRole) retry = b;
            if (box->buttonRole(b) == QMessageBox::RejectRole) close = b;
        }
        if (boxes == 1 && CHECK(retry)) {
            // The recording finished meanwhile
            QFile::remove(file);
            CHECK(QFile::copy(complete, file));
            retried = true;
            retry->click();
            return;
        }
        if (close) close->click(); // a second message: the retry failed
    };
    QTimer::singleShot(0, answer);
    w.importFiles({file}, false);
    CHECK_EQ(boxes, 1);
    CHECK(text.contains("recording.mp4"));
    // The real reason first, then what a moment may change
    CHECK(informative.startsWith(T("Grund: %1").arg(QString())));
    CHECK(informative.contains(T("Die Datei wurde gerade erst geschrieben – läuft die Aufnahme, der Download oder die "
                                 "Kopie noch, klappt es vielleicht gleich. Erneut versuchen?")));
    CHECK(retried);
    const MediaInfo* m = project->mediaInfo(file);
    CHECK(m && m->hasVideo && m->length == 50);

    // Two damaged files, one written moments ago, one an hour old: counted, each marked for itself
    {
        const QString fresh = dir.filePath("fresh.mp4"), old = dir.filePath("old.mp4");
        for (const QString& p : {fresh, old}) {
            QFile f(p);
            CHECK(f.open(QIODevice::WriteOnly) && f.write(QByteArray(4096, 'x')) == 4096);
        }
        QFile o(old);
        CHECK(o.open(QIODevice::ReadWrite)
              && o.setFileTime(QDateTime::currentDateTime().addSecs(-3600), QFileDevice::FileModificationTime));
        o.close();
        boxes = 1; // closes the box
        QTimer::singleShot(0, answer);
        w.importFiles({fresh, old}, false);
        CHECK_EQ(boxes, 2);
        CHECK(text.contains(T("schneidi konnte in %1 Dateien kein Bild und keinen Ton finden:").arg(2)));
        CHECK(text.contains("fresh.mp4 (" + T("gerade erst geschrieben") + ")"));
        CHECK(text.contains("old.mp4") && !text.contains("old.mp4 ("));
        CHECK(!project->mediaInfo(fresh) && !project->mediaInfo(old));
    }

    // Dragging a file over the timeline never runs the long stream analysis on the UI thread (sound that starts
    // 12 s into an MPEG-TS): a quick look, the full probe in the background - and the import of the drop takes that
    // one instead of probing again
    const QString late = Check::makeMedia(dir.filePath("late.ts"),
                                          {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25:duration=20", "-itsoffset",
                                           "12", "-f", "lavfi", "-i", "sine=duration=5", "-map", "0", "-map", "1", "-c:v",
                                           "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac"});
    TimelineView* tv = w.findChild<TimelineView*>();
    if (CHECK(!late.isEmpty()) && CHECK(tv)) {
        MediaProbe::forgetForTest();
        const int before = MediaProbe::longAnalysesForTest();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(late)});
        const QPoint pt(TimelineView::kHeaderW + 100, 60);
        QDragEnterEvent enter(pt, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tv, &enter);
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before); // not on the UI thread
        CHECK(enter.isAccepted()); // the picture was found by the quick look
        QDragLeaveEvent leave;
        QApplication::sendEvent(tv, &leave);
        QElapsedTimer t;
        t.start();
        while (MediaProbe::longAnalysesForTest() == before && t.elapsed() < 20000) QTest::qWait(50);
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1); // in the background
        QTest::qWait(200);                                       // its result into the cache
        MediaProbe::forgetForTest(); // a probe now would analyse twice again
        w.importFiles({late}, false);
        const MediaInfo* lm = project->mediaInfo(late);
        CHECK(lm && lm->hasVideo && lm->hasAudio);
        CHECK_EQ(MediaProbe::longAnalysesForTest(), before + 1); // not probed again: the drop preview's result
    }
    return Check::result();
}
