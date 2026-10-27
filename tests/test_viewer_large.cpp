// Large viewer (Workspace › Large Viewer, the viewer's button, MainWindow::setViewerLarge): on every page the panels
// beside the viewer and the timeline hide, the viewer gets their room, the timeline keeps its size and its zoom/scroll
// (core rule); the same action, Esc, a panel button or a page switch bring back exactly the layout before (which
// panels, splitter sizes), the saved layout settings stay untouched. With the Green Screen pipette on, Esc cancels the
// pipette first.
// VIEWER_LARGE_DUMP=<folder>: pictures of the window before/with the large viewer (Edit and Color page).

#include "check.h"

#include "app/AiChatPanel.h"
#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/KeyColorPicker.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QPointer>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <clocale>

namespace {

QString dumpDir() { return qEnvironmentVariable("VIEWER_LARGE_DUMP"); }

void dump(QWidget* w, const QString& name)
{
    if (!dumpDir().isEmpty()) w->grab().save(dumpDir() + "/" + name + ".png");
}

// Visibility of every widget and the sizes of every splitter in the window
struct Layout {
    QHash<QWidget*, bool> hidden;
    QHash<QSplitter*, QList<int>> sizes;
    QList<QPointer<QWidget>> alive; // widgets a panel rebuilt meanwhile are not compared
    static Layout of(QWidget* w)
    {
        Layout l;
        for (QWidget* c : w->findChildren<QWidget*>()) {
            l.hidden[c] = c->isHidden();
            l.alive << c;
        }
        for (QSplitter* s : w->findChildren<QSplitter*>())
            if (s->isVisible()) l.sizes[s] = s->sizes(); // the other pages' splitters are laid out when shown
        return l;
    }
};

bool sameLayout(const Layout& a, const Layout& b, const char* where)
{
    bool same = true;
    QSet<QWidget*> alive;
    for (const QPointer<QWidget>& p : a.alive)
        if (p) alive.insert(p);
    for (auto it = a.hidden.begin(); it != a.hidden.end(); ++it)
        if (alive.contains(it.key()) && b.hidden.contains(it.key()) && b.hidden.value(it.key()) != it.value()
            && !it.key()->objectName().startsWith("qt_scrollarea")) { // scroll bars follow the sizes
            QStringList chain;
            for (QWidget* p = it.key(); p; p = p->parentWidget())
                chain << QString("%1#%2").arg(p->metaObject()->className(), p->objectName());
            Check::report(false, "visibility changed", __FILE__, __LINE__,
                          QString("%1: %2 (%3)").arg(where, chain.join(" < ")).arg(it.value() ? "was hidden" : "was shown"));
            same = false;
        }
    // Splitter sizes to the pixel, but Qt may round one pixel differently the first time (no drift: checked below)
    auto near = [](const QList<int>& x, const QList<int>& y) {
        if (x.size() != y.size()) return false;
        for (int i = 0; i < x.size(); ++i)
            if (std::abs(x[i] - y[i]) > 1) return false;
        return true;
    };
    for (auto it = a.sizes.begin(); it != a.sizes.end(); ++it)
        if (alive.contains(it.key()) && b.sizes.contains(it.key()) && !near(b.sizes.value(it.key()), it.value())) {
            QStringList x, y;
            for (int v : it.value()) x << QString::number(v);
            for (int v : b.sizes.value(it.key())) y << QString::number(v);
            Check::report(false, "splitter sizes changed", __FILE__, __LINE__,
                          QString("%1: %2 %3 -> %4").arg(where, it.key()->objectName(), x.join(' '), y.join(' ')));
            same = false;
        }
    return same;
}

QVariantMap savedSettings()
{
    QSettings s;
    QVariantMap out;
    for (const QString& k : s.allKeys())
        if (k.startsWith("layout/") || k.startsWith("panels/")) out[k] = s.value(k);
    return out;
}

void settle()
{
    QTest::qWait(30);
    QApplication::processEvents();
}

QToolButton* toggleNamed(MainWindow& w, const QString& text)
{
    for (auto* b : w.findChildren<QToolButton*>())
        if (b->isCheckable() && b->text().endsWith(text)) return b;
    return nullptr;
}

void testKeys(MainWindow& w)
{
    InputBindings& b = InputBindings::instance();
    CHECK(b.action("viewer_large") != nullptr);
    CHECK_EQ(b.defaultInputs("viewer_large"), QStringList({"", ""}));
    // Why it has no default: DaVinci's Alt+F is the File menu's mnemonic in English and French (QMenuBar takes
    // Alt+<mnemonic> as a window shortcut, the two would take turns)
    QStringList menus;
    for (QAction* a : w.menuBar()->actions()) menus << a->text(); // German keys (tests run in German)
    CHECK(menus.contains("&Datei"));
    bool fileMenu = false;
    for (const char* lang : {"en", "fr"})
        fileMenu |= QKeySequence::mnemonic(I18n::translate(lang, "&Datei")) == QKeySequence("Alt+F");
    CHECK(fileMenu);
}

void testEditPage(MainWindow& w, Engine& engine)
{
    QAction* large = InputBindings::instance().action("viewer_large");
    auto* viewer = w.findChild<Viewer*>();
    auto* timeline = w.findChild<TimelinePanel*>();
    auto* inspector = w.findChild<Inspector*>();
    auto* pool = w.findChild<MediaPool*>();
    auto* button = viewer->findChild<QToolButton*>("viewer_large");
    if (!CHECK(large && viewer && timeline && inspector && pool && button)) return;
    w.showPage(MainWindow::Page::Edit);
    // Many panels on: pool, effects, inspector, mixer, transcript, AI chat
    for (const char* t : {"Effects", "Mixer", "Inspector", "Media Pool"})
        if (QToolButton* tb = toggleNamed(w, T(t))) tb->setChecked(true);
    w.showAiChat(true);
    settle();
    // A zoom and scroll of the user's own
    TimelineView* tv = timeline->view();
    tv->setZoomAndScroll(7.5, 12.0, 0);
    const ViewState view = tv->view();
    const Layout before = Layout::of(&w);
    const QVariantMap settings = savedSettings();
    const int viewerWidth = viewer->width(), timelineHeight = timeline->height(), timelineWidth = timeline->width();
    dump(&w, "edit-before");

    button->click();
    settle();
    CHECK(w.viewerLarge() && large->isChecked() && button->isChecked());
    CHECK(inspector->isHidden() || !inspector->isVisible());
    CHECK(!pool->isVisible() && !inspector->isVisible() && !w.aiChatPanel()->isVisible());
    CHECK(viewer->isVisible() && timeline->isVisible());
    // The viewer takes the room beside it (almost the whole width), the timeline its height (± a handle)
    CHECK(viewer->width() > viewerWidth + 300);
    CHECK(viewer->width() > w.width() - 20);
    CHECK(std::abs(timeline->height() - timelineHeight) <= 2);
    CHECK(timeline->width() >= timelineWidth);
    dump(&w, "edit-large");
    // Zoom and scroll stay (core rule), the saved layout too
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);
    CHECK(savedSettings() == settings);

