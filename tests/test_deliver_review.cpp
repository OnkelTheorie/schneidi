// SPDX-License-Identifier: AGPL-3.0-only
// Deliver › "Prüf-Render" (0.17): the checkbox is off by default, makes a review job (540 p, file name with the
// suffix, not for audio only), the queue renders it with the marks of `render review:"video"` (the cut lamp lit at the
// cut, not before it) while a normal job of the same timeline stays without them; the flag survives the project file;
// loading a job takes the box and name back; cancel and the queue going away stop the marks; an edit while the marks
// are made does not change what renders.
// DELIVER_REVIEW_DUMP=<file.png>: picture of the Deliver page with the box checked.
#include "check.h"

#include "cli/Commands.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/RenderJob.h"
#include "core/ReviewSheet.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEventLoop>
#include <QFileInfo>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>

#include <atomic>
#include <functional>

#include <cmath>

namespace {

bool waitFor(RenderQueue& q, int ms = 180000)
{
    if (!q.isRunning()) return true;
    QEventLoop loop;
    QObject::connect(&q, &RenderQueue::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return !q.isRunning();
}

bool waitUntil(const std::function<bool()>& done, int ms)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

QColor meanColor(const QImage& img, QRect r)
{
    r = r.adjusted(r.width() / 4, r.height() / 4, -r.width() / 4, -r.height() / 4);
    long long sr = 0, sg = 0, sb = 0, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            sr += c.red();
            sg += c.green();
            sb += c.blue();
            ++n;
        }
    return n ? QColor(int(sr / n), int(sg / n), int(sb / n)) : QColor();
}

bool isLit(const QColor& c, const QColor& lamp)
{
    return std::abs(c.red() - lamp.red()) < 50 && std::abs(c.green() - lamp.green()) < 50
           && std::abs(c.blue() - lamp.blue()) < 50;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("deliver_review");
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    Check::initEngine();
    QTemporaryDir dir;
    const QString green = Check::makeMedia(dir.filePath("green.mkv"), {"-f", "lavfi", "-i", "color=c=0x10c020:s=1280x720:d=2:r=25",
                                                                        "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    const QString blue = Check::makeMedia(dir.filePath("blue.mkv"), {"-f", "lavfi", "-i", "color=c=0x1030e0:s=1280x720:d=2:r=25",
                                                                      "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    if (green.isEmpty() || blue.isEmpty()) return Check::skip("ffmpeg could not make the media");

    Project p;
    {
        ProjectData d;
        d.format.width = 1280;
        d.format.height = 720;
        d.format.rate = {25, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        Clip a;
        a.id = 1;
        a.mediaPath = green;
        a.in = 0;
        a.out = 49;
        Clip b = a;
        b.id = 2;
        b.mediaPath = blue;
        b.start = 50; // the cut at frame 50
        d.timeline.video[0].clips << a << b;
        d.lastClipId = 2;
        p.load(d);
        for (const QString& f : {green, blue}) p.addMedia(Engine::probe(p.format(), f));
    }

    DeliverPanel panel(&p);
    panel.resize(520, 900);
    auto* review = panel.findChild<QCheckBox*>("renderReview");
    auto* name = panel.findChild<QLineEdit*>("renderName");
    auto* folder = panel.findChild<QLineEdit*>("renderFolder");
    auto* format = panel.findChild<QComboBox*>("renderFormat");
    if (!CHECK(review && name && folder && format)) return Check::result();
    folder->setText(dir.path());
    name->setText("Schnitt");
    // Off by default: the normal export never has the marks
    CHECK(!review->isChecked());
    CHECK(review->isEnabled());
    RenderJob plain = panel.makeJob();
    CHECK(!plain.review);
    CHECK_EQ(QFileInfo(plain.path).completeBaseName(), QString("Schnitt"));
    review->setChecked(true);
    RenderJob job = panel.makeJob();
    CHECK(job.review);
    // Always 540 p, every frame encoded: the resolution and smart render do not apply
    if (auto* res = panel.findChild<QCheckBox*>("renderSmart"); CHECK(res)) CHECK(!res->isEnabled());
    CHECK_EQ(job.size, QSize(960, 540)); // 540 p like `render review:"video"`
    CHECK_EQ(QFileInfo(job.path).completeBaseName(), "Schnitt - " + T("Prüf-Render")); // never over the real export
    if (const QString dump = qEnvironmentVariable("DELIVER_REVIEW_DUMP"); !dump.isEmpty()) {
        panel.show();
        QApplication::processEvents();
        panel.grab().save(dump);
    }
    // Audio only: nothing to burn in
    const int wav = format->findData("wav");
    if (CHECK(wav >= 0)) {
        const int before = format->currentIndex();
        format->setCurrentIndex(wav);
        CHECK(!review->isEnabled());
        CHECK(!panel.makeJob().review);
        format->setCurrentIndex(before);
        CHECK(review->isEnabled());
    }
    review->setChecked(false);
    // The flag in the project file (an older version without it renders a normal export)
    {
        RenderJob r = job;
        const QVector<RenderJob> back = RenderQueueJson::fromJson(RenderQueueJson::toJson({r, plain}));
        if (CHECK_EQ(back.size(), 2)) {
            CHECK(back[0].review);
            CHECK(!back[1].review);
            CHECK(back[0] == r);
        }
    }

    // Render both: the review job with the marks, the plain one (same size) without
    // What main.cpp hands in
    RenderQueue::setReviewMarks([](const ProjectData& data, int sequence, const std::atomic<bool>& cancel) {
        return Cli::reviewOverlay(data, sequence, &cancel);
    });
    job.id = 1;
    job.settings.encoder = VideoEncoder::Software;
    plain.id = 2;
    plain.size = QSize(960, 540);
    plain.settings.encoder = VideoEncoder::Software;
    p.setRenderQueue({job, plain});
    RenderQueue& q = *panel.renderQueue();
    CHECK(q.start());
    CHECK(waitFor(q));
    for (const RenderJob& j : p.renderQueue())
        if (!CHECK(j.status == RenderStatus::Done)) std::printf("       job %d: %s\n", j.id, qPrintable(j.message));
    const ProjectFormat of{960, 540, {25, 1}};
    const QSize os(960, 540);
    using namespace ReviewSheet;
    const auto lamp = [&](const QString& file, Lamp l, int frame) {
        const QImage img = Check::mediaFrame(file, of, frame);
        return img.isNull() ? QColor() : meanColor(img, lampRect(l, os));
    };
    if (CHECK(QFileInfo::exists(job.path))) {
        CHECK(isLit(lamp(job.path, Lamp::Cut, 50), lampColor(Lamp::Cut)));
        CHECK(!isLit(lamp(job.path, Lamp::Cut, 45), lampColor(Lamp::Cut)));
        // The strip at the bottom is drawn over the picture (darker than the green clip under it)
        const QImage g = Check::mediaFrame(job.path, of, 20);
        if (CHECK(!g.isNull())) {
            CHECK_EQ(g.size(), os);
            const QRect strip = stripRect(os);
            CHECK(meanColor(g, QRect(20, strip.top() + strip.height() / 2 - 4, 60, 8)).green() < 110);
        }
    }
    if (CHECK(QFileInfo::exists(plain.path))) {
        // The lamp's place shows the blue clip, the strip's the green one
        const QColor c = lamp(plain.path, Lamp::Cut, 50);
        CHECK(c.blue() > 180 && c.red() < 60);
        const QImage g = Check::mediaFrame(plain.path, of, 20);
        if (CHECK(!g.isNull())) {
            const QRect strip = stripRect(os);
            CHECK(meanColor(g, QRect(20, strip.top() + strip.height() / 2 - 4, 60, 8)).green() > 150);
        }
    }

    // "Einstellungen laden" of a job (double-click in the queue): a review job checks the box again and keeps one
    // suffix; a normal one unchecks it (it was made a review job unasked) and gets its full size back
    {
        RenderQueuePanel* qp = panel.queuePanel();
        review->setChecked(false);
        emit qp->loadJobRequested(1); // the review job
        CHECK(review->isChecked());
        CHECK_EQ(name->text(), QString("Schnitt"));
        const RenderJob again = panel.makeJob();
        CHECK(again.review);
        CHECK_EQ(QFileInfo(again.path).completeBaseName(), "Schnitt - " + T("Prüf-Render"));
        CHECK_EQ(again.size, QSize(960, 540));
        review->setChecked(false); // as a normal export: the preset's size, not the review's 540 p
        CHECK_EQ(panel.makeJob().size, QSize(1920, 1080));
        review->setChecked(true);
        emit qp->loadJobRequested(2); // the normal job with the box checked
        CHECK(!review->isChecked());
        CHECK(!panel.makeJob().review);
        CHECK_EQ(panel.makeJob().size, QSize(960, 540)); // its own size
    }

    // The marks can be stopped: Cli::reviewOverlay with the flag set makes nothing
    {
        std::atomic<bool> stop{true};
        CHECK(!Cli::reviewOverlay(p.data(), 0, &stop));
        stop = false;
        CHECK(bool(Cli::reviewOverlay(p.data(), 0, &stop)));
    }
    // Cancel while the marks are made, and a queue that goes away meanwhile: the marks are told to stop (they used to
    // run on - minutes for an uncached game recording - and the queue's end waited for them on the GUI thread)
    static std::atomic<int> started{0}, stopped{0};
    static std::atomic<bool> release{false};
    RenderQueue::setReviewMarks([](const ProjectData&, int, const std::atomic<bool>& cancel) -> RenderQueue::Overlay {
        ++started;
        QElapsedTimer t;
        t.start();
        while (!cancel && !release && t.elapsed() < 20000) QThread::msleep(5);
        if (cancel) ++stopped;
        return {};
    });
    {
        RenderJob r = job;
        r.status = RenderStatus::Queued;
        p.setRenderQueue({r});
        CHECK(q.start());
        CHECK(waitUntil([] { return started == 1; }, 5000));
        QElapsedTimer t;
        t.start();
        q.cancel();
        CHECK(!q.isRunning());
        CHECK(waitUntil([] { return stopped == 1; }, 3000));
        CHECK(t.elapsed() < 3000);
        {
            RenderQueue other(&p);
            CHECK(other.start({r.id}));
            CHECK(waitUntil([] { return started == 2; }, 5000));
            t.restart();
        } // destroyed while its marks are made
        CHECK(t.elapsed() < 3000);
        CHECK_EQ(stopped.load(), 2);
    }
    // An edit while the marks are made: the job renders the timeline the marks were made of (4 s, not 2 s)
    {
        RenderJob r = job;
        r.status = RenderStatus::Queued;
        p.setRenderQueue({r});
        QFile::remove(r.path);
        started = 0;
        release = false;
        CHECK(q.start());
        CHECK(waitUntil([] { return started == 1; }, 5000));
        p.edit("cut", [](Timeline& tl) { tl.video[0].clips.removeLast(); });
        release = true;
        CHECK(waitFor(q));
        if (CHECK(QFileInfo::exists(r.path))) {
            const MediaInfo m = Engine::probe(p.format(), r.path);
            if (!CHECK(std::abs(m.length - 100) <= 1)) std::printf("       length %d\n", m.length);
        }
    }
    RenderQueue::setReviewMarks({});
    return Check::result();
}
