// SPDX-License-Identifier: AGPL-3.0-only
// The Edit page's layout (MainWindow::updateRightColumn, hideViewerArea/restoreViewerArea):
// - the Mixer sits in its own column below the Inspector: making the timeline taller (the handle above it) leaves the
//   mixer's height alone, the mixer's top starts level with the timeline's;
// - with the preview on a second monitor and its area hidden, the timeline moves up into the viewer's place: from the
//   top bar down between the left column and the Inspector, whose widths stay; the mixer keeps its height; with the
//   viewer back the layout is exactly the one before; the timeline's zoom/scroll never change (core rule);
// - a layout saved by an older version (the timeline beside the mixer in "editBottom") loads.
// EDIT_LAYOUT_DUMP=<folder>: pictures of the normal and the hidden-viewer layout, with and without the mixer, at
// 1920x1080 and 1366x768.

#include "check.h"

#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/Mixer.h"
#include "ui/SecondScreen.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <clocale>

namespace {

using Plan = SecondScreenPlan;

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
}

void dump(QWidget* w, const QString& name)
{
    const QString dir = qEnvironmentVariable("EDIT_LAYOUT_DUMP");
    if (!dir.isEmpty()) w->grab().save(dir + "/" + name + ".png");
}

struct Sizes {
    QHash<QSplitter*, QList<int>> sizes;
    QHash<QWidget*, bool> hidden;
    static Sizes of(QWidget* w)
    {
        Sizes s;
        for (QSplitter* sp : w->findChildren<QSplitter*>())
            if (sp->isVisible()) {
                s.sizes[sp] = sp->sizes();
                for (int i = 0; i < sp->count(); ++i) s.hidden[sp->widget(i)] = sp->widget(i)->isHidden();
            }
        return s;
    }
};

void same(const Sizes& a, const Sizes& b, const QString& where)
{
    for (auto it = a.hidden.begin(); it != a.hidden.end(); ++it)
        Check::report(b.hidden.value(it.key(), !it.value()) == it.value(), "same visibility", __FILE__, __LINE__,
                      QString("%1: %2").arg(where, it.key()->metaObject()->className()));
    for (auto it = a.sizes.begin(); it != a.sizes.end(); ++it) {
        const QList<int> x = it.value(), y = b.sizes.value(it.key());
        bool near = x.size() == y.size();
        for (int i = 0; near && i < x.size(); ++i) near = std::abs(x[i] - y[i]) <= 1;
        QStringList sa, sb;
        for (int v : x) sa << QString::number(v);
        for (int v : y) sb << QString::number(v);
        Check::report(near, "same splitter sizes", __FILE__, __LINE__,
                      QString("%1: %2 %3 -> %4").arg(where, it.key()->objectName(), sa.join(' '), sb.join(' ')));
    }
}

struct Ui {
    MainWindow& w;
    Viewer* viewer = nullptr;
    TimelinePanel* timeline = nullptr;
    Inspector* inspector = nullptr;
    MediaPool* pool = nullptr;
    Mixer* mixer = nullptr;
    QSplitter* editMain = nullptr;
    QSplitter* editTop = nullptr;
    QToolButton* mixerToggle = nullptr;
    explicit Ui(MainWindow& win) : w(win)
    {
        viewer = w.findChild<Viewer*>();
        timeline = w.findChild<TimelinePanel*>();
        inspector = w.findChild<Inspector*>();
        pool = w.findChild<MediaPool*>();
        mixer = w.findChild<Mixer*>();
        editMain = w.findChild<QSplitter*>("editMain");
        editTop = w.findChild<QSplitter*>("editTop");
        mixerToggle = w.findChild<QToolButton*>("panelToggle_mixer");
    }
    bool ok() const { return viewer && timeline && inspector && pool && mixer && editMain && editTop && mixerToggle; }
    QRect rect(QWidget* x) const { return QRect(x->mapTo(&w, QPoint(0, 0)), x->size()); }
};

bool sameView(const ViewState& a, const ViewState& b) { return a.pxPerFrame == b.pxPerFrame && a.leftFrame == b.leftFrame; }

