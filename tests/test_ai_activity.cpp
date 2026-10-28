// SPDX-License-Identifier: AGPL-3.0-only
// AI visible in the main window (MainWindowLive.cpp, TimelineView AI marks, app/AiLog): what an AI changes lights up
// (new = filled, edited = outline, deleted = ghost) without moving the view; changes outside the view get an arrow
// at the edge; the AI log lists every step and the work outside the app with progress and result; the indicator
// says what the AI is doing ("AI looks for silence … 42 %"); a click on a log entry selects its clips and jumps there
// (only then the view moves); log length and highlight follow Settings › AI.
// AI_DUMP=<folder>: ai-marks.png (window with marks and an arrow), ai-log.png (the log), ai-indicator.png (top bar).

#include "check.h"

#include "app/AiLog.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPainter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <clocale>
#include <functional>

namespace {

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

bool waitFor(const std::function<bool()>& done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

void test(const QString& dir)
{
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=4",
                                                        "-f", "lavfi", "-i", "sine=frequency=440:duration=4", "-c:v",
                                                        "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-shortest"});
    if (!CHECK(!a.isEmpty())) return;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({a, a, a}, true); // V1/A1: three clips of 100 frames
    QApplication::processEvents();
    auto* tv = w.findChild<TimelineView*>();
    auto* label = w.findChild<QToolButton*>("live_label");
    if (!CHECK(tv && label)) return;
    Project* p = w.liveProject();
    const QVector<Clip> v1 = p->timeline().video[0].clips;
    if (!CHECK_EQ(v1.size(), 3)) return;
    // A clip far to the right, outside the view
    int far = 0;
    p->edit("far", [&](Timeline& tl) {
        Clip c = v1[0];
        c.id = far = p->newClipId();
        c.linkId = 0;
        c.start = 20000;
        tl.video[0].clips << c;
    });
    const ViewState view = tv->view();
    w.liveConnectionChanged(true, "schneidi-cli mcp");

    // An edit: clip 2 new, clip 1 edited, a clip deleted after clip 3, the far clip new (outside the view)
    LiveChange c;
    c.command = "edit";
    c.step = "split, delete";
    c.added = {v1[1].id, far};
    c.changed = {v1[0].id};
    c.removed = {{999, p->currentSequence(), {TrackKind::Video, 0}, 300, 350, "gone.mp4"}};
    w.liveChanged(c);
    CHECK_EQ(tv->aiMarkCount(), 4); // 3 clips + 1 ghost
    const QVector<TimelineView::AiArrow> arrows = tv->aiArrows();
    if (CHECK_EQ(arrows.size(), 1)) CHECK(arrows[0].edge == Qt::RightEdge);
    // Core rule: the view stays where the user left it
    CHECK_EQ(tv->view().leftFrame, view.leftFrame);
    CHECK_EQ(tv->view().pxPerFrame, view.pxPerFrame);
    CHECK_EQ(tv->view().scrollY, view.scrollY);
    const QString dump = qEnvironmentVariable("AI_DUMP");
    settle(); // layout of the top bar (nested layouts need several rounds)
    if (!dump.isEmpty()) w.grab().save(QDir(dump).filePath("ai-marks.png"));
    // Each mark fades out on its own
    CHECK(waitFor([&] { return tv->aiMarkCount() == 0; }, w.aiSettings().highlightMs + 2000));
    CHECK(tv->aiArrows().isEmpty());

    // The log: the step with counts and names
    AiLog* log = w.aiLog();
    if (!CHECK_EQ(log->entries().size(), 1)) return;
    const AiLog::Entry edit = log->entries()[0];
    CHECK_EQ(edit.step, QString("split, delete"));
    CHECK(edit.detail.contains(T("neu: %1").arg(2)));
    CHECK(edit.detail.contains(T("geändert: %1").arg(1)));
    CHECK(edit.detail.contains(T("gelöscht: %1").arg(1)));
    CHECK(edit.detail.contains("gone.mp4"));
    CHECK_EQ(edit.frame, v1[0].start);

