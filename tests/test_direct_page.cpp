// SPDX-License-Identifier: AGPL-3.0-only
// The Direct page "Regie" (0.25), with the fake backend (app/DirectBackend.h) until the `direct` command exists:
// - built lazily: a new window has none of its widgets, the first showing builds it; its button sits right of Studio,
//   the key (Shift+1, page_direct) and `app page` show it, it hides like the other pages;
// - the viewer moves into its middle column; material comes in by a Media Pool drop (its list MIME), by file URLs and
//   as "the current timeline"; a recipe card's title asks for suggestions (progress in BackgroundJobs, ✕ cancels);
// - suggestions on/off (each, all), the sum follows; the order changes by moving rows; Delete on a suggestion or a
//   material clip takes it out of that list and never deletes the timeline's selected clips;
// - a click on a suggestion previews it in the viewer, stops at its end and leaves the project unmodified (no marks);
// - Übernehmen = exactly one undo step (a new timeline with the checked ranges); undo takes it back;
// - the start page setting (pages/start): the app starts on it, New Project goes to it, opening a project does not;
//   a hidden start page gives the nearest shown one; the default is Edit;
// - layout: German and Russian, normal and 1.5x font, with a real user's settings (tests/data/tours/user-settings.conf):
//   no button or label of the page narrower than it needs, nothing outside the page, the window does not grow.
// - the own wish line (R5): hidden without a chat provider, shown with one; its context note for the chat (material,
//   recipe, session, suggestions) stays within 2000 characters with many suggestions and counts what it left out;
//   Enter on an empty line asks nothing; Delete in the line edits its text and takes no suggestion out; a wish the
//   chat cannot send ends with its error under the line.
// DIRECT_PAGE_DUMP=<folder>: a picture of the window per layout case.

#include "check.h"

#include "app/AiChat.h"
#include "app/AiSettings.h"
#include "app/DirectBackend.h"
#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/PageVisibility.h"
#include "app/Theme.h"
#include "cli/Live.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/BackgroundJobs.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/direct/DirectMaterial.h"
#include "ui/direct/DirectPage.h"
#include "ui/direct/DirectRecipes.h"
#include "ui/direct/DirectSuggestions.h"
#include "ui/direct/DirectWish.h"
#include "ui/timeline/TimelineOverview.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTranslator>
#include <QUndoStack>
#include <QUrl>
#include <clocale>
#include <functional>

using Page = MainWindow::Page;

namespace {

bool waitFor(const std::function<bool()>& ok, int ms = 8000)
{
    QElapsedTimer t;
    t.start();
    while (!ok() && t.elapsed() < ms) QTest::qWait(20);
    return ok();
}

QAction* action(const QString& id)
{
    for (const InputBindings::Entry& e : InputBindings::instance().entries())
        if (e.id == id) return e.action;
    return nullptr;
}

bool viewerSays(Viewer* v, const QString& text)
{
    for (QLabel* l : v->findChildren<QLabel*>())
        if (l->text() == text) return true;
    return false;
}

// The center of a row's check indicator
QPoint checkIndicator(QListWidget* list, QListWidgetItem* item)
{
    QStyleOptionViewItem opt;
    opt.initFrom(list->viewport());
    opt.rect = list->visualItemRect(item);
    opt.features = QStyleOptionViewItem::HasCheckIndicator | QStyleOptionViewItem::HasDisplay
                   | QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::WrapText;
    opt.decorationSize = list->iconSize();
    opt.checkState = item->checkState();
    return list->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &opt, list).center();
}