void testLayout(MainWindow& w, const QSize& size)
{
    const QString tag = QString("%1x%2").arg(size.width()).arg(size.height());
    w.resize(size);
    w.showPage(MainWindow::Page::Edit);
    settle();
    Ui ui(w);
    if (!CHECK(ui.ok())) return;
    TimelineView* tv = ui.timeline->view();
    tv->setZoomAndScroll(6.0, 10.0, 0);
    const ViewState view = tv->view();

    for (const bool withMixer : {false, true}) {
        const QString name = tag + (withMixer ? "-mixer" : "");
        ui.mixerToggle->setChecked(withMixer);
        settle();
        CHECK(ui.mixer->isVisible() == withMixer);
        CHECK(ui.inspector->isVisible());
        if (withMixer) {
            // Below the Inspector, in the same column; its top level with the timeline's
            const QRect insp = ui.rect(ui.inspector), mix = ui.rect(ui.mixer), tl = ui.rect(ui.timeline);
            CHECK(mix.top() >= insp.bottom());
            CHECK(mix.left() >= tl.right());
            CHECK(std::abs(mix.left() - insp.left()) <= 1);
            CHECK(std::abs(mix.top() - tl.top()) <= 8);
            CHECK(ui.mixer->parentWidget() != ui.timeline->parentWidget());
        } else {
            CHECK(ui.inspector->parentWidget() == ui.editTop); // in the top row as ever
        }
        dump(&w, "normal-" + name);

        // The handle above the timeline: the timeline grows, the mixer keeps its height
        const int mixerHeight = ui.mixer->height(), timelineHeight = ui.timeline->height();
        const QList<int> mainBefore = ui.editMain->sizes();
        QList<int> taller = mainBefore;
        const int delta = size.height() >= 1000 ? 150 : 60; // the top row's panels have their minimum heights
        taller[0] -= delta;
        taller[1] += delta;
        ui.editMain->setSizes(taller);
        settle();
        CHECK(ui.timeline->height() >= timelineHeight + delta - 10);
        if (withMixer) CHECK_EQ(ui.mixer->height(), mixerHeight);
        ui.editMain->setSizes(mainBefore);
        settle();
        CHECK(std::abs(ui.timeline->height() - timelineHeight) <= 1);

        // The preview onto a second monitor, its area hidden: the timeline in the viewer's place
        const Sizes before = Sizes::of(&w);
        const int poolWidth = ui.pool->width(), inspectorWidth = ui.inspector->width();
        const int columnHeight = ui.editMain->height();
        tv->setFocus();
        CHECK(w.focusWidget() == tv);
        w.setSecondScreenMode(Plan::Mode::HideViewer);
        w.showPreviewOn(QGuiApplication::primaryScreen());
        settle();
        CHECK(w.previewOutput());
        CHECK(ui.viewer->isHidden());
        CHECK(ui.timeline->isVisible());
        CHECK(ui.timeline->height() >= columnHeight - 4); // from the top bar down
        CHECK(ui.rect(ui.timeline).left() >= ui.rect(ui.pool).right());
        CHECK(ui.rect(ui.timeline).right() <= ui.rect(ui.inspector).left());
        CHECK(std::abs(ui.pool->width() - poolWidth) <= 1);
        CHECK(std::abs(ui.inspector->width() - inspectorWidth) <= 1);
        if (withMixer) CHECK_EQ(ui.mixer->height(), mixerHeight);
        CHECK(sameView(tv->view(), view));
        CHECK(w.focusWidget() == tv); // the keys still go to the timeline
        dump(&w, "hidden-" + name);
        // Its height can change there too: the mixer keeps its own
        if (withMixer) {
            auto* right = w.findChild<QSplitter*>("editRight");
            if (CHECK(right && right->isVisible())) {
                QList<int> s = right->sizes();
                s[0] += 60;
                s[1] -= 60;
                right->setSizes(s);
                settle();
                CHECK(ui.timeline->height() >= columnHeight - 4);
                right->setSizes(before.sizes.value(right));
                settle();
            }
        }
        // A page switch and back: still up there
        w.showPage(MainWindow::Page::Color);
        settle();
        CHECK(ui.viewer->isHidden());
        w.showPage(MainWindow::Page::Edit);
        settle();
        CHECK(ui.viewer->isHidden());
        CHECK(ui.timeline->height() >= columnHeight - 4);
        CHECK(sameView(tv->view(), view));

        // The preview back in the window: exactly as before
        w.showPreviewOn(nullptr);
        settle();
        CHECK(!w.previewOutput());
        CHECK(ui.viewer->isVisible());
        same(before, Sizes::of(&w), "back " + name);
        CHECK(sameView(tv->view(), view));
        w.setSecondScreenMode(Plan::Mode::KeepArea);
    }
    // The left column widened by the user while the viewer is hidden (a drag on its handle): it keeps that width when
    // the viewer comes back, the viewer takes its old place
    {
        w.setSecondScreenMode(Plan::Mode::HideViewer);
        w.showPreviewOn(QGuiApplication::primaryScreen());
        settle();
        const int at = ui.editTop->indexOf(ui.timeline);
        if (CHECK(ui.viewer->isHidden() && at > 0)) {
            const int poolWidth = ui.editTop->widget(at - 1)->width();
            QSplitterHandle* handle = ui.editTop->handle(at);
            const QPoint mid(handle->width() / 2, handle->height() / 2);
            QTest::mousePress(handle, Qt::LeftButton, {}, mid);
            QTest::mouseMove(handle, mid + QPoint(40, 0));
            QTest::mouseRelease(handle, Qt::LeftButton, {}, mid + QPoint(40, 0));
            settle();
            const int widened = ui.editTop->widget(at - 1)->width();
            CHECK(widened > poolWidth + 20);
            w.showPreviewOn(nullptr);
            settle();
            CHECK(ui.viewer->isVisible());
            CHECK(ui.editTop->indexOf(ui.viewer) == at);
            CHECK(std::abs(ui.editTop->widget(at - 1)->width() - widened) <= 1);
            CHECK(sameView(tv->view(), view));
        }
        w.setSecondScreenMode(Plan::Mode::KeepArea);
        settle();
    }
    // The mixer's column made narrower by the user: a page switch keeps that width (it only grows when toggled on)
    {
        auto* right = w.findChild<QSplitter*>("editRight");
        auto* row = right ? qobject_cast<QSplitter*>(right->parentWidget()) : nullptr;
        if (CHECK(row && ui.mixer->isVisible() && row->orientation() == Qt::Horizontal && row->indexOf(right) > 0)) {
            const int at = row->indexOf(right);
            QList<int> sizes = row->sizes();
            const int narrow = std::max(right->minimumSizeHint().width(), 120);
            if (CHECK(sizes[at] > narrow + 20)) {
                sizes[at - 1] += sizes[at] - narrow;
                sizes[at] = narrow;
                row->setSizes(sizes);
                settle();
                const int width = right->width();
                w.showPage(MainWindow::Page::Color);
                settle();
                w.showPage(MainWindow::Page::Edit);
                settle();
                CHECK_EQ(right->width(), width);
            }
        }
    }
    // The mixer off again: the Inspector back in the top row with its width
    ui.mixerToggle->setChecked(false);
    settle();
    CHECK(ui.inspector->parentWidget() == ui.editTop && ui.inspector->isVisible());
    CHECK(ui.mixer->isHidden());
    // Mixer on with the Inspector off: the mixer has the column to itself
    w.findChild<QToolButton*>("panelToggle_inspector")->setChecked(false);
    ui.mixerToggle->setChecked(true);
    settle();
    CHECK(ui.mixer->isVisible() && ui.inspector->isHidden());
    CHECK(ui.mixer->height() >= ui.editMain->height() - 4);
    w.findChild<QToolButton*>("panelToggle_inspector")->setChecked(true);
    ui.mixerToggle->setChecked(false);
    settle();
    CHECK(ui.inspector->isVisible() && ui.inspector->parentWidget() == ui.editTop);
    CHECK(sameView(tv->view(), view));
}

