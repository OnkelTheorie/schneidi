// SPDX-License-Identifier: AGPL-3.0-only
// The display goes away under a running app (X server killed, logging out): Qt ends the process with exit(1) right
// in the event loop. exit() runs the static destructors while playback, render and decoder threads still work - they
// then crashed in what was already destroyed (seen: FileStamp's store from the renderer, the reader's registry).
// The real program on an own Xvfb display, playing a clip; the Xvfb is killed mid-playback: the app must end with
// exit code 1 and never with a signal (SIGSEGV/SIGABRT). Several rounds (the crash depended on timing).
// Linux with Xvfb only (skipped elsewhere).

#include "check.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

namespace {

// Starts Xvfb on a free display (it picks one and says which: -displayfd); "" = did not start
QString startXvfb(QProcess& x)
{
    x.setProgram(QStandardPaths::findExecutable("Xvfb"));
    x.setArguments({"-displayfd", "1", "-screen", "0", "1280x720x24", "-nolisten", "tcp"});
    x.start();
    if (!x.waitForStarted(5000)) return {};
    QElapsedTimer t;
    t.start();
    QByteArray out;
    while (!out.contains('\n') && !t.hasExpired(10000)) {
        x.waitForReadyRead(100);
        out += x.readAllStandardOutput();
    }
    const QByteArray number = out.trimmed();
    return number.isEmpty() ? QString() : ":" + QString::fromLatin1(number);
}

// One round: app playing on its own display, display killed after `playMs`. Returns a description of how it ended.
void round(const QString& clip, int playMs)
{
    QProcess x;
    const QString display = startXvfb(x);
    if (!CHECK(!display.isEmpty())) return;
    QProcess app;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("DISPLAY", display);
    env.insert("QT_QPA_PLATFORM", "xcb");
    env.insert("SCHNEIDI_LIVE", "0");
    env.insert("SCHNEIDI_AUDIO_DEVICE", "null");
    env.remove("WAYLAND_DISPLAY");
    app.setProcessEnvironment(env);
    app.setProcessChannelMode(QProcess::MergedChannels);
    app.start(Check::appPath(), {"--new-instance", "--demo", "--actions", "play_pause", clip});
    if (!CHECK(app.waitForStarted(10000))) {
        x.kill();
        x.waitForFinished(5000);
        return;
    }
    QThread::msleep(playMs);
    CHECK(app.state() == QProcess::Running); // still playing when the display goes
    x.kill();
    x.waitForFinished(5000);
    const bool ended = app.waitForFinished(30000);
    if (!CHECK(ended)) {
        app.kill();
        app.waitForFinished(5000);
        return;
    }
    const QByteArray out = app.readAll();
    const bool clean = app.exitStatus() == QProcess::NormalExit && app.exitCode() == 1;
    if (!CHECK(clean))
        std::printf("       after %d ms: %s, code %d\n%s\n", playMs,
                    app.exitStatus() == QProcess::CrashExit ? "crashed" : "exited", app.exitCode(),
                    out.right(800).constData());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("display-loss");
#if !defined(Q_OS_LINUX)
    return Check::skip("X11 only");
#endif
    if (QStandardPaths::findExecutable("Xvfb").isEmpty()) return Check::skip("no Xvfb");
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    if (Check::appPath().isEmpty()) return Check::skip("schneidi not built");
    QTemporaryDir dir;
    const QString clip = Check::makeMedia(
        dir.filePath("clip.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=30", "-c:v", "libx264",
                                   "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!clip.isEmpty())) return Check::result();
    for (int playMs : {4000, 5000, 6000}) round(clip, playMs);
    return Check::result();
}
