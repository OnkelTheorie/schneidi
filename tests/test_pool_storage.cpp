// Test Media Pool filter and Media Storage (0.13): filter by kind/flag/clip color (core/PoolFilter.h, filter button,
// combined with search and bins, icon and list view, hundreds of clips), the pool's bindable keys (scoped: Delete in
// the bin list never reaches the timeline), Media Storage marks for files in the Media Pool, hover scrub pictures
// (engine/ScrubFrames.h, ui/HoverScrub.h: never waits), and looking at a Media Storage file in the source view
// without importing it; an edit from there imports it first (one undo step), marks set there go along.
// POOL_STORAGE_DUMP=<folder>: pictures of the filter menu, the active filter and the storage marks.
#include "check.h"

#include "app/InputBindings.h"
#include "app/KeyBindingsDialog.h"
#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/PoolFilter.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "engine/ScrubFrames.h"
#include "ui/HoverScrub.h"
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"
#include "ui/PoolFilterButton.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QHelpEvent>
#include <QToolTip>
#include <QToolButton>
#include <QTest>
#include <QThread>
#include <QTreeWidget>
#include <QUndoStack>
#include <clocale>
#include <functional>

namespace {

MediaInfo media(const QString& name, bool video, bool audio, bool image = false)
{
    return MediaInfo{"/x/" + name, name, 100, video, audio, image};
}

// Processes events until `done` (at most `ms`)
bool waitFor(const std::function<bool()>& done, int ms = 20000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

// Key as the window system delivers it: first the shortcut check (accepted = the widget takes the key, no window
// shortcut fires), then the key itself
bool sendKey(QWidget* w, int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent over(QEvent::ShortcutOverride, key, mods);
    over.ignore();
    QApplication::sendEvent(w, &over);
    QKeyEvent press(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &press);
    return over.isAccepted();
}

QListWidgetItem* itemNamed(QListWidget* list, const QString& path, int role = Qt::UserRole)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(role).toString() == path) return list->item(i);
    return nullptr;
}

void testFilterLogic()
{
    MediaInfo v = media("v.mp4", true, true);
    v.flags = {"red", "blue"};
    v.clipColor = "orange";
    const MediaInfo a = media("a.wav", false, true);
    const MediaInfo i = media("i.png", true, false, true);
    Sequence seq;
    seq.name = "Timeline 1";

    PoolFilter f;
    CHECK(!f.active() && f.matches(v) && f.matches(a) && f.matches(i) && f.matches(seq));
    CHECK(f.summary().isEmpty());
    CHECK_EQ(PoolFilter::kindOf(v), PoolFilter::Video);
    CHECK_EQ(PoolFilter::kindOf(a), PoolFilter::Audio);
    CHECK_EQ(PoolFilter::kindOf(i), PoolFilter::Image);
    // Kinds: any of them
    f.kinds = PoolFilter::Audio | PoolFilter::Image;
    CHECK(f.active() && !f.matches(v) && f.matches(a) && f.matches(i) && !f.matches(seq));
    f.kinds = PoolFilter::Timeline;
    CHECK(f.matches(seq) && !f.matches(v));
    // Flags: any of the ticked ones; timelines have none
    f = {};
    f.flags = {"blue", "green"};
    CHECK(f.matches(v) && !f.matches(a) && !f.matches(seq));
    // Colors, "none" = without a clip color
    f = {};
    f.colors = {"orange"};
    CHECK(f.matches(v) && !f.matches(a));
    f.colors = {PoolFilter::NoColor};
    CHECK(!f.matches(v) && f.matches(a) && f.matches(i) && !f.matches(seq));
    // Groups combine: every ticked group must match
    f = {};
    f.kinds = PoolFilter::Video;
    f.flags = {"red"};
    f.colors = {"blue"};
    CHECK(!f.matches(v));
    f.colors = {"orange", "blue"};
    CHECK(f.matches(v));
    CHECK(f.summary().contains(T("Rot")) && f.summary().contains(T("Orange")) && f.summary().contains(T("Video")));
}

