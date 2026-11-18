// SPDX-License-Identifier: AGPL-3.0-only
// The main window fits its screen (MainWindow::placeOnScreen/fitToScreen, main.cpp before showMaximized): it never
// starts larger than the screen's available area - a 1024x768 screen, a 1280x1024 one at QT_SCALE_FACTOR=1.25 had the
// Inspector and the right of the viewer off-screen (without a window manager, maximizing does nothing) - and is not a
// small window on a big screen. The rule itself with made-up screens; the real window on the screen it runs on (run it
// under Xvfb with several -screen sizes: the frame stays inside availableGeometry). The Edit page with a clip selected
// fits a 1024 px wide screen (de, en; Russian with a big font at least 1280 px): tool buttons shorten their text
// (CompactButton), panel titles elide, the viewer's minimum is smaller.

#include "check.h"

#include "app/MainWindow.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "engine/Engine.h"
#include "ui/DrawOverlay.h"
#include "ui/Inspector.h"
#include "core/I18n.h"
#include "core/Types.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QCursor>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <functional>

namespace {

void testRule()
{
    const QMargins f = MainWindow::kFrameEstimate;
    const QRect vga(0, 0, 1024, 768), big(0, 0, 2560, 1440), right(1920, 0, 1280, 1024);
    auto inside = [&](const QRect& r, const QRect& screen) { return screen.contains(r.marginsAdded(f)); };
    // The old fixed 1600x950 on a small screen: shrunk to 90 % of it, centred, frame inside
    QRect r = MainWindow::fitToScreen(QRect(0, 0, 1600, 950), vga);
    CHECK(inside(r, vga) && r.width() >= 800 && r.height() >= 600);
    // A first start (nothing wanted) on a big screen: most of it
    r = MainWindow::fitToScreen({}, big);
    CHECK(inside(r, big) && r.width() >= 2200 && r.height() >= 1200);
    // Fits but hangs off the edge: moved in, size kept
    r = MainWindow::fitToScreen(QRect(900, 600, 800, 500), vga);
    CHECK(inside(r, vga) && r.size() == QSize(800, 500));
    // Saved on a monitor that is gone (off this screen): its size, centred here
    r = MainWindow::fitToScreen(QRect(5000, 100, 1000, 700), right);
    CHECK(inside(r, right) && r.size() == QSize(1000, 700));
    // Already inside: unchanged
    CHECK(MainWindow::fitToScreen(QRect(100, 100, 800, 600), vga) == QRect(100, 100, 800, 600));
}

void testWindow(Engine& engine)
{
    // The start screen is the one under the mouse cursor (else the primary one): never none while there is a screen
    QScreen* start = MainWindow::startScreen();
    CHECK(start);
    if (start && QGuiApplication::screenAt(QCursor::pos())) CHECK(start == QGuiApplication::screenAt(QCursor::pos()));

    MainWindow w(&engine);
    w.disableAutosave();
    w.placeOnScreen();
    w.show();
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QRect avail = w.screen()->availableGeometry();
    std::printf("screen %dx%d (available %d,%d %dx%d), window frame %d,%d %dx%d, minimum %dx%d\n",
                w.screen()->geometry().width(), w.screen()->geometry().height(), avail.x(), avail.y(), avail.width(),
                avail.height(), w.frameGeometry().x(), w.frameGeometry().y(), w.frameGeometry().width(),
                w.frameGeometry().height(), w.minimumSizeHint().width(), w.minimumSizeHint().height());
    // A screen smaller than the window's minimum (the offscreen platform's 800x800) cannot hold it: nothing to check
    if (w.minimumSizeHint().width() > avail.width() || w.minimumSizeHint().height() > avail.height()) {
        std::printf("screen smaller than the window's minimum: not checked (run under Xvfb, e.g. 1024x768)\n");
        return;
    }
    Check::report(avail.contains(w.frameGeometry()), "window frame inside the screen", __FILE__, __LINE__,
                  QString("frame %1,%2 %3x%4").arg(w.frameGeometry().x()).arg(w.frameGeometry().y())
                      .arg(w.frameGeometry().width()).arg(w.frameGeometry().height()));
    // Most of the screen: not a small window on a big one
    CHECK(w.width() >= avail.width() * 8 / 10 || w.width() >= w.minimumSizeHint().width());
}

// Paint mode (key P) in a small window: the brush bar fits the viewer (two rows if needed) and never makes the window
// bigger (a 1000x640 window grew to 1188x773); Esc ends painting
void testPaintBar(Engine& engine)
{
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1000, 640);
    w.show();
    w.editor()->addTitle(200, -1, 50, TitleKind::Text); // a text title selected: the Inspector's usual width
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QSize least = w.minimumSizeHint();
    w.editor()->addTitle(0, -1, 50, TitleKind::Drawing); // selected: the Inspector shows its section
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    const QSize before = w.size(); // (at least the window's minimum)
    // A selected drawing does not make the window's minimum wider (its section made the Edit page ~70 px wider)
    std::printf("window minimum with a text title selected %dx%d, with a drawing selected %dx%d (a 1024x768 screen holds %dx%d)\n", least.width(),
                least.height(), w.minimumSizeHint().width(), w.minimumSizeHint().height(),
                1024 - MainWindow::kFrameEstimate.left() - MainWindow::kFrameEstimate.right(),
                768 - MainWindow::kFrameEstimate.top() - MainWindow::kFrameEstimate.bottom());
    CHECK(w.minimumSizeHint().width() <= least.width());
    QAction* paint = nullptr;
    for (QAction* a : w.findChildren<QAction*>())
        if (a->shortcut() == QKeySequence("P")) paint = a;
    if (!CHECK(paint)) return;
    paint->trigger();
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
    auto* bar = w.findChild<DrawBar*>();
    if (!CHECK(bar && bar->isVisible())) return;
    std::printf("window %dx%d -> %dx%d (minimum %dx%d), bar %dx%d in %d px\n", before.width(), before.height(),
                w.width(), w.height(), w.minimumSizeHint().width(), w.minimumSizeHint().height(), bar->width(),
                bar->height(), bar->parentWidget()->width());
    CHECK(w.size() == before);
    CHECK(bar->geometry().right() < bar->parentWidget()->width());
    // A narrow viewer: the bar goes to two rows, still inside
    w.resize(760, 640);
    QTest::qWait(50);
    QApplication::processEvents();
    CHECK(bar->geometry().right() < bar->parentWidget()->width() || bar->parentWidget()->width() < 200);
    // Esc ends painting (a key reaches only the active window: without a window manager it may not get active)
    w.activateWindow();
    if (!QTest::qWaitForWindowActive(&w, 2000)) {
        std::printf("window not active: Esc not checked\n");
        return;
    }
    // In a text/number field (the Inspector's) Esc is the field's: the edit is left, painting goes on
    QLineEdit* field = nullptr;
    auto* inspector = w.findChild<Inspector*>();
    if (inspector)
        for (QLineEdit* e : inspector->findChildren<QLineEdit*>())
            if (e->isVisible() && e->isEnabled() && e->focusPolicy() != Qt::NoFocus) field = e;
    if (CHECK(field)) {
        field->setFocus();
        QApplication::processEvents();
        if (field->hasFocus()) {
            QTest::keyClick(field, Qt::Key_Escape);
            QApplication::processEvents();
            CHECK(!field->hasFocus());
            CHECK(bar->isVisible() && paint->isChecked());
        } else std::printf("field did not take the focus: Esc in a field not checked\n");
    }
    // Elsewhere Esc ends painting
    if (QWidget* f = QApplication::focusWidget(); f && (qobject_cast<QLineEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f)))
        f->clearFocus();
    QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : static_cast<QWidget*>(&w), Qt::Key_Escape);
    QApplication::processEvents();
    CHECK(!bar->isVisible() && !paint->isChecked());
}

