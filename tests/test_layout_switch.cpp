// SPDX-License-Identifier: AGPL-3.0-only
// The layout forms switch live by the window's size (MainWindow::wantedForm, setLayoutForm): desktop -> tablet ->
// phone -> tablet -> desktop with the same widgets; the project, the selection, the playhead, zoom and scroll stay
// exactly (core rule), and back on the desktop every splitter, panel toggle, panel place and the page are as before.

#include "check.h"

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/Types.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/phone/PhoneShell.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QMenuBar>
#include <QSplitter>
#include <QTest>
#include <QToolButton>

namespace {

void settle(int ms = 300)
{
    for (int i = 0; i < ms / 20; ++i) {
        QTest::qWait(20);
        QApplication::processEvents();
    }
}

struct Snapshot {
    QVector<QPair<QSplitter*, QList<int>>> splitters;
    QVector<QPair<QWidget*, QWidget*>> parents; // widget -> its parent
    QVector<QPair<QToolButton*, bool>> toggles;
    QVector<QPair<QWidget*, bool>> shown;
};

Snapshot take(MainWindow& w, const QVector<QWidget*>& panels)
{
    Snapshot s;
    for (QSplitter* sp : w.centralWidget()->findChildren<QSplitter*>())
        if (sp->isVisible()) s.splitters << qMakePair(sp, sp->sizes()); // (hidden pages: never laid out)
    for (QWidget* p : panels) {
        s.parents << qMakePair(p, p->parentWidget());
        s.shown << qMakePair(p, !p->isHidden());
    }
    for (QToolButton* t : w.findChildren<QToolButton*>())
        if (t->isCheckable() && t->objectName().startsWith("toggle")) s.toggles << qMakePair(t, t->isChecked());
    return s;
}

void compare(const Snapshot& a, const Snapshot& b, const char* when)
{
    int splitDiff = 0, parentDiff = 0, toggleDiff = 0, shownDiff = 0;
    for (const auto& [sp, sizes] : a.splitters)
        for (const auto& [sp2, sizes2] : b.splitters)
            if (sp == sp2 && sizes != sizes2) {
                ++splitDiff;
                QStringList x, y;
                for (int v : sizes) x << QString::number(v);
                for (int v : sizes2) y << QString::number(v);
                std::printf("  splitter %s: %s -> %s\n", qPrintable(sp->objectName()), qPrintable(x.join(',')),
                            qPrintable(y.join(',')));
            }
    for (int i = 0; i < a.parents.size(); ++i) {
        if (a.parents[i] != b.parents[i]) {
            ++parentDiff;
            std::printf("  parent differs: %s\n", a.parents[i].first->metaObject()->className());
        }
        if (a.shown[i] != b.shown[i]) ++shownDiff;
    }
    for (int i = 0; i < std::min(a.toggles.size(), b.toggles.size()); ++i)
        if (a.toggles[i] != b.toggles[i]) ++toggleDiff;
    Check::report(splitDiff == 0 && parentDiff == 0 && toggleDiff == 0 && shownDiff == 0, when, __FILE__, __LINE__,
                  QString("splitters %1, parents %2, toggles %3, shown %4").arg(splitDiff).arg(parentDiff)
                      .arg(toggleDiff).arg(shownDiff));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    qunsetenv("SCHNEIDI_LAYOUT"); // the forms by size here
    QApplication app(argc, argv);
    Check::initApp("layout-switch");
    Theme::apply(app);
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();

    // The thresholds
    using F = MainWindow::LayoutForm;
    CHECK(MainWindow::wantedForm({1920, 1080}) == F::Desktop);
    CHECK(MainWindow::wantedForm({960, 1050}) == F::Tablet);   // half a desktop screen
    CHECK(MainWindow::wantedForm({841, 701}) == F::Tablet);    // an unfolded foldable
    CHECK(MainWindow::wantedForm({411, 914}) == F::Phone);
    CHECK(MainWindow::wantedForm({914, 411}) == F::Phone);
    CHECK(MainWindow::wantedForm({1400, 550}) == F::Desktop);  // short but wide: a desktop window
    CHECK(MainWindow::wantedForm({1095, 800}, F::Desktop) == F::Desktop); // (hysteresis)
    CHECK(MainWindow::wantedForm({1095, 800}, F::Tablet) == F::Tablet);
    CHECK(MainWindow::wantedForm({610, 900}, F::Phone) == F::Phone);
    CHECK(MainWindow::wantedForm({610, 900}, F::Tablet) == F::Tablet);

    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    settle();
    CHECK(w.layoutForm() == F::Desktop);
    // Something to keep: a title selected, the playhead, zoom and scroll
    w.editor()->addTitle(30, -1, 100, TitleKind::Text);
    TimelineView* view = w.findChild<TimelineView*>();
    view->setZoomAndScroll(3.5, 20, 0);
    view->setPlayhead(55);
    settle(100);
    const QSet<int> selection = w.liveSelection()->ids();
    CHECK(!selection.isEmpty());
    const QVector<QWidget*> panels{w.findChild<Viewer*>(), w.findChild<TimelinePanel*>(), w.findChild<MediaPool*>(),
                                   w.findChild<Inspector*>()};
    const Snapshot before = take(w, panels);
    const auto same = [&](const char* when) {
        Check::report(w.liveSelection()->ids() == selection, "selection kept", __FILE__, __LINE__, when);
        Check::report(view->view().pxPerFrame == 3.5 && view->view().leftFrame == 20.0, "zoom and scroll kept",
                      __FILE__, __LINE__, QString("%1: %2 / %3").arg(when).arg(view->view().pxPerFrame)
                                                .arg(view->view().leftFrame));
        Check::report(view->playhead() == 55, "playhead kept", __FILE__, __LINE__, when);
        CHECK(w.editor()->project()->isModified());
    };

    for (int round = 0; round < 2; ++round) {
        // Narrower: the tablet form (rail, viewer over timeline, the Inspector docked)
        w.resize(900, 760);
        settle();
        CHECK(w.layoutForm() == F::Tablet);
        if (PhoneShell* shell = w.phoneShell(); CHECK(shell)) {
            CHECK(shell->tablet() && shell->sheet() == "inspector");
            CHECK(shell->isAncestorOf(panels[0]) && shell->isAncestorOf(panels[1]));
            CHECK(!w.menuBar()->isVisible());
        }
        same("tablet");
        // A phone's width: the phone form; panels opened as sheets
        w.resize(411, 760);
        settle();
        CHECK(w.layoutForm() == F::Phone);
        if (PhoneShell* shell = w.phoneShell(); CHECK(shell)) {
            CHECK(!shell->tablet());
            shell->openSheet("media");
            shell->openSheet("color");
            shell->openSheet("export");
            settle(100);
        }
        same("phone");
        // Back: tablet, then the desktop exactly as before
        w.resize(900, 760);
        settle();
        CHECK(w.layoutForm() == F::Tablet);
        w.resize(1400, 900);
        settle();
        CHECK(w.layoutForm() == F::Desktop);
        CHECK(!w.phoneShell() && w.menuBar()->isVisible());
        same("desktop again");
        compare(before, take(w, panels), round == 0 ? "desktop as before (1)" : "desktop as before (2)");
        CHECK(TimelineView::kHeaderW == TimelineView::kHeaderWide);
    }
    // The setting pins a form at once (no restart)
    qputenv("SCHNEIDI_LAYOUT", "phone");
    w.resize(1400, 901);
    settle();
    CHECK(w.layoutForm() == F::Phone);
    qputenv("SCHNEIDI_LAYOUT", "desktop");
    w.resize(1400, 900);
    settle();
    CHECK(w.layoutForm() == F::Desktop);
    return Check::result();
}