void testFilterButton(const QString& dump)
{
    PoolFilterButton b;
    PoolFilter last;
    int changes = 0;
    QObject::connect(&b, &PoolFilterButton::changed, [&](const PoolFilter& f) {
        last = f;
        ++changes;
    });
    QMenu* menu = b.filterMenu();
    const QString inactiveStyle = b.styleSheet();
    // Tick "Video" and the red flag (the flag menu is a submenu)
    QAction* video = nullptr;
    QMenu* flags = nullptr;
    for (QAction* a : menu->actions()) {
        if (a->text() == T("Video")) video = a;
        if (a->menu() && a->text().startsWith(T("Flags"))) flags = a->menu();
    }
    if (!CHECK(video && flags)) return;
    video->trigger();
    for (QAction* a : flags->actions())
        if (a->text() == T("Rot")) a->trigger();
    CHECK_EQ(changes, 2);
    CHECK_EQ(last.kinds, unsigned(PoolFilter::Video));
    CHECK_EQ(last.flags, QSet<QString>{"red"});
    CHECK(video->isChecked());
    CHECK(b.styleSheet() != inactiveStyle); // an active filter is clearly visible
    CHECK(b.toolTip().contains(T("Rot")));
    CHECK_EQ(flags->title(), T("Flags (%1)").arg(1));
    if (!dump.isEmpty()) {
        menu->popup(QPoint(0, 0));
        QApplication::processEvents();
        menu->grab().save(QDir(dump).filePath("pool-filter-menu.png"));
        menu->hide();
    }
    // Reset
    for (QAction* a : menu->actions())
        if (a->text() == T("Filter zurücksetzen")) a->trigger();
    CHECK(!last.active() && !video->isChecked());
    CHECK_EQ(b.styleSheet(), inactiveStyle);
}