void settle()
{
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

void useRunner(MainWindow& w)
{
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
}

void drop(QWidget* target, QMimeData* mime)
{
    const QPoint at = target->rect().center();
    QDragEnterEvent enter(at, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDropEvent e(QPointF(at), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &e);
}

// A project with one video clip of `file` (frames 0-99) on V1/A1
void loadProject(MainWindow& w, const QString& file)
{
    ProjectData d;
    d.format.rate = {25, 1};
    TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
    TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
    Clip v;
    v.id = 1;
    v.mediaPath = file;
    v.start = 0;
    v.in = 0;
    v.out = 99;
    v.linkId = 1;
    Clip a = v;
    a.id = 2;
    d.timeline.video[0].clips.push_back(v);
    d.timeline.audio[0].clips.push_back(a);
    w.liveProject()->load(d);
    w.liveProject()->undoStack()->clear();
}

int timelineClips(MainWindow& w)
{
    int n = 0;
    for (const Track& t : w.liveProject()->timeline().video) n += int(t.clips.size());
    for (const Track& t : w.liveProject()->timeline().audio) n += int(t.clips.size());
    return n;
}

void testWindow(Engine& engine, const QString& a, const QString& b)
{
    QSettings().remove(PageVisibility::kSettingsKey);
    QSettings().remove(PageVisibility::kStartKey);
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    useRunner(w);
    if (!CHECK(QTest::qWaitForWindowExposed(&w))) return;
    loadProject(w, a);
    Project* p = w.liveProject();

    // Lazy, the button right of Studio, the default start page is Edit; the page's keys are registered already
    CHECK(!w.directPage());
    CHECK(action("direct_remove_suggestion") && action("direct_remove_material"));
    CHECK(!w.findChild<DirectPage*>());
    CHECK(w.currentPage() == Page::Edit);
    auto* studio = w.findChild<QToolButton*>("pageButton_studio");
    auto* direct = w.findChild<QToolButton*>("pageButton_direct");
    auto* deliver = w.findChild<QToolButton*>("pageButton_deliver");
    if (!CHECK(studio && direct && deliver)) return;
    CHECK(studio->x() < direct->x() && direct->x() < deliver->x());
    CHECK(!direct->icon().isNull()); // the drawn clapperboard
    // The key shows it
    QAction* key = action("page_direct");
    if (CHECK(key)) {
        CHECK_EQ(key->shortcut(), QKeySequence("Shift+1"));
        key->trigger();
    }
    CHECK(w.currentPage() == Page::Direct);
    DirectPage* page = w.directPage();
    if (!CHECK(page)) return;
    CHECK(page->isVisible());
    auto* viewer = w.findChild<Viewer*>();
    CHECK(viewer && page->center()->indexOf(viewer) == 0);
    CHECK(!page->wishSlot()->isVisible()); // R5: no own wish line yet
    CHECK_EQ(w.livePage(), QString("direct"));
    CHECK(viewerSays(viewer, "Vorschau")); // not "Timeline": the viewer shows previews here
    settle();
    // Away and back: the same page, the viewer back in the middle with the height the user gave it
    page->center()->setSizes({300, 500});
    settle();
    const QList<int> centerSizes = page->center()->sizes();
    w.showPage(Page::Edit);
    CHECK(page->center()->indexOf(viewer) < 0);
    CHECK(viewerSays(viewer, "Timeline"));
    CHECK(w.liveShowPage("direct"));
    CHECK(w.directPage() == page && page->center()->indexOf(viewer) == 0);
    settle();
    Check::report(std::abs(page->center()->sizes().value(0) - centerSizes.value(0)) <= 2, "viewer height kept", __FILE__,
                  __LINE__, QString("%1 -> %2").arg(centerSizes.value(0)).arg(page->center()->sizes().value(0)));
    settle();

    // Material: a Media Pool drop, file URLs (a folder and a missing file left out), no doubles
    DirectMaterial* mat = page->material();
    CHECK(mat->isEmpty());
    CHECK(!page->recipes()->suggestButton()->isEnabled()); // nothing to work on
    {
        auto* mime = new QMimeData;
        mime->setData(MediaPool::ListMimeType, a.toUtf8());
        drop(mat, mime);
        delete mime;
    }
    CHECK_EQ(mat->files(), QStringList{a});
    {
        auto* mime = new QMimeData;
        mime->setUrls({QUrl::fromLocalFile(b), QUrl::fromLocalFile(a), QUrl::fromLocalFile(QFileInfo(b).absolutePath()),
                       QUrl::fromLocalFile(b + ".missing")});
        drop(mat, mime);
        delete mime;
    }
    CHECK_EQ(mat->files(), QStringList({a, b}));
    CHECK(page->recipes()->suggestButton()->isEnabled());
    // The current timeline as the material, and back
    mat->setUseTimeline(true);
    CHECK(mat->materialJson().contains("timeline"));
    mat->setUseTimeline(false);
    CHECK_EQ(mat->materialJson().value("files").toArray().size(), 2);

    // A recipe card's title asks for suggestions: progress at the bottom right, then the rows
    DirectSuggestions* sug = page->suggestions();
    auto* jobs = w.findChild<BackgroundJobs*>();
    auto* shortTitle = page->findChild<QToolButton*>("direct_recipe_short");
    if (!CHECK(shortTitle && jobs)) return;
    shortTitle->click();
    CHECK(page->busy());
    CHECK(jobs->hasJob("direct"));
    CHECK(waitFor([&] { return !page->busy(); }));
    if (!CHECK(sug->hasSession())) return;
    CHECK_EQ(sug->list()->count(), 4); // the fake: two per file
    CHECK_EQ(sug->checkedOrder().size(), 2);
    CHECK(!sug->sumText().isEmpty());
    CHECK(sug->sumLabel()->text().contains(QString::number(45))); // "… von 45 s"
    CHECK(page->applyButton()->isEnabled());
    CHECK(!page->overview()->timeline().video.isEmpty() && page->overview()->timeline().video[0].clips.size() == 2);
    CHECK(!p->isModified()); // suggestions change nothing

    // Cancel a run through its job's ✕
    page->recipes()->suggestButton()->click();
    CHECK(page->busy());
    if (auto* cancel = jobs->findChild<QToolButton*>(); CHECK(cancel)) {
        CHECK(waitFor([&] { return cancel->isVisible(); }, 2000));
        cancel->click();
    }
    CHECK(!page->busy());
    CHECK(!sug->hasSession() || sug->list()->count() == 4);
    // Again for the rest of the test
    page->recipes()->suggestButton()->click();
    CHECK(waitFor([&] { return !page->busy() && sug->list()->count() == 4; }));

    // On/off: all off = nothing to apply, all on, one by one
    sug->setAllChecked(false);
    CHECK(sug->checkedOrder().isEmpty());
    CHECK(!page->applyButton()->isEnabled());
    CHECK(page->overview()->timeline().video[0].clips.isEmpty());
    sug->setAllChecked(true);
    CHECK_EQ(sug->checkedOrder().size(), 4);
    CHECK(page->overview()->timeline().video[0].clips.size() == 4);
    const double all = sug->checkedSeconds();
    sug->list()->item(3)->setCheckState(Qt::Unchecked);
    CHECK(sug->checkedSeconds() < all);
    CHECK_EQ(sug->checkedOrder().size(), 3);

    // Order: the last row to the top (a drag inside the list moves the model's rows)
    const QList<int> before = sug->rowOrder();
    CHECK(sug->list()->model()->moveRow({}, 3, {}, 0));
    const QList<int> after = sug->rowOrder();
    CHECK_EQ(after.first(), before.last());
    // The choice's order: the checked rows as listed (the moved one is off: it is not in it)
    CHECK_EQ(sug->choice().value("order").toArray().first().toInt(), sug->checkedOrder().first());
    CHECK(!sug->choice().value("order").toArray().contains(before.last()));
    sug->setChecked(before.last(), true);
    CHECK_EQ(sug->choice().value("order").toArray().first().toInt(), before.last());

    // Delete on a suggestion: the row goes, the timeline's selected clips stay
    w.liveSelection()->set({1, 2});
    const int clips = timelineClips(w);
    const int undoBefore = p->undoStack()->count();
    sug->list()->setFocus();
    sug->list()->setCurrentRow(0);
    sug->list()->item(0)->setSelected(true);
    QTest::keyClick(sug->list(), Qt::Key_Delete);
    settle();
    CHECK_EQ(sug->list()->count(), 3);
    CHECK_EQ(timelineClips(w), clips);
    CHECK_EQ(p->undoStack()->count(), undoBefore);
    QTest::keyClick(sug->list(), Qt::Key_Backspace);
    settle();
    CHECK_EQ(sug->list()->count(), 2);
    CHECK_EQ(timelineClips(w), clips);
    // Delete in the material list: out of the material, not the timeline's clips
    mat->list()->setFocus();
    mat->list()->setCurrentRow(1);
    QTest::keyClick(mat->list(), Qt::Key_Delete);
    settle();
    CHECK_EQ(mat->files(), QStringList{a});
    CHECK_EQ(timelineClips(w), clips);
    CHECK_EQ(p->undoStack()->count(), undoBefore);
    // The material changed: the old suggestions are gone, a new run brings new ones
    CHECK(!sug->hasSession());
    page->recipes()->suggestButton()->click();
    CHECK(waitFor([&] { return !page->busy() && sug->list()->count() == 2; }));
    sug->setAllChecked(true);

    // A click on a suggestion: the file in the viewer from the row's start, stops at its end, the project unmodified
    CHECK(!p->isModified());
    {
        QListWidgetItem* item = sug->list()->item(0);
        const QJsonObject s = sug->suggestion(item->data(Qt::UserRole).toInt());
        const QRect r = sug->list()->visualItemRect(item);
        QTest::mouseClick(sug->list()->viewport(), Qt::LeftButton, {}, r.center() + QPoint(r.width() / 4, 0));
        CHECK(waitFor([&] { return viewer->showsSource() || !engine.timelinePositions(); }, 3000));
        CHECK(engine.speed() != 0.0); // playing from the row's start
        const int out = int(std::lround(s.value("to_s").toDouble() * 25));
        CHECK(waitFor([&] { return engine.speed() == 0.0; }, 10000));
        Check::report(std::abs(engine.position() - out) <= 3,
                      "preview stops at the row's end", __FILE__, __LINE__,
                      QString("at %1, end %2").arg(engine.position()).arg(out));
        CHECK(!p->isModified());
        CHECK_EQ(p->undoStack()->count(), undoBefore);
        CHECK(p->mediaInfo(b) == nullptr); // not imported by a preview
    }
    // A click on a row's checkbox toggles it and previews nothing
    {
        engine.pause();
        QListWidgetItem* item = sug->list()->item(1);
        const Qt::CheckState before = item->checkState();
        const int at = engine.position();
        QTest::mouseClick(sug->list()->viewport(), Qt::LeftButton, {}, checkIndicator(sug->list(), item));
        settle();
        CHECK(item->checkState() != before);
        CHECK(engine.speed() == 0.0);
        CHECK_EQ(engine.position(), at);
        item->setCheckState(Qt::Checked);
    }

    // In/Out of the current row: the sum, the strip and the choice follow
    {
        sug->list()->setCurrentRow(0);
        const int n = sug->list()->item(0)->data(Qt::UserRole).toInt();
        const double from = sug->suggestion(n).value("from_s").toDouble();
        const double sumBefore = sug->checkedSeconds();
        sug->inBox()->setValue(from + 0.4);
        CHECK(std::abs(sug->range(n).first - (from + 0.4)) < 0.01);
        CHECK(std::abs(sug->checkedSeconds() - (sumBefore - 0.4)) < 0.01);
        const QJsonArray adjust = sug->choice().value("adjust").toArray();
        CHECK_EQ(adjust.size(), 1);
        CHECK_EQ(adjust.first().toObject().value("n").toInt(), n);
        CHECK_EQ(page->overview()->timeline().video[0].clips.value(0).in, int(std::lround((from + 0.4) * 25)));
        sug->setRange(n, -5, 1e6); // clamped to the file (6 s)
        CHECK(sug->range(n).first == 0.0 && std::abs(sug->range(n).second - 6.0) < 0.05);
        sug->setRange(n, from + 0.4, sug->suggestion(n).value("to_s").toDouble());
    }

    // Übernehmen: exactly one undo step, a new timeline with the checked ranges
    const int sequences = int(p->sequences().size());
    CHECK(page->applyButton()->toolTip().contains("neue Timeline"));
    page->applyButton()->click();
    CHECK(page->applying());
    CHECK(!page->applyButton()->isEnabled()); // once at a time
    CHECK(jobs->hasJob("direct_apply"));
    CHECK(waitFor([&] { return !page->applying(); }));
    CHECK_EQ(p->undoStack()->count(), undoBefore + 1);
    CHECK_EQ(int(p->sequences().size()), sequences + 1);
    CHECK_EQ(int(p->timeline().video.value(0).clips.size()), 2);
    {
        const int n = sug->list()->item(0)->data(Qt::UserRole).toInt();
        CHECK(std::abs(p->timeline().video.value(0).clips.value(0).in - sug->range(n).first * 25) <= 1); // the adjusted In
    }
    p->undoStack()->undo();
    CHECK_EQ(int(p->sequences().size()), sequences);
    CHECK_EQ(timelineClips(w), clips);

    // The timeline's editing keys do nothing on Regie: focus outside the lists, and no focus at all
    {
        w.liveSelection()->set({1, 2});
        const int undo = p->undoStack()->count();
        page->applyButton()->setFocus();
        const auto target = [&w]() -> QWidget* { return QApplication::focusWidget() ? QApplication::focusWidget() : &w; };
        for (const auto key : {Qt::Key_Delete, Qt::Key_Backspace}) QTest::keyClick(target(), key);
        QTest::keyClick(target(), Qt::Key_V, Qt::ControlModifier);
        QTest::keyClick(target(), Qt::Key_B, Qt::ControlModifier);
        if (QWidget* f = QApplication::focusWidget()) f->clearFocus();
        QTest::keyClick(&w, Qt::Key_Delete);
        action("copy")->trigger();
        for (const char* id : {"delete", "delete_alt", "ripple_delete", "cut", "paste", "split", "split_alt", "insert_clip",
                               "overwrite_clip", "nudge_right", "trim_start"})
            if (QAction* act = action(id); CHECK(act)) act->trigger();
        settle();
        CHECK_EQ(timelineClips(w), clips);
        CHECK_EQ(p->undoStack()->count(), undo);
    }

    // The material changed after the suggestions: Übernehmen says so and offers new suggestions
    {
        QFile f(a);
        CHECK(f.open(QIODevice::ReadWrite));
        CHECK(f.setFileTime(QDateTime::currentDateTime().addSecs(5), QFileDevice::FileModificationTime));
        f.close();
        const int undo = p->undoStack()->count();
        page->applyButton()->click();
        CHECK(waitFor([&] { return !page->applying(); }));
        CHECK_EQ(p->undoStack()->count(), undo);
        auto* again = sug->findChild<QPushButton*>("direct_resuggest");
        if (CHECK(again)) {
            CHECK(again->isVisible());
            again->click();
            CHECK(waitFor([&] { return !page->busy() && sug->hasSession(); }));
            CHECK(!again->isVisible());
        }
    }

    // Variants (short): a picker, only the chosen variant counts (rows, sum, strip, choice)
    if (auto* variants = page->findChild<QSpinBox*>("direct_short_variants"); CHECK(variants)) {
        variants->setValue(2);
        page->recipes()->suggestButton()->click();
        CHECK(waitFor([&] { return !page->busy() && sug->hasSession(); }));
        CHECK_EQ(sug->variantCount(), 2);
        CHECK(sug->variantBox()->isVisible());
        CHECK_EQ(sug->rowOrder().size(), 1); // one file, one row per variant
        const int one = sug->rowOrder().first();
        sug->setVariant(2);
        CHECK_EQ(sug->rowOrder().size(), 1);
        CHECK(sug->rowOrder().first() != one);
        CHECK_EQ(sug->checkedOrder(), sug->rowOrder()); // a variant chosen the first time: its rows on
        CHECK_EQ(sug->choice().value("variant").toInt(), 2);
        CHECK_EQ(sug->choice().value("on").toArray().size(), 1);
        CHECK_EQ(int(page->overview()->timeline().video[0].clips.size()), 1);
        variants->setValue(1);
    }

    // Audio-only material lies on the audio track of the result strip; the talk recipe's tooltip says it copies
    {
        const QJsonObject session{
            {"id", "x"}, {"recipe", "talk"},
            {"material", QJsonArray{QJsonObject{{"file", a}, {"length_s", 6}, {"has_video", false}}}},
            {"suggestions", QJsonArray{QJsonObject{{"n", 1}, {"kind", "moment"}, {"file", a}, {"from_s", 1.0},
                                                   {"to_s", 2.0}, {"reason", QJsonObject{{"key", "loud"}}}, {"on", true}}}},
            {"order", QJsonArray{1}}};
        sug->setSession(session);
        CHECK(page->overview()->timeline().video[0].clips.isEmpty());
        CHECK_EQ(int(page->overview()->timeline().audio[0].clips.size()), 1);
        CHECK(page->applyButton()->toolTip().contains("Kopie"));
        CHECK(page->applyButton()->toolTip().contains("bis man übernimmt"));
    }

    // A run never outlives its material: a change ends it, its late reply is dropped
    page->recipes()->suggestButton()->click();
    CHECK(page->busy());
    mat->addFiles({b});
    CHECK(!page->busy());
    CHECK(!jobs->hasJob("direct"));
    CHECK(!sug->hasSession());
    QTest::qWait(500);
    CHECK(!sug->hasSession());

    // In Schneiden öffnen
    page->openEditButton()->click();
    CHECK(w.currentPage() == Page::Edit);

    // Hideable: off the bar, Shift+1 still shows it while it is current
    w.setPageHidden(Page::Direct, true);
    CHECK(!direct->isVisible());
    w.showPage(Page::Direct);
    CHECK(direct->isVisible());
    w.showAllPages();
    CHECK(w.currentPage() == Page::Direct);
    w.setPageHidden(Page::Direct, true); // the current one goes: the nearest shown page (the right one on a tie)
    CHECK(w.currentPage() == Page::Deliver);
    w.showAllPages();
    CHECK(direct->isVisible());
    CHECK(!QSettings().contains(PageVisibility::kSettingsKey)); // a test run: never saved

    // ... nor its project: New Project ends it and empties the page
    w.showPage(Page::Direct);
    page->recipes()->suggestButton()->click();
    CHECK(page->busy());
    action("project_new")->trigger();
    CHECK(!page->busy());
    CHECK(!jobs->hasJob("direct"));
    CHECK(mat->isEmpty());
    QTest::qWait(500);
    CHECK(!sug->hasSession());
}

// pages/start: the app starts there, New Project goes there, opening a project stays
void testStartPage(Engine& engine, const QString& a, const QString& projectDir)
{
    QSettings().remove(PageVisibility::kSettingsKey);
    QSettings().setValue(PageVisibility::kStartKey, "direct");
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1400, 900);
        w.show();
        CHECK(w.currentPage() == Page::Direct);
        CHECK(w.directPage() != nullptr);
        CHECK_EQ(w.startPage(), QString("direct"));
        // Opening a project keeps the page the user is on
        w.showPage(Page::Color);
        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        const QString file = projectDir + "/start.schneidi";
        QString err;
        CHECK(ProjectFile::save(d, file, &err));
        CHECK(w.openProject(file));
        CHECK(w.currentPage() == Page::Color);
        // New Project goes to the start page
        action("project_new")->trigger();
        CHECK(w.currentPage() == Page::Direct);
        // The setting changed in the window (a test run: not saved)
        w.setStartPage("media");
        CHECK_EQ(w.startPage(), QString("media"));
        CHECK_EQ(QSettings().value(PageVisibility::kStartKey).toString(), QString("direct"));
        w.showPage(Page::Edit);
        action("project_new")->trigger();
        CHECK(w.currentPage() == Page::Media);
        w.setStartPage("nope"); // unknown: unchanged
        CHECK_EQ(w.startPage(), QString("media"));
    }
    // A hidden start page: the nearest shown one (Direct sits between Studio and Deliver)
    QSettings().setValue(PageVisibility::kSettingsKey, QStringList{"direct"});
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        CHECK(w.currentPage() == Page::Deliver || w.currentPage() == Page::Studio);
        CHECK(w.currentPage() == Page::Deliver); // the right one first on a tie
        CHECK(!w.directPage()); // not built for nothing
    }
    // Unknown saved id = Edit
    QSettings().remove(PageVisibility::kSettingsKey);
    QSettings().setValue(PageVisibility::kStartKey, "future_page");
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        CHECK(w.currentPage() == Page::Edit);
    }
    // PageVisibility itself: saved with the hidden pages, the default not stored
    {
        PageVisibility v({"media", "direct", "edit"});
        v.load();
        CHECK_EQ(v.start(), QString("edit"));
        CHECK(v.setStart("direct"));
        CHECK(!v.setStart("direct"));
        CHECK(!v.setStart("nope"));
        v.save();
        CHECK_EQ(QSettings().value(PageVisibility::kStartKey).toString(), QString("direct"));
        CHECK(v.setHidden("direct", true));
        CHECK_EQ(v.startShown(), QString("edit"));
        CHECK(v.setStart("edit"));
        v.save();
        CHECK(!QSettings().contains(PageVisibility::kStartKey));
    }
    QSettings().remove(PageVisibility::kSettingsKey);
    QSettings().remove(PageVisibility::kStartKey);
    Q_UNUSED(a);
}