// A layout saved by 0.19 (timeline | mixer in "editBottom"): loads, the sizes that still exist are used
void testOldSaved(Engine& engine)
{
    QSettings s;
    s.setValue("layout/editMain", QVariantList{300, 600});
    s.setValue("layout/editTop", QVariantList{300, 900, 250, 400});
    s.setValue("layout/editBottom", QVariantList{1200, 400});
    s.setValue("panels/mixer", true);
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    settle();
    Ui ui(w);
    if (!CHECK(ui.ok())) return;
    CHECK(ui.mixer->isVisible() && ui.inspector->isVisible() && ui.timeline->isVisible());
    const QList<int> main = ui.editMain->sizes();
    CHECK(main.size() == 2 && main[1] > main[0]); // the saved proportion
    CHECK(ui.editTop->count() == 4);
    CHECK(ui.mixer->parentWidget() == w.findChild<QSplitter*>("editRight"));
    s.remove("layout");
    s.remove("panels/mixer");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("edit-layout");
    QSettings().clear();
    QTemporaryDir dir;
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    testOldSaved(engine);
    MainWindow w(&engine);
    w.disableAutosave();
    w.show();
    w.activateWindow();
    CHECK(QTest::qWaitForWindowActive(&w));
    if (Check::haveFfmpeg()) {
        const QString file = Check::makeMedia(dir.path() + "/clip.mp4",
            {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=2", "-c:v", "libx264", "-preset", "ultrafast",
             "-pix_fmt", "yuv420p"});
        if (!file.isEmpty()) w.importFiles({file}, true);
        settle();
    }
    testLayout(w, {1920, 1080});
    testLayout(w, {1366, 768});
    return Check::result();
}
