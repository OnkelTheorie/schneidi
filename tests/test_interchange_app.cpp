// Timeline import in the app (File › Import Timeline): an FCP7 XML from "another computer" (Windows paths) whose
// files lie next to it becomes a new, opened timeline in an empty project, which takes the file's format; one undo
// step removes it again
#include "check.h"

#include "app/MainWindow.h"
#include "core/Interchange.h"
#include "core/Project.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>
#include <clocale>

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("interchange-app");
    QTemporaryDir dir;
    const QString a = Check::makeMedia(dir.filePath("A001.mp4"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=50:duration=4",
                                                                   "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
                                                                   "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                                   "-c:a", "aac", "-shortest"});
    const QString b = Check::makeMedia(dir.filePath("A002.mp4"), {"-f", "lavfi", "-i", "smptebars=size=320x240:rate=50:duration=4",
                                                                   "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    // A camera file with a start timecode (EDL source times count in it)
    const QString cam = Check::makeMedia(dir.filePath("cam.mov"), {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=50:duration=4",
                                                                    "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                                    "-timecode", "01:00:10:05"});
    if (a.isEmpty() || b.isEmpty() || cam.isEmpty()) return Check::kSkip;
    // Premiere-style: 50 fps, 320x240, a dissolve between A001 and A002 (their in/out cover it), A001's sound linked
    const QByteArray xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE xmeml>
<xmeml version="4"><sequence id="sequence-1"><name>Rohschnitt</name>
 <rate><timebase>50</timebase><ntsc>FALSE</ntsc></rate>
 <media><video><format><samplecharacteristics><width>320</width><height>240</height></samplecharacteristics></format>
  <track>
   <clipitem id="clipitem-1"><name>A001.mp4</name><start>0</start><end>-1</end><in>10</in><out>120</out>
    <file id="file-1"><pathurl>file://localhost/D%3a/Schnitt/A001.mp4</pathurl></file>
    <link><linkclipref>clipitem-1</linkclipref></link><link><linkclipref>clipitem-3</linkclipref></link></clipitem>
   <transitionitem><start>90</start><end>110</end><alignment>center</alignment>
    <effect><name>Cross Dissolve</name><effectid>Cross Dissolve</effectid></effect></transitionitem>
   <clipitem id="clipitem-2"><name>A002.mp4</name><start>-1</start><end>180</end><in>10</in><out>100</out>
    <file id="file-2"><pathurl>file://localhost/D%3a/Schnitt/A002.mp4</pathurl></file></clipitem>
  </track></video>
  <audio><track>
   <clipitem id="clipitem-3"><name>A001.mp4</name><start>0</start><end>100</end><in>10</in><out>110</out>
    <file id="file-1"/><sourcetrack><mediatype>audio</mediatype><trackindex>1</trackindex></sourcetrack></clipitem>
  </track></audio></media>
 <marker><in>75</in><out>-1</out></marker>
</sequence></xmeml>)";
    const QString xmlPath = dir.filePath("rohschnitt.xml");
    QFile f(xmlPath);
    if (!CHECK(f.open(QIODevice::WriteOnly) && f.write(xml) == xml.size())) return Check::result();
    f.close();

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    CHECK_EQ(Engine::startTimecode(cam), QString("01:00:10:05"));
    CHECK_EQ(Engine::startTimecode(a), QString());
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    Project* p = w.liveProject();
    const int timelinesBefore = int(p->sequences().size());
    CHECK(w.importTimelineFile(xmlPath));
    QApplication::processEvents();
    CHECK_EQ(int(p->sequences().size()), timelinesBefore + 1);
    CHECK_EQ(p->sequenceName(p->currentSequence()), QString("Rohschnitt"));
    CHECK(p->format().rate == (FrameRate{50, 1}));
    CHECK_EQ(p->format().width, 320);
    const Timeline& tl = p->timeline();
    if (CHECK_EQ(tl.video[0].clips.size(), 2)) {
        const Clip& c1 = tl.video[0].clips[0];
        const Clip& c2 = tl.video[0].clips[1];
        CHECK_EQ(c1.mediaPath, QFileInfo(a).absoluteFilePath()); // found next to the XML file
        CHECK_EQ(c1.in, 10);
        CHECK_EQ(c1.length(), 100);
        CHECK_EQ(c2.start, 100);
        CHECK_EQ(c2.in, 20);
        CHECK_EQ(c1.transOut, 20);
        CHECK_EQ(c2.transIn, 20);
    }
    if (CHECK_EQ(tl.audio[0].clips.size(), 1)) CHECK(tl.audio[0].clips[0].linkId == tl.video[0].clips[0].linkId);
    CHECK_EQ(tl.markers, QVector<int>{75});
    CHECK(p->mediaInfo(QFileInfo(a).absoluteFilePath()) && p->mediaInfo(QFileInfo(b).absoluteFilePath()));
    // Undo: the imported timeline is gone in one step (the project format is a step of its own in the same macro)
    p->undoStack()->undo();
    QApplication::processEvents();
    CHECK_EQ(int(p->sequences().size()), timelinesBefore);
    // A file that is not a timeline
    QFile bad(dir.filePath("kaputt.otio"));
    if (bad.open(QIODevice::WriteOnly)) bad.write("{nope");
    bad.close();
    QTimer::singleShot(200, [] { // the warning is modal
        if (QWidget* m = QApplication::activeModalWidget()) m->close();
    });
    CHECK(!w.importTimelineFile(dir.filePath("kaputt.otio")));
    // An EDL from another program: the clip starts 2 s into the camera file (01:00:12:05); read in the project's
    // 25 fps (the import above was undone)
    QFile edl(dir.filePath("kamera.edl"));
    if (edl.open(QIODevice::WriteOnly))
        edl.write("TITLE: Kamera\nFCM: NON-DROP FRAME\n\n"
                  "001  AX       V     C        01:00:12:05 01:00:13:05 01:00:00:00 01:00:01:00\n"
                  "* FROM CLIP NAME: cam.mov\n");
    edl.close();
    CHECK(w.importTimelineFile(dir.filePath("kamera.edl")));
    QApplication::processEvents();
    CHECK_EQ(p->sequenceName(p->currentSequence()), QString("Kamera"));
    if (CHECK_EQ(p->timeline().video[0].clips.size(), 1)) {
        CHECK_EQ(p->timeline().video[0].clips[0].in, 50);
        CHECK_EQ(p->timeline().video[0].clips[0].length(), 25);
    }
    return Check::result();
}
