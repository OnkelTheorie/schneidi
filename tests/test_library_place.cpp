// SPDX-License-Identifier: AGPL-3.0-only
// Placing from the Effects Library in the main window (MainWindow::addShape/onDrop, EffectsLibrary::setHistory):
// a "shape:<kind>" entry double-clicked or dropped into the timeline becomes a shape title of that kind with the
// kind's box (Shapes::sizeForKind, a line a line's thickness); recently used counts only what changed the project
// (an effect double-clicked without a clip to take it does not).

#include "check.h"

#include "app/MainWindow.h"
#include "core/EffectFavorites.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Shapes.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/MediaPool.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QListWidget>
#include <QSettings>
#include <QTest>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>

namespace {

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

// The shape title clips of the timeline, in order of their ids
QVector<Clip> shapeClips(Project* p)
{
    QVector<Clip> out;
    for (const Track& t : p->timeline().video)
        for (const Clip& c : t.clips)
            if (c.title.isShape()) out << c;
    std::sort(out.begin(), out.end(), [](const Clip& a, const Clip& b) { return a.id < b.id; });
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("library-place");
    for (const char* k : {"effects/favorites", "effects/recent"}) QSettings().remove(k);
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1600, 950);
    w.show();
    settle();
    Project* project = w.liveProject();
    auto* lib = w.findChild<EffectsLibrary*>();
    auto* tv = w.findChild<TimelineView*>();
    if (!CHECK(lib && tv && project)) return Check::result();
    auto* tree = lib->findChild<QTreeWidget*>();
    QListWidget* list = lib->list();
    auto showCategory = [&](const QString& text) {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == text) {
                tree->setCurrentItem(*it);
                return true;
            }
        return false;
    };
    auto doubleClick = [&](const QString& data) {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toString() == data) {
                emit list->itemDoubleClicked(list->item(i));
                settle();
                return true;
            }
        return false;
    };
    const double thickness = Shapes::lineThickness(project->timelineFormat().height);

    // An effect double-clicked on an empty timeline changes nothing: not recently used
    if (CHECK(showCategory(T("Videoeffekte")))) {
        const int before = project->undoStack()->index();
        CHECK(doubleClick("fx:blur"));
        CHECK_EQ(project->undoStack()->index(), before);
        CHECK(!EffectFavorites::recent().contains("fx:blur"));
    }

    // Double-click: the plain shape (ellipse, the default box), then an arrow with the arrow's box
    if (!CHECK(showCategory(T("Formen & Zeichnen")))) return Check::result();
    CHECK(doubleClick("shape:ellipse"));
    QVector<Clip> shapes = shapeClips(project);
    if (!CHECK_EQ(int(shapes.size()), 1)) return Check::result();
    const ShapeStyle plain = shapes[0].title.shape;
    CHECK(plain.kind == ShapeKind::Ellipse);
    CHECK(!EffectFavorites::recent().isEmpty() && EffectFavorites::recent().first() == "shape:ellipse");

    CHECK(doubleClick("shape:arrow"));
    shapes = shapeClips(project);
    if (CHECK_EQ(int(shapes.size()), 2)) {
        const ShapeStyle& s = shapes[1].title.shape;
        const QSizeF want = Shapes::sizeForKind(plain, ShapeKind::Arrow, thickness);
        CHECK(s.kind == ShapeKind::Arrow);
        CHECK(qFuzzyCompare(s.width, want.width()) && qFuzzyCompare(s.height, want.height()));
    }
    CHECK_EQ(EffectFavorites::recent().value(0), QString("shape:arrow"));

    // Drop into the timeline (the drag's Media Pool data): an arrow and a line, each with its kind's box
    const int end = TimelineOps::endFrame(project->timeline());
    emit tv->dropRequested({MediaPool::shapeItem("arrow")}, end + 10, -1, PlaceParts::Both);
    settle();
    emit tv->dropRequested({MediaPool::shapeItem("line")}, end + 400, -1, PlaceParts::Both);
    settle();
    shapes = shapeClips(project);
    if (CHECK_EQ(int(shapes.size()), 4)) {
        const ShapeStyle& arrow = shapes[2].title.shape;
        const QSizeF wantArrow = Shapes::sizeForKind(plain, ShapeKind::Arrow, thickness);
        CHECK(arrow.kind == ShapeKind::Arrow);
        CHECK(qFuzzyCompare(arrow.width, wantArrow.width()) && qFuzzyCompare(arrow.height, wantArrow.height()));
        const ShapeStyle& line = shapes[3].title.shape;
        const QSizeF wantLine = Shapes::sizeForKind(plain, ShapeKind::Line, thickness);
        CHECK(line.kind == ShapeKind::Line);
        CHECK(qFuzzyCompare(line.width, wantLine.width()) && qFuzzyCompare(line.height, wantLine.height()));
        CHECK(qFuzzyCompare(line.height, thickness));
    }
    // An unknown kind: the plain shape (no crash, the default circle)
    emit tv->dropRequested({MediaPool::shapeItem("nope")}, end + 800, -1, PlaceParts::Both);
    settle();
    shapes = shapeClips(project);
    if (CHECK_EQ(int(shapes.size()), 5)) CHECK(shapes[4].title.shape.kind == ShapeKind::Ellipse);

    for (const char* k : {"effects/favorites", "effects/recent"}) QSettings().remove(k);
    return Check::result();
}