// Nothing of the page cut off: every shown button, label and box at least as wide as it needs (or eliding), inside
// the page; the window keeps its size
QString layoutProblems(DirectPage* page)
{
    QStringList out;
    const QRect area = page->rect();
    for (QWidget* c : page->findChildren<QWidget*>()) {
        if (!c->isVisibleTo(page) || !c->isVisible()) continue;
        if (c->parentWidget() && qobject_cast<QAbstractScrollArea*>(c->parentWidget()->parentWidget())) continue;
        const bool control = qobject_cast<QAbstractButton*>(c) || qobject_cast<QComboBox*>(c)
                             || (qobject_cast<QLabel*>(c) && !static_cast<QLabel*>(c)->wordWrap()
                                 && c->objectName() != "PanelTitle");
        if (!control) continue;
        // Inside a scrolled area (the recipe cards): only the width counts
        const QRect r(c->mapTo(page, QPoint(0, 0)), c->size());
        const QString name = QString("%1 %2 '%3'")
                                 .arg(c->metaObject()->className(), c->objectName(),
                                      qobject_cast<QAbstractButton*>(c) ? static_cast<QAbstractButton*>(c)->text()
                                      : qobject_cast<QLabel*>(c)        ? static_cast<QLabel*>(c)->text()
                                                                        : QString());
        if (c->width() + 1 < c->minimumSizeHint().width())
            out << QString("%1 %2 px < %3").arg(name).arg(c->width()).arg(c->minimumSizeHint().width());
        if (qobject_cast<QAbstractButton*>(c) && c->width() + 1 < c->sizeHint().width()
            && !qobject_cast<QToolButton*>(c))
            out << QString("%1 cut: %2 px < %3").arg(name).arg(c->width()).arg(c->sizeHint().width());
        bool scrolled = false;
        for (QWidget* up = c->parentWidget(); up && up != page; up = up->parentWidget())
            if (qobject_cast<QAbstractScrollArea*>(up)) scrolled = true;
        if (!scrolled && (r.right() > area.right() + 1 || r.left() < area.left() - 1))
            out << QString("%1 outside the page (%2..%3 of %4)").arg(name).arg(r.left()).arg(r.right()).arg(area.width());
    }
    return out.join("; ");
}