    // Again: exactly as before
    large->trigger();
    settle();
    CHECK(!w.viewerLarge() && !large->isChecked() && !button->isChecked());
    sameLayout(before, Layout::of(&w), "action again");
    CHECK_EQ(viewer->width(), viewerWidth);
    CHECK(tv->view().pxPerFrame == view.pxPerFrame && tv->view().leftFrame == view.leftFrame);
    CHECK(savedSettings() == settings);

    // On and off again and again: the same layout every time
    const Layout once = Layout::of(&w);
    for (int i = 0; i < 6; ++i) {
        large->trigger();
        settle();
    }
    for (auto it = once.sizes.begin(); it != once.sizes.end(); ++it) CHECK(it.key()->sizes() == it.value());
    CHECK(once.sizes.size() >= 4); // editTop, editMain, editBottom, the chat's

    // Esc ends it
    large->trigger();
    settle();
    CHECK(w.viewerLarge());
    QTest::keyClick(&w, Qt::Key_Escape);
    settle();
    CHECK(!w.viewerLarge() && !large->isChecked());
    sameLayout(before, Layout::of(&w), "Esc");

    // A panel button ends it: the layout comes back with that panel switched as asked
    large->trigger();
    settle();
    QToolButton* inspectorToggle = toggleNamed(w, T("Inspector"));
    if (CHECK(inspectorToggle)) {
        inspectorToggle->click();
        settle();
        CHECK(!w.viewerLarge() && !inspector->isVisible() && pool->isVisible() && w.aiChatPanel()->isVisible());
        inspectorToggle->click();
        settle();
        CHECK(inspector->isVisible());
    }