void testPoolFilter(const QString& dump)
{
    Project p;
    MediaInfo v1 = media("v1.mp4", true, true), v2 = media("v2.mp4", true, false), a1 = media("a1.wav", false, true),
              i1 = media("i1.png", true, false, true), v3 = media("v3.mp4", true, true);
    v1.flags = {"red"};
    v1.clipColor = "orange";
    v2.flags = {"blue"};
    a1.clipColor = "blue";
    v3.flags = {"red"};
    const int sub = p.addBin(0, "Unter");
    v3.bin = sub;
    for (const MediaInfo& m : {v1, v2, a1, i1, v3}) p.addMedia(m);

    Engine engine;
    MediaPool pool(&p, &engine);
    pool.setSaveSettings(false);
    pool.resize(700, 400);
    pool.show();
    QApplication::processEvents();
    auto* list = pool.findChild<QListWidget*>();
    auto* reset = pool.findChild<QToolButton*>("pool_filter_reset");
    if (!CHECK(list && reset)) return;
    CHECK_EQ(list->count(), 6); // Text title, Timeline 1, v1, v2, a1, i1 (v3 is in the sub-bin)
    CHECK(reset->isHidden());

    for (MediaPool::ViewMode mode : {MediaPool::ViewMode::Thumbnails, MediaPool::ViewMode::List}) {
        pool.setViewMode(mode);
        PoolFilter f;
        f.kinds = PoolFilter::Video; // from the sub-bins too, without the title entry
        pool.setFilter(f);
        CHECK_EQ(list->count(), 3);
        CHECK(itemNamed(list, "/x/v3.mp4") && !itemNamed(list, MediaPool::TitleItem));
        CHECK(!reset->isHidden());
        f.kinds |= PoolFilter::Timeline;
        pool.setFilter(f);
        CHECK_EQ(list->count(), 4);
        f = {};
        f.flags = {"red"};
        pool.setFilter(f);
        CHECK_EQ(list->count(), 2); // v1, v3
        f.colors = {"orange"};
        pool.setFilter(f);
        CHECK_EQ(list->count(), 1);
        f = {};
        f.colors = {PoolFilter::NoColor};
        pool.setFilter(f);
        CHECK_EQ(list->count(), 3); // v2, i1, v3
        f = {};
        f.kinds = PoolFilter::Audio;
        pool.setFilter(f);
        CHECK_EQ(list->count(), 1);
        f.kinds = PoolFilter::Image;
        pool.setFilter(f);
        CHECK_EQ(list->count(), 1);
        // Nothing matches: a hint instead of an empty list
        auto* hint = pool.findChild<QLabel*>("pool_empty_hint");
        if (CHECK(hint)) {
            CHECK(hint->isHidden());
            f.flags = {"red"}; // no red still image
            pool.setFilter(f);
            CHECK_EQ(list->count(), 0);
            CHECK(hint->isVisible() && hint->text() == T("Nichts passt zum Filter"));
            if (mode == MediaPool::ViewMode::Thumbnails && !dump.isEmpty())
                pool.grab().save(QDir(dump).filePath("pool-filter-empty.png"));
            pool.setFilter({});
            pool.setSearch("gibtsnicht");
            CHECK(hint->isVisible() && hint->text() == T("Nichts passt zur Suche"));
            pool.setSearch("");
            CHECK(hint->isHidden());
            CHECK_EQ(list->count(), 6);
        }
        // Combined with the search and with the bins
        f = {};
        f.flags = {"red"};
        pool.setFilter(f);
        pool.setSearch("v3");
        CHECK_EQ(list->count(), 1);
        pool.setSearch("");
        pool.setCurrentBin(sub);
        CHECK_EQ(list->count(), 1);
        pool.setCurrentBin(0);
        // The title says what is filtered (all of it in the tooltip, cut with "…" when the pool is narrow); the reset
        // button is only a cross with a tooltip, so the narrow pool keeps its room for the title
        if (auto* title = pool.findChild<QLabel*>("pool_bin_title"); CHECK(title))
            CHECK_EQ(title->toolTip(), T("Filter in „%1“: %2").arg(p.binName(0), f.summary()));
        CHECK_EQ(reset->toolTip(), T("Filter zurücksetzen"));
        CHECK(reset->sizeHint().width() < 60);
        // Reset with the button beside the bin title
        if (mode == MediaPool::ViewMode::Thumbnails && !dump.isEmpty()) pool.grab().save(QDir(dump).filePath("pool-filter-active.png"));
        reset->click();
        CHECK(!pool.filter().active());
        CHECK_EQ(list->count(), 6);
        CHECK(reset->isHidden());
    }
    pool.setViewMode(MediaPool::ViewMode::Thumbnails);
    // The open search field takes the place of the panel title: the pool needs no more room than before (the filter
    // button had made the header too wide, opening the search pushed the Viewer aside)
    {
        QApplication::processEvents();
        const int closed = pool.minimumSizeHint().width();
        pool.setSearch("v");
        QApplication::processEvents();
        CHECK(pool.minimumSizeHint().width() <= closed);
        pool.setSearch("");
        QApplication::processEvents();
        CHECK_EQ(pool.minimumSizeHint().width(), closed);
    }

    // Hundreds of clips: filtering stays instant
    Project big;
    for (int i = 0; i < 600; ++i) {
        MediaInfo m = media(QString("clip%1.mp4").arg(i, 3, 10, QChar('0')), true, true);
        if (i % 3 == 0) m.flags = {"green"};
        big.addMedia(m);
    }
    MediaPool bigPool(&big, &engine);
    bigPool.setSaveSettings(false);
    bigPool.resize(700, 400);
    bigPool.show();
    QApplication::processEvents();
    auto* bigList = bigPool.findChild<QListWidget*>();
    PoolFilter green;
    green.flags = {"green"};
    QElapsedTimer t;
    t.start();
    bigPool.setFilter(green);
    const qint64 on = t.restart();
    bigPool.setFilter({});
    const qint64 off = t.elapsed();
    CHECK_EQ(bigPool.filter().active(), false);
    std::printf("Filter 600 clips: on %lld ms, off %lld ms\n", on, off);
    CHECK(on < 1500 && off < 1500);
    bigPool.setFilter(green);
    CHECK_EQ(bigList->count(), 200);
}