void testLayout(Engine& engine, const QString& a, const QString& b)
{
    const QString user = Check::testData() + "/tours/user-settings.conf";
    const QFont normal = QApplication::font();
    QFont big = normal;
    big.setPointSizeF(normal.pointSizeF() * 1.5);
    for (const char* lang : {"de", "ru"}) {
        for (const bool large : {false, true}) {
            QSettings().clear();
            if (QSettings from(user, QSettings::IniFormat); CHECK(!from.allKeys().isEmpty()))
                for (const QString& k : from.allKeys()) QSettings().setValue(k, from.value(k));
            for (QTranslator* t : qApp->findChildren<QTranslator*>()) {
                QCoreApplication::removeTranslator(t);
                delete t;
            }
            I18n::install(lang);
            QApplication::setFont(large ? big : normal);
            const QString where = QString("%1 %2").arg(lang, large ? "large" : "normal");
            for (const QSize size : {QSize(1366, 768), QSize(1920, 1050)}) {
                MainWindow w(&engine);
                w.disableAutosave();
                w.resize(size);
                w.show();
                settle();
                w.showPage(Page::Direct);
                DirectPage* page = w.directPage();
                if (!CHECK(page)) continue;
                page->material()->addFiles({a, b});
                page->recipes()->suggestButton()->click();
                CHECK(waitFor([&] { return !page->busy(); }));
                settle();
                const QString p = layoutProblems(page);
                const QString at = QString("%1 %2x%3").arg(where).arg(size.width()).arg(size.height());
                Check::report(p.isEmpty(), "page not cut off", __FILE__, __LINE__, at + ": " + p);
                Check::report(w.size() == size, "window keeps its size", __FILE__, __LINE__,
                              QString("%1: %2x%3").arg(at).arg(w.width()).arg(w.height()));
                // Every page button whole (7 in the bar)
                for (const Page pg : MainWindow::allPages()) {
                    auto* btn = w.findChild<QToolButton*>("pageButton_" + MainWindow::pageId(pg));
                    if (!CHECK(btn)) continue;
                    Check::report(btn->width() >= btn->sizeHint().width(), "page button whole", __FILE__, __LINE__,
                                  QString("%1: %2 %3 < %4").arg(at, MainWindow::pageId(pg)).arg(btn->width()).arg(btn->sizeHint().width()));
                }
                if (const QString dir = qEnvironmentVariable("DIRECT_PAGE_DUMP"); !dir.isEmpty())
                    w.grab().save(QString("%1/direct-%2-%3-%4.png").arg(dir, lang, large ? "large" : "normal").arg(size.width()));
            }
        }
    }
    QApplication::setFont(normal);
    I18n::install("de");
    QSettings().clear();
}

