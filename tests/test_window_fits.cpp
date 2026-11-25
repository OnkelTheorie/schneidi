// SPDX-License-Identifier: AGPL-3.0-only
// The main window fits a 1920x1080 screen at interface size 150 % (QT_SCALE_FACTOR 1.5: 1280x720 logical pixels) on
// every page, in Russian (long words) and German, with a render job in the queue: its minimum size never asks for more (25.11.: Russian at 150 % made
// the window 2003 px wide on Export and Direct - the queue, the suggestions and "Open in Edit" were cut off).
// WINDOW_FITS_DUMP=1: the widest widgets of each page.

#include "check.h"

#include "app/MainWindow.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/RenderJob.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QDir>
#include <QLayout>
#include <QSettings>
#include <QTest>
#include <algorithm>
#include <clocale>

namespace {

void settle()
{
    for (int i = 0; i < 3; ++i) {
        QTest::qWait(30);
        QApplication::processEvents();
    }
}

QString pageName(MainWindow::Page p)
{
    switch (p) {
    case MainWindow::Page::Media: return "Media";
    case MainWindow::Page::Edit: return "Edit";
    case MainWindow::Page::Deliver: return "Deliver";
    case MainWindow::Page::Color: return "Color";
    case MainWindow::Page::Comp: return "Comp";
    case MainWindow::Page::Studio: return "Studio";
    case MainWindow::Page::Direct: return "Direct";
    }
    return {};
}

// The visible widgets that ask for the most width (their minimum), with their class and object name
void dumpWidest(QWidget* top)
{
    struct Row {
        int w;
        QString what;
    };
    QVector<Row> rows;
    for (QWidget* c : top->findChildren<QWidget*>()) {
        if (!c->isVisible()) continue;
        const int w = std::max(c->minimumWidth(), c->minimumSizeHint().width());
        QString path;
        for (QWidget* p = c; p && p != top; p = p->parentWidget())
            path.prepend(QString("/%1%2").arg(p->metaObject()->className(), p->objectName().isEmpty() ? QString() : "#" + p->objectName()));
        rows << Row{w, path};
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.w > b.w; });
    const int many = qEnvironmentVariableIntValue("WINDOW_FITS_DUMP") > 1 ? 400 : 40;
    for (int i = 0; i < std::min<int>(many, rows.size()); ++i) std::printf("         %5d %s\n", rows[i].w, qPrintable(rows[i].what.right(220)));
}

void check(Engine& engine, const QString& lang)
{
    I18n::switchTo(lang);
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1280, 720);
    w.show();
    settle();
    // WINDOW_FITS_PROJECT=<file>: with that project open (a copy: autosave is off)
    if (const QString file = qEnvironmentVariable("WINDOW_FITS_PROJECT"); !file.isEmpty()) {
        CHECK(w.openProject(file));
        settle();
    }
    // A job in the render queue with a size limit (its card), as after "Add to render queue"
    {
        RenderJob j;
        j.id = 1;
        j.path = QDir::temp().filePath("Короткое видео для проверки ширины окна.mp4");
        j.settings.format = "h264";
        j.settings.maxMb = 9;
        j.size = QSize(1080, 1920);
        w.liveProject()->setRenderQueue({j});
        settle();
    }
    for (const MainWindow::Page p : MainWindow::allPages()) {
        w.showPage(p);
        settle();
        const QSize min = w.minimumSizeHint().expandedTo(w.minimumSize());
        const QString where = QString("%1 %2: minimum %3x%4").arg(lang, pageName(p)).arg(min.width()).arg(min.height());
        std::printf("       %s\n", qPrintable(where));
        // Width with room to spare (a tenth): the review's window (Xvfb, the user's settings, a project with longer
        // names) came out 5 % wider than here
        const bool fits = min.width() <= 1152 && min.height() <= 720;
        Check::report(fits, "fits 1280x720 (width with a tenth to spare)", __FILE__, __LINE__, where);
        if (!fits || qEnvironmentVariableIsSet("WINDOW_FITS_DUMP")) dumpWidest(&w);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("window-fits");
    QSettings().clear();
    // The panels as a user had them (the file browser and the Inspector shown)
    QSettings().setValue("panels/mediaStorage", true);
    QSettings().setValue("panels/inspector", true);
    I18n::install("de");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    std::setlocale(LC_NUMERIC, "C");
    check(engine, "ru");
    check(engine, "de");
    return Check::result();
}
