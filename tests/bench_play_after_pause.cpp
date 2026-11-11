// SPDX-License-Identifier: AGPL-3.0-only
// Benchmark (not a CTest test): what Play costs after a long pause, when the preview renderer closed its sources (as
// trim() did after 30 s unused until 0.21) against sources still open; and the memory an open source holds.
//   time to first frame: paused at frame P of a clip, then the next frame P+1 is asked for (Play) - from a renderer that
//     still has the source (its decoder awake: among the 4 most recently used) and from one without (opens the file,
//     finds the first key frame, opens the decoder, seeks and decodes from the key frame before P+1)
//   memory: RSS with N different files open (decoders asleep except the 4 most recent, video/Reader) and after
//     closing them all
// Run: QT_QPA_PLATFORM=offscreen SCHNEIDI_GPU=0 ./build-tests/tests/bench_play_after_pause <video>... [--runs 3]
//        [--sources 12]
#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/MemoryRelease.h"
#include "engine/picture/Renderer.h"
#include "engine/video/Reader.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cstdio>

namespace {

Picture::Options previewOptions(const ProjectFormat& format)
{
    Picture::Options o;
    o.format = format;
    o.mode = Picture::Mode::Preview;
    o.output = QSize(1280, 720);
    o.parallelFrames = 4;
    return o;
}

Timeline oneClip(const QString& path, int frames, int start = 0)
{
    Clip c;
    c.id = 1;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = frames - 1;
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = {c};
    return tl;
}

double ms(QElapsedTimer& t) { return t.nsecsElapsed() / 1e6; }

double median(QVector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.isEmpty() ? 0 : v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("bench-play-after-pause");
    QStringList files;
    int runs = 3, sourceCount = 12;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--runs" && i + 1 < argc) runs = atoi(argv[++i]);
        else if (a == "--sources" && i + 1 < argc) sourceCount = atoi(argv[++i]);
        else files << a;
    }
    if (files.isEmpty()) {
        std::printf("usage: bench_play_after_pause <video>... [--runs 3] [--sources 12]\n");
        return 2;
    }
    for (const QString& file : files) {
        auto probe = Video::Reader::open(file);
        if (!probe) {
            std::printf("%s: cannot open\n", qPrintable(file));
            continue;
        }
        const Video::Info info = probe->info();
        probe.reset();
        ProjectFormat format;
        format.width = 1920;
        format.height = 1080;
        format.rate = FrameRate{info.frameRate.num, info.frameRate.den};
        const double fps = av_q2d(info.frameRate);
        const int frames = int(double(info.duration) / AV_TIME_BASE * fps);
        const int pause = frames / 2 + 37; // in the middle of a GOP
        const Timeline tl = oneClip(file, frames);
        QVector<double> warm, cold, warmNext, coldNext;
        for (int run = 0; run < runs; ++run) {
            {
                Picture::Renderer r(tl, previewOptions(format));
                r.setDirection(1);
                r.render(pause);
                QThread::msleep(1500); // paused (the decoder stays awake: among the 4 most recent)
                QElapsedTimer t;
                t.start();
                r.render(pause + 1);
                warm << ms(t);
                t.restart();
                for (int k = 2; k < 12; ++k) r.render(pause + k);
                warmNext << ms(t) / 10;
            }
            {
                Picture::Renderer r(tl, previewOptions(format));
                r.setDirection(1);
                QElapsedTimer t;
                t.start();
                r.render(pause + 1); // the source was closed: opened again
                cold << ms(t);
                t.restart();
                for (int k = 2; k < 12; ++k) r.render(pause + k);
                coldNext << ms(t) / 10;
            }
        }
        std::printf("%s (%dx%d %s %.2f fps): first frame after the pause: open %.1f ms, closed %.1f ms; next frames "
                    "%.1f / %.1f ms\n",
                    qPrintable(QFileInfo(file).fileName()), info.width, info.height, qPrintable(info.codec), fps,
                    median(warm), median(cold), median(warmNext), median(coldNext));
    }
    // Memory: N sources of different names (links to the first file), each drawn once, one after the other
    QTemporaryDir dir;
    QStringList links;
    for (int i = 0; i < sourceCount; ++i) {
        const QString link = dir.filePath(QString("s%1.%2").arg(i).arg(QFileInfo(files[0]).suffix()));
        if (QFile::link(files[0], link)) links << link;
    }
    auto probe = Video::Reader::open(files[0]);
    const Video::Info info = probe->info();
    probe.reset();
    ProjectFormat format;
    format.width = 1920;
    format.height = 1080;
    format.rate = FrameRate{info.frameRate.num, info.frameRate.den};
    const int each = 60;
    Timeline tl;
    tl.video.resize(1);
    for (int i = 0; i < links.size(); ++i) {
        Clip c;
        c.id = i + 1;
        c.mediaPath = links[i];
        c.start = i * each;
        c.in = 300;
        c.out = 300 + each - 1;
        tl.video[0].clips << c;
    }
    MemoryRelease::release();
    const qint64 base = MemoryRelease::residentBytes();
    {
        Picture::Renderer r(tl, previewOptions(format));
        r.setDirection(1);
        for (int i = 0; i < links.size(); ++i)
            for (int k = 0; k < 10; ++k) r.render(i * each + k);
        MemoryRelease::release();
        const qint64 open = MemoryRelease::residentBytes();
        QThread::msleep(6000); // decoders not among the 4 most recent go to sleep (4 s)
        MemoryRelease::release();
        const qint64 asleep = MemoryRelease::residentBytes();
        std::printf("memory, %d sources (%s): before %lld MB, all just used %lld MB, the 4 most recent awake and "
                    "the rest asleep %lld MB, %d sources open\n",
                    int(links.size()), qPrintable(QFileInfo(files[0]).fileName()), base / 1048576, open / 1048576,
                    asleep / 1048576, r.stats().sourcesOpen);
    }
    MemoryRelease::release();
    std::printf("memory after closing them: %lld MB\n", MemoryRelease::residentBytes() / 1048576);
    return 0;
}