// The fake backend itself: the contract's shape, cancel, apply's ops
void testFake()
{
    int runs = 0;
    QJsonObject lastArgs;
    FakeDirectBackend fake(
        [&](const QString& command, const QJsonObject& args, const QString&) {
            ++runs;
            lastArgs = args;
            CHECK_EQ(command, QString("edit"));
            return QJsonObject{{"ok", true}, {"result", QJsonObject{}}};
        },
        [](const QString&) { return 10.0; }, [] { return QString("7"); }, 5);
    const QJsonObject s = fake.makeSession("short", {{"files", QJsonArray{"/x/a.mkv", "/x/b.mkv"}}}, {{"max_s", 30}});
    CHECK_EQ(s.value("recipe").toString(), QString("short"));
    CHECK_EQ(s.value("suggestions").toArray().size(), 4);
    CHECK_EQ(s.value("order").toArray().size(), 2);
    CHECK_EQ(s.value("target_s").toDouble(), 30.0);
    const QJsonObject first = s.value("suggestions").toArray().first().toObject();
    for (const char* k : {"n", "kind", "file", "from_s", "to_s", "reason", "on"}) CHECK(first.contains(k));
    CHECK(!first.value("reason").toObject().value("key").toString().isEmpty());
    CHECK_EQ(fake.makeSession("talk", {{"timeline", "@current"}}, {}).value("suggestions").toArray().first().toObject()
                 .value("kind").toString(), QString("pause"));
    // Errors, cancel
    QObject receiver;
    QJsonObject reply;
    fake.suggest("nope", {{"files", QJsonArray{"/x/a.mkv"}}}, {}, {}, [&](const QJsonObject& r) { reply = r; }, &receiver);
    CHECK(waitFor([&] { return !reply.isEmpty(); }, 2000));
    CHECK_EQ(reply.value("error").toObject().value("code").toString(), QString("UNKNOWN_RECIPE"));
    reply = {};
    fake.suggest("short", {{"files", QJsonArray{}}}, {}, {}, [&](const QJsonObject& r) { reply = r; }, &receiver);
    CHECK(waitFor([&] { return !reply.isEmpty(); }, 2000));
    CHECK_EQ(reply.value("error").toObject().value("code").toString(), QString("NO_MATERIAL"));
    reply = {};
    int progress = 0;
    auto h = fake.suggest("short", {{"files", QJsonArray{"/x/a.mkv"}}}, {}, [&](double, const QString&) { ++progress; },
                          [&](const QJsonObject& r) { reply = r; }, &receiver);
    h.cancel();
    CHECK_EQ(reply.value("error").toObject().value("code").toString(), QString("CANCELED"));
    reply = {};
    QTest::qWait(60);
    CHECK(reply.isEmpty()); // nothing after the cancel
    h.cancel();             // again: nothing
    CHECK(reply.isEmpty());
    // Apply: in the background, the edit's ops; unknown sessions expired, changed material stale
    {
        QTemporaryDir tmp;
        const QString f = tmp.path() + "/m.wav";
        QFile file(f);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        reply = {};
        fake.suggest("short", {{"files", QJsonArray{f}}}, {{"variants", 2}}, {}, [&](const QJsonObject& r) { reply = r; }, &receiver);
        CHECK(waitFor([&] { return !reply.isEmpty(); }, 2000));
        const QJsonObject session = reply.value("result").toObject().value("session").toObject();
        const QJsonArray sg = session.value("suggestions").toArray();
        CHECK_EQ(sg.at(1).toObject().value("variant").toInt(), 2);
        const QJsonObject choice{{"on", QJsonArray{1}}, {"order", QJsonArray{1}},
                                 {"adjust", QJsonArray{QJsonObject{{"n", 1}, {"from_s", 1.5}, {"to_s", 3.0}}}}};
        const QJsonArray ops = fake.opsFor(session, choice);
        CHECK_EQ(ops.size(), 2);
        CHECK_EQ(ops.at(1).toObject().value("in").toString(), QString("1.500s"));
        reply = {};
        fake.apply(session, choice, {}, [&](const QJsonObject& r) { reply = r; }, &receiver);
        CHECK(reply.isEmpty()); // not at once
        CHECK(waitFor([&] { return !reply.isEmpty(); }, 2000));
        CHECK(reply.value("ok").toBool());
        CHECK_EQ(runs, 1);
        CHECK_EQ(lastArgs.value("ops").toArray().size(), 2);
        CHECK_EQ(fake.applyNow(QJsonObject{{"id", "nope"}}, choice).value("error").toObject().value("code").toString(),
                 QString("SESSION_EXPIRED"));
        CHECK(file.open(QIODevice::Append));
        file.write("more"); // another size: the material changed
        file.close();
        CHECK_EQ(fake.applyNow(session, choice).value("error").toObject().value("code").toString(), QString("SESSION_STALE"));
        // Talk: a copy of the open timeline without the pauses
        const QJsonObject talk = fake.makeSession("talk", {{"timeline", "@current"}}, {});
        const QJsonArray talkOps = fake.opsFor(talk, {{"on", QJsonArray{1}}, {"order", QJsonArray{1}}});
        CHECK_EQ(talkOps.at(0).toObject().value("op").toString(), QString("new_timeline"));
        CHECK_EQ(talkOps.at(0).toObject().value("from").toString(), QString("7"));
        CHECK_EQ(talkOps.at(1).toObject().value("op").toString(), QString("delete_range"));
        // Canceled before it ran: nothing edited
        reply = {};
        auto ah = fake.apply(session, choice, {}, [&](const QJsonObject& r) { reply = r; }, &receiver);
        ah.cancel();
        CHECK_EQ(reply.value("error").toObject().value("code").toString(), QString("CANCELED"));
        QTest::qWait(60);
        CHECK_EQ(runs, 1);
    }
    // The receiver gone: no callback
    reply = {};
    {
        auto* gone = new QObject;
        fake.suggest("short", {{"files", QJsonArray{"/x/a.mkv"}}}, {}, {}, [&](const QJsonObject& r) { reply = r; }, gone);
        delete gone;
        QTest::qWait(60);
        CHECK(reply.isEmpty());
    }
}

