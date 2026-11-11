// SPDX-License-Identifier: AGPL-3.0-only
// Background work at the bottom right of the page bar (ui/BackgroundJobs) and the proxies' batch progress
// (ui/ProxyProgress).
// ProxyBatch on made-up queues: count, overall fraction (finished files plus the running one's share), files that
// leave without a proxy drop out of the batch, a new batch starts at zero. BackgroundJobs alone: shown while a job
// runs, the first running job's text and percent, "+1" for a second job, ✕ calls the job's cancel, the done text stays
// for the hold time and then the indicator hides. In the app with tiny synthetic clips and the real ProxyManager
// (ffmpeg): the indicator shows "Proxy 1/3 – …" while pending, the tooltip names the waiting files, the Media Pool's
// thin bar shows too; after the last file "Proxies done", then it hides; ✕ cancels the queue.
// BACKGROUND_JOBS_DUMP=<folder>: pictures of the page bar and the pool header; BACKGROUND_JOBS_LANG=ru (another
// language), BACKGROUND_JOBS_FONT=1.25 (larger font) to check that the texts fit.

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "engine/Engine.h"
#include "engine/ProxyManager.h"
#include "ui/BackgroundJobs.h"
#include "ui/ElidedLabel.h"
#include "ui/MediaPool.h"
#include "ui/ProxyProgress.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProgressBar>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <clocale>
#include <cmath>

