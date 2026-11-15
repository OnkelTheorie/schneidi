// SPDX-License-Identifier: AGPL-3.0-only
// The Studio page (0.23):
// - built lazily: a new window has none of its widgets (the start stays as fast), the first showing builds it;
// - its button sits between Color and Deliver, the key (Shift+7, action page_studio) and `app page` (LiveHost) show it,
//   `app state` tells the page; it hides like the other pages;
// - the viewer moves into its top row; the lanes start at the timeline's zoom and scroll, and a model change (a track
//   more) never changes them (core rule);
// - the pen: a drag over a clip's lane makes volume keyframes at the drawn level, one undo step per stroke;
// - the meters take the channel count from the levels (6 channels = 5.1 bars; back to 2), never a fixed stereo pair;
// - layout: at 1024 px wide, in German and Russian, normal and large font, nothing of the page is cut off (every label,
//   button, box at least its minimum width and inside its parent) and the window does not grow; the tools column is
//   there; at 1920 px with two tracks it fills the room between the strips and the master (no gap), cards side by side,
//   every strip shown whole.
// - tools (Block 2), each one undo step with the right effect on the project: "Stimme säubern" (voiceclean on the
//   target clips + EQ preset on the track), ducking (keys down while the voice sounds; again with another amount;
//   off), "Lautheit auf Ziel" (op mixer target_lufs through the user runner, limiter on, before -> after shown), the
//   target follows the selection, recording from the page (synthetic input) onto the chosen track, effect sounds
//   (search, preview in the player, place at the playhead, drop onto a lane); a volume keyframe dragged and deleted
//   on the lane; the keys A / P pick Auswahl / Stift on the page.
// STUDIO_PAGE_DUMP=<folder>: a picture of the window per layout case.

#include "check.h"

#include <QScrollBar>

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/VoiceoverRecorder.h"
#include "cli/Live.h"
#include "core/Automation.h"
#include "core/EffectRegistry.h"
#include "core/Equalizer.h"
#include "core/Selection.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/EffectFolders.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "core/Voiceover.h"
#include "engine/Engine.h"
#include "ui/Viewer.h"
#include "ui/studio/StudioMixer.h"
#include "ui/EffectsLibrary.h"
#include "ui/studio/StudioPage.h"
#include "ui/studio/StudioTools.h"
#include "ui/studio/StudioTracks.h"

#include <QWheelEvent>
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QFile>
#include <QDir>
#include <QProgressDialog>
#include <QMenu>
#include <QScrollArea>
#include <QSlider>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

using Page = MainWindow::Page;

