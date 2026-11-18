// SPDX-License-Identifier: AGPL-3.0-only
// Everyone arranges the panels the way they want: every splitter size the user drags is remembered and comes back on
// the next start, wider and narrower alike (MainWindow::watchSizes, ui/SplitterMemory for the splitters inside panels).
// A drag marks the splitter as the user's ("layout/userSet"); only sizes never set by the user (defaults, values saved
// by older versions) may be fitted to their content on start - today the Edit page's right column (Inspector over
// Mixer, MainWindow::fitRightColumn). "Reset panel sizes" forgets the markers and fits again.
// Each "start" is a new MainWindow on the same (isolated) settings.

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/Mixer.h"

#include <QAction>
#include <QApplication>
#include <QSettings>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTest>
#include <clocale>
#include <memory>

namespace {

void settle()
{
    for (int i = 0; i < 4; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
}

std::unique_ptr<MainWindow> start(Engine& engine)
{
    auto w = std::make_unique<MainWindow>(&engine); // autosave on: the layout is written as for a user
    w->resize(1600, 900);
    w->show();
    settle();
    return w;
}

// A drag on the handle before widget `index` by dx/dy pixels, as the user does it (emits splitterMoved)
void drag(QSplitter* s, int index, int delta)
{
    QSplitterHandle* handle = s->handle(index);
    const QPoint mid(handle->width() / 2, handle->height() / 2);
    const QPoint to = mid + (s->orientation() == Qt::Horizontal ? QPoint(delta, 0) : QPoint(0, delta));
    QTest::mousePress(handle, Qt::LeftButton, {}, mid);
    QTest::mouseMove(handle, to);
    QTest::mouseRelease(handle, Qt::LeftButton, {}, to);
    settle();
}

QStringList userSet() { return QSettings().value("layout/userSet").toStringList(); }

QSplitter* splitter(MainWindow& w, const char* name) { return w.findChild<QSplitter*>(name); }

void clean()
{
    QSettings s;
    s.remove("layout");
    s.setValue("panels/mixer", true);
}

// The right column: dragged wider and narrower, each survives a restart; the marker is set
void testRightColumn(Engine& engine)
{
    clean();
    int width = 0;
    {
        auto w = start(engine);
        QSplitter* row = splitter(*w, "editRow");
        QSplitter* right = splitter(*w, "editRight");
        if (!CHECK(row && right && right->isVisible())) return;
        const int before = right->width();
        drag(row, 1, -150); // the handle left of the column: 150 px wider
        width = right->width();
        CHECK(width >= before + 140);
        CHECK(userSet().contains("editRow"));
    }
    {
        auto w = start(engine);
        QSplitter* right = splitter(*w, "editRight");
        if (!CHECK(right)) return;
        Check::report(std::abs(right->width() - width) <= 2, "dragged wider: kept after a restart", __FILE__, __LINE__,
                      QString("%1 px, dragged to %2").arg(right->width()).arg(width));
        // Narrower than the default now
        drag(splitter(*w, "editRow"), 1, 40); // (not so far that it collapses)
        width = right->width();
    }
    {
        auto w = start(engine);
        QSplitter* right = splitter(*w, "editRight");
        if (!CHECK(right)) return;
        Check::report(std::abs(right->width() - width) <= 2, "dragged narrower: kept after a restart", __FILE__,
                      __LINE__, QString("%1 px, dragged to %2").arg(right->width()).arg(width));
        // Narrower than all strips need (they scroll then): the user's choice too, not widened again on start
        auto* mixer = w->findChild<Mixer*>();
        drag(splitter(*w, "editRow"), 1, right->width() - (mixer ? mixer->naturalWidth() : 300) + 30);
        width = right->width();
        CHECK(mixer && width < mixer->naturalWidth());
    }
    {
        auto w = start(engine);
        QSplitter* right = splitter(*w, "editRight");
        if (!CHECK(right)) return;
        Check::report(std::abs(right->width() - width) <= 2, "narrower than the strips: kept after a restart",
                      __FILE__, __LINE__, QString("%1 px, dragged to %2").arg(right->width()).arg(width));
    }
}

// A width saved by an older version (no marker): fitted to the content on start; reset: fitted, markers gone
void testOldValueAndReset(Engine& engine)
{
    clean();
    QSettings().setValue("layout/editRow", QVariantList{1397, 520});
    {
        auto w = start(engine);
        QSplitter* right = splitter(*w, "editRight");
        auto* mixer = w->findChild<Mixer*>();
        if (!CHECK(right && mixer)) return;
        Check::report(right->width() <= std::max(mixer->naturalWidth(), 340) + 16, "old saved width fitted", __FILE__,
                      __LINE__, QString("%1 px").arg(right->width()));
        // Dragged (wider again) and reset: fitted, no markers left
        drag(splitter(*w, "editRow"), 1, -200);
        drag(splitter(*w, "editMain"), 1, -60);
        CHECK(userSet().contains("editRow") && userSet().contains("editMain"));
        QAction* reset = nullptr;
        for (const InputBindings::Entry& e : InputBindings::instance().entries())
            if (e.id == "layout_reset") reset = e.action;
        if (!CHECK(reset)) return;
        reset->trigger();
        settle();
        CHECK(userSet().isEmpty());
        Check::report(right->width() <= std::max(mixer->naturalWidth(), 340) + 16, "reset: fitted", __FILE__, __LINE__,
                      QString("%1 px").arg(right->width()));
    }
}

// Other splitters: the viewer/timeline height, the left column (Media Pool over Effects), inside the Media Pool
void testOtherSplitters(Engine& engine)
{
    clean();
    QSettings().setValue("panels/effects", true);
    QSettings().setValue("panels/mediaPool", true);
    int timeline = 0, effects = 0, bins = 0;
    {
        auto w = start(engine);
        QSplitter* main = splitter(*w, "editMain");
        QSplitter* left = splitter(*w, "editLeft");
        auto* pool = w->findChild<MediaPool*>();
        QSplitter* poolSplit = pool ? pool->findChild<QSplitter*>("mediaPoolBins") : nullptr;
        if (!CHECK(main && left && pool && poolSplit && left->isVisible() && poolSplit->isVisible())) return;
        drag(main, 1, -80); // the timeline 80 px taller
        timeline = main->widget(1)->height();
        const int effectsAt = left->indexOf(w->findChild<EffectsLibrary*>());
        if (CHECK(effectsAt > 0)) {
            drag(left, effectsAt, -60);
            effects = left->widget(effectsAt)->height();
        }
        drag(poolSplit, 1, 50); // the bin list 50 px wider
        bins = poolSplit->widget(0)->width();
        CHECK(userSet().contains("editMain"));
    }
    {
        auto w = start(engine);
        QSplitter* main = splitter(*w, "editMain");
        QSplitter* left = splitter(*w, "editLeft");
        auto* pool = w->findChild<MediaPool*>();
        QSplitter* poolSplit = pool ? pool->findChild<QSplitter*>("mediaPoolBins") : nullptr;
        if (!CHECK(main && left && poolSplit)) return;
        Check::report(std::abs(main->widget(1)->height() - timeline) <= 2, "timeline height kept", __FILE__, __LINE__,
                      QString("%1 px, dragged to %2").arg(main->widget(1)->height()).arg(timeline));
        const int effectsAt = left->indexOf(w->findChild<EffectsLibrary*>());
        if (CHECK(effectsAt > 0))
            Check::report(std::abs(left->widget(effectsAt)->height() - effects) <= 2, "effects height kept", __FILE__,
                          __LINE__, QString("%1 px, dragged to %2").arg(left->widget(effectsAt)->height()).arg(effects));
        Check::report(std::abs(poolSplit->widget(0)->width() - bins) <= 2, "Media Pool bins width kept", __FILE__,
                      __LINE__, QString("%1 px, dragged to %2").arg(poolSplit->widget(0)->width()).arg(bins));
    }
    QSettings().remove("panels/effects");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("layout-memory");
    QSettings().clear();
    std::setlocale(LC_NUMERIC, "C");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testRightColumn(engine);
    testOldValueAndReset(engine);
    testOtherSplitters(engine);
    return Check::result();
}
