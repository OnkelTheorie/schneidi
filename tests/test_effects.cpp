// SPDX-License-Identifier: AGPL-3.0-only
// Test effects: editor (add/remove/undo/split), project file, Inspector, Effects Library (categories by purpose,
// search across groups, favourites, recently used, shapes & drawing, saved collapsed state; EFFECTS_SHOT=<folder>
// saves pictures of it, EFFECTS_PT=<points> a larger font),
// dragging filters onto the timeline (offscreen)
#include "check.h"

#include "core/EffectFavorites.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "app/Theme.h"
#include "ui/Inspector.h"
#include "ui/EffectsLibrary.h"
#include "ui/timeline/TimelineView.h"
#include "ui/MediaPool.h"
#include "ui/ScrubField.h"
#include <QCollator>
#include <QDir>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidgetItemIterator>
#include <memory>
#include <QMimeData>
#include <QTreeWidget>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QDropEvent>

#include <QApplication>
#include <QTemporaryDir>
#include <QUndoStack>

namespace {
constexpr QSize kIconTest{48, 27};
} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("effects");
    const QString lang = argc > 1 ? argv[1] : "de";
    I18n::install(lang);
    Theme::apply(app);
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();

    Project project;
    Selection sel;
    Editor editor(&project, &sel);
    const QString media = "/x/detail.mp4"; // file does not have to exist (no preview)
    project.addMedia(MediaInfo{media, "detail.mp4", 125, true, false, false});
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    Clip c;
    c.id = project.newClipId();
    c.mediaPath = media;
    c.out = 99;
    tl.video[0].clips << c;
    Clip a = c; // audio clip: gets no effects
    a.id = project.newClipId();
    tl.audio[0].clips << a;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, a.id, 0});
    const int vid = c.id, aid = a.id;

    auto clip = [&](int id) { return TimelineOps::findClip(project.timeline(), id); };
    editor.addEffect({vid, aid}, "color");
    CHECK(EffectRegistry::has(*clip(vid), "color"));
    CHECK(!EffectRegistry::has(*clip(aid), "color"));
    const int undoCount = project.undoStack()->count();
    editor.addEffect({vid}, "color"); // twice: nothing
    CHECK(project.undoStack()->count() == undoCount);
    editor.addEffect({vid}, "blur");
    CHECK(clip(vid)->effects.size() == 2);
    project.undoStack()->undo();
    CHECK(clip(vid)->effects.size() == 1);
    project.undoStack()->redo();
    CHECK(clip(vid)->effects.size() == 2 && clip(vid)->effects[1].effectId == "blur");

    // Values + keyframes
    editor.modifyClips({vid}, "t", [](Clip& x) {
        EffectRegistry::instance(x, "color")->params["temperature"] = 40.0;
        Keys::setKey(x, AnimParam::FxBrightness, 0, -20);
        Keys::setKey(x, AnimParam::FxBrightness, 80, 30);
    });
    CHECK(Keys::valueAt(*clip(vid), AnimParam::FxBrightness, 40) == 5.0);

    // Save -> load identical
    const QString path = tmp.filePath("fx.schneidi");
    QString err;
    CHECK(ProjectFile::save(project.data(), path, &err));
    ProjectData loaded;
    CHECK(ProjectFile::load(path, &loaded, &err));
    const Clip* lc = TimelineOps::findClip(loaded.timeline, vid);
    CHECK(lc && lc->effects == clip(vid)->effects);
    CHECK(lc && lc->keys == clip(vid)->keys);

    // Splitting takes over effects, keyframes distributed
    editor.splitAtPlayhead(50);
    const Track& t0 = project.timeline().video[0];
    CHECK(t0.clips.size() == 2 && t0.clips[1].effects == t0.clips[0].effects);
    CHECK(Keys::valueAt(t0.clips[1], AnimParam::FxBrightness, 0) == Keys::valueAt(*lc, AnimParam::FxBrightness, 50));
    project.undoStack()->undo();

    // Removing also deletes the keyframes of the effect parameters
    editor.removeEffect({vid}, "color");
    CHECK(!EffectRegistry::has(*clip(vid), "color") && !Keys::animated(*clip(vid), AnimParam::FxBrightness));
    project.undoStack()->undo();
    CHECK(EffectRegistry::has(*clip(vid), "color") && Keys::animated(*clip(vid), AnimParam::FxBrightness));
    // Inspector "Remove All" (Effects tab): every effect and its keyframes in one undo step, Green Screen stays
    {
        const QVector<EffectInstance> before = clip(vid)->effects;
        editor.modifyClips({vid}, "t", [](Clip& x) { EffectRegistry::add(x, "chromakey"); });
        editor.removeAllEffects({vid});
        CHECK_EQ(clip(vid)->effects.size(), 1);
        CHECK(EffectRegistry::has(*clip(vid), "chromakey") && !Keys::animated(*clip(vid), AnimParam::FxBrightness));
        project.undoStack()->undo();
        CHECK(EffectRegistry::has(*clip(vid), "color") && Keys::animated(*clip(vid), AnimParam::FxBrightness));
        editor.modifyClips({vid}, "t", [](Clip& x) { EffectRegistry::remove(x, "chromakey"); });
        CHECK(clip(vid)->effects == before);
    }

    // Draw Inspector (image is not saved)
    Inspector insp(&editor);
    insp.resize(380, 1250);
    sel.set({vid});
    insp.setPlayhead(40);
    insp.show();
    app.processEvents();
    CHECK(!insp.grab().isNull());
    // Change order (blur first) -> Inspector re-sorts
    editor.modifyClips({vid}, "t", [](Clip& x) { std::swap(x.effects[0], x.effects[1]); });
    app.processEvents();
    CHECK(!insp.grab().isNull());
    // Every effect has exactly one category, every category has effects, ids round-trip
    {
        QSet<EffectCategory> used;
        for (const EffectDescriptor& d : EffectRegistry::all()) {
            Check::report(d.category != EffectCategory::None, "effect has a category", __FILE__, __LINE__, d.id);
            used.insert(d.category);
        }
        for (const EffectCategoryInfo& c : kEffectCategories) {
            Check::report(used.contains(c.category), "category has effects", __FILE__, __LINE__, c.id);
            EffectCategory back = EffectCategory::None;
            CHECK(EffectRegistry::categoryFromId(QString::fromLatin1(c.id), &back) && back == c.category);
            CHECK(!EffectRegistry::categoryName(c.category).isEmpty());
        }
        EffectCategory none;
        CHECK(!EffectRegistry::categoryFromId("nope", &none));
    }

    // Effects Library: fresh favourites, recently used and collapsed state
    for (const char* k : {"effects/favorites", "effects/recent", "effects/collapsed", "effects/expanded"}) QSettings().remove(k);
    if (const QString pt = qEnvironmentVariable("EFFECTS_PT"); !pt.isEmpty()) { // EFFECTS_PT=15: a large font
        QFont f = QApplication::font();
        f.setPointSizeF(pt.toDouble());
        QApplication::setFont(f);
    }
    EffectsLibrary lib;
    lib.resize(460, 420);
    auto* tree = lib.findChild<QTreeWidget*>();
    auto* search = lib.findChild<QLineEdit*>();
    auto* list = lib.list();
    lib.show();
    app.processEvents();
    CHECK(!lib.grab().isNull());
    auto category = [&](const QString& text, QTreeWidgetItem* in = nullptr) -> QTreeWidgetItem* {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == text && (!in || (*it)->parent() == in)) return *it;
        return nullptr;
    };
    auto rowOf = [&](const QString& data) {
        for (int i = 0; i < list->count(); ++i)
            if (list->item(i)->data(Qt::UserRole).toString() == data) return i;
        return -1;
    };
    auto isHeader = [&](int row) { return list->item(row)->flags() == Qt::NoItemFlags; };
    auto headerAbove = [&](int row) {
        for (int i = row; i >= 0; --i)
            if (isHeader(i)) return list->item(i)->text();
        return QString();
    };
    // Order of the top level
    {
        QStringList tops;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) tops << tree->topLevelItem(i)->text(0);
        CHECK_EQ(tops, QStringList({T("Favoriten"), T("Zuletzt benutzt"), T("Übergänge"), T("Titel"), T("Formen & Zeichnen"),
                                    T("Videoeffekte"), T("Audioeffekte"), "LUTs", T("Klänge"), T("Compositing-Rezepte"),
                                    "Presets"}));
        CHECK(category(T("Verlaufsblenden"), category(T("Übergänge"))));
    }
    // Videoeffekte: a header per category (in the registry's order), A-Z inside, every library video effect once
    QTreeWidgetItem* videoFx = category(T("Videoeffekte"));
    if (CHECK(videoFx)) {
        tree->setCurrentItem(videoFx);
        QStringList headers, expected;
        for (const EffectCategoryInfo& c : kEffectCategories)
            for (const EffectDescriptor& d : EffectRegistry::all())
                if (d.library && d.video && d.category == c.category && c.category != EffectCategory::Custom) {
                    expected << T(c.name);
                    break;
                }
        int entries = 0;
        QCollator collator(QLocale{});
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        for (int i = 0; i < list->count(); ++i) {
            if (isHeader(i)) {
                headers << list->item(i)->text();
                continue;
            }
            ++entries;
            if (i > 0 && !isHeader(i - 1))
                Check::report(collator.compare(list->item(i - 1)->text(), list->item(i)->text()) <= 0, "A-Z inside a group",
                              __FILE__, __LINE__, list->item(i)->text());
        }
        CHECK_EQ(headers, expected);
        int videoEffects = 0;
        for (const EffectDescriptor& d : EffectRegistry::all())
            videoEffects += d.library && d.video && d.category != EffectCategory::Custom;
        CHECK_EQ(entries, videoEffects);
        CHECK_EQ(videoFx->childCount(), int(expected.size()));
        CHECK_EQ(headerAbove(rowOf("fx:blur")), T("Unschärfe"));
        if (const auto dir = qEnvironmentVariable("EFFECTS_SHOT"); !dir.isEmpty())
            lib.grab().save(QDir(dir).filePath(QString("effects-%1-video.png").arg(lang)));
        // A category alone: no header, only its effects
        if (QTreeWidgetItem* stylize = category(T("Stilisieren"), videoFx); CHECK(stylize)) {
            tree->setCurrentItem(stylize);
            CHECK(list->count() == 4 && !isHeader(0) && rowOf("frei0r.scanline0r") < 0 && rowOf("fx:frei0r.scanline0r") >= 0);
        }
    }
    // Audioeffekte by purpose as well
    if (QTreeWidgetItem* audioFx = category(T("Audioeffekte")); CHECK(audioFx)) {
        tree->setCurrentItem(audioFx);
        CHECK(rowOf("fx:denoise") > 0 && headerAbove(rowOf("fx:denoise")) == T("Reparieren"));
        CHECK(rowOf("fx:voiceclean") > 0 && headerAbove(rowOf("fx:voiceclean")) == T("Stimme"));
        CHECK(rowOf("fx:blur") < 0);
    }
    // Searching looks through everything (whatever is selected) and names the group of each hit: "Clip in Form" by its
    // words (Form, Kreis, shape, circle), with a picture
    {
        tree->setCurrentItem(category(T("Titel")));
        for (const char* word : {"Form", "kreis", "shape", "Circle"}) {
            search->setText(QString::fromLatin1(word));
            app.processEvents();
            const int row = rowOf("fx:clip_shape");
            Check::report(row > 0, "search finds Clip in Form", __FILE__, __LINE__, word);
            if (row > 0) {
                CHECK_EQ(headerAbove(row), T("Videoeffekte › %1").arg(T("Keying & Masken")));
                // Not the blank grey tile: a picture with colours in it
                const QImage icon = list->item(row)->icon().pixmap(32, 32).toImage();
                QSet<QRgb> colours;
                for (int y = 0; y < icon.height(); y += 2)
                    for (int x = 0; x < icon.width(); x += 2) colours.insert(icon.pixel(x, y));
                CHECK(colours.size() > 8);
            }
        }
        search->setText("Form");
        app.processEvents();
        CHECK(rowOf("shape:arrow") > 0 && headerAbove(rowOf("shape:arrow")) == T("Formen & Zeichnen"));
        if (const auto dir = qEnvironmentVariable("EFFECTS_SHOT"); !dir.isEmpty())
            lib.grab().save(QDir(dir).filePath(QString("effects-%1-search.png").arg(lang)));
        search->setText(T("Rauschunterdrückung"));
        app.processEvents();
        CHECK(rowOf("fx:denoise") > 0 && headerAbove(rowOf("fx:denoise")) == T("Audioeffekte › %1").arg(T("Reparieren")));
        search->setText(T("Überblendung"));
        app.processEvents();
        CHECK(rowOf("video:cross_dissolve") > 0);
        // Every key rebuilds the list, the icons are made once: more typing adds no new ones
        search->setText("Form");
        app.processEvents();
        const qsizetype icons = lib.cachedIconCount();
        CHECK(icons > 0);
        for (const char* word : {"F", "Fo", "For", "Form", "", "Bl", "Blur"}) {
            search->setText(QString::fromLatin1(word));
            app.processEvents();
        }
        CHECK(lib.cachedIconCount() == icons);
        search->clear();
    }
    // Shapes & drawing: the paint tool (double-click only) and every shape kind, dragged as a shape of that kind
    if (QTreeWidgetItem* shapes = category(T("Formen & Zeichnen")); CHECK(shapes)) {
        tree->setCurrentItem(shapes);
        CHECK_EQ(list->count(), 1 + int(std::size(kShapeKinds)));
        lib.setDrawShortcut(QKeySequence("P"));
        CHECK(list->item(0)->data(Qt::UserRole).toString() == "draw" && list->item(0)->text().contains("(P)")
              && !(list->item(0)->flags() & Qt::ItemIsDragEnabled));
        QSignalSpy draw(&lib, &EffectsLibrary::drawRequested), shape(&lib, &EffectsLibrary::shapeRequested);
        emit list->itemDoubleClicked(list->item(0));
        CHECK_EQ(draw.size(), 1);
        if (const int row = rowOf("shape:arrow"); CHECK(row > 0)) {
            emit list->itemDoubleClicked(list->item(row));
            CHECK(shape.size() == 1 && shape.first().first().toString() == "arrow");
            std::unique_ptr<QMimeData> mime(list->model()->mimeData({list->model()->index(row, 0)}));
            CHECK(mime && QString::fromUtf8(mime->data(MediaPool::MimeType)) == MediaPool::shapeItem("arrow"));
            CHECK(MediaPool::isTitleItem(MediaPool::shapeItem("arrow")) && MediaPool::shapeKindOf(MediaPool::shapeItem("arrow")) == "arrow");
        }
        if (const auto dir = qEnvironmentVariable("EFFECTS_SHOT"); !dir.isEmpty())
            lib.grab().save(QDir(dir).filePath(QString("effects-%1-shapes.png").arg(lang)));
        // Used: the shape is the most recent entry (the paint tool is not an entry that gets applied)
        CHECK(!EffectFavorites::recent().isEmpty() && EffectFavorites::recent().first() == "shape:arrow");
    }
    // Favourites: empty = a hint; context menu/F toggle, the star, kept for the next start, missing items left out
    {
        QTreeWidgetItem* favorites = category(T("Favoriten"));
        tree->setCurrentItem(favorites);
        CHECK(list->count() == 1 && isHeader(0) && list->item(0)->text().contains("F"));
        tree->setCurrentItem(videoFx);
        const QImage plain = list->item(rowOf("fx:blur"))->icon().pixmap(kIconTest).toImage();
        lib.toggleFavorite("fx:blur");
        CHECK(EffectFavorites::isFavorite("fx:blur"));
        CHECK(list->item(rowOf("fx:blur"))->icon().pixmap(kIconTest).toImage() != plain); // the star
        CHECK(list->item(rowOf("fx:blur"))->toolTip().startsWith(T("Favorit")));
        // Key F on the selected entry
        list->setCurrentRow(rowOf("fx:frei0r.glow"));
        QTest::keyClick(list, Qt::Key_F);
        CHECK(EffectFavorites::isFavorite("fx:frei0r.glow"));
        QTest::keyClick(list, Qt::Key_F);
        CHECK(!EffectFavorites::isFavorite("fx:frei0r.glow"));
        // A LUT inside the effects folder is kept relative to it; gone items are just not listed
        CHECK_EQ(EffectFavorites::key(QString("lut:") + EffectFolders::lutDir() + "/Film/x.cube"), QString("lut:@/LUTs/Film/x.cube"));
        CHECK_EQ(EffectFavorites::filePath("lut:@/LUTs/Film/x.cube"), EffectFolders::lutDir() + "/Film/x.cube");
        EffectFavorites::setFavorite("fx:gone", true);
        EffectFavorites::setFavorite("lut:@/LUTs/gone.cube", true);
        EffectFavorites::setFavorite("video:cross_dissolve", true);
        tree->setCurrentItem(favorites);
        CHECK(rowOf("fx:blur") > 0 && rowOf("video:cross_dissolve") > 0);
        CHECK_EQ(headerAbove(rowOf("fx:blur")), T("Videoeffekte › %1").arg(T("Unschärfe")));
        int real = 0;
        for (int i = 0; i < list->count(); ++i) real += !isHeader(i);
        CHECK_EQ(real, 2);
        if (const auto dir = qEnvironmentVariable("EFFECTS_SHOT"); !dir.isEmpty())
            lib.grab().save(QDir(dir).filePath(QString("effects-%1-favorites.png").arg(lang)));
        EffectsLibrary again; // persisted
        auto* t2 = again.findChild<QTreeWidget*>();
        t2->setCurrentItem(t2->topLevelItem(0));
        bool found = false;
        for (int i = 0; i < again.list()->count(); ++i) found |= again.list()->item(i)->data(Qt::UserRole).toString() == "fx:blur";
        CHECK(found);
        // Off again from the Favourites themselves
        lib.toggleFavorite("fx:blur");
        CHECK(rowOf("fx:blur") < 0);
    }
    // Recently used: newest first, no duplicates, at most 10; double-click counts, gone items left out
    {
        QSettings().remove("effects/recent");
        for (int i = 0; i < 12; ++i) EffectFavorites::noteUsed(QString("fx:x%1").arg(i));
        EffectFavorites::noteUsed("fx:x5");
        const QStringList r = EffectFavorites::recent();
        CHECK(r.size() == EffectFavorites::kRecentMax && r.first() == "fx:x5" && r.count("fx:x5") == 1 && r[1] == "fx:x11"
              && !r.contains("fx:x1"));
        tree->setCurrentItem(category(T("Übergänge")));
        emit list->itemDoubleClicked(list->item(rowOf("video:cross_dissolve")));
        tree->setCurrentItem(videoFx);
        emit list->itemDoubleClicked(list->item(rowOf("fx:blur")));
        tree->setCurrentItem(category(T("Zuletzt benutzt")));
        CHECK(list->count() == 2 && list->item(0)->data(Qt::UserRole).toString() == "fx:blur"
              && list->item(1)->data(Qt::UserRole).toString() == "video:cross_dissolve");
        CHECK(list->item(0)->toolTip().contains(T("Unschärfe")));
    }
    // Collapsed state: saved by category number; a category closed by default (LUTs) opened stays open; the older
    // saved form (only the collapsed ones) still works
    {
        QTreeWidgetItem* luts = category("LUTs");
        CHECK(luts && !luts->isExpanded() && category(T("Übergänge"))->isExpanded());
        luts->setExpanded(true);
        videoFx->setExpanded(false);
        {
            EffectsLibrary again;
            auto* t2 = again.findChild<QTreeWidget*>();
            for (int i = 0; i < t2->topLevelItemCount(); ++i) {
                const QString text = t2->topLevelItem(i)->text(0);
                if (text == "LUTs") CHECK(t2->topLevelItem(i)->isExpanded());
                if (text == T("Videoeffekte")) CHECK(!t2->topLevelItem(i)->isExpanded());
            }
        }
        QSettings().remove("effects/expanded");
        QSettings().setValue("effects/collapsed", QStringList{"0:", "16:"}); // old Toolbox, Audioeffekte
        {
            EffectsLibrary again;
            auto* t2 = again.findChild<QTreeWidget*>();
            for (int i = 0; i < t2->topLevelItemCount(); ++i) {
                const QString text = t2->topLevelItem(i)->text(0);
                if (text == T("Audioeffekte")) CHECK(!t2->topLevelItem(i)->isExpanded());
                if (text == T("Videoeffekte") || text == T("Übergänge")) CHECK(t2->topLevelItem(i)->isExpanded());
            }
        }
        for (const char* k : {"effects/favorites", "effects/recent", "effects/collapsed", "effects/expanded"}) QSettings().remove(k);
    }
    // The Inspector's Video tab adds it ("Form…" under Beschneiden)
    {
        auto* button = insp.findChild<QPushButton*>("ClipShapeButton");
        if (CHECK(button)) {
            button->click();
            app.processEvents();
            const Clip* c = TimelineOps::findClip(project.timeline(), vid);
            CHECK(c && EffectRegistry::has(*c, "clip_shape"));
            editor.removeEffect({vid}, "clip_shape");
        }
    }

    // Drag filter onto the timeline: audio track accepts nothing, video clip gets the effect (one undo step)
    editor.removeEffect({vid}, "blur");
    TimelineView tv(&editor);
    tv.resize(1200, 400);
    tv.show();
    app.processEvents();
    const int before = project.undoStack()->count();
    int hitY = -1;
    for (int y = TimelineView::kRulerH + 2; y < 400 && hitY < 0; y += 6) {
        QMimeData mime;
        mime.setData(EffectsLibrary::EffectMimeType, "blur");
        const QPoint pt(TimelineView::kHeaderW + 30, y);
        QDragEnterEvent enter(pt, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tv, &enter);
        QDragMoveEvent move(pt, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tv, &move);
        QDropEvent drop(QPointF(pt), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&tv, &drop);
        if (EffectRegistry::has(*clip(vid), "blur")) hitY = y;
    }
    CHECK(hitY >= 0);
    CHECK(project.undoStack()->count() == before + 1);
    CHECK(!EffectRegistry::has(*clip(aid), "blur"));

    // Trim tool: tracks are added while dragging (e.g. shortcut) -> preview no longer matches the
    // rows; drawing must not read beyond the tracks of the old preview
    if (hitY >= 0) {
        tv.setTool(TimelineView::Tool::Trim);
        const QPoint at(TimelineView::kHeaderW + 150, hitY);
        auto mouse = [&](QEvent::Type t, QPoint pt, Qt::MouseButtons buttons) {
            QMouseEvent e(t, QPointF(pt), tv.mapToGlobal(QPointF(pt)), Qt::LeftButton, buttons, Qt::NoModifier);
            QApplication::sendEvent(&tv, &e);
        };
        mouse(QEvent::MouseButtonPress, at, Qt::LeftButton);
        mouse(QEvent::MouseMove, at + QPoint(20, 0), Qt::LeftButton);
        CHECK(!tv.grab().isNull());
        project.edit("Spuren", [](Timeline& t) { TimelineOps::ensureTracks(t, TrackKind::Video, 6); });
        CHECK(!tv.grab().isNull());
        mouse(QEvent::MouseMove, at + QPoint(24, 0), Qt::LeftButton);
        CHECK(!tv.grab().isNull());
        mouse(QEvent::MouseButtonRelease, at + QPoint(24, 0), Qt::NoButton);
        CHECK(!tv.grab().isNull());
        tv.setTool(TimelineView::Tool::Select);
    }

    // Drag number field: slow movement (1 px per event) and Shift (fine) change the value in both directions,
    // even if a step is smaller than the displayed precision (remainder is collected instead of discarded)
    {
        auto drag = [](ScrubField& f, const QVector<int>& xs, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            auto send = [&](QEvent::Type t, int x, Qt::MouseButtons buttons) {
                QMouseEvent e(t, QPointF(x, 5), f.mapToGlobal(QPointF(x, 5)), Qt::LeftButton, buttons, mods);
                QApplication::sendEvent(&f, &e);
            };
            send(QEvent::MouseButtonPress, xs.first(), Qt::LeftButton);
            for (int x : xs) send(QEvent::MouseMove, x, Qt::LeftButton);
            send(QEvent::MouseButtonRelease, xs.last(), Qt::NoButton);
        };
        auto pixels = [](int from, int to) {
            QVector<int> xs;
            for (int x = from; from < to ? x <= to : x >= to; x += from < to ? 1 : -1) xs << x;
            return xs;
        };
        ScrubField soft(0, 100, 0.5, 0); // like softness/border of a wipe
        soft.setValue(50);
        drag(soft, {40, 44}); // dragging starts at 3 px: +2
        CHECK_EQ(soft.value(), 52.0);
        drag(soft, pixels(44, 34)); // 10 px to the left, one at a time: -5
        CHECK_EQ(soft.value(), 47.0);
        ScrubField outline(0, 40, 0.1, 1); // like outline of a title
        outline.setValue(4);
        drag(outline, pixels(10, 60), Qt::ShiftModifier); // fine: 0.01 per px
        CHECK(outline.value() > 4.3);
    }
    return Check::result();
}
