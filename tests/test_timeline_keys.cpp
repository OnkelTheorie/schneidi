// Keys of the timeline in the real window: every action id once, no key used twice within a scope in any built-in
// set (the multicam angle keys share Alt+1-8 with the video target tracks on purpose: only one of them is on at a
// time), the Slip/Slide tools (menu, toolbar, a slip in progress shows four-up in the viewer), the track lock
// action, the transcript's cut keys next to the timeline's Delete, and Delete & co. in the Media Pool, Media Storage
// and Effects never removing timeline clips.
// SLIPSLIDE_SHOT=<dir> saves screenshots (toolbar, slip in progress).
#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/TrimFrames.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"
#include "ui/RenderQueuePanel.h"
#include "ui/TranscriptPanel.h"
#include "ui/TrimView.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QListWidget>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoStack>
#include <clocale>
#include <cmath>
#include <functional>

namespace {

void mouse(QWidget& w, QEvent::Type type, QPoint pt, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pt, w.mapToGlobal(pt), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QApplication::sendEvent(&w, &e);
}

bool waitUntil(const std::function<bool()>& ok, int ms = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(20);
    return ok();
}

QAction* action(const QString& id)
{
    return InputBindings::instance().action(id);
}

// Every action id once; per built-in set no input used by two actions of the same scope (allowed: the multicam
// angle switch and the video target track of the same number, see MainWindow::setMulticamMode)
void testBindings()
{
    InputBindings& b = InputBindings::instance();
    QSet<QString> ids;
    for (const auto& e : b.entries()) {
        CHECK(!ids.contains(e.id));
        ids.insert(e.id);
    }
    for (const char* id : {"tool_slip", "tool_slide", "lock_tracks", "transcript_cut"}) CHECK(ids.contains(id));
    for (const auto& e : b.entries()) {
        if (e.id == "tool_slip" || e.id == "tool_slide") {
            CHECK_EQ(e.category, T("Timeline"));
            CHECK(e.defaultKey.isEmpty() && e.scope.isEmpty());
        }
        if (e.id == "lock_tracks") CHECK(e.defaultKey.isEmpty() && e.scope.isEmpty());
        if (e.id == "transcript_cut") {
            CHECK_EQ(e.category, T("Transkript"));
            CHECK_EQ(e.scope, QString("transcript"));
        }
    }
    // Why the slip/slide tools have no keys: Premiere's Y and U are taken
    CHECK_EQ(b.defaultInputs("select_forward").value(0), QString("Y"));
    CHECK_EQ(b.defaultInputs("edit_point_type").value(0), QString("U"));
    CHECK_EQ(b.defaultInputs("transcript_cut"), QStringList({"Del", "Backspace"}));
    CHECK_EQ(b.defaultInputs("tool_slip"), QStringList({"", ""}));

    static const QRegularExpression pair("^(multicam_switch|target_video)_(\\d)$");
    for (const QString& preset : InputBindings::presetIds()) {
        b.applyPreset(preset);
        QHash<QString, QStringList> uses; // scope + input -> actions
        for (const auto& e : b.entries())
            for (const QString& in : b.inputs(e.id))
                if (!in.isEmpty()) uses[e.scope + '\n' + in] << e.id;
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            if (it->size() < 2) continue;
            bool allowed = it->size() == 2;
            QSet<QString> numbers;
            for (const QString& id : *it) {
                const auto m = pair.match(id);
                allowed &= m.hasMatch();
                numbers.insert(m.captured(2));
            }
            Check::report(allowed && numbers.size() == 1, "key used twice", __FILE__, __LINE__,
                          QString("%1: %2 (%3)").arg(QString(it.key()).replace('\n', ' '), it->join(", "), preset));
        }
        // The transcript's keys do not clash with the timeline's Delete (other scope) and keep them in every set
        CHECK_EQ(b.inputs("transcript_cut"), QStringList({"Del", "Backspace"}));
        CHECK_EQ(b.inputs("tool_slip"), QStringList({"", ""}));
    }
    b.applyPreset("standard");
}

