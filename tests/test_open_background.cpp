// SPDX-License-Identifier: AGPL-3.0-only
// Opening files in the background (Engine::setOpenInBackground, the app's mode): MLT probes every file it opens,
// which decodes pictures – on one core 25 s for a 12-clip 1080p60 project, all of it in the GUI thread. Now the
// window stays usable: updateTimeline() returns at once, the build follows when the files are open.
// Checked: the timeline is built in the end and shows the right picture; changes during the opening are not lost
// (the newest timeline wins, also when it needs more files); a format change and quitting during an opening.
#include "check.h"

#include "engine/Engine.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

namespace {

Clip mk(int id, const QString& path, int start, int length)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Until the engine has opened everything (false = not within 30 s)
bool waitOpened(Engine& engine)
{
    QElapsedTimer t;
    t.start();
    while (engine.opening() && t.elapsed() < 30000) wait(10);
    return !engine.opening();
}

// Shown picture at `frame`: the middle pixel of the next frameReady after the seek
QColor shownAt(Engine& engine, int frame)
{
    QColor color;
    QEventLoop loop;
    const QMetaObject::Connection c = QObject::connect(&engine, &Engine::frameReady, &engine, [&](const QImage& img) {
        if (engine.position() != frame || img.isNull()) return;
        color = img.pixelColor(img.width() / 2, img.height() / 2);
        loop.quit();
    });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    engine.seek(frame);
    loop.exec();
    QObject::disconnect(c);
    return color;
}

bool near(const QColor& c, int r, int g, int b)
{
    return c.isValid() && std::abs(c.red() - r) < 40 && std::abs(c.green() - g) < 40 && std::abs(c.blue() - b) < 40;
}

QString text(const QColor& c) { return c.isValid() ? c.name() : QStringLiteral("(kein Bild)"); }

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("open_background");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    auto make = [&](const char* name, const char* color) {
        return Check::makeMedia(tmp.filePath(name), {"-f", "lavfi", "-i", QString("color=c=%1:size=320x180:rate=25:d=4").arg(color),
                                                     "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", "4",
                                                     "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac"});
    };
    const QString red = make("red.mp4", "red"), blue = make("blue.mp4", "blue"), green = make("green.mp4", "lime");
    if (!CHECK(!red.isEmpty() && !blue.isEmpty() && !green.isEmpty())) return Check::result();

    auto timeline = [](std::initializer_list<QString> files) {
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        int start = 0, id = 1;
        for (const QString& f : files) {
            tl.video[0].clips << mk(id++, f, start, 50);
            tl.audio[0].clips << mk(id++, f, start, 50);
            start += 50;
        }
        return tl;
    };

    {
        Engine engine;
        QString error;
        if (!engine.init(&error)) {
            std::printf("Engine: %s\n", qPrintable(error));
            return Check::skip("Engine startet nicht");
        }
        engine.setFormat(ProjectFormat{320, 180, {25, 1}});
        engine.setOpenInBackground(true);
        engine.showTimeline(0);

        // Opens in the background; a change meanwhile that needs one more file is built afterwards
        engine.updateTimeline(timeline({red}));
        CHECK(engine.opening());
        engine.updateTimeline(timeline({red, blue}));
        CHECK(waitOpened(engine));
        const QColor first = shownAt(engine, 10), second = shownAt(engine, 60);
        Check::report(near(first, 255, 0, 0), "near(first, 255, 0, 0)", __FILE__, __LINE__, "Bild 10: " + text(first));
        Check::report(near(second, 0, 0, 255), "near(second, 0, 0, 255)", __FILE__, __LINE__, "Bild 60: " + text(second));

        // Edits without new files are built at once (nothing to open)
        Timeline swapped = timeline({blue, red});
        engine.updateTimeline(swapped);
        CHECK(!engine.opening());
        const QColor swappedColor = shownAt(engine, 10);
        Check::report(near(swappedColor, 0, 0, 255), "near(swappedColor, 0, 0, 255)", __FILE__, __LINE__, "getauscht, Bild 10: " + text(swappedColor));

        // Format change while opening: the waiting timeline is built in the new format
        engine.updateTimeline(timeline({green, red}));
        engine.setFormat(ProjectFormat{640, 360, {25, 1}});
        engine.showTimeline(0);
        CHECK(waitOpened(engine));
        const QColor afterFormat = shownAt(engine, 10);
        Check::report(near(afterFormat, 0, 255, 0), "near(afterFormat, 0, 255, 0)", __FILE__, __LINE__, "nach Formatwechsel, Bild 10: " + text(afterFormat));

        // Quitting while opening: no crash, no hang
        engine.setFormat(ProjectFormat{320, 180, {25, 1}});
        engine.updateTimeline(timeline({red, blue, green}));
        CHECK(engine.opening());
    }
    CHECK(true); // quitting during an opening neither crashed nor hung
    return Check::result();
}
