// SPDX-License-Identifier: AGPL-3.0-only
// Clip › "Musik auf Länge…" (0.17, app/FitMusicDialog -> op fit_music through Cli::Live::runForUser): in the clip
// context menu only with an audio clip; the dialog takes the timeline's length (the other clips) and cuts the song to
// it within a frame as one undo step named after the entry (undo brings the whole song back); the In/Out range starts
// the song at In; a song shorter than the length and a cut song that would cover another clip are refused in the
// user's language with nothing changed. FIT_MUSIC_DUMP=<file.png>: picture of the dialog.
#include "check.h"

#include "app/MainWindow.h"
#include "app/InputBindings.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUndoStack>

#include <cmath>
#include <functional>

namespace {

bool waitUntil(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

// Answers the next fit_music dialog with `fill` (nullptr: cancel); the next message box's text into *box
void answerDialog(const std::function<void(QDialog*)>& fill, QString* box = nullptr, const QString& dump = {})
{
    QTimer::singleShot(0, [fill, box, dump] {
        waitUntil([] { return QApplication::activeModalWidget() && QApplication::activeModalWidget()->objectName() == "fit_music_dialog"; });
        auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dlg || dlg->objectName() != "fit_music_dialog") return;
        if (!dump.isEmpty()) dlg->grab().save(dump);
        if (!fill) return dlg->reject();
        fill(dlg);
        if (box)
            QTimer::singleShot(0, [box] {
                waitUntil([] { return qobject_cast<QMessageBox*>(QApplication::activeModalWidget()) != nullptr; }, 60000);
                if (auto* m = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    *box = m->text();
                    m->accept();
                }
            });
        if (auto* ok = dlg->findChild<QPushButton*>("fit_music_ok")) ok->click();
    });
}

QVector<Clip> songParts(Project* p) { return p->timeline().audio.isEmpty() ? QVector<Clip>{} : p->timeline().audio[0].clips; }

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("fit_music_app");
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    Check::initEngine();
    // 120 BPM for 64 s: a click on every beat, a kick on every bar one, a louder one every 8 bars
    const QString song = Check::makeMedia(
        tmp.filePath("song.wav"),
        {"-f", "lavfi", "-i",
         "aevalsrc='if(lt(mod(t,0.5),0.03),0.5*sin(2*PI*1500*t)*exp(-mod(t,0.5)*80),0)"
         "+if(lt(mod(t,2),0.15),(0.5+0.3*lt(mod(t,16),2))*sin(2*PI*55*t)*exp(-mod(t,2)*20),0)'"
         ":s=48000:d=64",
         "-c:a", "pcm_s16le"});
    const QString picture = Check::makeMedia(tmp.filePath("picture.mkv"), {"-f", "lavfi", "-i", "color=c=0x305070:s=320x180:d=30:r=25",
                                                                           "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    const QString other = Check::makeMedia(tmp.filePath("other.wav"), {"-f", "lavfi", "-i", "sine=f=300:d=3", "-c:a", "pcm_s16le"});
    if (song.isEmpty() || picture.isEmpty() || other.isEmpty()) return Check::skip("ffmpeg could not make the media");

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    Project* p = w.liveProject();
    {
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        d.format.rate = {25, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 2);
        Clip v;
        v.id = 1;
        v.mediaPath = picture;
        v.in = 0;
        v.out = 749; // 30 s
        Clip a;
        a.id = 2;
        a.mediaPath = song;
        a.in = 0;
        a.out = 64 * 25 - 1;
        d.timeline.video[0].clips << v;
        d.timeline.audio[0].clips << a;
        d.lastClipId = 2;
        p->load(d);
        for (const QString& f : {picture, song, other}) p->addMedia(Engine::probe(p->format(), f));
    }
    QAction* action = InputBindings::instance().action("fit_music");
    if (!CHECK(action)) return Check::result();
    CHECK_EQ(action->text(), T("Musik auf Länge…"));
    w.liveSelection()->set({2});

    // Cancel: nothing happens
    const int undo0 = p->undoStack()->index();
    answerDialog(nullptr, nullptr, qEnvironmentVariable("FIT_MUSIC_DUMP"));
    action->trigger();
    CHECK_EQ(p->undoStack()->index(), undo0);
    CHECK_EQ(songParts(p).size(), 1);

    // The timeline's length (the 30 s picture), the default without In/Out
    QString box;
    answerDialog([](QDialog* dlg) {
        auto* tl = dlg->findChild<QRadioButton*>("fit_music_timeline");
        if (CHECK(tl)) {
            CHECK(tl->isEnabled() && tl->isChecked());
            CHECK(tl->text().contains(QString("30") + I18n::decimalPoint() + "0"));
        }
        auto* inout = dlg->findChild<QRadioButton*>("fit_music_inout");
        if (CHECK(inout)) CHECK(!inout->isEnabled());
    }, &box);
    action->trigger();
    if (!CHECK(box.isEmpty())) std::printf("       %s\n", qPrintable(box));
    {
        const QVector<Clip> parts = songParts(p);
        CHECK(parts.size() >= 2);
        if (!parts.isEmpty()) {
            CHECK_EQ(parts.first().start, 0);
            CHECK(std::abs(parts.last().end() - 750) <= 1);
        }
        CHECK_EQ(p->undoStack()->index(), undo0 + 1);
        CHECK_EQ(p->undoStack()->undoText(), T("Musik auf Länge"));
        p->undoStack()->undo(); // the whole song again
        CHECK_EQ(songParts(p).size(), 1);
        CHECK_EQ(songParts(p).value(0).end(), 64 * 25);
    }

    // In/Out range: the song starts at In and fills the range
    p->edit("inout", [](Timeline& tl) {
        tl.markIn = 100;
        tl.markOut = 100 + 20 * 25 - 1;
    });
    w.liveSelection()->set({2});
    box.clear();
    answerDialog([](QDialog* dlg) {
        auto* inout = dlg->findChild<QRadioButton*>("fit_music_inout");
        if (CHECK(inout)) CHECK(inout->isEnabled() && inout->isChecked());
    }, &box);
    action->trigger();
    CHECK(box.isEmpty());
    {
        const QVector<Clip> parts = songParts(p);
        if (CHECK(!parts.isEmpty())) {
            CHECK_EQ(parts.first().start, 100);
            CHECK(std::abs(parts.last().end() - 600) <= 1);
        }
        p->undoStack()->undo();
    }

    // Longer than the song: refused in the user's language, nothing changed
    const int undo1 = p->undoStack()->index();
    w.liveSelection()->set({2});
    box.clear();
    answerDialog([](QDialog* dlg) {
        auto* secs = dlg->findChild<QDoubleSpinBox*>("fit_music_seconds");
        if (CHECK(secs)) secs->setValue(90);
        auto* custom = dlg->findChild<QRadioButton*>("fit_music_custom");
        if (CHECK(custom)) CHECK(custom->isChecked());
        auto* hint = dlg->findChild<QLabel*>("fit_music_hint"); // said before the op refuses it
        if (CHECK(hint)) CHECK(hint->text().startsWith(T("Der Song ist nur %1 lang – kürzer als die gewünschte Länge.").left(16)));
    }, &box);
    action->trigger();
    CHECK_EQ(box, T("Der Song ist nur %1 lang – kürzer als die gewünschte Länge (%2).")
                      .arg(T("%1 s").arg(QString("64") + I18n::decimalPoint() + "0"),
                           T("%1 s").arg(QString("90") + I18n::decimalPoint() + "0")));
    CHECK_EQ(p->undoStack()->index(), undo1);
    CHECK_EQ(songParts(p).size(), 1);

    // The cut song would cover another clip of its track: refused (not overwritten), in the user's language
    p->edit("other", [&](Timeline& tl) {
        Clip o;
        o.id = 10;
        o.mediaPath = other;
        o.in = 0;
        o.out = 74;
        o.start = 64 * 25 + 10; // after the whole song, inside the In/Out range below
        tl.audio[0].clips << o;
        tl.markIn = 1500;
        tl.markOut = 1500 + 20 * 25 - 1;
    });
    const int undo2 = p->undoStack()->index();
    w.liveSelection()->set({2});
    box.clear();
    answerDialog([](QDialog*) {}, &box);
    action->trigger();
    CHECK_EQ(box, T("Der gekürzte Song würde andere Clips auf seiner Spur überdecken – den Clip daneben verschieben oder "
                    "eine freie Spur nehmen."));
    CHECK_EQ(p->undoStack()->index(), undo2);
    CHECK_EQ(songParts(p).size(), 2); // the song and the other clip, untouched

    return Check::result();
}