void testWish(Engine& engine, const QString& a, const QString& b)
{
    QSettings().remove("ai");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    useRunner(w);
    if (!CHECK(QTest::qWaitForWindowExposed(&w))) return;
    loadProject(w, a);
    CHECK(w.liveShowPage("direct"));
    DirectPage* page = w.directPage();
    if (!CHECK(page)) return;
    DirectWish* wish = page->wish();
    CHECK(wish == page->wishSlot());
    // No provider: hidden, no hint anywhere on the page
    CHECK(!wish->isVisible());
    for (QLabel* l : page->findChildren<QLabel*>()) CHECK(!l->isVisible() || !l->text().contains("KI"));
    // A provider (a local server: no key needed) shows it
    AiSettings s = w.aiSettings();
    s.chatProvider = "ollama";
    s.chatAddress = "http://127.0.0.1:9/v1";
    w.setAiSettings(s);
    CHECK(wish->isVisible());
    CHECK(wish->edit()->placeholderText().contains("AWP"));
    CHECK(!wish->busyRow()->isVisible());

    // The context: material, recipe, options and the suggestions as listed - within 2000 characters
    // (a third file whose name has a bracket and a control character: quoted, escaped, no raw "]" in the note)
    const QString dir = QFileInfo(a).absolutePath();
    const QString odd = dir + QString("/c]d\x01.mp4");
    {
        QFile f(odd);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    page->material()->addFiles({a, b, odd});
    page->recipes()->setRecipe("short");
    const QString empty = page->wishContext();
    CHECK(empty.contains("`direct`"));
    const QString folder = QString("folder \"%1\": ").arg(QDir::toNativeSeparators(dir));
    CHECK(empty.contains(folder + "f1=\"a.mp4\", f2=\"b.mp4\", f3=\"c\\u005dd.mp4\""));
    CHECK_EQ(int(empty.count(folder)), 1); // the folder once
    CHECK(!empty.contains(a));            // names, not every full path
    CHECK(!empty.contains(']') && !empty.contains(QChar(1)));
    CHECK(empty.contains("Recipe: \"short\""));
    CHECK(empty.contains("No suggestions yet"));
    CHECK(!empty.contains("plan:true"));
    CHECK(page->wishContext(true).contains("plan:true")); // plan mode: an apply must be a plan
    page->material()->clear();
    page->material()->addFiles({a, b});
    QJsonArray many;
    for (int n = 1; n <= 150; ++n)
        many << QJsonObject{{"n", n}, {"kind", "moment"}, {"file", n % 2 ? a : b}, {"stream", 0},
                            {"from_s", n * 0.5}, {"to_s", n * 0.5 + 4}, {"score", 10},
                            {"reason", QJsonObject{{"key", "loud"}, {"text", "loud"}}}, {"on", n <= 3}};
    const QJsonObject session{{"id", "d42"}, {"recipe", "short"}, {"options", QJsonObject{{"max_s", 30}}},
                              {"material", QJsonArray{QJsonObject{{"file", a}, {"length_s", 6.0}},
                                                      QJsonObject{{"file", b}, {"length_s", 6.0}}}},
                              {"suggestions", many}, {"order", QJsonArray{3, 1, 2}}, {"length_s", 12.0}, {"target_s", 30.0}};
    page->suggestions()->setSession(session);
    page->suggestions()->setChecked(2, false);
    const QString note = page->wishContext();
    CHECK(note.size() <= DirectPage::kWishContextChars);
    CHECK(note.contains("Session \"d42\": 150 suggestions"));
    CHECK(note.contains("f1=\"a.mp4\" (6.0 s)"));
    CHECK(note.contains("{\"max_s\": 30}"));
    CHECK(!note.contains(']'));
    CHECK(note.contains("\n3 moment f1 1.5-5.5 loud on\n"));  // the order's first row first
    CHECK(note.contains("\n2 moment f2 1.0-5.0 loud off\n")); // switched off by the user
    CHECK(note.contains("more suggestions not listed)"));
    CHECK(note.contains("apply:{on:<list of n>"));
    // Options have a budget of their own; the session line always stays
    {
        QJsonObject big = session;
        QJsonObject o;
        for (int i = 0; i < 80; ++i) o[QString("option_%1").arg(i)] = QString(40, 'x');
        big["options"] = o;
        page->suggestions()->setSession(big);
        const QString n2 = page->wishContext();
        CHECK(n2.size() <= DirectPage::kWishContextChars);
        CHECK(n2.contains("Session \"d42\": 150 suggestions"));
        CHECK(n2.contains("\n1 moment f1 "));
        page->suggestions()->setSession(session);
        page->suggestions()->setChecked(2, false);
    }
    // While a wish runs: Übernehmen waits
    CHECK(page->applyButton()->isEnabled());
    page->setWishBusy(true);
    CHECK(!page->applyButton()->isEnabled());
    CHECK(wish->busyRow()->isVisible());
    page->setWishBusy(false);
    CHECK(page->applyButton()->isEnabled());
    std::printf("       wish context: %lld characters\n", qint64(note.size()));

    // Enter on an empty line: nothing; Delete in the line edits the text, never a suggestion
    QSignalSpy asked(wish, &DirectWish::wishRequested);
    wish->edit()->setFocus();
    wish->edit()->setText("   ");
    QTest::keyClick(wish->edit(), Qt::Key_Return);
    CHECK_EQ(asked.count(), 0);
    CHECK(!wish->busy());
    const int rows = page->suggestions()->list()->count();
    page->suggestions()->list()->setCurrentRow(0);
    page->suggestions()->list()->item(0)->setSelected(true);
    wish->edit()->setFocus();
    wish->edit()->setText("abc");
    wish->edit()->setCursorPosition(0);
    QTest::keyClick(wish->edit(), Qt::Key_Delete);
    CHECK_EQ(wish->edit()->text(), QString("bc"));
    wish->edit()->setCursorPosition(2);
    QTest::keyClick(wish->edit(), Qt::Key_Backspace);
    CHECK_EQ(wish->edit()->text(), QString("b"));
    CHECK_EQ(page->suggestions()->list()->count(), rows);
    CHECK_EQ(int(page->suggestions()->session().value("suggestions").toArray().size()), 150);

    // A wish that cannot go out (test run: the chat sends nothing): its error under the line, not busy
    w.setAiChatOffline(true);
    wish->edit()->setText("nur Runden mit AWP");
    QTest::keyClick(wish->edit(), Qt::Key_Return);
    CHECK_EQ(asked.count(), 1);
    CHECK(!wish->busy());
    CHECK(wish->noteIsError());
    CHECK_EQ(wish->note(), T("Testlauf: der KI-Chat sendet nichts.")); // (the layout cases left another language)
    CHECK_EQ(wish->edit()->text(), QString("nur Runden mit AWP")); // kept to refine it

    // The provider gone: hidden again
    s.chatProvider.clear();
    s.chatAddress.clear();
    w.setAiSettings(s);
    CHECK(!wish->isVisible());
    QSettings().remove("ai");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("direct-page");
    Theme::apply(app);
    std::setlocale(LC_NUMERIC, "C");
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir dir;
    const QStringList args = {"-f", "lavfi", "-i", "testsrc=size=320x180:rate=25:duration=6", "-f", "lavfi", "-i",
                              "sine=frequency=330:duration=6", "-shortest", "-c:v", "libx264", "-pix_fmt", "yuv420p"};
    const QString a = Check::makeMedia(dir.path() + "/a.mp4", args);
    const QString b = Check::makeMedia(dir.path() + "/b.mp4", args);
    if (!CHECK(!a.isEmpty() && !b.isEmpty())) return Check::result();
    testFake();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testWindow(engine, a, b);
    testStartPage(engine, a, dir.path());
    testLayout(engine, a, b);
    testWish(engine, a, b);
    return Check::result();
}