void settle(int rounds = 3)
{
    for (int i = 0; i < rounds; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

void mouse(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), Qt::LeftButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

QAbstractButton* pageButton(MainWindow& w, const char* id)
{
    return w.findChild<QAbstractButton*>(QString("pageButton_") + id);
}

void testWindow(Engine& engine, const QString& audioFile)
{
    QSettings().remove("layout");
    QSettings().remove("studio/penTarget"); // the default: the track
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    if (!CHECK(QTest::qWaitForWindowExposed(&w))) return;
    settle();

    // Lazy: nothing of the page before its first showing
    CHECK(w.studioPage() == nullptr);
    CHECK(w.findChild<StudioPage*>() == nullptr);
    CHECK(w.findChild<StudioMixer*>() == nullptr);
    CHECK(w.findChild<StudioTracks*>() == nullptr);

    // Its button between Color and Deliver
    QAbstractButton* color = pageButton(w, "color");
    QAbstractButton* studio = pageButton(w, "studio");
    QAbstractButton* deliver = pageButton(w, "deliver");
    if (!CHECK(color && studio && deliver)) return;
    CHECK(color->x() < studio->x() && studio->x() < deliver->x());
    CHECK(studio->isVisible());

    w.importFiles({audioFile}, true);
    settle(5);
    Project* project = w.liveProject();
    if (!CHECK(!project->timeline().audio.isEmpty() && !project->timeline().audio[0].clips.isEmpty())) return;
    const int clipId = project->timeline().audio[0].clips[0].id;

    // The key shows it (built now), the viewer in its top row
    QAction* key = InputBindings::instance().action("page_studio");
    if (!CHECK(key)) return;
    CHECK_EQ(key->shortcut().toString(), QString("Shift+7"));
    key->trigger();
    settle();
    CHECK(w.currentPage() == Page::Studio);
    StudioPage* page = w.studioPage();
    if (!CHECK(page)) return;
    CHECK(page->isVisible());
    CHECK(studio->isChecked());
    CHECK_EQ(w.livePage(), QString("studio"));
    Viewer* viewer = w.findChild<Viewer*>();
    CHECK(viewer && viewer->parentWidget() == page->top() && viewer->isVisible());

    // The lanes start at the timeline's zoom; a model change leaves them alone
    StudioTracks* tracks = page->tracks();
    const ViewState& tv = w.findChild<TimelineView*>()->view();
    CHECK(std::abs(tracks->view().pxPerFrame - tv.pxPerFrame) < 1e-9);
    const double zoom = tracks->view().pxPerFrame, left = tracks->view().leftFrame;
    project->edit("track", [](Timeline& tl) { tl.audio << Track{TrackKind::Audio}; });
    settle();
    CHECK_EQ(tracks->view().pxPerFrame, zoom);
    CHECK_EQ(tracks->view().leftFrame, left);
    CHECK_EQ(page->mixer()->stripCount(), int(project->timeline().audio.size()));
    {
        // Alt+wheel zooms like on the timeline, although Qt delivers it on the x axis while Alt is held
        const double before = tracks->view().pxPerFrame;
        const QPointF at(StudioTracks::kHeaderW + 50, 40);
        QWheelEvent alt(at, tracks->mapToGlobal(at), QPoint(), QPoint(120, 0), Qt::NoButton, Qt::AltModifier,
                        Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(tracks, &alt);
        CHECK(tracks->view().pxPerFrame > before * 1.05);
        tracks->setView(before, tracks->view().leftFrame);
    }
    project->undoStack()->undo();
    settle();

    // Pen: a stroke over the clip's lane at -20 dB, one undo step, keys at that level (clip mode; the default is the
    // track, tested below)
    auto* pen = page->findChild<QToolButton*>("studio_tool_pen");
    if (!CHECK(pen)) return;
    CHECK(tracks->tool() == StudioTracks::Tool::Points); // the page's default: points on the line (rubber band)
    pen->click();
    CHECK(tracks->tool() == StudioTracks::Tool::Pen);
    auto* penClip = page->findChild<QToolButton*>("studio_pen_clip");
    auto* penTrack = page->findChild<QToolButton*>("studio_pen_track");
    if (!CHECK(penClip && penTrack)) return;
    CHECK(penTrack->isChecked() && tracks->penTarget() == StudioTracks::PenTarget::Track);
    penClip->click();
    CHECK(tracks->penTarget() == StudioTracks::PenTarget::Clip);
    CHECK_EQ(QSettings().value("studio/penTarget").toString(), QString("clip"));
    const Clip* clip = TimelineOps::findClip(project->timeline(), clipId);
    if (!CHECK(clip)) return;
    // Shift+Z (zoom_fit) on this page fits the tracks like the Fit button, not the hidden timeline
    tracks->setView(tracks->view().pxPerFrame * 4, 10);
    if (QAction* fitKey = InputBindings::instance().action("zoom_fit"); CHECK(fitKey)) fitKey->trigger();
    settle();
    const ViewState byKey = tracks->view();
    tracks->fit(); // the user's Fit button: the whole clip in view
    settle();
    CHECK(byKey.pxPerFrame == tracks->view().pxPerFrame && byKey.leftFrame == tracks->view().leftFrame);
    const QRect lane = tracks->laneRect(0).adjusted(0, 4, 0, -4);
    const int y = lane.bottom() - int(std::lround(VolumeDraw::posOf(-20) * (lane.height() - 1)));
    const int f0 = clip->start + clip->length() / 4, f1 = clip->start + clip->length() * 3 / 4;
    QUndoStack* undo = project->undoStack();
    const int before = undo->index();
    mouse(tracks, QEvent::MouseButtonPress, QPoint(int(tracks->xOf(f0)), y), Qt::LeftButton);
    for (int i = 1; i <= 10; ++i) {
        const double f = f0 + (f1 - f0) * i / 10.0;
        mouse(tracks, QEvent::MouseMove, QPoint(int(tracks->xOf(f)), y), Qt::LeftButton);
    }
    mouse(tracks, QEvent::MouseButtonRelease, QPoint(int(tracks->xOf(f1)), y), Qt::NoButton);
    settle();
    CHECK_EQ(undo->index(), before + 1);
    clip = TimelineOps::findClip(project->timeline(), clipId);
    if (CHECK(clip) && CHECK(Keys::animated(*clip, AnimParam::Volume))) {
        const double mid = Keys::valueAt(*clip, AnimParam::Volume, clip->length() / 2);
        Check::report(std::abs(mid + 20) < 1.0, "drawn level", __FILE__, __LINE__, QString("%1 dB").arg(mid));
        CHECK(std::abs(Keys::valueAt(*clip, AnimParam::Volume, 0) - clip->volumeDb) < 1e-6); // before the stroke: as it was
    }
    undo->undo();
    clip = TimelineOps::findClip(project->timeline(), clipId);
    CHECK(clip && !Keys::animated(*clip, AnimParam::Volume));
    // A click without dragging, a stroke straight up and down in place: nothing (no 1-frame dip or spike)
    {
        const int at = undo->index();
        const QPoint here(int(tracks->xOf(clip->start + clip->length() / 2)), y);
        mouse(tracks, QEvent::MouseButtonPress, here, Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, here, Qt::NoButton);
        mouse(tracks, QEvent::MouseButtonPress, here, Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, here - QPoint(0, 20), Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, here + QPoint(0, 20), Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, here + QPoint(0, 20), Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at);
        clip = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(clip && !Keys::animated(*clip, AnimParam::Volume));
    }

    // Track mode: the same stroke becomes the track's automation (one undo step), the clip keeps no keys; it reaches
    // past the clip's end into the empty lane
    penTrack->click();
    CHECK(tracks->penTarget() == StudioTracks::PenTarget::Track);
    {
        const int at = undo->index();
        // into the empty lane past the clip as far as the view goes (Fit shows the project: little room after it)
        const int g0 = clip->start + clip->length() / 2;
        const int g1 = std::min(clip->end() + 20, int(tracks->frameAt(tracks->width() - 24)));
        mouse(tracks, QEvent::MouseButtonPress, QPoint(int(tracks->xOf(g0)), y), Qt::LeftButton);
        for (int i = 1; i <= 10; ++i)
            mouse(tracks, QEvent::MouseMove, QPoint(int(tracks->xOf(g0 + (g1 - g0) * i / 10.0)), y), Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, QPoint(int(tracks->xOf(g1)), y), Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at + 1);
        CHECK_EQ(undo->undoText(), QString("Spur-Lautstärke zeichnen"));
        const Track& a1 = project->timeline().audio[0];
        clip = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(clip && !Keys::animated(*clip, AnimParam::Volume));
        CHECK(!a1.volumeAuto.isEmpty());
        const double mid = Automation::levelAt(a1, (g0 + clip->end()) / 2.0);
        Check::report(std::abs(mid + 20) < 1.0, "drawn track level", __FILE__, __LINE__, QString("%1 dB").arg(mid));
        if (g1 > clip->end() + 2) CHECK(std::abs(Automation::levelAt(a1, (clip->end() + g1) / 2.0) + 20) < 1.0); // past the clip
        CHECK(std::abs(Automation::levelAt(a1, clip->start) - a1.volumeDb) < 1e-6); // before the stroke: the fader
        // The dots: a point drags (one step) and a right click deletes it
        int shownAt = -1;
        for (int i = 0; i < a1.volumeAuto.size() && shownAt < 0; ++i)
            if (!tracks->autoPoint(0, i).isNull() && a1.volumeAuto[i].frame > g0 + 2) shownAt = i;
        if (CHECK(shownAt >= 0)) {
            page->findChild<QToolButton*>("studio_tool_select")->click();
            const QPoint dot = tracks->autoPoint(0, shownAt).toPoint();
            const int before2 = undo->index();
            mouse(tracks, QEvent::MouseButtonPress, dot, Qt::LeftButton);
            mouse(tracks, QEvent::MouseMove, dot - QPoint(0, 6), Qt::LeftButton);
            mouse(tracks, QEvent::MouseMove, dot - QPoint(0, 12), Qt::LeftButton);
            mouse(tracks, QEvent::MouseButtonRelease, dot - QPoint(0, 12), Qt::NoButton);
            settle();
            CHECK_EQ(undo->index(), before2 + 1);
            CHECK(Automation::levelAt(project->timeline().audio[0], project->timeline().audio[0].volumeAuto[shownAt].frame) > -19.5);
            const int count = int(project->timeline().audio[0].volumeAuto.size());
            QContextMenuEvent menu(QContextMenuEvent::Mouse, tracks->autoPoint(0, shownAt).toPoint());
            QApplication::sendEvent(tracks, &menu);
            settle();
            CHECK_EQ(int(project->timeline().audio[0].volumeAuto.size()), count - 1);
            undo->undo();
            undo->undo();
            pen->click();
        }
        // The mixer's fader shows the level at the playhead (read mode)
        engine.seek((g0 + clip->end()) / 2);
        settle();
        QTest::qWait(80);
        auto* fader = page->findChild<QWidget*>("studio_fader_A1");
        if (CHECK(fader)) {
            const QLabel* value = nullptr;
            for (QLabel* l : fader->parentWidget()->findChildren<QLabel*>())
                if (l->text().endsWith(" dB") && l->toolTip().contains("Automation")) value = l;
            const double shown = value ? value->text().section(' ', 0, 0).replace(QChar(0x2212), '-').toDouble() : 1e9;
            const double want = Automation::levelAt(project->timeline().audio[0], engine.position());
            Check::report(std::abs(shown - want) < 0.11, "fader shows the automated level", __FILE__, __LINE__,
                          QString("%1 / %2 at %3").arg(value ? value->text() : "-").arg(want).arg(engine.position()));
        }
        undo->undo();
        CHECK(project->timeline().audio[0].volumeAuto.isEmpty());
    }

    // Pan mode (track mode): the pen draws the pan automation (top right, bottom left), one undo step; the mixer's
    // pan shows the automated pan at the playhead. Write mode is a toggle, off by default.
    auto* autoPan = page->findChild<QToolButton*>("studio_auto_pan");
    auto* autoVolume = page->findChild<QToolButton*>("studio_auto_volume");
    auto* write = page->findChild<QToolButton*>("studio_write");
    if (CHECK(autoPan && autoVolume && write)) {
        CHECK(autoVolume->isChecked() && autoPan->isEnabled() && tracks->autoTarget() == AutoTarget::Volume);
        CHECK(!write->isChecked() && !page->mixer()->writeMode());
        write->click();
        CHECK(page->mixer()->writeMode());
        write->click();
        CHECK(!page->mixer()->writeMode());
        pen->click();
        autoPan->click();
        CHECK(tracks->autoTarget() == AutoTarget::Pan);
        const int at = undo->index();
        const QRect body = tracks->laneRect(0).adjusted(0, 4, 0, -4);
        const int py = body.bottom() - int(std::lround(0.2 * (body.height() - 1))); // pan -60
        const int g0 = clip->start + clip->length() / 4, g1 = clip->start + clip->length() * 3 / 4;
        mouse(tracks, QEvent::MouseButtonPress, QPoint(int(tracks->xOf(g0)), py), Qt::LeftButton);
        for (int i = 1; i <= 10; ++i)
            mouse(tracks, QEvent::MouseMove, QPoint(int(tracks->xOf(g0 + (g1 - g0) * i / 10.0)), py), Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, QPoint(int(tracks->xOf(g1)), py), Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at + 1);
        CHECK_EQ(undo->undoText(), QString("Spur-Pan zeichnen"));
        const Track& a1 = project->timeline().audio[0];
        CHECK(a1.volumeAuto.isEmpty() && !a1.panAuto.isEmpty());
        const double mid = Automation::panAt(a1, (g0 + g1) / 2.0);
        Check::report(std::abs(mid + 60) < 2.0, "drawn track pan", __FILE__, __LINE__, QString::number(mid));
        engine.seek((g0 + g1) / 2);
        settle();
        QTest::qWait(80);
        bool shown = false;
        for (QLabel* l : page->mixer()->findChildren<QLabel*>())
            shown |= l->toolTip().contains("Pan-Automation") && l->text().startsWith("L");
        CHECK(shown);
        undo->undo();
        CHECK(project->timeline().audio[0].panAuto.isEmpty());
        autoVolume->click();
        CHECK(tracks->autoTarget() == AutoTarget::Volume);
    }

    // Points: a click on the track's level line sets a point, dragging it right away is the same undo step; a double
    // click puts it at 0 dB, a right click deletes it. In clip mode the click sets a key of the clip.
    if (auto* points = page->findChild<QToolButton*>("studio_tool_points"); CHECK(points)) {
        points->click();
        CHECK(tracks->tool() == StudioTracks::Tool::Points);
        const QRect body = tracks->laneRect(0).adjusted(0, 4, 0, -4);
        const auto yAt = [&](double db) { return body.bottom() - int(std::lround(VolumeDraw::posOf(db) * (body.height() - 1))); };
        const int f = clip->start + clip->length() / 2;
        const double fader = project->timeline().audio[0].volumeDb;
        const QPoint on(int(tracks->xOf(f)), yAt(fader));
        const int at = undo->index();
        // Beside the line: no point (a click there moves the playhead as with Select)
        mouse(tracks, QEvent::MouseButtonPress, on + QPoint(0, 25), Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, on + QPoint(0, 25), Qt::NoButton);
        CHECK_EQ(undo->index(), at);
        mouse(tracks, QEvent::MouseButtonPress, on, Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, on - QPoint(0, 10), Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, on - QPoint(0, 20), Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, on - QPoint(0, 20), Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at + 1);
        const Track& a1 = project->timeline().audio[0];
        CHECK_EQ(int(a1.volumeAuto.size()), 1);
        CHECK(Automation::levelAt(a1, f) > fader + 0.5);
        const QPoint dot = tracks->autoPoint(0, 0).toPoint();
        if (CHECK(!dot.isNull())) {
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(dot), tracks->mapToGlobal(QPointF(dot)), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tracks, &dbl);
            settle();
            CHECK(std::abs(Automation::levelAt(project->timeline().audio[0], f)) < 1e-6);
            QContextMenuEvent menu(QContextMenuEvent::Mouse, tracks->autoPoint(0, 0).toPoint());
            QApplication::sendEvent(tracks, &menu);
            settle();
            CHECK(project->timeline().audio[0].volumeAuto.isEmpty());
        }
        while (undo->index() > at) undo->undo();
        // Clip mode: a key of the clip on its (orange) line
        penClip->click();
        const int at2 = undo->index();
        clip = TimelineOps::findClip(project->timeline(), clipId);
        const QPoint onClip(int(tracks->xOf(f)), yAt(clip->volumeDb));
        mouse(tracks, QEvent::MouseButtonPress, onClip, Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, onClip, Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at2 + 1);
        clip = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(clip && clip->keys.value(AnimParam::Volume).size() == 1 && project->timeline().audio[0].volumeAuto.isEmpty());
        undo->undo();
        clip = TimelineOps::findClip(project->timeline(), clipId);
        penTrack->click();
    }
    page->findChild<QToolButton*>("studio_tool_select")->click();

    // Playing moves the playhead every frame: only its old and new column are drawn anew, not the whole lanes
    {
        tracks->setPlayhead(clip->start + 10);
        settle();
        const int paints = tracks->paintCount();
        tracks->setPlayhead(clip->start + 11);
        settle();
        CHECK(tracks->paintCount() > paints);
        const QRegion drawn = tracks->lastPaintRegion();
        int area = 0, widest = 0;
        for (const QRect& r : drawn) {
            area += r.width() * r.height();
            widest = std::max(widest, r.width());
        }
        Check::report(widest <= StudioTracks::kHeaderW && area < tracks->width() * tracks->height() / 10
                          && drawn.contains(QPoint(int(tracks->xOf(clip->start + 11)), tracks->height() / 2)),
                      "playhead move draws its columns only", __FILE__, __LINE__,
                      QString("widest %1 px, %2 px² of %3").arg(widest).arg(area).arg(tracks->width() * tracks->height()));
    }

    // A trimmed clip's keys outside it are not shown, not grabbed, and a drag stays inside the clip
    {
        const int in0 = clip->in;
        project->edit("trimmed keys", [&](Timeline& tl) {
            Clip& c = tl.audio[0].clips[0];
            c.in = in0 + 20; // trimmed at the start: keys before it are outside
            Keys::setKey(c, AnimParam::Volume, -15, -10); // source frame in - 15: hidden
            Keys::setKey(c, AnimParam::Volume, 10, -5);
            Keys::setKey(c, AnimParam::Volume, 40, -5);
        });
        tracks->fit();
        settle();
        const Clip* c = TimelineOps::findClip(project->timeline(), clipId);
        if (CHECK(c) && CHECK_EQ(c->keys.value(AnimParam::Volume).size(), 3)) {
            CHECK(tracks->keyPoint(clipId, 0).isNull()); // outside: hidden
            const QPointF k1 = tracks->keyPoint(clipId, 1);
            if (CHECK(!k1.isNull())) {
                // Dragged far to the left: stops at the clip's first frame, not at its hidden neighbour
                const int at = undo->index();
                const QPoint from = k1.toPoint(), to(int(tracks->xOf(c->start - 40)), from.y());
                mouse(tracks, QEvent::MouseButtonPress, from, Qt::LeftButton);
                mouse(tracks, QEvent::MouseMove, to, Qt::LeftButton);
                mouse(tracks, QEvent::MouseButtonRelease, to, Qt::NoButton);
                settle();
                CHECK_EQ(undo->index(), at + 1);
                c = TimelineOps::findClip(project->timeline(), clipId);
                if (CHECK(c)) CHECK_EQ(c->keys.value(AnimParam::Volume).value(1).frame - c->in, 0);
            }
            // Pressing where the hidden key would be (left of the clip) grabs nothing
            const QPoint hidden(int(tracks->xOf(c->start - 15)), tracks->laneRect(0).center().y());
            const int at = undo->index();
            mouse(tracks, QEvent::MouseButtonPress, hidden, Qt::LeftButton);
            mouse(tracks, QEvent::MouseMove, hidden + QPoint(30, 0), Qt::LeftButton);
            mouse(tracks, QEvent::MouseButtonRelease, hidden + QPoint(30, 0), Qt::NoButton);
            settle();
            CHECK_EQ(undo->index(), at);
        }
        while (undo->index() > 0 && undo->undoText() != "trimmed keys") undo->undo();
        undo->undo();
        clip = TimelineOps::findClip(project->timeline(), clipId);
        CHECK(clip && clip->in == in0 && !Keys::animated(*clip, AnimParam::Volume));
    }

    // A take onto a new track shows on a lane of its own below the others while it records
    {
        const int lanes = tracks->laneCount();
        CHECK_EQ(lanes, int(project->timeline().audio.size()));
        Voiceover::LiveTake take(0, 0, project->frameRate(), 48000);
        take.add(96000, 0.5f); // two seconds
        tracks->setLiveTake(&take, int(project->timeline().audio.size()));
        settle();
        CHECK_EQ(tracks->laneCount(), lanes + 1);
        tracks->setLiveTake(nullptr, -1);
        settle();
        CHECK_EQ(tracks->laneCount(), lanes);
    }

    // Meters: as many bars as the mix has channels
    StudioMixer* mixer = page->mixer();
    const int groups = mixer->stripCount() + 1;
    mixer->onLevels(QVector<float>(6 * groups, -12.f));
    CHECK_EQ(mixer->channels(), 6);
    CHECK_EQ(mixer->meterChannels(0), 6);
    CHECK_EQ(mixer->meterChannels(-1), 6);
    mixer->onLevels(QVector<float>(2 * groups, -12.f));
    CHECK_EQ(mixer->meterChannels(0), 2);
    mixer->onLevels(QVector<float>(2 * groups + 1, -12.f)); // the track count changing: ignored
    CHECK_EQ(mixer->meterChannels(0), 2);
    // 5.1/7.1: the master names its speakers under the bars (wider bars, the strip grows); tracks and stereo do not
    CHECK(mixer->meterLabels(-1).isEmpty() && mixer->meterLabels(0).isEmpty());
    QWidget* masterFader = page->findChild<QWidget*>("studio_fader_master");
    const int stereoW = masterFader ? masterFader->width() : 0;
    mixer->onLevels(QVector<float>(6 * groups, -12.f));
    settle();
    CHECK(mixer->meterLabels(-1) == QStringList({"L", "R", "C", "LFE", "Ls", "Rs"}));
    CHECK(mixer->meterLabels(0).isEmpty());
    if (CHECK(masterFader)) {
        CHECK(masterFader->width() >= masterFader->minimumWidth() && masterFader->width() >= stereoW);
        std::printf("Master-Pegel 5.1: %d px breit (Stereo %d)\n", masterFader->width(), stereoW);
    }
    mixer->onLevels(QVector<float>(8 * groups, -12.f));
    settle();
    CHECK(mixer->meterLabels(-1) == QStringList({"L", "R", "C", "LFE", "Lrs", "Rrs", "Lss", "Rss"}));
    if (masterFader) CHECK(masterFader->width() >= masterFader->minimumWidth());
    mixer->onLevels(QVector<float>(2 * groups, -12.f));
    CHECK(mixer->meterLabels(-1).isEmpty());

    // 5.1 project: the surround sends show in the strips; a drag on one is one undo step (Editor::setTrackSurround)
    QWidget* sends = page->findChild<QWidget*>("studio_surround_A1");
    if (CHECK(sends)) {
        CHECK(!sends->isVisible()); // stereo: as before
        ProjectFormat f = project->format();
        f.audioChannels = 6;
        project->setFormat(f);
        settle();
        CHECK(sends->isVisible());
        QWidget* depth = sends->findChildren<QWidget*>().value(0);
        if (CHECK(depth)) {
            const int idx = undo->index();
            const QPoint a(depth->width() / 4, depth->height() / 2), b(depth->width() * 3 / 4, depth->height() / 2);
            mouse(depth, QEvent::MouseButtonPress, a, Qt::LeftButton);
            mouse(depth, QEvent::MouseMove, (a + b) / 2, Qt::LeftButton);
            mouse(depth, QEvent::MouseMove, b, Qt::LeftButton);
            mouse(depth, QEvent::MouseButtonRelease, b, Qt::NoButton);
            settle();
            CHECK_EQ(undo->index(), idx + 1);
            const double got = project->timeline().audio[0].panDepth;
            Check::report(got >= 40 && got <= 60, "depth dragged", __FILE__, __LINE__, QString::number(got));
            undo->undo();
        }
        f.audioChannels = 2;
        project->setFormat(f);
        settle();
        CHECK(!sends->isVisible());
    }

    // The live connection: switch pages by id
    CHECK(w.liveShowPage("edit"));
    CHECK(w.currentPage() == Page::Edit);
    // Hidden: the mixer's meters stop and timeline changes wait until the page shows again
    {
        const auto running = [mixer] {
            for (QTimer* t : mixer->findChildren<QTimer*>(Qt::FindDirectChildrenOnly))
                if (t->isActive()) return true;
            return false;
        };
        CHECK(!running());
        const int strips = mixer->stripCount();
        project->edit("track", [](Timeline& tl) { tl.audio << Track{TrackKind::Audio}; });
        settle();
        CHECK_EQ(mixer->stripCount(), strips); // not yet
        CHECK(w.liveShowPage("studio"));
        settle();
        CHECK_EQ(mixer->stripCount(), strips + 1);
        CHECK(running());
        project->undoStack()->undo();
        settle();
        CHECK_EQ(mixer->stripCount(), strips);
        CHECK(w.liveShowPage("edit"));
    }
    CHECK(!w.liveShowPage("fairlight"));
    CHECK(w.liveShowPage("studio"));
    CHECK(w.currentPage() == Page::Studio);
    CHECK(viewer->parentWidget() == page->top());

    // Hidden like the others: away from it to the nearest shown page
    w.setPageHidden(Page::Studio, true);
    CHECK(w.currentPage() == Page::Deliver || w.currentPage() == Page::Color);
    CHECK(!studio->isVisible());
    w.showAllPages();
    CHECK(studio->isVisible());
}


void mouseButton(QWidget* w, QEvent::Type type, const QPoint& pos, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

bool waitFor(const std::function<bool()>& ok, int ms = 8000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(20);
    return ok();
}

// The tools column, one by one on a two-track project: A1 the voice (frames 50-150), A2 the music (0-200)
void testTools(Engine& engine, const QString& voiceFile, const QString& musicFile)
{
    QSettings().remove("layout");
    QSettings().remove("studio/duckAmount");
    QSettings().remove("studio/duckBy");
    QSettings().setValue("audio/loudnessTarget", -14.0);
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    if (!CHECK(QTest::qWaitForWindowExposed(&w))) return;
    Project* p = w.liveProject();
    {
        ProjectData d;
        d.format.rate = {25, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 2);
        Clip v;
        v.id = 1;
        v.mediaPath = voiceFile;
        v.start = 50;
        v.in = 0;
        v.out = 99;
        d.timeline.audio[0].clips << v;
        Clip m;
        m.id = 2;
        m.mediaPath = musicFile;
        m.in = 0;
        m.out = 199;
        d.timeline.audio[1].clips << m;
        d.lastClipId = 2;
        p->load(d);
        p->addMedia(Engine::probe(p->format(), voiceFile));
        p->addMedia(Engine::probe(p->format(), musicFile));
    }
    w.showPage(Page::Studio);
    settle(5);
    StudioPage* page = w.studioPage();
    if (!CHECK(page)) return;
    StudioTools* tools = page->tools();
    StudioTracks* tracks = page->tracks();
    if (!CHECK(tools && tools->isVisible())) return;
    QUndoStack* undo = p->undoStack();
    const auto clip = [p](int id) { return TimelineOps::findClip(p->timeline(), id); };
    const auto click = [&](const char* name) {
        auto* b = tools->findChild<QToolButton*>(name);
        if (!CHECK(b)) return;
        b->click();
        settle();
    };

    // The target follows the selection
    p->closeMerge();
    tools->setTargetTrack(0);
    tracks->fit();
    settle();
    mouse(tracks, QEvent::MouseButtonPress, QPoint(int(tracks->xOf(20)), tracks->laneRect(1).center().y()), Qt::LeftButton);
    mouse(tracks, QEvent::MouseButtonRelease, QPoint(int(tracks->xOf(20)), tracks->laneRect(1).center().y()), Qt::NoButton);
    settle();
    CHECK_EQ(tools->targetTrack(), 1);
    CHECK_EQ(tools->targetClips(), QVector<int>{2});
    auto* targetBox = tools->findChild<QComboBox*>("studio_target");
    if (CHECK(targetBox)) CHECK_EQ(targetBox->currentData().toInt(), 1);

    // Stimme säubern: voiceclean on the voice clip and the EQ preset on A1, one step; undo takes both back
    tools->setTargetTrack(0);
    CHECK_EQ(tools->targetClips(), QVector<int>{1});
    int at = undo->index();
    click("studio_clean_voice");
    CHECK_EQ(undo->index(), at + 1);
    CHECK(clip(1) && EffectRegistry::has(*clip(1), "voiceclean"));
    CHECK(clip(2) && !EffectRegistry::has(*clip(2), "voiceclean"));
    CHECK(p->timeline().audio[0].eq == Equalizer::voicePreset());
    CHECK(p->timeline().audio[1].eq == TrackEq{});
    undo->undo();
    CHECK(clip(1) && !EffectRegistry::has(*clip(1), "voiceclean"));
    CHECK(p->timeline().audio[0].eq == TrackEq{});

    // Ducking: the music down while the voice sounds (frame 100 is in it, frame 10 not), one step
    tools->setTargetTrack(1);
    auto* amount = tools->findChild<QSlider*>("studio_duck_amount");
    if (!CHECK(amount)) return;
    CHECK_EQ(amount->value(), -12);
    at = undo->index();
    click("studio_duck");
    CHECK_EQ(undo->index(), at + 1);
    if (CHECK(clip(2) && Keys::animated(*clip(2), AnimParam::Volume))) {
        const double inVoice = Keys::valueAt(*clip(2), AnimParam::Volume, 100), before = Keys::valueAt(*clip(2), AnimParam::Volume, 10);
        Check::report(std::abs(inVoice - (clip(2)->volumeDb - 12)) < 0.1 && std::abs(before - clip(2)->volumeDb) < 0.1,
                      "ducked under the voice", __FILE__, __LINE__, QString("%1 / %2 dB").arg(inVoice).arg(before));
    }
    CHECK(clip(1) && !Keys::animated(*clip(1), AnimParam::Volume)); // the voice stays
    // Again with another amount: replaces the curve (one more step), the amount is remembered
    amount->setValue(-20);
    at = undo->index();
    click("studio_duck");
    CHECK_EQ(undo->index(), at + 1);
    if (clip(2)) CHECK(std::abs(Keys::valueAt(*clip(2), AnimParam::Volume, 100) - (clip(2)->volumeDb - 20)) < 0.1);
    CHECK_EQ(QSettings().value("studio/duckAmount").toInt(), -20);
    // Off: the keys go, one step
    auto* unduck = tools->findChild<QToolButton*>("studio_unduck");
    CHECK(unduck && unduck->isEnabled());
    at = undo->index();
    click("studio_unduck");
    CHECK_EQ(undo->index(), at + 1);
    CHECK(clip(2) && !Keys::animated(*clip(2), AnimParam::Volume));
    CHECK(unduck && !unduck->isEnabled());
    // Durch: all other audible tracks by default; picked tracks instead (remembered); the music's own track or one
    // that is not there does not count (then the default again)
    auto* duckBy = tools->findChild<QToolButton*>("studio_duck_by");
    if (CHECK(duckBy)) CHECK_EQ(duckBy->text(), QString("Alle anderen Spuren"));
    CHECK_EQ(tools->duckTracks(), QVector<int>{0});
    p->edit("third", [](Timeline& tl) { tl.audio << Track{TrackKind::Audio}; });
    p->edit("third clip", [&](Timeline& tl) {
        Clip c = *TimelineOps::findClip(tl, 1);
        c.id = 3;
        c.start = 300;
        tl.audio[2].clips << c;
    });
    settle();
    CHECK_EQ(tools->duckTracks(), (QVector<int>{0, 2}));
    tools->setDuckBy({2});
    CHECK_EQ(tools->duckTracks(), QVector<int>{2});
    if (duckBy) CHECK_EQ(duckBy->text(), QString("A3"));
    CHECK_EQ(QSettings().value("studio/duckBy").toList(), QVariantList{2});
    if (QMenu* m = duckBy ? duckBy->menu() : nullptr; CHECK(m)) {
        emit m->aboutToShow();
        CHECK_EQ(m->actions().size(), 4); // all others, separator, A1, A3 (not the music's A2)
    }
    tools->setDuckBy({1, 7}); // the music itself, a track not there
    CHECK_EQ(tools->duckTracks(), (QVector<int>{0, 2}));
    tools->setDuckBy({});
    undo->undo();
    undo->undo();
    settle();

    // Lautheit auf Ziel: Podcast -16, the master limiter on, one step, before -> after shown
    auto* preset = tools->findChild<QComboBox*>("studio_loudness_preset");
    if (!CHECK(preset && preset->count() == 3)) return;
    CHECK_EQ(preset->currentData().toDouble(), -14.0); // the meter's target
    preset->setCurrentIndex(1);
    CHECK_EQ(tools->loudnessPreset(), -16.0);
    // Canceled while it measures (a dialog with progress): nothing changed, no step; recording is off meanwhile and
    // AIs wait (the user's command holds the app)
    {
        auto* record = tools->findChild<QToolButton*>("studio_record");
        const QByteArray before = ProjectFile::toJson(p->data(), QString());
        const int at = undo->index();
        int ticks = 0;
        bool recordOff = false, aiWaits = false;
        QTimer poll;
        poll.setInterval(0);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            auto* d = tools->findChild<QProgressDialog*>();
            if (!d || d->wasCanceled() || ++ticks < 3) return;
            recordOff = record && !record->isEnabled();
            aiWaits = Cli::Live::runForUser(&w, "info", {{"project", "@app"}}, "x").value("error").toObject().value("code").toString() == "APP_BUSY";
            d->cancel();
        });
        poll.start();
        CHECK(!tools->loudnessToTarget());
        poll.stop();
        CHECK(ticks >= 3);
        CHECK(recordOff);
        CHECK(aiWaits);
        CHECK(record && record->isEnabled());
        CHECK_EQ(undo->index(), at);
        CHECK(ProjectFile::toJson(p->data(), QString()) == before);
        if (auto* line = tools->findChild<QLabel*>("studio_loudness_result"); CHECK(line))
            CHECK(line->text().startsWith("Abgebrochen"));
    }
    const double masterBefore = p->timeline().masterVolumeDb;
    const bool limiterBefore = p->timeline().masterLimiter;
    at = undo->index();
    click("studio_loudness_apply");
    CHECK_EQ(undo->index(), at + 1);
    CHECK(p->timeline().masterLimiter);
    CHECK(std::abs(p->timeline().masterVolumeDb - masterBefore) > 0.05);
    auto* loudLine = tools->findChild<QLabel*>("studio_loudness_result");
    if (CHECK(loudLine)) {
        Check::report(loudLine->isVisible() && loudLine->text().contains(QChar(0x2192)), "before -> after", __FILE__,
                      __LINE__, loudLine->text());
        CHECK(loudLine->text().contains(QString("16").replace('.', I18n::decimalPoint())));
    }
    CHECK_EQ(QSettings().value("audio/loudnessTarget").toDouble(), -16.0);
    undo->undo();
    CHECK_EQ(p->timeline().masterVolumeDb, masterBefore);
    CHECK_EQ(p->timeline().masterLimiter, limiterBefore);

    // Effect sounds: listed, the search narrows, Anhören plays it in the player, Einfügen puts it at the playhead
    auto* sounds = tools->findChild<QListWidget*>("studio_sounds");
    auto* search = tools->findChild<QLineEdit*>("studio_sound_search");
    if (!CHECK(sounds && search && sounds->count() > 6)) return;
    search->setText("Whoosh");
    int shown = 0;
    for (int i = 0; i < sounds->count(); ++i) shown += sounds->item(i)->isHidden() ? 0 : 1;
    CHECK(shown > 0 && shown < sounds->count());
    CHECK(sounds->currentItem() && !sounds->currentItem()->isHidden() && sounds->currentItem()->text().contains("Whoosh"));
    click("studio_sound_play");
    CHECK(waitFor([&] { return engine.mode() == Engine::Mode::Source; }, 3000));
    engine.pause();
    TimelineView* tv = w.findChild<TimelineView*>();
    tv->seekRequested(75);
    settle(5);
    const int clipsBefore = [&] {
        int n = 0;
        for (const Track& t : p->timeline().audio) n += int(t.clips.size());
        return n;
    }();
    at = undo->index();
    click("studio_sound_place");
    CHECK_EQ(undo->index(), at + 1);
    int clipsAfter = 0;
    for (const Track& t : p->timeline().audio) clipsAfter += int(t.clips.size());
    CHECK_EQ(clipsAfter, clipsBefore + 1);
    undo->undo();
    // A drop onto a lane: there (op 'sfx' anchors the sound's moment at the drop frame)
    {
        const QByteArray data = sounds->currentItem()->data(Qt::UserRole).toByteArray();
        auto* mime = new QMimeData;
        mime->setData(EffectsLibrary::SoundMimeType, data);
        const QPoint pos(int(tracks->xOf(120)), tracks->laneRect(0).center().y());
        QDragEnterEvent enter(pos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(tracks, &enter);
        CHECK(enter.isAccepted());
        QDropEvent drop(QPointF(pos), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
        at = undo->index();
        QApplication::sendEvent(tracks, &drop);
        CHECK(waitFor([&] { return undo->index() == at + 1; }, 3000));
        delete mime;
        undo->undo();
    }
    search->clear();
    // An own sound added to the sounds folder: in the list without a restart (the folder is watched)
    {
        const QString own = EffectFolders::soundDir() + "/studio-test-own.wav";
        QDir().mkpath(EffectFolders::soundDir());
        QFile::remove(own);
        const int count = sounds->count();
        CHECK(QFile::copy(voiceFile, own));
        CHECK(waitFor([&] { return sounds->count() == count + 1; }, 5000));
        CHECK(!sounds->findItems("studio-test-own", Qt::MatchContains).isEmpty());
        QFile::remove(own);
        CHECK(waitFor([&] { return sounds->count() == count; }, 5000));
    }

    // A volume keyframe on the lane: dragged (one step, later and lower), deleted with a right click (one step)
    p->edit("keys", [](Timeline& tl) {
        Clip& c = tl.audio[0].clips[0];
        Keys::setKey(c, AnimParam::Volume, 10, 0);
        Keys::setKey(c, AnimParam::Volume, 50, 0);
        Keys::setKey(c, AnimParam::Volume, 90, 0);
    });
    p->closeMerge();
    tracks->fit();
    settle();
    const QPointF key = tracks->keyPoint(1, 1);
    if (CHECK(!key.isNull())) {
        at = undo->index();
        const QPoint from = key.toPoint(), to(int(tracks->xOf(50 + 60 + 10)), from.y() + tracks->laneHeight() / 4);
        mouse(tracks, QEvent::MouseButtonPress, from, Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
        mouse(tracks, QEvent::MouseMove, to, Qt::LeftButton);
        mouse(tracks, QEvent::MouseButtonRelease, to, Qt::NoButton);
        settle();
        CHECK_EQ(undo->index(), at + 1);
        if (const Clip* c = clip(1); CHECK(c)) {
            const KeyTrack k = c->keys.value(AnimParam::Volume);
            if (CHECK_EQ(k.size(), 3)) {
                Check::report(k[1].frame - c->in == 70, "key moved in time", __FILE__, __LINE__, QString::number(k[1].frame));
                Check::report(k[1].value < -1, "key moved down", __FILE__, __LINE__, QString::number(k[1].value));
            }
        }
        const QPoint now = tracks->keyPoint(1, 1).toPoint();
        at = undo->index();
        QContextMenuEvent menu(QContextMenuEvent::Mouse, now, tracks->mapToGlobal(now));
        QApplication::sendEvent(tracks, &menu);
        CHECK_EQ(undo->index(), at + 1);
        if (const Clip* c = clip(1); CHECK(c)) CHECK_EQ(c->keys.value(AnimParam::Volume).size(), 2);
    }

    // The keys: P = Stift, A = Auswahl on this page; painting stays off
    QAction* draw = InputBindings::instance().action("tool_draw");
    QAction* select = InputBindings::instance().action("tool_select");
    if (CHECK(draw && select)) {
        draw->trigger();
        CHECK(tracks->tool() == StudioTracks::Tool::Pen);
        CHECK(!draw->isChecked());
        select->trigger();
        CHECK(tracks->tool() == StudioTracks::Tool::Select);
    }

    // Recording from the page: onto the chosen track (A2, armed), stopped from the page, one step
    auto* recorder = w.findChild<VoiceoverRecorder*>();
    auto* record = tools->findChild<QToolButton*>("studio_record");
    auto* recordTrack = tools->findChild<QComboBox*>("studio_record_track");
    // The input device, the same choice as in the Sound menu: the system default first
    auto* input = tools->findChild<QComboBox*>("studio_record_input");
    if (CHECK(input) && CHECK(input->count() >= 1)) {
        CHECK_EQ(input->itemText(0), QString("Systemstandard"));
        CHECK_EQ(input->itemData(0).toString(), QString());
        CHECK(input->isEnabled());
    }
    if (CHECK(recorder && record && recordTrack) && CHECK(record->isEnabled())) {
        CHECK_EQ(recordTrack->currentData().toInt(), -1); // Automatisch
        const int a2 = recordTrack->findData(1);
        recordTrack->setCurrentIndex(a2);
        emit recordTrack->activated(a2);
        CHECK_EQ(recorder->armedTrack(), 1);
        engine.seek(0);
        settle();
        const int clipsOnA2 = int(p->timeline().audio[1].clips.size());
        at = undo->index();
        record->click();
        CHECK(recorder->recording());
        if (CHECK(waitFor([&] { return recorder->live(); }))) {
            CHECK(record->isChecked());
            CHECK(input && !input->isEnabled()); // not while it records
            QTest::qWait(1200);
            record->click(); // stop
            CHECK(waitFor([&] { return !recorder->recording(); }));
            settle(5);
            CHECK_EQ(recorder->placedTrack(), 1);
            CHECK_EQ(undo->index(), at + 1);
            CHECK(!record->isChecked());
            Check::report(p->timeline().audio[1].clips.size() >= clipsOnA2, "take on A2", __FILE__, __LINE__,
                          QString::number(p->timeline().audio[1].clips.size()));
        }
        recorder->setArmedTrack(-1);
        CHECK_EQ(recordTrack->currentData().toInt(), -1);
    }
}

// Nothing of the page cut off: every label, button and box at least its minimum width, inside its parent
QString cutOff(QWidget* root)
{
    QStringList bad;
    for (QWidget* c : root->findChildren<QWidget*>()) {
        if (!c->isVisible()) continue;
        bool inViewer = false;
        for (QWidget* p = c->parentWidget(); p && p != root; p = p->parentWidget()) inViewer |= qobject_cast<Viewer*>(p) != nullptr;
        if (inViewer) continue; // the viewer's own (moved here; its layout is tested with it)
        if (!qobject_cast<QLabel*>(c) && !qobject_cast<QAbstractButton*>(c) && !qobject_cast<QComboBox*>(c)) continue;
        if (c->objectName() == "qt_scrollarea_hcontainer" || c->objectName() == "qt_scrollarea_vcontainer") continue;
        const QString name = QString("%1 %2 '%3'")
                                 .arg(c->metaObject()->className(), c->objectName(),
                                      qobject_cast<QLabel*>(c)            ? qobject_cast<QLabel*>(c)->text()
                                      : qobject_cast<QAbstractButton*>(c) ? qobject_cast<QAbstractButton*>(c)->text()
                                                                          : QString());
        if (c->width() + 1 < c->minimumSizeHint().width())
            bad << QString("%1 %2 < %3 px").arg(name).arg(c->width()).arg(c->minimumSizeHint().width());
        // Inside its parent (scrolled strips may be outside the scroll area's view: their parent is the strip)
        if (QWidget* p = c->parentWidget(); p && !p->rect().contains(c->geometry()))
            bad << QString("%1 outside its parent").arg(name);
    }
    return bad.join("; ");
}

void testLayout(Engine& engine, const QString& audioFile)
{
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.5);
    const QString dumpDir = qEnvironmentVariable("STUDIO_PAGE_DUMP");
    for (const char* lang : {"de", "ru"})
        for (bool large : {false, true}) {
            const QString where = QString("%1 %2").arg(lang, large ? "large" : "normal");
            I18n::install(lang);
            QApplication::setFont(large ? big : normal);
            QSettings().remove("layout");
            {
                MainWindow w(&engine);
                w.disableAutosave();
                w.resize(1024, 700);
                w.show();
                if (!CHECK(QTest::qWaitForWindowExposed(&w))) continue;
                w.importFiles({audioFile}, true);
                w.liveProject()->edit("tracks", [](Timeline& tl) {
                    while (tl.audio.size() < 3) tl.audio << Track{TrackKind::Audio};
                });
                if (large) { // the surround sends take room too (5.1)
                    ProjectFormat f = w.liveProject()->format();
                    f.audioChannels = 6;
                    w.liveProject()->setFormat(f);
                }
                settle();
                w.showPage(Page::Studio);
                settle(5);
                StudioPage* page = w.studioPage();
                if (!CHECK(page)) continue;
                Check::report(w.width() == 1024, "window not wider", __FILE__, __LINE__,
                              QString("%1: %2 px").arg(where).arg(w.width()));
                Check::report(page->width() <= 1024, "page fits", __FILE__, __LINE__, QString("%1: %2 px").arg(where).arg(page->width()));
                if (qEnvironmentVariableIsSet("STUDIO_DEBUG"))
                    for (QWidget* c : page->mixer()->findChildren<QWidget*>())
                        if (c->minimumSizeHint().width() > 100 || c->minimumWidth() > 100)
                            std::printf("%s %s min %d hint %d minW %d\n", c->metaObject()->className(), qPrintable(c->objectName()),
                                        c->minimumSizeHint().width(), c->sizeHint().width(), c->minimumWidth());
                // The tools column is there, at least one card wide
                StudioTools* tools = page->tools();
                Check::report(tools && tools->isVisible() && tools->width() >= 160, "tools column",
                              __FILE__, __LINE__, QString("%1: %2 px").arg(where).arg(tools ? tools->width() : 0));
                const QString cut = cutOff(page);
                Check::report(cut.isEmpty(), "nothing cut off", __FILE__, __LINE__, where + ": " + cut);
                // The lanes and the faders keep a usable size
                Check::report(page->tracks()->laneHeight() >= StudioTracks::kMinLaneH, "lanes tall enough", __FILE__,
                              __LINE__, QString("%1: %2 px").arg(where).arg(page->tracks()->laneHeight()));
                for (const char* name : {"studio_fader_master", "studio_fader_A1", "studio_fader_A3"})
                    if (auto* fader = page->findChild<QWidget*>(name); CHECK(fader)) {
                        // Whole and tall enough: inside the mixer's view, not cut off at its bottom
                        const QRect r(fader->mapTo(page->mixer(), QPoint(0, 0)), fader->size());
                        Check::report(fader->height() >= 80 && page->mixer()->rect().contains(r), "fader whole", __FILE__,
                                      __LINE__, QString("%1 %2: %3 px, bottom %4 of %5").arg(where, name).arg(fader->height())
                                                    .arg(r.bottom()).arg(page->mixer()->height()));
                    }
                if (!dumpDir.isEmpty()) w.grab().save(QString("%1/studio-%2-%3.png").arg(dumpDir, lang, large ? "large" : "normal"));
            }
        }
    I18n::install("de");
    QApplication::setFont(normal);

    // 1920 px, two tracks: the tools fill the room between the strips and the master (the gap before 0.23 Block 2)
    QSettings().remove("layout");
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1920, 1080);
        w.show();
        if (!CHECK(QTest::qWaitForWindowExposed(&w))) return;
        w.importFiles({audioFile}, true);
        settle();
        w.showPage(Page::Studio);
        settle(5);
        StudioPage* page = w.studioPage();
        if (!CHECK(page)) return;
        StudioTools* tools = page->tools();
        auto* strips = page->mixer()->findChild<QScrollArea*>();
        QWidget* master = page->findChild<QWidget*>("studio_fader_master");
        if (CHECK(tools && strips && master)) {
            const int stripsRight = strips->mapTo(page->mixer(), QPoint(strips->width(), 0)).x();
            const int toolsLeft = tools->mapTo(page->mixer(), QPoint(0, 0)).x();
            const int toolsRight = tools->mapTo(page->mixer(), QPoint(tools->width(), 0)).x();
            const int masterLeft = master->parentWidget()->mapTo(page->mixer(), QPoint(0, 0)).x();
            Check::report(std::abs(toolsLeft - stripsRight) <= 2 && std::abs(masterLeft - toolsRight) <= 2, "no gap",
                          __FILE__, __LINE__, QString("strips end %1, tools %2-%3, master %4").arg(stripsRight)
                                                  .arg(toolsLeft).arg(toolsRight).arg(masterLeft));
            Check::report(tools->columns() >= 2, "cards side by side", __FILE__, __LINE__, QString::number(tools->columns()));
            // Every strip whole (the scroll area once took its own small size hint: one strip, the tools the rest)
            Check::report(strips->horizontalScrollBar()->maximum() == 0, "all strips shown", __FILE__, __LINE__,
                          QString("%1 px for %2 strips, scrolls %3 px").arg(strips->width()).arg(page->mixer()->stripCount())
                              .arg(strips->horizontalScrollBar()->maximum()));
            const QString cut = cutOff(page);
            Check::report(cut.isEmpty(), "nothing cut off", __FILE__, __LINE__, "1920: " + cut);
        }
        if (!dumpDir.isEmpty()) w.grab().save(QString("%1/studio-1920.png").arg(dumpDir));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qputenv("SCHNEIDI_AUDIO_INPUT", "sine:440"); // the take of the recording tool: a synthetic input
    QApplication app(argc, argv);
    Check::initApp("studio-page");
    Theme::apply(app);
    std::setlocale(LC_NUMERIC, "C");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir dir;
    const QString audio = Check::makeMedia(dir.path() + "/voice.wav",
                                           {"-f", "lavfi", "-i", "sine=frequency=220:duration=4", "-ac", "2"});
    if (!CHECK(!audio.isEmpty())) return Check::result();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    const QString music = Check::makeMedia(dir.path() + "/music.wav",
                                           {"-f", "lavfi", "-i", "sine=frequency=440:duration=8", "-ac", "2"});
    if (!CHECK(!music.isEmpty())) return Check::result();
    testWindow(engine, audio);
    testTools(engine, audio, music);
    testLayout(engine, audio);
    return Check::result();
}