namespace {

QString dumpDir() { return qEnvironmentVariable("BACKGROUND_JOBS_DUMP"); }

void dump(QWidget* w, const QString& name, QRect area = {})
{
    if (dumpDir().isEmpty()) return;
    QDir().mkpath(dumpDir());
    w->grab(area.isNull() ? QRect(QPoint(0, 0), QSize(-1, -1)) : area)
        .save(dumpDir() + "/" + name + "-" + qEnvironmentVariable("BACKGROUND_JOBS_LANG", "de") + ".png");
}

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void testBatch()
{
    ProxyBatch b;
    QSet<QString> made;
    const auto has = [&](const QString& p) { return made.contains(p); };
    CHECK(!b.active());
    CHECK(!b.update({"a", "b", "c"}, has));
    CHECK(b.active());
    CHECK_EQ(b.total(), 3);
    CHECK_EQ(b.done, 0);
    CHECK(near(b.fraction(0), 0.0));
    CHECK(near(b.fraction(50), 0.5 / 3));
    // More queued while it runs: the batch grows
    CHECK(!b.update({"a", "b", "c", "d"}, has));
    CHECK_EQ(b.total(), 4);
    // a finished, b canceled: b leaves the batch, a counts
    made.insert("a");
    CHECK(!b.update({"c", "d"}, has));
    CHECK_EQ(b.done, 1);
    CHECK_EQ(b.dropped, 1);
    CHECK_EQ(b.total(), 3);
    CHECK(near(b.fraction(34), (1 + 0.34) / 3));
    CHECK(near(b.fraction(250), 2.0 / 3)); // clamped
    made.insert("c");
    made.insert("d");
    CHECK(b.update({}, has)); // ended
    CHECK(!b.active());
    CHECK_EQ(b.done, 3);
    CHECK_EQ(b.dropped, 1);
    CHECK(!b.update({}, has)); // ends only once
    // A new batch starts at zero
    CHECK(!b.update({"e"}, has));
    CHECK_EQ(b.done, 0);
    CHECK_EQ(b.dropped, 0);
    CHECK_EQ(b.total(), 1);
}

// ElidedLabel at its own size hint shows its whole text (the hint was the integer advance, at which elidedText()
// could already cut: "Прокс…отовы" in the status bar)
void testElidedHint()
{
    for (const QString& text : {QString("Proxys fertig"), QString("Прокси готовы"), QString("Прокси 2/3 – clip-2.mp4 – 0 %"),
                                QString("Proxy 2/5 – clip.mp4 – 34 %")}) {
        ElidedLabel l(Qt::ElideMiddle);
        l.setFullText(text);
        CHECK_EQ(l.fontMetrics().elidedText(text, Qt::ElideMiddle, l.sizeHint().width()), text);
        CHECK_EQ(l.fontMetrics().elidedText(text, Qt::ElideRight, l.sizeHint().width()), text);
    }
}

void testIndicator()
{
    BackgroundJobs jobs;
    jobs.setHoldMs(80);
    jobs.resize(400, 24);
    CHECK(jobs.isHidden());
    int canceled = 0;
    BackgroundJobs::Job job;
    job.text = "Proxy 2/5 – clip.mp4 – 34 %";
    job.fraction = 0.268;
    job.toolTip = "Waiting:\n  b.mp4";
    job.cancel = [&] { ++canceled; };
    jobs.setJob("proxies", job);
    CHECK(!jobs.isHidden());
    CHECK_EQ(jobs.text(), job.text);
    CHECK_EQ(jobs.percent(), 27);
    CHECK(jobs.toolTip().contains("b.mp4"));
    auto* cancel = jobs.findChild<QToolButton*>("background_jobs_cancel");
    if (CHECK(cancel && !cancel->isHidden())) {
        cancel->click();
        CHECK_EQ(canceled, 1);
    }
    // A second job: the first stays in front, the other counted; both in the tooltip
    BackgroundJobs::Job other;
    other.text = "Transcript – 10 %";
    other.fraction = -1; // busy
    jobs.setJob("transcript", other);
    CHECK(jobs.text().startsWith(job.text));
    CHECK(jobs.text().contains(T("(+%1)").arg(1)));
    CHECK(jobs.toolTip().contains("Transcript"));
    // The first finishes: the running one comes to the front (busy bar: no percent)
    jobs.finishJob("proxies", T("Proxys fertig"));
    CHECK_EQ(jobs.text(), other.text);
    CHECK_EQ(jobs.percent(), -1);
    jobs.removeJob("transcript");
    CHECK_EQ(jobs.text(), T("Proxys fertig"));
    CHECK(cancel && cancel->isHidden());
    CHECK(!jobs.isHidden());
    QTest::qWait(150);
    CHECK(jobs.isHidden());
    CHECK(jobs.jobIds().isEmpty());
    // A job coming back during the hold time keeps it
    jobs.setJob("proxies", job);
    jobs.finishJob("proxies", "done");
    jobs.setJob("proxies", job);
    QTest::qWait(150);
    CHECK(!jobs.isHidden());
    CHECK_EQ(jobs.text(), job.text);
}

QStringList makeClips(const QString& dir, const QString& prefix, int n)
{
    QStringList out;
    for (int i = 0; i < n; ++i) {
        const QString f = Check::makeMedia(
            QString("%1/%2-%3.mp4").arg(dir, prefix).arg(i + 1),
            {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=1", "-c:v", "libx264", "-preset", "ultrafast",
             "-pix_fmt", "yuv420p"});
        if (!f.isEmpty()) out << f;
    }
    return out;
}

void testApp(MainWindow& w, Engine& engine, const QString& dir)
{
    auto* jobs = w.findChild<BackgroundJobs*>();
    auto* pool = w.findChild<MediaPool*>();
    if (!CHECK(jobs && pool)) return;
    QWidget* bar = w.findChild<QWidget*>("PageBar"); // the bottom row (the status bar itself is hidden)
    if (!CHECK(bar && bar->isAncestorOf(jobs))) return;
    CHECK(jobs->isHidden());
    jobs->setHoldMs(300);
    ProxyManager* proxies = engine.proxies();
    ProxyProgress* progress = ProxyProgress::of(proxies);
    CHECK_EQ(ProxyProgress::of(proxies), progress); // one per manager
    auto* poolBar = pool->findChild<QProgressBar*>("pool_proxy_bar");
    if (!CHECK(poolBar)) return;
    CHECK(poolBar->isHidden());

    const QStringList clips = makeClips(dir, "clip", 3);
    if (!CHECK_EQ(clips.size(), 3)) return;
    w.importFiles(clips, true);
    settle();
    proxies->generate(clips);
    // At once: shown, the first of three, the others waiting in the tooltip
    CHECK(!jobs->isHidden());
    CHECK(jobs->text().startsWith(T("Proxy %1/%2 – %3 – %4 %").arg(1).arg(3).arg("clip-1.mp4").arg(0).section(" – ", 0, 1)));
    CHECK(jobs->toolTip().contains(T("Wartend:")));
    CHECK(jobs->toolTip().contains("clip-2.mp4") && jobs->toolTip().contains("clip-3.mp4"));
    CHECK(!jobs->toolTip().contains("clip-1.mp4\n")); // the running one is not waiting
    CHECK_EQ(jobs->percent(), 0);
    CHECK(!poolBar->isHidden());
    settle();
    dump(bar, "statusbar-running");
    // Counts up file by file, never backwards
    int lastPercent = 0;
    bool sawSecond = false;
    QElapsedTimer t;
    t.start();
    while (proxies->pendingCount() > 0 && t.elapsed() < 60000) {
        QTest::qWait(10);
        if (jobs->percent() >= 0) {
            CHECK(jobs->percent() >= lastPercent);
            lastPercent = jobs->percent();
        }
        if (!sawSecond && jobs->text().contains("2/3")) {
            sawSecond = true;
            CHECK(jobs->percent() >= 33);
            CHECK(poolBar->value() >= 333);
            dump(bar, "statusbar-second");
            dump(pool, "pool-second", QRect(0, 0, pool->width(), 60));
        }
    }
    CHECK_EQ(proxies->pendingCount(), 0);
    CHECK(sawSecond);
    for (const QString& c : clips) CHECK(proxies->hasProxy(c));
    // Done: says so, then hides; the pool's bar goes at once
    CHECK(!jobs->isHidden());
    CHECK_EQ(jobs->text(), T("Proxys fertig"));
    CHECK(poolBar->isHidden());
    settle();
    dump(bar, "statusbar-done");
    QTest::qWait(450);
    CHECK(jobs->isHidden());

    // Cancel with the ✕: everything waiting goes, "canceled", then hidden
    const QStringList more = makeClips(dir, "more", 3);
    if (!CHECK_EQ(more.size(), 3)) return;
    proxies->generate(more);
    CHECK(!jobs->isHidden());
    CHECK(jobs->text().contains("1/3"));
    auto* cancel = jobs->findChild<QToolButton*>("background_jobs_cancel");
    if (!CHECK(cancel && cancel->isVisible())) return;
    QTest::mouseClick(cancel, Qt::LeftButton);
    settle();
    CHECK_EQ(proxies->pendingCount(), 0);
    CHECK_EQ(jobs->text(), T("Proxy-Erzeugung abgebrochen"));
    CHECK(poolBar->isHidden());
    QTest::qWait(450);
    CHECK(jobs->isHidden());
    for (const QString& c : more) CHECK(!proxies->hasProxy(c));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("background-jobs");
    I18n::install(qEnvironmentVariable("BACKGROUND_JOBS_LANG", "de"));
    if (const double scale = qEnvironmentVariable("BACKGROUND_JOBS_FONT").toDouble(); scale > 0) {
        QFont font = QApplication::font();
        font.setPointSizeF(font.pointSizeF() * scale);
        QApplication::setFont(font);
    }
    testBatch();
    testElidedHint();
    testIndicator();
    if (!Check::haveFfmpeg()) return Check::result();
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1366, 768);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    testApp(w, engine, dir.path());
    return Check::result();
}
