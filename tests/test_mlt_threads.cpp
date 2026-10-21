// Opening the first producers in parallel right after MLT starts (Media Pool thumbnails when a project opens,
// parallel opening before an export) crashed about 1 start in 10: MLT creates its service caches lazily and without a
// lock. Engine::initMlt creates them first (Bundle::prepareMltCaches). The race only exists for the very first
// producers of a process -> the test starts itself several times; each child opens 16 producers at the same moment.
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
    const QByteArray resource = media.toUtf8();
    for (int i = 0; i < 16; ++i)
        threads.emplace_back([&] {
            ++ready;
            while (ready < 16) std::this_thread::yield(); // all at once
            Mlt::Producer p(profile, "avformat", resource.constData());
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
