// SPDX-License-Identifier: AGPL-3.0-only
// The app at rest renders nothing: a video with a big blur shape over it (a blur fill reads the picture below: the
// frame is made on the CPU), the shape selected with the Inspector showing it, the playhead inside it. After a change
// (the exit animation set in the Inspector, a seek) the picture is made once; then over half a second no frame is
// rendered or shown and the process uses next to no CPU. Guards against a repaint loop (a frame shown leading to the
// next render) that costs nothing visible with a cheap frame and a whole core with an expensive one.
// SCHNEIDI_IDLE_PROJECT=<file.schneidi> (+ SCHNEIDI_IDLE_FRAME, SCHNEIDI_IDLE_CLIP) runs the same on a real project.

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <clocale>
#include <cstdio>
#include <ctime>

namespace {

// Runs the event loop for ms (sleeping while there is nothing to do: the CPU it uses is the app's)
void settle(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct Activity {
    int rendered = 0, shown = 0;
    double cpuMs = 0;
};

// What happens over `ms` with nobody touching anything
Activity idleFor(Engine& engine, int ms)
{
    int shown = 0;
    const auto c = QObject::connect(&engine, &Engine::frameReady, [&shown](const QImage&) { ++shown; });
    const int rendered = engine.pictureStats().rendered;
    const std::clock_t cpu = std::clock();
    settle(ms);
    Activity a;
    a.cpuMs = double(std::clock() - cpu) * 1000.0 / CLOCKS_PER_SEC;
    a.rendered = engine.pictureStats().rendered - rendered;
    a.shown = shown;
    QObject::disconnect(c);
    return a;
}

// The Inspector's combo box for the exit animation: the second one listing the title animations by name
QComboBox* exitBox(MainWindow& w)
{
    QComboBox* found = nullptr;
    int seen = 0;
    for (QComboBox* b : w.findChildren<QComboBox*>()) {
        if (b->count() != int(std::size(kTitleAnims))) continue;
        bool anims = true;
        for (int i = 0; i < b->count() && anims; ++i)
            anims = b->itemData(i).toInt() == int(kTitleAnims[i].anim) && b->itemText(i) == T(kTitleAnims[i].name);
        if (anims && ++seen == 2) found = b;
    }
    return found;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("idle-render");
    I18n::setLanguage("de");
    I18n::install("de");
    QTemporaryDir dir;
    const QString realProject = qEnvironmentVariable("SCHNEIDI_IDLE_PROJECT");
    if (realProject.isEmpty() && !Check::haveFfmpeg()) {
        std::puts("SKIP: no ffmpeg for the test clip");
        return 77;
    }
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.show();
    int frame = 60, shapeId = 0;
    if (!realProject.isEmpty()) {
        if (!CHECK(w.openProject(realProject))) return Check::result();
        frame = qEnvironmentVariableIntValue("SCHNEIDI_IDLE_FRAME");
        shapeId = qEnvironmentVariableIntValue("SCHNEIDI_IDLE_CLIP");
    } else {
        const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
            {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=4", "-c:v", "libx264", "-preset", "ultrafast",
             "-pix_fmt", "yuv420p"});
        if (!CHECK(!file.isEmpty())) return Check::result();
        w.importFiles({file}, true);
        settle(300);
    }
    Project* project = w.liveProject();
    if (!shapeId) {
        if (!CHECK(!project->timeline().video.isEmpty() && !project->timeline().video[0].clips.isEmpty()))
            return Check::result();
        // A big blur circle over the video, shadow and outline on (as a user had it), entering and leaving animated
        w.editor()->addTitle(0, -1, 120, TitleKind::Shape);
        shapeId = *w.liveSelection()->ids().begin();
        QString why;
        project->edit(QStringLiteral("shape"), [&](Timeline& tl) {
            Clip* c = TimelineOps::findClip(tl, shapeId);
            if (!c) return;
            c->title.shape.kind = ShapeKind::Ellipse;
            c->title.shape.width = c->title.shape.height = 900;
            c->title.shape.fill = ShapeFill::Blur;
            c->title.shape.amount = 50;
            c->title.outlineOn = true;
            c->title.outlineWidth = 0;
            c->title.look.shadowOn = true;
            c->title.look.shadowOpacity = 0.031;
            c->title.look.shadowSoft = 8.1;
            c->title.enter = TitleAnim::SlideUp;
            c->title.enterLen = 1.376;
        });
    }
    if (!CHECK(TimelineOps::findClip(project->timeline(), shapeId))) return Check::result();
    w.liveSelection()->set({shapeId});
    w.liveSeek(frame);
    settle(1500); // the first picture (decoder opening)
    std::printf("after selecting and seeking: %d rendered, %d shown\n", engine.pictureStats().rendered,
                engine.pictureStats().shown);

    // The exit animation set in the Inspector (one undo step, the picture made again once)
    QComboBox* box = exitBox(w);
    if (CHECK(box)) {
        box->setCurrentIndex(box->findData(int(TitleAnim::Pop)));
        emit box->activated(box->currentIndex());
        const Clip* c = TimelineOps::findClip(project->timeline(), shapeId);
        CHECK(c && c->title.exit == TitleAnim::Pop);
    }
    settle(1000);
    const Activity a = idleFor(engine, 500);
    std::printf("idle 500 ms: %d rendered, %d shown, %.0f ms CPU\n", a.rendered, a.shown, a.cpuMs);
    CHECK_EQ(a.rendered, 0);
    CHECK_EQ(a.shown, 0);
    // Next to nothing (a render of the 1080p blur frame alone is tens of ms; timers and the like a few)
    CHECK(a.cpuMs < 100);
    // A longer look for the real project
    if (!realProject.isEmpty()) {
        const Activity b = idleFor(engine, 3000);
        std::printf("idle 3 s: %d rendered, %d shown, %.0f ms CPU\n", b.rendered, b.shown, b.cpuMs);
    }
    return Check::result();
}
