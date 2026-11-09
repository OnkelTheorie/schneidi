// SPDX-License-Identifier: AGPL-3.0-only
// A media file overwritten under its name with other content (re-rendered shorter elsewhere): the Media Pool entry
// takes the new length, picture/sound and streams (probed again in the background, no undo step), what the user set
// stays, and the status bar says when clips now reach past the file's end (review 2026-11-09: the length stayed
// stale, clips ran past the new end and the export differed from the preview).

#include "check.h"

#include "app/MainWindow.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "engine/FileStamp.h"

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <clocale>

namespace {

bool overwrite(const QString& from, const QString& to, int step)
{
    QFile::remove(to);
    if (!QFile::copy(from, to)) return false;
    QFile f(to);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(1700000000 + step * 100), QFileDevice::FileModificationTime);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("media-reprobe");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    std::setlocale(LC_NUMERIC, "C");
    FileStamp::setSettleMsForTest(300);
    QTemporaryDir dir;
    auto tone = [&](const char* name, int seconds) {
        return Check::makeMedia(dir.filePath(name), {"-f", "lavfi", "-i", QString("sine=frequency=440:duration=%1").arg(seconds),
                                                     "-c:a", "pcm_s16le"});
    };
    const QString longer = tone("long.wav", 4), shorter = tone("short.wav", 1);
    if (!CHECK(!longer.isEmpty() && !shorter.isEmpty())) return Check::result();

    // ---- Core: the probe's facts replace the old ones, the user's settings stay, marks past the new end go
    {
        Project p;
        MediaInfo m{"/x/a.wav", "a.wav", 100, false, true, false};
        m.clipColor = "orange";
        m.markIn = 10;
        m.markOut = 80;
        m.markers = {5, 90};
        p.addMedia(m);
        MediaInfo probed{"/x/a.wav", "a.wav", 50, true, true, false};
        CHECK(p.refreshMedia(probed));
        const MediaInfo* now = p.mediaInfo("/x/a.wav");
        if (CHECK(now)) {
            CHECK_EQ(now->length, 50);
            CHECK(now->hasVideo);
            CHECK_EQ(now->clipColor, QString("orange"));
            CHECK_EQ(now->markIn, 10);
            CHECK_EQ(now->markOut, 49);
            CHECK(now->markers == QVector<int>{5});
        }
        CHECK(!p.refreshMedia(probed));                         // nothing different
        CHECK(!p.refreshMedia(MediaInfo{"/x/a.wav", "a.wav"})); // unreadable now (being written): kept
        CHECK(!p.refreshMedia(MediaInfo{"/x/b.wav", "b.wav", 10, false, true, false})); // not in the pool
    }

    // ---- The app: the file in the timeline overwritten with a shorter one
    const QString file = dir.filePath("voice.wav");
    if (!CHECK(overwrite(longer, file, 1))) return Check::result();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    w.importFiles({file}, true);
    Project* project = w.liveProject();
    const MediaInfo* before = project->mediaInfo(file);
    if (!CHECK(before && before->length > 0)) return Check::result();
    const int longLength = before->length;
    CHECK_EQ(project->clipsPastEnd(file), 0);
    const int undo = project->undoStack()->index();
    QTest::qWait(300);
    CHECK(overwrite(shorter, file, 2));
    engine.checkFiles(); // the app active again; it looks once more when the change has settled
    QElapsedTimer t;
    t.start();
    while (project->mediaInfo(file)->length == longLength && t.elapsed() < 10000) QTest::qWait(50);
    const int shortLength = project->mediaInfo(file)->length;
    std::printf("length %d -> %d frames\n", longLength, shortLength);
    CHECK(shortLength > 0 && shortLength < longLength / 2);
    CHECK_EQ(project->undoStack()->index(), undo); // no undo step
    CHECK_EQ(project->clipsPastEnd(file), 1);
    CHECK_EQ(w.statusBar()->currentMessage(), T("%1 ist jetzt kürzer: 1 Clip reicht über sein Ende hinaus").arg("voice.wav"));
    return Check::result();
}
