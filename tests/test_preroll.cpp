// SPDX-License-Identifier: AGPL-3.0-only
// Test decoder pre-roll (engine/Preroll, TimelineBuilder::prerollPoints): clips of the same file at a dissolve
// or hard cut get their own producers alternately, transition + body of a clip read in one go;
// jump points are correct; pre-decoded points do not change the displayed images.
#include "check.h"

#include "core/ProjectFormat.h"
#include "engine/Preroll.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>
#include <clocale>
#include <cstring>

namespace {

QString g_media;
const ProjectFormat kFmt{640, 360, {25, 1}};

Clip clip(int id, int start, int in, int out)
{
    Clip c;
    c.id = id;
    c.mediaPath = g_media;
    c.start = start;
    c.in = in;
    c.out = out;
    return c;
}

// A | dissolve | B | C (split, seamless) | D (hard cut, other spot of the same file)
Timeline scene()
{
    Clip a = clip(1, 0, 0, 29), b = clip(2, 30, 50, 69), c = clip(3, 50, 70, 79), d = clip(4, 60, 10, 29);
    a.transOut = b.transIn = 10; // Cross Dissolve 25..35, B from source frame 45
    Timeline tl;
    tl.video.resize(1);
    tl.video[0].clips = {a, b, c, d};
    return tl;
}

QImage grab(Mlt::Producer& p, int pos)
{
    p.seek(pos);
    std::unique_ptr<Mlt::Frame> f(p.get_frame());
    if (!f) return {};
    mlt_image_format fmt = mlt_image_rgba;
    int w = kFmt.width, h = kFmt.height;
    const uint8_t* d = f->get_image(fmt, w, h);
    if (!d) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), d, size_t(w) * h * 4);
    return img;
}

void testPoints()
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder builder(*prof);
    auto tractor = builder.build(scene());
    const auto& pts = builder.prerollPoints();
    QString list;
    for (const auto& p : pts) list += QString("[%1 frei ab %2 Quelle %3] ").arg(p.frame).arg(p.freeFrom).arg(p.source);
    // A at the start, B (fading-in side) from 25/source 45, D from 60/source 10 on A's producer (free from 35).
    // No jump at B body (35), C (seamless) and the fading-out side of A.
    if (!CHECK_EQ(int(pts.size()), 3)) qInfo("%s", qPrintable(list));
    if (pts.size() != 3) return;
    CHECK(pts[0].frame == 0 && pts[0].source == 0);
    CHECK(pts[1].frame == 25 && pts[1].source == 45 && pts[1].freeFrom == 0);
    CHECK(pts[2].frame == 60 && pts[2].source == 10 && pts[2].freeFrom == 35);
    CHECK(pts[2].producer->get_producer() == pts[0].producer->get_producer()); // D reads from A's producer
    CHECK(pts[1].producer->get_producer() != pts[0].producer->get_producer());
    for (const auto& p : pts) CHECK(!p.audio);
}

void testSameImages()
{
    // Reference: fresh builder without pre-roll
    auto prof = makeProfile(kFmt);
    TimelineBuilder ref(*prof);
    auto refTractor = ref.build(scene());
    TimelineBuilder builder(*prof);
    auto tractor = builder.build(scene());
    {
        Preroll preroll;
        preroll.setPoints(builder.prerollPoints(), 50, QSize(kFmt.width, kFmt.height));
        preroll.update(40, 1.0); // D (60) is due, A's producer free since 35
        QThread::msleep(1500);
        preroll.update(12, 1.0); // jump back: B (25) again
        QThread::msleep(1500);
    }
    for (int pos = 20; pos < 80; ++pos) {
        const QImage a = grab(*tractor, pos), b = grab(*refTractor, pos);
        if (!CHECK(!a.isNull() && a == b)) {
            qInfo("anderes Bild an Frame %d", pos);
            break;
        }
    }
}

// clear() waits for a running decode and hands back all producers (Engine::setFormat then tears down the profile)
void testClearWaits()
{
    auto prof = makeProfile(kFmt);
    auto builder = std::make_unique<TimelineBuilder>(*prof);
    auto tractor = builder->build(scene());
    const std::vector<PrerollPoint> pts = builder->prerollPoints();
    if (!CHECK_EQ(int(pts.size()), 3)) return;
    Preroll preroll;
    for (int i = 0; i < 5; ++i) {
        preroll.setPoints(builder->prerollPoints(), 50, QSize(kFmt.width, kFmt.height));
        preroll.update(40, 1.0);
        QThread::msleep(5 + 10 * i); // right in the middle of decoding
        preroll.clear();
        for (const auto& p : pts) CHECK_EQ(long(p.producer.use_count()), 2L); // pts and builder, nobody else
    }
    tractor.reset();
    builder.reset();
    prof.reset();
    preroll.update(60, 1.0); // nothing left to do, must not touch anything released
    QThread::msleep(50);
}

// A rebuild (new points) waits for a decode of the old graph that is still running: playback starts on the new
// graph right after, and the builder hands it the same cached producers
void testRebuildWaits()
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder builder(*prof);
    auto tractor = builder.build(scene());
    const std::vector<PrerollPoint> pts = builder.prerollPoints();
    if (!CHECK_EQ(int(pts.size()), 3)) return;
    Preroll preroll;
    for (int i = 0; i < 5; ++i) {
        preroll.setPoints(builder.prerollPoints(), 50, QSize(kFmt.width, kFmt.height));
        preroll.update(40, 1.0);
        QThread::msleep(5 + 10 * i); // right in the middle of decoding
        preroll.update(40, 0.0);     // no new decode after this one
        preroll.setPoints(builder.prerollPoints(), 50, QSize(kFmt.width, kFmt.height));
        for (const auto& p : pts) CHECK_EQ(long(p.producer.use_count()), 3L); // pts, builder, the new points
    }
    preroll.clear();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("preroll");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // Long GOP like OBS recordings (only one keyframe)
    g_media = Check::makeMedia(tmp.filePath("src.mp4"),
                               {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-c:v", "libx264",
                                "-preset", "ultrafast", "-g", "250", "-pix_fmt", "yuv420p"});
    if (!CHECK(!g_media.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    {
        auto prof = makeProfile(kFmt);
        Mlt::Transition qt(*prof, "qtblend");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }
    testPoints();
    testSameImages();
    testClearWaits();
    testRebuildWaits();
    return Check::result();
}
