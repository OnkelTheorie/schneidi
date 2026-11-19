// SPDX-License-Identifier: AGPL-3.0-only
// The Direct page "Regie" (0.25), with the fake backend (app/DirectBackend.h) until the `direct` command exists:
// - built lazily: a new window has none of its widgets, the first showing builds it; its button sits right of Media,
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
// DIRECT_PAGE_DUMP=<folder>: a picture of the window per layout case.

#include "check.h"

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
#include "ui/timeline/TimelineOverview.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
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

    // Lazy, the button right of Media, the default start page is Edit
    CHECK(!w.directPage());
    CHECK(!w.findChild<DirectPage*>());
    CHECK(w.currentPage() == Page::Edit);
    auto* media = w.findChild<QToolButton*>("pageButton_media");
    auto* direct = w.findChild<QToolButton*>("pageButton_direct");
    auto* edit = w.findChild<QToolButton*>("pageButton_edit");
    if (!CHECK(media && direct && edit)) return;
    CHECK(media->x() < direct->x() && direct->x() < edit->x());
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
    // Away and back: the same page, the viewer back in the middle
    w.showPage(Page::Edit);
    CHECK(page->center()->indexOf(viewer) < 0);
    CHECK(w.liveShowPage("direct"));
    CHECK(w.directPage() == page && page->center()->indexOf(viewer) == 0);
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

    // Übernehmen: exactly one undo step, a new timeline with the checked ranges
    const int sequences = int(p->sequences().size());
    page->applyButton()->click();
    settle();
    CHECK_EQ(p->undoStack()->count(), undoBefore + 1);
    CHECK_EQ(int(p->sequences().size()), sequences + 1);
    CHECK_EQ(int(p->timeline().video.value(0).clips.size()), 2);
    p->undoStack()->undo();
    CHECK_EQ(int(p->sequences().size()), sequences);
    CHECK_EQ(timelineClips(w), clips);

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
    CHECK(w.currentPage() == Page::Edit);
    w.showAllPages();
    CHECK(direct->isVisible());
    CHECK(!QSettings().contains(PageVisibility::kSettingsKey)); // a test run: never saved
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
    // A hidden start page: the nearest shown one (Direct sits between Media and Edit)
    QSettings().setValue(PageVisibility::kSettingsKey, QStringList{"direct"});
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        CHECK(w.currentPage() == Page::Edit || w.currentPage() == Page::Media);
        CHECK(w.currentPage() == Page::Edit); // the right one first on a tie
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
        [](const QString&) { return 10.0; }, 5);
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
    // The receiver gone: no callback
    {
        auto* gone = new QObject;
        fake.suggest("short", {{"files", QJsonArray{"/x/a.mkv"}}}, {}, {}, [&](const QJsonObject& r) { reply = r; }, gone);
        delete gone;
        QTest::qWait(60);
        CHECK(reply.isEmpty());
    }
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
    return Check::result();
}