    // With the pipette on, Esc cancels the pipette first, the next Esc the large viewer
    if (const Clip* c = w.liveProject()->timeline().video.value(0).clips.isEmpty()
            ? nullptr : &w.liveProject()->timeline().video[0].clips.first()) {
        w.liveSelection()->set({c->id});
        engine.seek(5);
        settle();
        auto* pipette = w.findChild<QToolButton*>("KeyPipette");
        large->trigger();
        settle();
        if (CHECK(pipette)) {
            pipette->click();
            settle();
            CHECK(qobject_cast<KeyColorPicker*>(viewer->overlay()) != nullptr);
            QTest::keyClick(&w, Qt::Key_Escape);
            settle();
            CHECK(!qobject_cast<KeyColorPicker*>(viewer->overlay()) && w.viewerLarge());
            QTest::keyClick(&w, Qt::Key_Escape);
            settle();
            CHECK(!w.viewerLarge());
        }
        w.liveSelection()->clear();
        w.setViewerLarge(false);
    }
}

void testOtherPages(MainWindow& w)
{
    QAction* large = InputBindings::instance().action("viewer_large");
    auto* viewer = w.findChild<Viewer*>();
    auto* timeline = w.findChild<TimelinePanel*>();
    if (!CHECK(large && viewer && timeline)) return;

    // Color page: the wheels and scopes below hide, the viewer gets their height, the timeline keeps its own
    w.showPage(MainWindow::Page::Color);
    settle();
    const Layout color = Layout::of(&w);
    const int viewerHeight = viewer->height(), timelineHeight = timeline->height();
    dump(&w, "color-before");
    large->trigger();
    settle();
    CHECK(w.viewerLarge());
    CHECK(viewer->height() > viewerHeight + 150);
    CHECK(std::abs(timeline->height() - timelineHeight) <= 2);
    CHECK(viewer->width() > w.width() - 20); // the chat beside the page hid too
    dump(&w, "color-large");
    large->trigger();
    settle();
    sameLayout(color, Layout::of(&w), "color page");
    // A page switch ends it (the page's layout first, then the switch)
    large->trigger();
    settle();
    w.showPage(MainWindow::Page::Edit);
    settle();
    CHECK(!w.viewerLarge() && !large->isChecked());

    // Media page (no timeline there) and Deliver page: the viewer takes the room of the other panels
    for (const MainWindow::Page page : {MainWindow::Page::Media, MainWindow::Page::Deliver}) {
        w.showPage(page);
        settle();
        const Layout was = Layout::of(&w);
        const int width = viewer->width(), height = viewer->height();
        large->trigger();
        settle();
        CHECK(w.viewerLarge());
        CHECK(viewer->width() > w.width() - 20);
        if (page == MainWindow::Page::Media) CHECK(viewer->height() > height + 150);
        else CHECK(viewer->width() > width + 300 && timeline->isVisible());
        large->trigger();
        settle();
        sameLayout(was, Layout::of(&w), page == MainWindow::Page::Media ? "media page" : "deliver page");
    }
    w.showPage(MainWindow::Page::Edit);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("viewer-large");
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    if (Check::haveFfmpeg()) {
        const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
            {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2", "-c:v", "libx264", "-preset",
             "ultrafast", "-pix_fmt", "yuv420p"});
        if (CHECK(!file.isEmpty())) w.importFiles({file}, true);
    }
    testKeys(w);
    testEditPage(w, engine);
    testOtherPages(w);
    return Check::result();
}
