// SPDX-License-Identifier: AGPL-3.0-only
// A lighter start (0.20): what the window does not show at start is built on its first use and then works as before.
// - The Color page (color wheels, scopes) does not exist after the main window's constructor; the page button, a grade
//   menu entry (copy grade) and the scopes toggle build it, once; its scopes then get the preview frames
// - LazyFontBox (Inspector title/subtitle font) remembers a font until it shows, then builds the real QFontComboBox
//   with it; a programmatic change emits nothing, a user's choice does
// - FrameView with the stored GPU check on a headless platform: the painter, nothing left to check later

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "ui/ColorPanel.h"
#include "ui/FrameView.h"
#include "ui/LazyFontBox.h"
#include "ui/ScopesPanel.h"

#include <QAction>
#include <QApplication>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <clocale>

namespace {

void trigger(const QString& id)
{
    QAction* a = InputBindings::instance().action(id);
    if (CHECK(a)) a->trigger();
}

void testColorPage()
{
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");
    {
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(1600, 950);
        w.show();
        CHECK(!w.findChild<ColorPanel*>());
        CHECK(!w.findChild<ScopesPanel*>());
        trigger("page_color");
        CHECK(w.currentPage() == MainWindow::Page::Color);
        auto* panel = w.findChild<ColorPanel*>();
        auto* scopes = w.findChild<ScopesPanel*>();
        CHECK(panel && panel->isVisible());
        CHECK(scopes && scopes->property("tourTarget").toString() == "scopes");
        // Built once: the Edit page and back keep the same widgets
        trigger("page_edit");
        trigger("page_color");
        CHECK_EQ(w.findChildren<ColorPanel*>().size(), qsizetype(1));
        CHECK_EQ(w.findChildren<ScopesPanel*>().size(), qsizetype(1));
        CHECK(w.findChild<ColorPanel*>() == panel);
    }
    {
        // A menu entry that needs the panel builds it without showing the page (no dead entry). Scopes the user hid
        // stay hidden, and building the page never shows a panel as a window of its own (it took the activation
        // from the main window: shortcuts such as Ctrl+B went nowhere)
        QSettings().setValue("color/scopesVisible", false);
        MainWindow w(&engine);
        w.disableAutosave();
        w.show();
        CHECK(QTest::qWaitForWindowActive(&w));
        CHECK(!w.findChild<ColorPanel*>());
        trigger("grade_copy");
        CHECK(w.findChild<ColorPanel*>());
        CHECK(w.currentPage() != MainWindow::Page::Color);
        CHECK(QApplication::activeWindow() == &w);
        auto* scopes = w.findChild<ScopesPanel*>();
        CHECK(scopes && scopes->isHidden() && !scopes->isWindow());
        QSettings().remove("color/scopesVisible");
    }
}

void testLazyFontBox()
{
    const QStringList families = QFontDatabase::families();
    if (families.size() < 2) {
        std::printf("Hinweis: weniger als zwei Schriften, LazyFontBox übersprungen\n");
        return;
    }
    QWidget host;
    auto* box = new LazyFontBox(&host);
    QSignalSpy spy(box, &LazyFontBox::currentFontChanged);
    box->setCurrentFont(QFont(families.at(1)));
    CHECK(!box->built());
    CHECK(!host.findChild<QFontComboBox*>());
    CHECK_EQ(box->currentFont().family(), families.at(1));
    host.show();
    CHECK(box->built());
    QFontComboBox* real = host.findChild<QFontComboBox*>();
    if (!CHECK(real)) return;
    CHECK_EQ(real->currentFont().family(), QFont(families.at(1)).family());
    CHECK_EQ(spy.count(), 0); // remembered and programmatic: no user choice
    box->setCurrentFont(QFont(families.at(0)));
    CHECK_EQ(spy.count(), 0);
    real->setCurrentIndex(real->findText(QFont(families.at(1)).family())); // like the user picking it
    CHECK_EQ(spy.count(), 1);
    CHECK_EQ(box->currentFont().family(), real->currentFont().family());
}

void testCachedGpuCheckHeadless()
{
    QSettings().setValue("viewer/gpuCheck", "1|0|Some GPU");
    FrameView::setUseCachedGpuCheck(true);
    FrameView view;
    CHECK(view.backend() == FrameView::Backend::Painter); // offscreen: the painter before any check
    CHECK(!view.gpuPending());
    CHECK(!view.surfaceDeferred()); // the deferred GPU surface is for X11 only
    FrameView::prepareWindow(&view);
    FrameView::attachDeferredSurfaces();
    CHECK(view.backend() == FrameView::Backend::Painter);
    FrameView::checkCachedGpu(); // nothing took the stored answer: nothing to check
    CHECK_EQ(QSettings().value("viewer/gpuCheck").toString(), QString("1|0|Some GPU"));
    QSettings().remove("viewer/gpuCheck");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("lazy-start");
    testLazyFontBox();
    testCachedGpuCheckHeadless();
    testColorPage();
    return Check::result();
}