// WINDOW_FIT_SHOT=<folder>: a picture of the window per case. WINDOW_FIT_DUMP=<px>: the visible widgets whose minimum width is at least that, as a tree (what holds the window wide)
void dumpMinimums(QWidget* w, int depth = 0)
{
    if (!w->isVisible()) return;
    // What a layout gives it at least (qSmartMinSize): a set minimum wins over the hint, Ignored needs nothing
    int min = w->minimumWidth() > 0 ? w->minimumWidth() : w->minimumSizeHint().width();
    if (w->minimumWidth() <= 0 && w->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) min = 0;
    min = qMin(min, w->maximumWidth());
    if (min >= qEnvironmentVariableIntValue("WINDOW_FIT_DUMP"))
        std::printf("%*s%s '%s' min %d (hint %d, set %d) width %d\n", depth * 2, "", w->metaObject()->className(),
                qPrintable(w->objectName()), min, w->minimumSizeHint().width(), w->minimumWidth(), w->width());
    for (QObject* o : w->children())
        if (auto* c = qobject_cast<QWidget*>(o); c && !c->isWindow()) dumpMinimums(c, depth + 1);
}

void settle()
{
    for (int i = 0; i < 5; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
}

// The Edit page with a clip selected fits a 1024x768 screen (it needed >= 1085 px): a video clip, each kind of title
// (text, counter, progress bar, shape, drawing) and an audio clip, with the default panels (Media Pool, Viewer and
// Inspector side by side) and with a user's layout (Effects Library instead of the pool, the mixer column on). The
// window's minimum width is what decides.
void testVgaEditPage(Engine& engine, const QString& media, const char* label, int screenWidth = 1024)
{
    const QMargins f = MainWindow::kFrameEstimate;
    const int room = screenWidth - f.left() - f.right();
    for (const bool columns : {false, true}) {
        QSettings().setValue("panels/mediaPool", !columns);
        QSettings().setValue("panels/effects", columns);
        QSettings().setValue("panels/mixer", columns);
        MainWindow w(&engine);
        w.disableAutosave();
        w.resize(room, 768 - f.top() - f.bottom());
        w.show();
        settle();
        Editor* ed = w.editor();
        if (!media.isEmpty()) {
            w.importFiles({media}, true);
            for (int i = 0; i < 40 && ed->project()->timeline().video.value(0).clips.isEmpty(); ++i) settle();
        }
        struct Case {
            QString name;
            std::function<void()> select;
        };
        auto first = [&](const QVector<Track>& tracks) {
            for (const Track& t : tracks)
                if (!t.clips.isEmpty()) return t.clips.first().id;
            return -1;
        };
        // The Inspector's longest pages: every effect on the clip (each one a section of its own)
        auto everyEffect = [&](const QVector<Track>& tracks, bool video) {
            const int id = first(tracks);
            for (const EffectDescriptor& d : EffectRegistry::all())
                if (d.video == video && d.library) ed->addEffect({id}, d.id);
            ed->selection()->set(QSet<int>{id});
        };
        QVector<Case> cases;
        if (!media.isEmpty()) {
            cases << Case{"video clip", [&] { ed->selection()->set(QSet<int>{first(ed->project()->timeline().video)}); }};
            cases << Case{"audio clip", [&] { ed->selection()->set(QSet<int>{first(ed->project()->timeline().audio)}); }};
            cases << Case{"video clip with every effect", [&] { everyEffect(ed->project()->timeline().video, true); }};
            cases << Case{"audio clip with every effect", [&] { everyEffect(ed->project()->timeline().audio, false); }};
        }
        for (const auto& k : kTitleKinds)
            cases << Case{k.id, [&, kind = k.kind] { ed->addTitle(400, -1, 50, kind); }};
        int widest = 0;
        for (const Case& c : cases) {
            c.select();
            settle();
            const int min = w.minimumSizeHint().width();
            widest = qMax(widest, min);
            std::printf("%s, %s: %s selected -> window minimum %d (room %d)\n", label,
                        columns ? "effects+mixer, no pool" : "default panels", qPrintable(c.name), min, room);
            if (qEnvironmentVariableIsSet("WINDOW_FIT_DUMP")) dumpMinimums(&w);
            if (const QString dir = qEnvironmentVariable("WINDOW_FIT_SHOT"); !dir.isEmpty())
                w.grab().save(QString("%1/%2-%3-%4.png").arg(dir, label, columns ? "columns" : "default", c.name));
            Check::report(min <= room && w.width() <= room, "the Edit page fits the screen's width", __FILE__,
                          __LINE__, QString("%1 (%2, %3 px screen): minimum %4, window %5 > %6").arg(c.name, label)
                                        .arg(screenWidth).arg(min).arg(w.width()).arg(room));
        }
    }
    QSettings().remove("panels");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("window-fit");
    testRule();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    testWindow(engine);
    testPaintBar(engine);
    // WINDOW_FIT_LANG / WINDOW_FIT_PT: one language/font only (measuring, 1024 px); else de and en at the default font
    // on 1024 px, and Russian with a big font (15 pt, the user's real settings) on 1280 px - with the fonts here it
    // fits 1024 px too (999 px under Xvfb, 1012 offscreen), other fonts (CI) may be wider
    QTemporaryDir dir;
    QString media;
    if (Check::haveFfmpeg())
        media = Check::makeMedia(dir.path() + "/clip.mp4",
                                 {"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25:duration=4", "-f", "lavfi", "-i",
                                  "sine=frequency=440:duration=4", "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt",
                                  "yuv420p", "-c:a", "aac", "-shortest"});
    const QFont font = QApplication::font();
    if (qEnvironmentVariableIsSet("WINDOW_FIT_LANG")) {
        I18n::install(qEnvironmentVariable("WINDOW_FIT_LANG"));
        QFont big = font;
        if (qEnvironmentVariableIsSet("WINDOW_FIT_PT")) big.setPointSizeF(qEnvironmentVariable("WINDOW_FIT_PT").toDouble());
        QApplication::setFont(big);
        testVgaEditPage(engine, media, "custom");
    } else {
        I18n::install("de");
        testVgaEditPage(engine, media, "de");
        I18n::install("en");
        testVgaEditPage(engine, media, "en");
        I18n::install("ru");
        QFont big = font;
        big.setPointSizeF(15);
        QApplication::setFont(big);
        testVgaEditPage(engine, media, "ru 15 pt", 1280);
        QApplication::setFont(font);
    }
    return Check::result();
}