void testPoolKeys(QWidget* windowKeys, const int* windowDeletes)
{
    Project p;
    Engine engine;
    MediaPool pool(&p, &engine);
    pool.setSaveSettings(false);
    InputBindings& b = InputBindings::instance();
    for (const MediaPool::KeyAction& k : pool.keyActions())
        b.registerScopedAction(k.action, k.id, T("Media Pool"), k.defaultKey, k.scope, k.defaultKey2);
    // The timeline's Delete as a window key in the same window
    windowKeys->resize(700, 400);
    pool.setParent(windowKeys);
    pool.show();
    windowKeys->show();
    QApplication::processEvents();
    auto* tree = pool.findChild<QTreeWidget*>();
    auto* search = pool.findChild<QLineEdit*>();
    if (!CHECK(tree && search)) return;

    // Delete and Backspace in the bin list remove the selected bin; the key never reaches the window
    for (int key : {Qt::Key_Delete, Qt::Key_Backspace}) {
        const int bin = p.addBin(0, "Weg");
        pool.setCurrentBin(bin);
        QApplication::processEvents();
        CHECK(sendKey(tree, key));
        CHECK(!p.bin(bin));
    }
    // Shift+Delete is not a pool key
    CHECK(!sendKey(tree, Qt::Key_Delete, Qt::ShiftModifier));

    // Bindable: another key, the old one does nothing in the bin list any more
    b.setInput("pool_remove_bin", 0, "Ctrl+D");
    int bin = p.addBin(0, "Weg");
    pool.setCurrentBin(bin);
    CHECK(!sendKey(tree, Qt::Key_Delete));
    CHECK(p.bin(bin));
    CHECK(sendKey(tree, Qt::Key_D, Qt::ControlModifier));
    CHECK(!p.bin(bin));
    b.setInputs("pool_remove_bin", b.defaultInputs("pool_remove_bin"));
    // Delete and Backspace are the two keys of one action (Key 1 / Key 2 in the key dialog)
    CHECK_EQ(b.inputs("pool_remove_bin"), QStringList({"Del", "Backspace"}));
    CHECK_EQ(b.inputs("pool_close_search"), QStringList({"Esc", ""}));
    CHECK_EQ(b.inputs("pool_remove_clips"), QStringList({"Del", "Backspace"}));
    // 0.13 test builds had Backspace in the bin list as an action of its own: its key becomes the second one
    {
        b.setInputs("pool_remove_bin", {"Del", ""}); // (writes the file)
        QFile f(b.filePath());
        if (CHECK(f.open(QIODevice::WriteOnly)))
            f.write(R"({"version":3,"shortcuts":{"pool_remove_bin":"Del","pool_remove_bin_alt":"Backspace"}})");
        f.close();
        b.load();
        CHECK_EQ(b.inputs("pool_remove_bin"), QStringList({"Del", "Backspace"}));
        b.setInputs("pool_remove_bin", b.defaultInputs("pool_remove_bin")); // saves the file
        if (CHECK(f.open(QIODevice::ReadOnly))) CHECK(!f.readAll().contains("pool_remove_bin_alt"));
    }

    // Esc in the search field clears and closes it
    pool.setSearch("abc");
    CHECK(sendKey(search, Qt::Key_Escape));
    CHECK(search->text().isEmpty() && search->isHidden());

    // The whole way through the window system (shortcut map with the window's Delete): the bin goes, the window's
    // Delete stays quiet; outside the pool the window's Delete works
    auto* outside = new QWidget(windowKeys); // e.g. the timeline
    outside->setFocusPolicy(Qt::StrongFocus);
    outside->setGeometry(0, 380, 50, 20);
    outside->show();
    windowKeys->activateWindow();
    if (QTest::qWaitForWindowActive(windowKeys)) {
        bin = p.addBin(0, "Weg");
        pool.setCurrentBin(bin);
        tree->setFocus();
        QApplication::processEvents();
        const int before = *windowDeletes;
        QTest::keyClick(tree, Qt::Key_Delete);
        CHECK(!p.bin(bin));
        CHECK_EQ(*windowDeletes, before);
        outside->setFocus();
        QApplication::processEvents();
        QTest::keyClick(outside, Qt::Key_Delete);
        CHECK_EQ(*windowDeletes, before + 1);
    } else {
        std::printf("window not active (offscreen): shortcut map path not tested\n");
    }
    delete outside;

    // Key dialog: the pool's Delete is no conflict with the window's Delete, two window Deletes are
    {
        KeyBindingsDialog dlg;
        auto* table = dlg.findChild<QTableWidget*>();
        if (CHECK(table)) {
            QString poolTip, windowTip;
            for (int r = 0; r < table->rowCount(); ++r) {
                const QString id = table->item(r, 1)->data(Qt::UserRole).toString();
                if (id == "pool_remove_bin") poolTip = table->item(r, 2)->toolTip();
                if (id == "pool_remove_bin") CHECK_EQ(table->item(r, 3)->text(), QKeySequence(Qt::Key_Backspace).toString(QKeySequence::NativeText));
                CHECK(id != "pool_remove_bin_alt");
                if (id == "test_window_delete") windowTip = table->item(r, 2)->toolTip();
                if (id == "pool_remove_bin") CHECK_EQ(table->item(r, 0)->text(), T("Media Pool"));
            }
            CHECK(poolTip.isEmpty());
            CHECK(!windowTip.isEmpty());
        }
    }
    pool.setParent(nullptr);
}

