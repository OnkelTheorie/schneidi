// SPDX-License-Identifier: AGPL-3.0-only
// Voice-over recording in the app (app/VoiceoverRecorder.h): arm A2 in the Mixer, the record button starts the
// timeline at the playhead and records (stand-in input: a tone paced by a timer, never the microphone), stopping
// playback ends it; the take lands in the Media Pool and on A2 at the playhead as one undo step, the viewer shows
// the red sign while it runs.
//
// By hand, real sound devices (never the speakers, never the microphone): VOICEOVER_E2E_INPUT=<input id> (the
// monitor of a test sink), VOICEOVER_E2E_OUTPUT=<that sink> and VOICEOVER_E2E_MEDIA=<file with sound> play the file
// into the sink and record its monitor; the placement is printed for comparing the take with the file (the take
// must sit exactly on the timeline's own sound). SCHNEIDI_AUDIO_DEVICE and SCHNEIDI_AUDIO_INPUT must be set empty.

#include "check.h"

#include "app/MainWindow.h"
#include "app/VoiceoverRecorder.h"
#include "core/Project.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "engine/audio/Device.h"
#include "ui/Viewer.h"

#include <QSettings>

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>

namespace {

void wait(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    const QString e2eInput = qEnvironmentVariable("VOICEOVER_E2E_INPUT");
    const QString e2eOutput = qEnvironmentVariable("VOICEOVER_E2E_OUTPUT");
    const bool e2e = !e2eInput.isEmpty() && !e2eOutput.isEmpty();
    if (!e2e) qputenv("SCHNEIDI_AUDIO_INPUT", "sine:440");
    QApplication app(argc, argv);
    Check::initApp("voiceover-app");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    if (e2e) {
        // Only into the named sink: it must be listed, else stop before anything plays (the fallback would be the
        // system default = the speakers)
        bool listed = false;
        for (const auto& o : Audio::Device::outputs("pulseaudio")) listed = listed || o.id == e2eOutput;
        if (!listed) return Check::skip("Testausgabe fehlt");
        QSettings().setValue("audio/driver", "pulseaudio");
        QSettings().setValue("audio/output", e2eOutput);
        QSettings().setValue("audio/input", e2eInput);
    }
    QTemporaryDir dir;
    const QString a = e2e ? qEnvironmentVariable("VOICEOVER_E2E_MEDIA") : Check::makeMedia(dir.path() + "/a.mp4",
                                       {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=6", "-f", "lavfi",
                                        "-i", "sine=frequency=1000:duration=6", "-c:v", "libx264", "-preset",
                                        "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!a.isEmpty())) return Check::result();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.importFiles({a}, true);
    Project* project = w.liveProject();
    auto* recorder = w.findChild<VoiceoverRecorder*>();
    auto* record = w.findChild<QToolButton*>("voiceover_record");
    auto* viewer = w.findChild<Viewer*>();
    if (!CHECK(recorder && record && viewer)) return Check::result();
    CHECK(record->isEnabled());

    engine.seek(25);
    wait(100);
    recorder->setArmedTrack(1); // the "R" of A2
    const int undoBefore = project->undoStack()->index();
    record->click();
    CHECK(recorder->recording());
    CHECK(record->isChecked());
    CHECK(viewer->recording());
    CHECK(engine.speed() == 1.0); // the timeline plays along
    wait(e2e ? 4000 : 1500);
    engine.pause(); // Space/K: stopping playback ends the take
    wait(50);
    CHECK(!recorder->recording());
    CHECK(!record->isChecked());
    CHECK(!viewer->recording());

    // The take: in the pool, on A2 from the playhead, latency taken off within the first second
    const Timeline& tl = project->timeline();
    CHECK(tl.audio.size() >= 2);
    if (tl.audio.size() >= 2 && CHECK(tl.audio[1].clips.size() == 1)) {
        const Clip& c = tl.audio[1].clips[0];
        const MediaInfo* m = project->mediaInfo(c.mediaPath);
        CHECK(m && m->hasAudio && !m->hasVideo);
        CHECK(QFileInfo(c.mediaPath).fileName().startsWith("voiceover-"));
        CHECK_EQ(QFileInfo(c.mediaPath).absolutePath(), QFileInfo(Voiceover::folder(QString())).absoluteFilePath());
        CHECK_EQ(c.start, 25);
        CHECK(c.in >= 0 && c.in <= 25);
        CHECK(m && c.out == m->length - 1);
        // ~1.5 s recorded (a busy machine: more)
        if (!e2e) CHECK(m && m->length >= 30 && m->length <= 80);
        if (e2e)
            std::printf("E2E take=%s start=%d subframe=%d in=%d fps=%g\n", qPrintable(c.mediaPath), c.start,
                        c.subframe, c.in, project->frameRate());
    }
    CHECK_EQ(project->undoStack()->index(), undoBefore + 1);
    project->undoStack()->undo();
    CHECK(project->timeline().audio.size() < 2 || project->timeline().audio[1].clips.isEmpty());

    // A locked track does not record
    recorder->setArmedTrack(0);
    project->edit("lock", [](Timeline& t) { t.audio[0].locked = true; });
    QString error;
    CHECK(!recorder->start(&error));
    CHECK(!error.isEmpty());
    CHECK(!recorder->recording());
    if (e2e) QSettings().clear();
    return Check::result();
}
