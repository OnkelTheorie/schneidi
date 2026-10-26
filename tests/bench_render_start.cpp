// Benchmark (not a CTest test): how long starting a job of the render queue blocks the GUI thread when the timeline
// reads many different files. Clicks "Render all" like the user (RenderQueuePanel), a timer in the GUI thread measures
// how late it fires: the longest stall and the sum of all stalls over 50 ms, from the click to the first rendered frame
// and from the last frame to the end of the job (closing the graph).
// Run: QT_QPA_PLATFORM=offscreen ./build/tests/bench_render_start [--files 30] [--media <folder>] [--seconds 2]
//   --media keeps the generated clips in that folder (and reuses them next time)
#include "check.h"

#include "core/Project.h"
#include "core/RenderJob.h"
#include "core/TimelineOps.h"
#include "engine/RenderQueue.h"
#include "ui/DeliverPanel.h"
#include "ui/RenderQueuePanel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <cstdio>

namespace {

// Stalls of the GUI thread: a 5 ms timer that notes how late each tick comes
struct StallMeter {
    QElapsedTimer clock;
    qint64 last = 0, worst = 0, sum = 0;
    bool on = false;
    QTimer timer;
    StallMeter()
    {
        clock.start();
        timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this] { tick(); });
        timer.start();
    }
    void tick()
    {
        const qint64 now = clock.elapsed();
        const qint64 gap = now - last;
        last = now;
        if (!on) return;
        worst = std::max(worst, gap);
        if (gap > 50) sum += gap;
    }
    void begin()
    {
        last = clock.elapsed();
        worst = sum = 0;
        on = true;
    }
    void end()
    {
        tick(); // a stall right before the end counts too
        on = false;
    }
};

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("bench-render-start");
    QStringList args = app.arguments().mid(1);
    auto option = [&](const QString& name, const QString& def) {
        const qsizetype i = args.indexOf(name);
        return i < 0 || i + 1 >= args.size() ? def : args[i + 1];
    };
    const int files = std::max(1, option("--files", "30").toInt());
    const double seconds = std::max(0.2, option("--seconds", "2").toDouble());
    QTemporaryDir tmp;
    const QString mediaDir = option("--media", tmp.path());
    QDir().mkpath(mediaDir);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg not found");

    // Different files (each one is opened and probed on its own): 1280x720, 25 fps, with sound
    QStringList media;
    for (int i = 0; i < files; ++i) {
        const QString path = QDir(mediaDir).filePath(QString("src%1.mp4").arg(i, 2, 10, QChar('0')));
        if (!QFileInfo::exists(path)
            && Check::makeMedia(path, {"-f", "lavfi", "-i", QString("testsrc2=size=1280x720:rate=25:duration=%1").arg(seconds + 1),
                                       "-f", "lavfi", "-i", QString("sine=frequency=%1:duration=%2").arg(200 + 20 * i).arg(seconds + 1),
                                       "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"})
                   .isEmpty())
            return 1;
        media << path;
    }
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");

    Project p;
    {
        ProjectData d;
        d.format.width = 1280;
        d.format.height = 720;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        const int len = int(seconds * 25);
        int id = 0;
        for (int i = 0; i < files; ++i) {
            Clip v;
            v.id = ++id;
            v.mediaPath = media[i];
            v.start = i * len;
            v.out = len - 1;
            v.linkId = i + 1;
            Clip a = v;
            a.id = ++id;
            d.timeline.video[0].clips << v;
            d.timeline.audio[0].clips << a;
        }
        d.lastClipId = id;
        d.lastLinkId = files;
        p.load(d);
    }
    // Only the start matters: a short In/Out range at the end (every file is still opened)
    RenderJob job;
    job.id = 1;
    job.path = QDir(tmp.path()).filePath("out.mp4");
    job.settings.format = "h264";
    job.settings.encoder = VideoEncoder::Software;
    job.settings.quality = 2;
    job.size = QSize(1280, 720);
    job.inOut = true;
    job.from = TimelineOps::endFrame(p.timeline()) - 25;
    job.to = TimelineOps::endFrame(p.timeline()) - 1;

    DeliverPanel deliver(&p);
    RenderQueuePanel* panel = deliver.queuePanel();
    RenderQueue* queue = deliver.renderQueue();
    StallMeter meter, whole;
    for (int run = 0; run < 3; ++run) {
        RenderJob j = job;
        j.path = QDir(tmp.path()).filePath(QString("out%1.mp4").arg(run));
        p.setRenderQueue({j});
        QEventLoop idle; // let the timer run before measuring
        QTimer::singleShot(100, &idle, &QEventLoop::quit);
        idle.exec();

        QElapsedTimer clock;
        qint64 clickMs = 0, frameMs = -1, lastProgressMs = 0, endMs = 0;
        qint64 startWorst = 0, startSum = 0;
        bool frames = false;
        QEventLoop loop;
        auto c1 = QObject::connect(queue, &RenderQueue::progress, [&](int, int percent) {
            lastProgressMs = clock.elapsed();
            if (percent > 0 && !frames) {
                frames = true;
                frameMs = lastProgressMs;
                meter.end();
                startWorst = meter.worst;
                startSum = meter.sum;
            }
        });
        auto c2 = QObject::connect(queue, &RenderQueue::finished, [&] {
            endMs = clock.elapsed();
            whole.end();
            loop.quit();
        });
        clock.start();
        meter.begin();
        whole.begin();
        panel->renderClicked();
        clickMs = clock.elapsed();
        QTimer::singleShot(300000, &loop, &QEventLoop::quit);
        loop.exec();
        if (!frames) { // short job: the first progress after 0 % may be 100 %
            meter.end();
            startWorst = meter.worst;
            startSum = meter.sum;
        }
        QObject::disconnect(c1);
        QObject::disconnect(c2);
        const RenderJob& done = p.renderQueue().first();
        std::printf("run %d: click %lld ms, first frame after %lld ms, finished after %lld ms (%s)\n"
                    "        GUI thread until the first frame: longest stall %lld ms, stalls > 50 ms together %lld ms\n"
                    "        whole job: longest stall %lld ms, stalls > 50 ms together %lld ms\n",
                    run, clickMs, frameMs, endMs, done.status == RenderStatus::Done ? "done" : qPrintable(done.message),
                    startWorst, startSum, whole.worst, whole.sum);
        std::fflush(stdout);
    }
    return 0;
}