void testStorageMarks(const QString& dir, const QString& dump)
{
    const QString a = Check::makeMedia(dir + "/a.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25:duration=2",
                                                        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!a.isEmpty())) return;
    QFile::copy(a, dir + "/b.mp4");
    const QString b = dir + "/b.mp4";
    MediaStorage storage;
    storage.setPersistent(false);
    storage.resize(700, 400);
    storage.show();
    storage.setFolder(dir);
    auto* list = storage.findChild<QListWidget*>();
    if (!CHECK(list)) return;
    QListWidgetItem* ia = itemNamed(list, a);
    QListWidgetItem* ib = itemNamed(list, b);
    if (!CHECK(ia && ib)) return;
    const QString mark = T("Schon im Media Pool");
    CHECK(!ia->toolTip().contains(mark));
    const qint64 before = ia->icon().cacheKey();
    storage.setPoolFiles({a});
    CHECK(storage.inPool(a) && !storage.inPool(b));
    CHECK(storage.inPool(dir + "/./a.mp4")); // the same file in another spelling
    CHECK(ia->toolTip().contains(mark) && !ib->toolTip().contains(mark));
    CHECK(ia->icon().cacheKey() != before);
    if (!dump.isEmpty()) storage.grab().save(QDir(dump).filePath("storage-marks.png"));
    // Hover scrub in the Media Storage: the thumbnail shows the picture under the mouse (with its pool mark)
    {
        const QImage normal = ia->icon().pixmap(QSize(144, 81)).toImage();
        const QRect r = list->visualItemRect(ia);
        storage.hoverScrub()->hover(QPoint(r.center().x() + r.width() / 4, r.top() + 20));
        CHECK(storage.hoverScrub()->item() == ia);
        CHECK(waitFor([&] { return storage.hoverScrub()->frames()->decoded() > 0; }));
        QApplication::processEvents();
        CHECK(ia->icon().pixmap(QSize(144, 81)).toImage() != normal);
        CHECK(ia->toolTip().contains(mark));
        if (!dump.isEmpty()) storage.grab().save(QDir(dump).filePath("storage-hover.png"));
        storage.hoverScrub()->leave();
        CHECK(!storage.hoverScrub()->item());
    }
    storage.setPoolFiles({});
    CHECK(!ia->toolTip().contains(mark));

    // Hundreds of files: opening the folder and marking stay instant (thumbnails come later in the background)
    QTemporaryDir many;
    for (int i = 0; i < 400; ++i) QFile::copy(a, many.filePath(QString("c%1.mp4").arg(i, 3, 10, QChar('0'))));
    QStringList half;
    for (int i = 0; i < 400; i += 2) half << many.filePath(QString("c%1.mp4").arg(i, 3, 10, QChar('0')));
    QElapsedTimer t;
    t.start();
    storage.setFolder(many.path());
    const qint64 open = t.restart();
    storage.setPoolFiles(half);
    const qint64 marked = t.elapsed();
    std::printf("Media Storage 400 files: open %lld ms, marks %lld ms\n", open, marked);
    CHECK_EQ(list->count(), 400);
    CHECK(open < 2000 && marked < 1000);
    storage.setFolder(dir);
}

