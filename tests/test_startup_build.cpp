// SPDX-License-Identifier: AGPL-3.0-only
// The main window is built in place: the panels grow in (or are moved once into) a tree that already hangs in the
// window, never a finished panel moved into another parent at the start. Every move of a built widget tree re-applies
// the app style sheet to all of its widgets (scroll areas lay their content out again): the Inspector moved into the
// mixer column at the start cost ~60 ms, the whole tree hung into the window at the end ~20 ms more (MainWindow::
// buildLayout). Checked with the mixer column on and off: no ParentChange for the big panels after their construction,
// and each of the Inspector's scroll areas sees the style change of its own insertion only.

#include "check.h"

#include "app/MainWindow.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"
#include "ui/MediaPool.h"
#include "ui/MediaStorage.h"
#include "ui/Mixer.h"
#include "ui/TranscriptPanel.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelinePanel.h"

#include <QApplication>
#include <QHash>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTest>
#include <algorithm>
#include <clocale>
#include <memory>

namespace {

bool inside(const QObject* o, const char* cls)
{
    for (const QObject* p = o; p; p = p->parent())
        if (p->inherits(cls)) return true;
    return false;
}

// Counts, application wide, ParentChange of the panels (seen only once their own constructor runs: during the base
// class constructor the object is not one yet) and StyleChange of the scroll areas inside the Inspector
class Counter : public QObject
{
public:
    QHash<QString, int> parentChanges;
    QHash<const QObject*, int> styleChanges;

    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::ParentChange) {
            for (const char* cls : {"Inspector", "MediaPool", "MediaStorage", "Viewer", "TimelinePanel", "EffectsLibrary",
                                    "Mixer", "TranscriptPanel"})
                if (o->metaObject()->className() == QByteArray(cls)) ++parentChanges[cls];
        } else if (e->type() == QEvent::StyleChange && qobject_cast<QScrollArea*>(o) && inside(o, "Inspector")) {
            ++styleChanges[o];
        }
        return false;
    }
};

void startWith(Engine& engine, bool mixer)
{
    const QString where = mixer ? "mixer column on" : "mixer column off";
    QSettings s;
    s.remove("layout");
    s.remove("panels");
    s.setValue("panels/mixer", mixer);
    Counter counter;
    qApp->installEventFilter(&counter);
    {
        auto w = std::make_unique<MainWindow>(&engine);
        w->resize(1600, 900);
        w->show();
        QTest::qWait(50);
        QApplication::processEvents();
        qApp->removeEventFilter(&counter);

        auto* inspector = w->findChild<Inspector*>();
        auto* right = w->findChild<QSplitter*>("editRight");
        auto* top = w->findChild<QSplitter*>("editTop");
        if (!CHECK(inspector && right && top)) return;
        // In its place as before: the column (Inspector over Mixer) or the top row's last place
        if (mixer) {
            Check::report(right->indexOf(inspector) == 0 && right->indexOf(w->findChild<Mixer*>()) == 1,
                          "Inspector over the mixer in the column", __FILE__, __LINE__, where);
            Check::report(right->isVisible() && inspector->isVisible(), "column shown", __FILE__, __LINE__, where);
        } else {
            Check::report(top->indexOf(inspector) == 3, "Inspector last in the top row", __FILE__, __LINE__, where);
            Check::report(!right->isVisible() && inspector->isVisible(), "no column", __FILE__, __LINE__, where);
        }
        // The Edit page's order: Media Storage, Media Pool, Effects; panel row; timeline under the top row
        auto* left = w->findChild<QSplitter*>("editLeft");
        if (CHECK(left))
            Check::report(left->indexOf(w->findChild<MediaStorage*>()) == 0 && left->indexOf(w->findChild<MediaPool*>()) == 1
                              && left->indexOf(w->findChild<EffectsLibrary*>()) == 2,
                          "left column order", __FILE__, __LINE__, where);
        Check::report(top->indexOf(w->findChild<Viewer*>()) == 1 && top->indexOf(w->findChild<TranscriptPanel*>()) == 2,
                      "top row order", __FILE__, __LINE__, where);
        auto* main = w->findChild<QSplitter*>("editMain");
        if (CHECK(main))
            Check::report(main->indexOf(top) == 0 && main->indexOf(w->findChild<TimelinePanel*>()) == 1,
                          "timeline below", __FILE__, __LINE__, where);
    }
    for (auto it = counter.parentChanges.cbegin(); it != counter.parentChanges.cend(); ++it)
        Check::report(it.value() == 0, "panel built in place, not moved at the start", __FILE__, __LINE__,
                      QString("%1: %2 moves (%3)").arg(it.key()).arg(it.value()).arg(where));
    int most = 0;
    for (int n : counter.styleChanges) most = std::max(most, n);
    Check::report(!counter.styleChanges.isEmpty() && most <= 1, "Inspector's tree styled once", __FILE__, __LINE__,
                  QString("%1 scroll areas, up to %2 style changes each (%3)")
                      .arg(counter.styleChanges.size())
                      .arg(most)
                      .arg(where));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("startup-build");
    QSettings().clear();
    std::setlocale(LC_NUMERIC, "C");
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    startWith(engine, true);
    startWith(engine, false);
    return Check::result();
}
