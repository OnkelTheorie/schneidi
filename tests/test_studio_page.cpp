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
//   button, box at least its minimum width and inside its parent) and the window does not grow.
// STUDIO_PAGE_DUMP=<folder>: a picture of the window per layout case.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/Viewer.h"
#include "ui/studio/StudioMixer.h"
#include "ui/studio/StudioPage.h"
#include "ui/studio/StudioTracks.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
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
    project->undoStack()->undo();
    settle();

    // Pen: a stroke over the clip's lane at -20 dB, one undo step, keys at that level
    auto* pen = page->findChild<QToolButton*>("studio_tool_pen");
    if (!CHECK(pen)) return;
    pen->click();
    CHECK(tracks->tool() == StudioTracks::Tool::Pen);
    const Clip* clip = TimelineOps::findClip(project->timeline(), clipId);
    if (!CHECK(clip)) return;
    tracks->fit(); // the user's Fit button: the whole clip in view
    settle();
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
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
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
    testWindow(engine, audio);
    testLayout(engine, audio);
    return Check::result();
}