    // Work outside the app: indicator and log follow it
    LiveActivity job;
    job.id = 7;
    job.tool = "silence";
    job.target = "/x/talk.wav";
    w.liveActivity(job);
    CHECK(label->text().contains(T("sucht Stille")));
    job.state = LiveActivity::State::Progress;
    job.percent = 42;
    w.liveActivity(job);
    CHECK(label->text().contains("42 %"));
    settle();
    if (!dump.isEmpty()) w.grab(QRect(0, 0, w.width(), 70)).save(QDir(dump).filePath("ai-indicator.png"));
    CHECK_EQ(log->entries().size(), 2);
    CHECK_EQ(log->entries()[0].step, QString("silence · talk.wav"));
    job.state = LiveActivity::State::Finished;
    job.summary = {{"count", 18}};
    w.liveActivity(job);
    CHECK_EQ(log->entries()[0].detail, T("Pausen gefunden: %1").arg(18));
    CHECK_EQ(label->text(), "● " + T("KI verbunden"));
    // Interrupted when the AI leaves (the live server ends the running work of a client that goes away)
    job.id = 8;
    job.tool = "render";
    job.state = LiveActivity::State::Started;
    w.liveActivity(job);
    LiveChange failed;
    failed.command = "edit";
    failed.step = "delete";
    failed.error = "no clip with id 999";
    w.liveChanged(failed);
    w.liveConnectionChanged(false, "schneidi-cli mcp");
    LiveActivity gone;
    gone.id = 8;
    gone.state = LiveActivity::State::Failed;
    gone.error = "interrupted";
    w.liveActivity(gone);
    CHECK(log->runningJob() == nullptr);
    CHECK_EQ(log->entries()[1].detail, T("abgebrochen"));
    CHECK(log->entries()[0].state == AiLog::Entry::State::Failed);

    // The log view; a click on the edit selects its clips and jumps there – now the view may move (the user asked)
    w.showAiLog();
    auto* logView = w.findChild<AiLogView*>();
    if (CHECK(logView)) {
        auto* list = logView->findChild<QTreeWidget*>("ai_log_list");
        CHECK(list && list->topLevelItemCount() == 4);
        if (!dump.isEmpty()) logView->grab().save(QDir(dump).filePath("ai-log.png"));
        emit logView->activated(edit.id);
        CHECK(w.liveSelection()->contains(v1[1].id) && w.liveSelection()->contains(far) && w.liveSelection()->contains(v1[0].id));
        CHECK(waitFor([&] { return tv->playhead() == v1[0].start; }));
        CHECK_EQ(tv->view().leftFrame, view.leftFrame); // v1[0] is in view already: no scrolling needed
        logView->hide();
    }
    // An entry whose clip is out of view: shown on the click
    LiveChange onlyFar;
    onlyFar.command = "edit";
    onlyFar.step = "move";
    onlyFar.changed = {far};
    w.liveChanged(onlyFar);
    CHECK_EQ(tv->view().leftFrame, view.leftFrame);
    w.showAiLog();
    emit logView->activated(log->entries()[0].id);
    CHECK(tv->view().leftFrame > view.leftFrame);
    CHECK(waitFor([&] { return tv->playhead() == 20000; }));
    logView->hide();

    // Settings › AI: highlight off = nothing lights up; the log keeps as many entries as set (0 = none)
    AiSettings s = w.aiSettings();
    s.highlight = false;
    s.logLength = 2;
    w.setAiSettings(s);
    CHECK_EQ(log->entries().size(), 2);
    w.liveChanged(c);
    CHECK_EQ(tv->aiMarkCount(), 0);
    CHECK_EQ(log->entries().size(), 2);
    s.highlight = true;
    s.distinguish = false;
    s.logLength = 0;
    w.setAiSettings(s);
    w.liveChanged(c);
    CHECK_EQ(tv->aiMarkCount(), 4);
    CHECK(log->entries().isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ai-activity");
    Theme::apply(app);
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::kSkip;
    QTemporaryDir dir;
    test(dir.path());
    return Check::result();
}