void testHoverScrub(const QString& dir)
{
    const QString a = dir + "/a.mp4";
    const QString wav = Check::makeMedia(dir + "/s.wav", {"-f", "lavfi", "-i", "sine=frequency=440:duration=1"});
    if (!CHECK(QFile::exists(a) && !wav.isEmpty())) return;
    QListWidget list;
    list.setViewMode(QListView::IconMode);
    list.setIconSize(QSize(144, 81));
    list.setGridSize(QSize(160, 120));
    list.resize(400, 300);
    list.show();
    QPixmap thumb(144, 81);
    thumb.fill(Qt::gray);
    auto* item = new QListWidgetItem(QIcon(thumb), "a", &list);
    auto* sound = new QListWidgetItem(QIcon(thumb), "s", &list);
    QImage shown;
    double at = -2;
    int calls = 0;
    HoverScrub scrub(
        &list, QSize(144, 81), [&](QListWidgetItem* it) { return it == item ? a : wav; },
        [&](QListWidgetItem*, const QImage& img, double f) {
            shown = img;
            at = f;
            ++calls;
        });
    QApplication::processEvents();
    const QRect r = list.visualItemRect(item);
    QElapsedTimer t;
    t.start();
    scrub.hover(QPoint(r.left() + 1, r.center().y()));
    CHECK(t.elapsed() < 100); // asks, never waits for decoding
    CHECK(at >= 0 && at < 0.1);
    CHECK(waitFor([&] { return !shown.isNull(); }));
    CHECK(shown.width() <= 144 && shown.height() <= 81 && shown.width() >= 140);
    const QImage first = shown;
    scrub.hover(QPoint(r.right() - 1, r.center().y()));
    if (!CHECK(at > 0.9))
        std::printf("item rect %d,%d %dx%d, at %f, under the mouse %s\n", r.x(), r.y(), r.width(), r.height(), at,
                    list.itemAt(QPoint(r.right() - 1, r.center().y())) == item ? "item" : "other");
    CHECK(waitFor([&] { return scrub.frames()->has(a, ScrubFrames::kSlots - 1); }));
    QApplication::processEvents();
    CHECK(shown != first); // testsrc2 moves: the end looks different from the start
    // While scrubbing no item tooltip covers the picture; over the name below it the tooltip still shows, and moving
    // back onto the picture hides it
    {
        item->setToolTip("a.mp4 – info");
        const auto tip = [&](const QPoint& pos) {
            QHelpEvent help(QEvent::ToolTip, pos, list.viewport()->mapToGlobal(pos));
            QApplication::sendEvent(list.viewport(), &help);
            QApplication::processEvents();
            return QToolTip::isVisible() && QToolTip::text() == item->toolTip();
        };
        const QPoint picture(r.center().x(), r.top() + 30), name(r.center().x(), r.bottom() - 4);
        scrub.hover(picture);
        CHECK(!tip(picture));
        scrub.hover(name);
        CHECK(tip(name));
        scrub.hover(picture);
        CHECK(waitFor([] { return !QToolTip::isVisible(); })); // Qt hides it after a short moment
    }
    scrub.leave();
    CHECK(at < 0);
    // A file without pictures: given up after one try, the thumbnail stays
    const QRect rs = list.visualItemRect(sound);
    scrub.hover(QPoint(rs.center().x(), rs.center().y()));
    CHECK(waitFor([&] { return scrub.frames()->failed(wav); }));
    QApplication::processEvents();
    CHECK(!scrub.item());
    // List view: no hover scrub
    list.setViewMode(QListView::ListMode);
    const int n = calls;
    scrub.hover(list.visualItemRect(item).center());
    CHECK_EQ(calls, n);
}

