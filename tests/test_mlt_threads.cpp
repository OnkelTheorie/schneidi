// Opening the first producers in parallel right after MLT starts (Media Pool thumbnails when a project opens,
// parallel opening before an export) crashed about 1 start in 10: MLT creates its service caches lazily and without a
// lock. Engine::initMlt creates them first (Bundle::prepareMltCaches). Same for the loader's tables and the producer
// metadata timewarp reads: two speed clips opened in parallel before an export crashed 5 exports in 6 (2026-10-22).
// The races only exist for the very first producers of a process -> the test starts itself several times; each child
// opens 16 producers at the same moment, directly, through the loader and as timewarp (speed clips).
#include "check.h"

#include "engine/Engine.h"

#include <Mlt.h>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int child(const QString& media)
{
    QString error;
    if (!Engine::initMlt(&error)) return 77;
    Mlt::Profile profile;
    std::atomic<int> ready{0}, opened{0};
    std::vector<std::thread> threads;
    // as the TimelineBuilder opens files: avformat directly, through the loader, speed clips (timewarp, also reversed)
    const QByteArray file = media.toUtf8();
    const QByteArray kinds[][2] = {{"avformat", file}, {"loader", file}, {"loader", "timewarp:0.5:" + file},
                                   {"loader", "timewarp:-1.5:" + file}};
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([&, i] {
            ++ready;
            while (ready < 16) std::this_thread::yield(); // all at once
            Mlt::Producer p(profile, kinds[i % 4][0].constData(), kinds[i % 4][1].constData());
            if (p.is_valid()) ++opened;
        });
    for (std::thread& t : threads) t.join();
    return opened == 16 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    if (argc == 3 && QByteArray(argv[1]) == "--child") return child(QString::fromLocal8Bit(argv[2]));
    Check::initApp("mlt_threads");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    const QString media = Check::makeMedia(tmp.filePath("a.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=160x90:rate=25:duration=1",
                                                                   "-f", "lavfi", "-i", "sine=duration=1", "-c:v", "libx264",
                                                                   "-c:a", "aac", "-shortest"});
    if (!CHECK(!media.isEmpty())) return Check::result();
    for (int run = 0; run < 20; ++run) {
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedChannels);
        p.start(QCoreApplication::applicationFilePath(), {"--child", media});
        if (!CHECK(p.waitForFinished(60000))) {
            p.kill();
            break;
        }
        if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 77) return Check::skip("MLT startet nicht");
        if (!CHECK(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)) {
            std::printf("       Lauf %d: %s, Code %d\n", run + 1, p.exitStatus() == QProcess::CrashExit ? "Absturz" : "Fehler",
                        p.exitCode());
            break;
        }
    }
    return Check::result();
}
