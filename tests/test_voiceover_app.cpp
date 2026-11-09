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
// VOICEOVER_DUMP=<png>: picture of the Mixer and the viewer while recording; VOICEOVER_TIMELINE_DUMP=<png>: the
// timeline with the growing take (and <png>-header.png: the armed track's header with its mic and meter).
//
// 0.20 "record where you are": the mic in an audio track's header arms it (its level shows there), Shift+R records
// and stops, the take grows live in the timeline (fed by the stand-in input) and ends at exactly the recorded
// length; nothing armed or selected, sound in the way or a locked track: a new track, one undo step with the clip.

#include "check.h"

#include "app/MainWindow.h"
#include "app/VoiceoverRecorder.h"
#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "engine/audio/Device.h"
#include "app/InputBindings.h"
#include "core/Selection.h"
#include "ui/Mixer.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QTest>

#include <QSettings>
#include <QShortcut>

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
    // Pictures in another language (VOICEOVER_LANG=ru; QT_FONT_DPI=120 for 1.25x text)
    if (const QString lang = qEnvironmentVariable("VOICEOVER_LANG"); !lang.isEmpty()) I18n::install(lang);
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
    if (const QString dump = qEnvironmentVariable("VOICEOVER_DUMP"); !dump.isEmpty()) {
        if (auto* mixer = w.findChild<Mixer*>()) mixer->show();
        wait(800);
        w.grab().save(dump);
    }
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
    // "Stimme säubern" next to the record button: voice cleaning on the armed track's clips, one undo step
    if (auto* clean = w.findChild<QToolButton*>("clean_voice"); CHECK(clean)) {
        clean->click();
        CHECK_EQ(project->undoStack()->index(), undoBefore + 2);
        const Timeline& now = project->timeline();
        CHECK(now.audio.size() >= 2 && now.audio[1].clips.size() == 1
              && EffectRegistry::has(now.audio[1].clips[0], "voiceclean"));
        CHECK(!now.audio[0].clips.isEmpty() && !EffectRegistry::has(now.audio[0].clips[0], "voiceclean"));
        project->undoStack()->undo();
    }
    project->undoStack()->undo();
    CHECK(project->timeline().audio.size() < 2 || project->timeline().audio[1].clips.isEmpty());

    // Esc ends the take also in the large viewer (its own Esc shortcut must not make the key ambiguous)
    w.setViewerLarge(true);
    record->click();
    CHECK(recorder->recording());
    w.setViewerLarge(true); // turned on again while recording
    int escapes = 0;
    for (QShortcut* s : w.findChildren<QShortcut*>())
        escapes += s->isEnabled() && s->key() == QKeySequence(Qt::Key_Escape);
    CHECK_EQ(escapes, 1);
    wait(300);
    engine.pause();
    wait(50);
    CHECK(!recorder->recording());
    w.setViewerLarge(false);
    if (CHECK_EQ(project->undoStack()->index(), undoBefore + 1)) project->undoStack()->undo();

    // A second take (the timeline already playing): pausing for the start must not end it at once
    engine.seek(25);
    engine.play();
    wait(200);
    record->click();
    wait(50);
    CHECK(recorder->recording());
    CHECK(viewer->recording());
    wait(500);
    engine.pause();
    wait(50);
    CHECK(!recorder->recording());
    CHECK(!viewer->recording());
    CHECK(project->timeline().audio.size() >= 2 && project->timeline().audio[1].clips.size() == 1);
    CHECK_EQ(project->undoStack()->index(), undoBefore + 1);
    project->undoStack()->undo();

    // ---- 0.20: the mic in the timeline's track header, the key, the growing take, the track choice
    auto* view = w.findChild<TimelineView*>();
    if (!CHECK(view)) return Check::result();
    recorder->setArmedTrack(-1);
    // A click on A1's mic arms it, a second one disarms; armed, its input level comes in (before recording)
    int levels = 0;
    const auto levelConn = QObject::connect(recorder, &VoiceoverRecorder::inputLevel, [&](float db) { levels += db > -40; });
    const QRect mic = view->micButtonRect(0);
    if (CHECK(!mic.isEmpty())) {
        QTest::mouseClick(view, Qt::LeftButton, {}, mic.center());
        CHECK_EQ(recorder->armedTrack(), 0);
        CHECK_EQ(view->armedTrack(), 0);
        wait(300);
        CHECK(levels > 0);
        if (const QString dump = qEnvironmentVariable("VOICEOVER_TIMELINE_DUMP"); !dump.isEmpty())
            view->grab(QRect(0, mic.top() - 6, TimelineView::kHeaderW, 52)).save(dump + "-header.png");
        QTest::mouseClick(view, Qt::LeftButton, {}, mic.center());
        CHECK_EQ(recorder->armedTrack(), -1);
    }
    QObject::disconnect(levelConn);

    // Nothing armed or selected: Shift+R records onto a new track; the take grows while it runs
    w.liveSelection()->clear();
    const int tracks = int(project->timeline().audio.size());
    engine.seek(25);
    wait(100);
    const int undoKey = project->undoStack()->index();
    int grew = 0;
    const auto growConn = QObject::connect(recorder, &VoiceoverRecorder::takeChanged, [&] { ++grew; });
    w.activateWindow();
    view->setFocus();
    QAction* key = InputBindings::instance().action("voiceover_record");
    CHECK(key && key->shortcut() == QKeySequence("Shift+R"));
    QTest::keyClick(view, Qt::Key_R, Qt::ShiftModifier);
    CHECK(recorder->recording());
    CHECK(key && key->isChecked()); // the toolbar's mic is lit
    CHECK_EQ(recorder->liveTrack(), tracks);
    wait(500);
    const double midway = recorder->take().lengthFrames();
    CHECK(midway > 3); // the clip in the timeline has grown (25 fps: ~12 frames by now)
    CHECK(grew > 5);
    float loudest = 0;
    for (float p : recorder->take().peaks()) loudest = std::max(loudest, p);
    CHECK(loudest > 0.2f); // the stand-in tone (-12 dBFS) in the live waveform
    CHECK(engine.position() > 25); // the playhead runs along
    if (const QString dump = qEnvironmentVariable("VOICEOVER_TIMELINE_DUMP"); !dump.isEmpty()) {
        wait(400);
        view->update();
        wait(50);
        w.findChild<TimelinePanel*>()->grab().save(dump);
    }
    wait(300);
    QTest::keyClick(view, Qt::Key_R, Qt::ShiftModifier); // the same key stops
    CHECK(!recorder->recording());
    CHECK(key && !key->isChecked());
    QObject::disconnect(growConn);
    CHECK_EQ(project->undoStack()->index(), undoKey + 1);
    CHECK_EQ(int(project->timeline().audio.size()), tracks + 1);
    CHECK_EQ(recorder->placedTrack(), tracks);
    if (project->timeline().audio.size() == tracks + 1 && CHECK(project->timeline().audio[tracks].clips.size() == 1)) {
        // Exactly the recorded length: the file holds every sample the take counted
        const Clip& c = project->timeline().audio[tracks].clips[0];
        const MediaInfo* m = project->mediaInfo(c.mediaPath);
        const double recorded = double(recorder->take().samples()) / recorder->take().rate() * project->frameRate();
        CHECK(m && std::abs(m->length - recorded) < 1.0);
        CHECK_EQ(c.start, 25);
        CHECK(m && c.out == m->length - 1);
        CHECK(recorded > midway);
    }
    project->undoStack()->undo(); // one step: the clip and its new track
    CHECK_EQ(int(project->timeline().audio.size()), tracks);
    CHECK_EQ(project->undoStack()->index(), undoKey);

    // A selected clip on A1 makes A1 "the" track - but its sound lies under the playhead: a new track again
    if (!project->timeline().audio[0].clips.isEmpty()) {
        w.liveSelection()->set({project->timeline().audio[0].clips[0].id});
        engine.seek(25);
        wait(100);
        QTest::keyClick(view, Qt::Key_R, Qt::ShiftModifier);
        CHECK_EQ(recorder->liveTrack(), tracks);
        wait(300);
        engine.pause();
        wait(50);
        CHECK(!recorder->recording());
        CHECK_EQ(int(project->timeline().audio.size()), tracks + 1);
        CHECK_EQ(project->timeline().audio[0].clips.size(), qsizetype(1)); // A1's sound stays whole
        project->undoStack()->undo();
    }

    // A locked track is not recorded over: the take gets a new track
    recorder->setArmedTrack(0);
    project->edit("lock", [](Timeline& t) { t.audio[0].locked = true; });
    QString error;
    engine.seek(25);
    wait(100);
    CHECK(recorder->start(&error));
    CHECK_EQ(recorder->liveTrack(), tracks);
    wait(300);
    CHECK(recorder->stop(&error));
    CHECK_EQ(recorder->placedTrack(), tracks);
    project->undoStack()->undo();
    // ---- Recording does not stop at the timeline's end: it plays on (open end), the playhead and the take run on
    {
        const double fps = project->frameRate();
        recorder->setArmedTrack(-1);
        w.liveSelection()->clear();
        // Only 1 s of content, recording from 0.5 s for 2 s: a clip from 0.5 s to 2.5 s
        const int undoTrim = project->undoStack()->index();
        project->edit("trim", [&](Timeline& t) {
            for (Track& tr : t.video) for (Clip& c : tr.clips) c.out = c.in + int(fps) - 1;
            for (Track& tr : t.audio) for (Clip& c : tr.clips) c.out = c.in + int(fps) - 1;
        });
        auto takeFor2s = [&](int from, const char* what) {
            engine.seek(from);
            wait(100);
            QTest::keyClick(view, Qt::Key_R, Qt::ShiftModifier);
            CHECK(recorder->recording());
            CHECK(engine.openEnd());
            wait(2000);
            CHECK(recorder->recording()); // the end of the content did not end it
            CHECK(engine.position() > from + int(1.5 * fps)); // the playhead ran on
            QTest::keyClick(view, Qt::Key_R, Qt::ShiftModifier);
            CHECK(!recorder->recording());
            CHECK(!engine.openEnd());
            const int track = recorder->placedTrack();
            const Timeline& tl = project->timeline();
            if (!CHECK(track >= 0 && track < tl.audio.size() && tl.audio[track].clips.size() == 1)) return;
            const Clip& c = tl.audio[track].clips[0];
            const int length = c.out - c.in + 1;
            const double recorded = double(recorder->take().samples()) / recorder->take().rate() * fps;
            std::printf("%s: clip %d..%d (%d frames, recorded %.1f)\n", what, c.start, c.start + length, length,
                        recorded);
            CHECK_EQ(c.start, from);
            CHECK(std::abs(c.in + length - recorded) < 1.0);
            CHECK(length >= int(1.8 * fps) && length <= int(2.8 * fps)); // ~2 s (a busy machine: a bit more)
        };
        takeFor2s(int(fps / 2), "1 s of content, from 0.5 s");
        project->undoStack()->undo();
        // An empty timeline: the playhead runs, a clip of 2 s at 0
        project->edit("empty", [](Timeline& t) {
            for (Track& tr : t.video) tr.clips.clear();
            for (Track& tr : t.audio) tr.clips.clear();
        });
        takeFor2s(0, "empty timeline");
        project->undoStack()->undo(); // the take
        project->undoStack()->undo(); // emptied
        project->undoStack()->undo(); // trimmed
        CHECK_EQ(project->undoStack()->index(), undoTrim);
    }

    // Quitting while recording: the take is placed first (so "save changes?" sees it), the file is complete
    project->edit("unlock", [](Timeline& t) { t.audio[0].locked = false; });
    recorder->setArmedTrack(1);
    engine.seek(25);
    wait(100);
    record->click();
    CHECK(recorder->recording());
    wait(400);
    const int beforeQuit = project->undoStack()->index();
    w.close();
    CHECK(!recorder->recording());
    CHECK_EQ(project->undoStack()->index(), beforeQuit + 1);
    CHECK(project->timeline().audio.size() >= 2 && project->timeline().audio[1].clips.size() == 1);

    if (e2e) QSettings().clear();
    return Check::result();
}
