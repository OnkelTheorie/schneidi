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

#include <QAbstractButton>
#include <QApplication>
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
        for (QAbstractButton* b : box->buttons())
            (box->buttonRole(b) == QMessageBox::AcceptRole ? retry : close) = b;
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
    CHECK_EQ(informative, T("Die Datei wird vielleicht noch geschrieben (Aufnahme, Download, Kopie) oder ist von einem "
                            "anderen Programm gesperrt. Erneut versuchen?"));
    CHECK(retried);
    const MediaInfo* m = project->mediaInfo(file);
    CHECK(m && m->hasVideo && m->length == 50);
    return Check::result();
}