void testWindow(const QString& media, const QString& shots)
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    testBindings();

    auto* tv = w.findChild<TimelineView*>();
    auto* panel = w.findChild<TimelinePanel*>();
    auto* viewer = w.findChild<Viewer*>();
    auto* ed = w.findChild<Editor*>();
    auto* p = w.findChild<Project*>();
    auto* sel = w.findChild<Selection*>();
    if (!CHECK(tv && panel && viewer && ed && p && sel)) return;

    // Tools from the menu (keys are bindable), the toolbar follows
    action("tool_slip")->trigger();
    CHECK(tv->tool() == TimelineView::Tool::Slip);
    CHECK(panel->findChild<QToolButton*>("tool_slip")->isChecked());
    action("tool_slide")->trigger();
    CHECK(tv->tool() == TimelineView::Tool::Slide);
    CHECK(panel->findChild<QToolButton*>("tool_slide")->isChecked());
    action("tool_select")->trigger();
    CHECK(tv->tool() == TimelineView::Tool::Select);

    // Track lock without a selection: the target tracks V1 + A1, again = unlocked, one undo step each
    const int steps = p->undoStack()->count();
    action("lock_tracks")->trigger();
    CHECK(p->timeline().video[0].locked && p->timeline().audio[0].locked && !p->timeline().video[1].locked);
    action("lock_tracks")->trigger();
    CHECK(!p->timeline().video[0].locked && !p->timeline().audio[0].locked);
    CHECK_EQ(p->undoStack()->count(), steps + 2);

    if (media.isEmpty()) return;
    // V1/A1: three pieces of the clip with spare material on both sides (frames 25-74, 100-149, 175-224 of 250)
    w.importFiles({media}, false);
    QApplication::processEvents();
    ed->placeSourceRange(media, 25, 74, 0, 0);
    ed->placeSourceRange(media, 100, 149, 50, 0);
    ed->placeSourceRange(media, 175, 224, 100, 0);
    if (!CHECK_EQ(p->timeline().video[0].clips.size(), 3)) return;
    tv->setSnapping(false);
    tv->zoomToFit();
    QApplication::processEvents();

    // Slip tool on the middle clip: the viewer shows four-up while dragging, one undo step on release
    action("tool_slip")->trigger();
    const double ppf = tv->view().pxPerFrame;
    auto x = [&](double frame) { return int(std::lround(TimelineView::kHeaderW + (frame - tv->view().leftFrame) * ppf)); };
    QVector<TrimFrames::View> views;
    QObject::connect(tv, &TimelineView::trimFramesChanged, [&](const TrimFrames::View& v) { views << v; });
    QPoint grab;
    for (int y = TimelineView::kRulerH + 4; y < tv->height() && views.isEmpty(); y += 6) { // find the V1 row
        grab = QPoint(x(75), y);
        mouse(*tv, QEvent::MouseButtonPress, grab, Qt::LeftButton);
        if (views.isEmpty()) mouse(*tv, QEvent::MouseButtonRelease, grab, Qt::NoButton);
    }
    if (!CHECK(!views.isEmpty()) || !CHECK(views.last().kind == TimelineOps::TrimKind::Slip)) {
        mouse(*tv, QEvent::MouseButtonRelease, grab, Qt::NoButton);
        return;
    }
    const int middle = p->timeline().video[0].clips[1].id;
    const QPoint to = grab + QPoint(int(std::lround(12 * ppf)), 0);
    mouse(*tv, QEvent::MouseMove, to, Qt::LeftButton);
    CHECK(viewer->trimShown());
    const TrimFrames::View v = viewer->trimView()->view();
    CHECK(v.kind == TimelineOps::TrimKind::Slip && v.main.size() == 2 && v.small.size() == 2);
    CHECK(waitUntil([&] {
        return viewer->trimView()->isExact(false, 0) && viewer->trimView()->isExact(false, 1)
               && viewer->trimView()->isExact(true, 0) && viewer->trimView()->isExact(true, 1);
    }));
    if (!shots.isEmpty()) {
        w.grab().save(QDir(shots).filePath("slip-in-progress.png"));
        panel->grab(QRect(0, 0, std::min(panel->width(), 700), 40)).save(QDir(shots).filePath("toolbar.png"));
    }
    const int undo0 = p->undoStack()->count();
    mouse(*tv, QEvent::MouseButtonRelease, to, Qt::NoButton);
    CHECK(!viewer->trimShown());
    CHECK_EQ(p->undoStack()->count(), undo0 + 1);
    const Clip* c = TimelineOps::findClip(p->timeline(), middle);
    CHECK(c && c->start == 50 && c->length() == 50 && c->in != 100);
    action("tool_select")->trigger();

    // Panels with items of their own: the keys that remove timeline clips (Delete, Backspace, ripple delete, cut) do
    // nothing on a Media Pool clip, an effect or a Media Storage file, whatever is selected in the timeline; Delete in
    // the bin list still removes the bin, in the timeline Delete deletes
    action("toggle_effects")->trigger();
    action("toggle_media_storage")->trigger();
    QApplication::processEvents();
    const auto timelineIds = [&] {
        QSet<int> ids;
        for (const Track& t : p->timeline().video + p->timeline().audio)
            for (const Clip& cl : t.clips) ids.insert(cl.id);
        return ids;
    };
    const QSet<int> placed = timelineIds();
    const int undoBefore = p->undoStack()->count();
    const QVector<QPair<int, Qt::KeyboardModifiers>> removing = {{Qt::Key_Delete, Qt::NoModifier},
                                                                 {Qt::Key_Backspace, Qt::NoModifier},
                                                                 {Qt::Key_Delete, Qt::ShiftModifier},
                                                                 {Qt::Key_X, Qt::ControlModifier}};
    auto* pool = w.findChild<MediaPool*>();
    auto* storage = w.findChild<MediaStorage*>();
    auto* effects = w.findChild<EffectsLibrary*>();
    if (!CHECK(pool && storage && effects)) return;
    for (QWidget* panelWidget : std::initializer_list<QWidget*>{pool, storage, effects}) {
        auto* list = panelWidget->findChild<QListWidget*>();
        if (!CHECK(list && list->isVisible())) continue;
        list->setFocus();
        for (const auto& [key, mods] : removing) {
            sel->set(placed);
            QTest::keyClick(list, Qt::Key(key), mods);
            QApplication::processEvents();
        }
        Check::report(timelineIds() == placed, "panel key removed timeline clips", __FILE__, __LINE__,
                      QString::fromLatin1(panelWidget->metaObject()->className()));
    }
    CHECK_EQ(p->undoStack()->count(), undoBefore);
    const int bin = p->addBin(0);
    pool->setCurrentBin(0);
    auto* bins = pool->findChild<QTreeWidget*>();
    if (CHECK(bins)) {
        for (QTreeWidgetItem* it : bins->findItems(p->binName(bin), Qt::MatchExactly | Qt::MatchRecursive))
            bins->setCurrentItem(it);
        bins->setFocus();
        sel->set(placed);
        QTest::keyClick(bins, Qt::Key_Delete);
        CHECK(!p->bin(bin));
        CHECK(timelineIds() == placed);
    }
    tv->setFocus();
    sel->set(placed);
    QTest::keyClick(tv, Qt::Key_Delete);
    CHECK(timelineIds().isEmpty());
    p->undoStack()->undo();
    CHECK(timelineIds() == placed);
    action("toggle_effects")->trigger();
    action("toggle_media_storage")->trigger();

    // Transcript next to the timeline: with words selected Delete cuts them (the selected clip stays), without
    // words Delete and the other keys that remove timeline clips do nothing (a click into the text clears the words)
    MediaTranscript t;
    t.words = {{1100, 1500, "eins"}, {1500, 2000, "zwei"}, {4200, 4800, "drei"}, {7200, 7800, "vier"}};
    p->setTranscript(media, t);
    action("toggle_transcript")->trigger();
    QApplication::processEvents();
    auto* transcript = w.findChild<TranscriptPanel*>();
    if (!CHECK(transcript && transcript->isVisible())) return;
    auto* text = transcript->findChild<QTextEdit*>();
    // The words in the timeline clips: "drei" may be slipped out of the middle clip above
    const int words = text ? int(transcript->words().size()) : 0;
    if (!CHECK(words >= 2)) return;
    const int last = p->timeline().video[0].clips.last().id;
    sel->set({last});
    text->setFocus();
    transcript->selectWords(0, 0);
    QTest::keyClick(text, Qt::Key_Delete);
    QApplication::processEvents();
    CHECK_EQ(p->undoStack()->undoText(), T("Wörter herausschneiden"));
    CHECK(TimelineOps::findClip(p->timeline(), last));
    CHECK_EQ(int(transcript->words().size()), words - 1);
    const int cutSteps = p->undoStack()->index();
    for (const auto& [key, mods] : removing) { // no words selected
        sel->set({last});
        QTest::keyClick(text, Qt::Key(key), mods);
        QApplication::processEvents();
    }
    CHECK_EQ(p->undoStack()->index(), cutSteps);
    CHECK(TimelineOps::findClip(p->timeline(), last));
    CHECK_EQ(int(transcript->words().size()), words - 1);

    // Deliver page (the timeline is there too): on a render job the timeline's ripple delete and cut stay quiet,
    // Delete removes the job
    action("page_deliver")->trigger();
    RenderJob job;
    job.id = 1;
    job.path = QDir::temp().filePath("schneidi-keys-test.mp4");
    p->setRenderQueue({job});
    QApplication::processEvents();
    auto* jobs = w.findChild<RenderQueuePanel*>() ? w.findChild<RenderQueuePanel*>()->findChild<QListWidget*>() : nullptr;
    if (CHECK(jobs && jobs->isVisible() && jobs->count() == 1)) {
        jobs->setFocus();
        jobs->item(0)->setSelected(true);
        for (const auto& [key, mods] : removing) {
            if (key == Qt::Key_Delete && mods == Qt::NoModifier) continue; // the job's own key, below
            if (key == Qt::Key_Backspace) continue;
            sel->set({last});
            QTest::keyClick(jobs, Qt::Key(key), mods);
            QApplication::processEvents();
        }
        CHECK(TimelineOps::findClip(p->timeline(), last));
        sel->set({last});
        QTest::keyClick(jobs, Qt::Key_Delete);
        QApplication::processEvents();
        CHECK(p->renderQueue().isEmpty());
        CHECK(TimelineOps::findClip(p->timeline(), last));
    }
    CHECK_EQ(p->undoStack()->index(), cutSteps);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("timeline_keys");
    I18n::install("de");
    Theme::apply(app);
    InputBindings::instance().load();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initMlt();
    QString media;
    if (Check::haveFfmpeg())
        media = Check::makeMedia(QDir(tmp.path()).filePath("clip.mp4"),
                                 {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=10", "-f", "lavfi", "-i",
                                  "sine=frequency=440:duration=10", "-c:v", "libx264", "-preset", "ultrafast",
                                  "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    else std::printf("Hinweis: ffmpeg fehlt, Clip-Teil übersprungen\n");
    testWindow(media, qEnvironmentVariable("SLIPSLIDE_SHOT"));
    return Check::result();
}
