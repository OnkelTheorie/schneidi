// SPDX-License-Identifier: AGPL-3.0-only
// The first video clip in an empty timeline with a format other than the project's (Settings › Timeline ›
// "First clip with a different format", TimelineOptions::clipFormat): "ask" (default) shows the question and adapts
// only on "Change", "always adapt" takes the clip's size and frame rate without a question, "never change" keeps the
// project's without one. A fixed frame rate (Project Settings) stays in every mode.

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "ui/MediaPool.h"
#include "ui/timeline/TimelineOptions.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <clocale>
#include <functional>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("clip-format-choice");
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    std::setlocale(LC_NUMERIC, "C");
    QTemporaryDir dir;
    const auto make = [&](const char* name, const char* size, int rate) {
        return Check::makeMedia(dir.filePath(name),
                                {"-f", "lavfi", "-i",
                                 QString("testsrc2=size=%1:rate=%2:duration=1").arg(size).arg(rate), "-c:v", "libx264",
                                 "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    };
    const QString a = make("a.mp4", "320x240", 30), b = make("b.mp4", "640x360", 50), c = make("c.mp4", "480x272", 24);
    if (!CHECK(!a.isEmpty() && !b.isEmpty() && !c.isEmpty())) return Check::result();

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    QSettings().remove("timeline");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    Project* project = w.liveProject();
    auto* pool = w.findChild<MediaPool*>();
    if (!CHECK(pool)) return Check::result();
    w.importFiles({a, b, c}, false);
    for (const QString& p : {a, b, c}) CHECK(project->mediaInfo(p));
    const ProjectFormat start = project->format();
    CHECK(start.width != 320 && start.width != 640);

    // Answers the format question (if one comes) with `change` and counts it
    int boxes = 0;
    QString question;
    bool change = false;
    bool waiting = false;
    std::function<void()> answer = [&] {
        if (!waiting) return;
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            QTimer::singleShot(20, answer);
            return;
        }
        ++boxes;
        question = box->text();
        for (QAbstractButton* button : box->buttons())
            if ((box->buttonRole(button) == QMessageBox::AcceptRole) == change) {
                button->click();
                return;
            }
    };
    // One clip from the pool onto the empty timeline (Media Pool › "Insert", picture and sound); then all undone
    const auto place = [&](const QString& path) {
        CHECK(project->timeline().video.value(0).clips.isEmpty());
        boxes = 0;
        waiting = true; // always armed: an unexpected question must not hang the test (it is counted)
        QTimer::singleShot(0, answer);
        emit pool->partsRequested({path}, PlaceParts::Both);
        QApplication::processEvents();
        waiting = false;
        CHECK(!project->timeline().video.value(0).clips.isEmpty());
        return boxes;
    };
    const auto reset = [&] {
        project->undoStack()->setIndex(0);
        QApplication::processEvents();
        CHECK(project->timeline().video.value(0).clips.isEmpty());
        CHECK(project->format() == start);
    };

    // Ask (default, as before): the question; "Nicht ändern" keeps, "Ändern" adapts
    CHECK(TimelineOptions::clipFormat() == TimelineOptions::ClipFormat::Ask);
    project->undoStack()->clear();
    change = false;
    CHECK_EQ(place(a), 1);
    CHECK_EQ(question, T("Das Format des Clips passt nicht zu den Projekteinstellungen. Projekt an den Clip anpassen?"));
    CHECK(project->format() == start);
    reset();
    change = true;
    CHECK_EQ(place(a), 1);
    CHECK_EQ(project->format().width, 320);
    CHECK_EQ(project->format().height, 240);
    CHECK_EQ(project->format().rate.fps(), 30.0);
    reset();

    // Always adapt: no question, the clip's size and frame rate
    TimelineOptions::setClipFormat(TimelineOptions::ClipFormat::Adapt);
    CHECK_EQ(place(b), 0);
    CHECK_EQ(project->format().width, 640);
    CHECK_EQ(project->format().height, 360);
    CHECK_EQ(project->format().rate.fps(), 50.0);
    reset();

    // Never change: no question, the project's format stays
    TimelineOptions::setClipFormat(TimelineOptions::ClipFormat::Keep);
    CHECK_EQ(place(c), 0);
    CHECK(project->format() == start);
    reset();

    QSettings().remove("timeline");
    return Check::result();
}
