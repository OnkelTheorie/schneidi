// Measuring program (not a CTest test): plays through Audio::Player on the real sound card and logs the clock, so a
// device can be pulled away while it plays (card profile off, headphones unplugged) – docs/dev/testen.md.
// The sound is a sine at -90 dBFS: inaudible, also when the system moves it to other speakers.
// Run: ./build-tests/tests/bench_device_loss --list
//      ./build-tests/tests/bench_device_loss [output id ("" = system default)] [seconds] [backend]
#include "check.h"

#include "engine/audio/Device.h"
#include "engine/audio/Player.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#include <chrono>
#include <cstdio>
#include <thread>

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    Check::initApp("bench-device-loss");
    const QStringList args = app.arguments().mid(1);
    const QString backend = args.value(2);
    if (args.value(0) == "--list") {
        for (const Audio::Device::Output& o : Audio::Device::outputs(backend))
            std::printf("%s%s\n  id: %s\n", qPrintable(o.name), o.isDefault ? " (Standard)" : "", qPrintable(o.id));
        return 0;
    }
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const int seconds = args.size() > 1 ? args[1].toInt() : 10;
    QTemporaryDir tmp;
    const QString tone = Check::makeMedia(tmp.filePath("tone.wav"),
                                          {"-f", "lavfi", "-i",
                                           QString("aevalsrc=3.16e-5*sin(2*PI*440*t):s=48000:d=%1").arg(seconds + 5),
                                           "-ac", "2", "-c:a", "pcm_f32le"});
    if (tone.isEmpty()) return 1;
    Timeline tl;
    tl.audio.resize(1);
    Clip c;
    c.id = 1;
    c.mediaPath = tone;
    c.out = (seconds + 4) * 30;
    tl.audio[0].clips << c;

    Audio::Player::Options opt;
    opt.output = args.value(0);
    opt.backend = backend;
    Audio::Player player(opt);
    player.setTimeline(tl, FrameRate{30, 1});
    player.play(0, 1.0);
    player.release();
    QElapsedTimer wall;
    wall.start();
    QString name;
    double last = 0, standStart = 0, longestStand = 0;
    while (wall.elapsed() < seconds * 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const double t = wall.nsecsElapsed() * 1e-9, pos = player.position() / 48000.0;
        if (player.deviceName() != name) {
            name = player.deviceName();
            std::printf("%7.3f Gerät: %s%s\n", t, qPrintable(name),
                        player.deviceError().isEmpty() ? "" : qPrintable(" – " + player.deviceError()));
        }
        if (pos > last) {
            if (t - standStart > 0.1) std::printf("%7.3f Uhr stand %.0f ms\n", t, (t - standStart) * 1000);
            longestStand = std::max(longestStand, t - standStart);
            standStart = t;
            last = pos;
        }
        std::fflush(stdout);
    }
    const Audio::Player::Stats s = player.stats();
    std::printf("Ende: Uhr %.3f s nach %.3f s Wandzeit, längster Stand %.0f ms, %d× neu geöffnet, %d× vom System "
                "verlegt, %d Aussetzer\n",
                player.position() / 48000.0, wall.nsecsElapsed() * 1e-9, longestStand * 1000, s.reopens, s.reroutes,
                s.underruns);
    return 0;
}