void testPreviewWithoutImport(const QString& dir)
{
    const QString a = dir + "/a.mp4", b = dir + "/b.mp4";
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.setProjectFormat(ProjectFormat{320, 180, FrameRate{25, 1}}); // the clips' format: no question on the first edit
    Project* p = w.liveProject();
    auto* storage = w.findChild<MediaStorage*>();
    auto* editor = w.findChild<Editor*>();
    if (!CHECK(storage && editor)) return;
    storage->setFolder(dir);
    InputBindings& keys = InputBindings::instance();

    // A click shows the file in the source view, nothing is imported
    emit storage->previewRequested(a);
    CHECK(engine.mode() == Engine::Mode::Source && engine.sourcePath() == a);
    CHECK(p->media().isEmpty());
    CHECK(waitFor([&] { return editor->previewSource().length == 50; })); // probed in the background
    CHECK(p->media().isEmpty());
    // In/Out in the source view stay with the preview
    engine.seek(10);
    keys.action("mark_in")->trigger();
    engine.seek(29);
    keys.action("mark_out")->trigger();
    CHECK_EQ(editor->previewSource().markIn, 10);
    CHECK_EQ(editor->previewSource().markOut, 29);
    CHECK(p->media().isEmpty());

    // F9 imports it first (with the marks), then inserts: one undo step, the import stays (like every import)
    const int steps = p->undoStack()->index();
    keys.action("insert_clip")->trigger();
    const MediaInfo* m = p->mediaInfo(a);
    if (CHECK(m)) {
        CHECK_EQ(m->markIn, 10);
        CHECK_EQ(m->markOut, 29);
    }
    CHECK(editor->previewSource().path.isEmpty());
    CHECK_EQ(p->undoStack()->index(), steps + 1);
    CHECK(!p->timeline().video.isEmpty() && p->timeline().video[0].clips.size() == 1);
    if (!p->timeline().video.isEmpty() && !p->timeline().video[0].clips.isEmpty())
        CHECK_EQ(p->timeline().video[0].clips[0].length(), 20);
    CHECK(storage->inPool(a) && !storage->inPool(b)); // the Media Storage marks it now
    p->undoStack()->undo();
    CHECK(p->timeline().video.isEmpty() || p->timeline().video[0].clips.isEmpty());
    CHECK(p->mediaInfo(a));

    // Clicking a file of the pool shows the pool clip with its marks
    emit storage->previewRequested(a);
    CHECK(editor->previewSource().path.isEmpty());
    CHECK(engine.mode() == Engine::Mode::Source && engine.sourcePath() == a);

    // Marks set while looking go along when the file is imported another way (double-click in the Media Storage)
    emit storage->previewRequested(b);
    CHECK(waitFor([&] { return editor->previewSource().length == 50; }));
    engine.seek(5);
    keys.action("mark_in")->trigger();
    CHECK(!p->mediaInfo(b));
    emit storage->importRequested({b});
    const MediaInfo* mb = p->mediaInfo(b);
    if (CHECK(mb)) CHECK_EQ(mb->markIn, 5);
    CHECK(editor->previewSource().path.isEmpty());
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("pool_storage");
    const QString dump = qEnvironmentVariable("POOL_STORAGE_DUMP");
    if (!dump.isEmpty()) { // pictures as in the app
        Theme::load();
        Theme::apply(app);
    }
    QFile::remove(InputBindings::instance().filePath());
    InputBindings::instance().load();
    // Window key like the main window's Delete (deletes timeline clips; here it counts). The second one only for the
    // key dialog's conflict mark (in no widget, so never active)
    QWidget window;
    int windowDeletes = 0;
    QAction windowDelete(&window), windowDelete2(&window);
    windowDelete.setShortcutContext(Qt::WindowShortcut);
    window.addAction(&windowDelete);
    QObject::connect(&windowDelete, &QAction::triggered, [&] { ++windowDeletes; });
    InputBindings::instance().registerAction(&windowDelete, "test_window_delete", "Test", QKeySequence(Qt::Key_Delete));
    InputBindings::instance().registerAction(&windowDelete2, "test_window_delete2", "Test", QKeySequence(Qt::Key_Delete));

    QElapsedTimer clock;
    clock.start();
    auto lap = [&](const char* what) { std::printf("%s: %lld ms\n", what, clock.restart()); };
    testFilterLogic();
    testFilterButton(dump);
    testPoolFilter(dump);
    lap("filter");
    testPoolKeys(&window, &windowDeletes);
    lap("keys");
    QTemporaryDir tmp;
    if (Check::haveFfmpeg() && CHECK(tmp.isValid())) {
        testStorageMarks(tmp.path(), dump);
        lap("storage marks");
        testHoverScrub(tmp.path());
        lap("hover scrub");
        testPreviewWithoutImport(tmp.path());
        lap("preview");
    }
    return Check::result();
}
