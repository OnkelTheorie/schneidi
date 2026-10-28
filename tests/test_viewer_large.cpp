// SPDX-License-Identifier: AGPL-3.0-only
// Large and full-screen viewer.
// Large viewer (Workspace › Large Viewer, the viewer's button, MainWindow::setViewerLarge): on every page the panels
// beside the viewer and the timeline hide, the viewer gets their room; a picture limited by the height takes height
// from the timeline, which keeps a third of the column and its zoom/scroll (core rule); the same action, Esc, a panel button or a page switch bring back exactly the layout before (which
// panels, splitter sizes), the saved layout settings stay untouched. With the Green Screen pipette on, Esc cancels the
// pipette first.
// Full-screen viewer (Workspace › Full-Screen Viewer, Ctrl+F, MainWindow::setViewerFullScreen): a window of its own
// over the screen with only the picture; Space/J/K/L/arrows act there, playback goes on across the switch, Esc, the same
// key and closing the window leave; the main window's layout stays as it was.
// VIEWER_LARGE_DUMP=<folder>: pictures of the window before/with the large viewer (Edit and Color page) and of the
// full-screen viewer.

#include "check.h"

#include "app/AiChatPanel.h"
#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "engine/Engine.h"
#include "ui/FullScreenViewer.h"
#include "ui/Inspector.h"
#include "ui/KeyPickWindow.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QAction>
#include <QApplication>
#include <QMenuBar>
#include <QPointer>
#include <QScrollBar>
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
    QHash<QScrollBar*, int> scrolls;
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
        for (QScrollBar* b : w->findChildren<QScrollBar*>())
            if (b->isVisible()) l.scrolls[b] = b->value();
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
    for (auto it = a.scrolls.begin(); it != a.scrolls.end(); ++it)
        if (alive.contains(it.key()) && b.scrolls.contains(it.key()) && b.scrolls.value(it.key()) != it.value()) {
            Check::report(false, "scroll position changed", __FILE__, __LINE__,
                          QString("%1: %2 in %3: %4 -> %5").arg(where, it.key()->objectName(),
                                                                it.key()->parentWidget()->parentWidget()->metaObject()->className())
                              .arg(it.value()).arg(b.scrolls.value(it.key())));
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

    // Full screen: Ctrl+F (DaVinci's Cinema Viewer key) in every built-in set, used by nothing else there
    CHECK(b.action("viewer_full_screen") != nullptr);
    for (const QString& preset : InputBindings::presetIds()) {
        b.applyPreset(preset);
        CHECK_EQ(b.inputs("viewer_full_screen").value(0), QString("Ctrl+F"));
        for (const auto& e : b.entries())
            if (e.id != "viewer_full_screen" && e.scope.isEmpty()) CHECK(!b.inputs(e.id).contains("Ctrl+F"));
    }
    b.applyPreset("standard");
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
    // The clip selected (the Inspector shows its sections) and the Inspector scrolled down
    if (const Clip* c = w.liveProject()->timeline().video.value(0).clips.isEmpty()
            ? nullptr : &w.liveProject()->timeline().video[0].clips.first())
        w.liveSelection()->set({c->id});
    settle();
    QScrollBar* inspectorScroll = nullptr;
    for (QScrollBar* b : inspector->findChildren<QScrollBar*>())
        if (b->orientation() == Qt::Vertical && b->maximum() > 100) inspectorScroll = b;
    if (CHECK(inspectorScroll)) inspectorScroll->setValue(inspectorScroll->maximum());
    // A zoom and scroll of the user's own
    TimelineView* tv = timeline->view();
    tv->setZoomAndScroll(7.5, 12.0, 0);
    const ViewState view = tv->view();
    const Layout before = Layout::of(&w);
    const QVariantMap settings = savedSettings();
    const int viewerWidth = viewer->width(), viewerHeight = viewer->height(), timelineHeight = timeline->height(),
              timelineWidth = timeline->width();
    dump(&w, "edit-before");

    button->click();
    settle();
    CHECK(w.viewerLarge() && large->isChecked() && button->isChecked());
    CHECK(inspector->isHidden() || !inspector->isVisible());
    CHECK(!pool->isVisible() && !inspector->isVisible() && !w.aiChatPanel()->isVisible());
    CHECK(viewer->isVisible() && timeline->isVisible());
    // The viewer takes the room beside it (almost the whole width); its 16:9 picture is then limited by the height,
    // so it takes height from the timeline too, which keeps a third of the column
    CHECK(viewer->width() > viewerWidth + 300);
    CHECK(viewer->width() > w.width() - 20);
    CHECK(viewer->height() > viewerHeight + 100);
    CHECK(timeline->height() < timelineHeight);
    CHECK(timeline->height() >= (viewer->height() + timeline->height()) / 3 - 4);
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
    // Off and on again at once (before the layout's second pass): stays large
    large->trigger();
    settle();
    large->trigger();
    large->trigger();
    settle();
    CHECK(w.viewerLarge() && viewer->width() > w.width() - 20 && viewer->height() > viewerHeight + 100);
    large->trigger();
    settle();
    sameLayout(before, Layout::of(&w), "off and on at once");

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
            const auto pickWindow = [&w]() -> KeyPickWindow* { // the pipette's window while it is open
                for (KeyPickWindow* k : w.findChildren<KeyPickWindow*>())
                    if (k->isVisible()) return k;
                return nullptr;
            };
            KeyPickWindow* window = pickWindow();
            if (CHECK(window)) QTest::keyClick(window, Qt::Key_Escape);
            settle();
            CHECK(!pickWindow() && w.viewerLarge());
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

    // Color page: the wheels and scopes below hide, the viewer gets their height; the timeline keeps its own (it is
    // less than a third there)
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

void testFullScreen(MainWindow& w, Engine& engine)
{
    QAction* full = InputBindings::instance().action("viewer_full_screen");
    auto* viewer = w.findChild<Viewer*>();
    auto* button = viewer ? viewer->findChild<QToolButton*>("viewer_full_screen") : nullptr;
    if (!CHECK(full && viewer && button)) return;
    w.showPage(MainWindow::Page::Edit);
    engine.seek(10);
    QTest::qWait(300);
    const Layout before = Layout::of(&w);
    const QImage shown = viewer->picture();

    // On with the button: a window of its own over the whole screen, the picture of the viewer in it
    button->click();
    settle();
    FullScreenViewer* fs = w.fullScreenViewer();
    if (!CHECK(fs)) return;
    CHECK(full->isChecked() && button->isChecked());
    CHECK(fs->isWindow() && fs->isVisible() && fs->isFullScreen());
    CHECK(fs->screen() == w.screen() && fs->geometry() == w.screen()->geometry());
    CHECK(QTest::qWaitForWindowActive(fs));
    CHECK(fs->picture().size() == shown.size() && !fs->picture().isNull());
    const QRectF r = fs->pictureRect();
    CHECK(r.width() >= fs->width() - 1 || r.height() >= fs->height() - 1); // as large as the screen allows
    if (!dumpDir().isEmpty()) fs->grab().save(dumpDir() + "/full-screen.png");

    // Keys of the window act here: Space plays, K stops, the arrows step, L plays forward
    const int at = engine.position();
    QTest::keyClick(fs, Qt::Key_Right);
    settle();
    CHECK_EQ(engine.position(), at + 1);
    QTest::keyClick(fs, Qt::Key_Left);
    settle();
    CHECK_EQ(engine.position(), at);
    QTest::keyClick(fs, Qt::Key_Space);
    settle();
    CHECK(engine.speed() != 0.0);
    QTest::keyClick(fs, Qt::Key_K);
    settle();
    CHECK(engine.speed() == 0.0);
    QTest::keyClick(fs, Qt::Key_L);
    settle();
    CHECK(engine.speed() > 0.0);
    // Playback goes on across the switch (not stopped, not started again)
    QTest::keyClick(fs, Qt::Key_Escape);
    QTest::qWait(50);
    CHECK(!w.fullScreenViewer() && !full->isChecked() && !button->isChecked());
    CHECK(engine.speed() > 0.0);
    engine.pause();
    settle();
    sameLayout(before, Layout::of(&w), "full screen");

    // The same key leaves too; while playing in it
    engine.seek(0);
    engine.togglePlay();
    full->trigger();
    settle();
    fs = w.fullScreenViewer();
    if (CHECK(fs && QTest::qWaitForWindowActive(fs))) {
        CHECK(engine.speed() != 0.0);
        QTest::keyClick(fs, Qt::Key_F, Qt::ControlModifier);
        QTest::qWait(50);
        CHECK(!w.fullScreenViewer() && !full->isChecked());
        CHECK(engine.speed() != 0.0);
    }
    engine.pause();
    // Closing the window (window manager) ends it as well
    full->trigger();
    settle();
    if (CHECK(w.fullScreenViewer())) {
        w.fullScreenViewer()->close();
        QTest::qWait(50);
        CHECK(!w.fullScreenViewer() && !full->isChecked());
    }
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
    testFullScreen(w, engine);
    return Check::result();
}
