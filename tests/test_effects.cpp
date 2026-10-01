// Test Effekte: Editor (Hinzufügen/Entfernen/Undo/Teilen), Projektdatei, Inspector, Effects Library,
// Filter auf die Timeline ziehen (offscreen)
#include "check.h"

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
#include "ui/ScrubField.h"
#include <QMimeData>
#include <QTreeWidget>
#include <QDropEvent>

#include <QApplication>
#include <QTemporaryDir>
#include <QUndoStack>

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
    const QString media = "/x/detail.mp4"; // Datei muss nicht existieren (keine Vorschau)
    project.addMedia(MediaInfo{media, "detail.mp4", 125, true, false, false});
    Timeline tl;
    tl.video.resize(2);
    tl.audio.resize(1);
    Clip c;
    c.id = project.newClipId();
    c.mediaPath = media;
    c.out = 99;
    tl.video[0].clips << c;
    Clip a = c; // Audio-Clip: bekommt keine Effekte
    a.id = project.newClipId();
    tl.audio[0].clips << a;
    project.load(ProjectData{ProjectFormat{}, 0, project.media(), {}, tl, a.id, 0});
    const int vid = c.id, aid = a.id;

    auto clip = [&](int id) { return TimelineOps::findClip(project.timeline(), id); };
    editor.addEffect({vid, aid}, "color");
    CHECK(EffectRegistry::has(*clip(vid), "color"));
    CHECK(!EffectRegistry::has(*clip(aid), "color"));
    const int undoCount = project.undoStack()->count();
    editor.addEffect({vid}, "color"); // doppelt: nichts
    CHECK(project.undoStack()->count() == undoCount);
    editor.addEffect({vid}, "blur");
    CHECK(clip(vid)->effects.size() == 2);
    project.undoStack()->undo();
    CHECK(clip(vid)->effects.size() == 1);
    project.undoStack()->redo();
    CHECK(clip(vid)->effects.size() == 2 && clip(vid)->effects[1].effectId == "blur");

    // Werte + Keyframes
    editor.modifyClips({vid}, "t", [](Clip& x) {
        EffectRegistry::instance(x, "color")->params["temperature"] = 40.0;
        Keys::setKey(x, AnimParam::FxBrightness, 0, -20);
        Keys::setKey(x, AnimParam::FxBrightness, 80, 30);
    });
    CHECK(Keys::valueAt(*clip(vid), AnimParam::FxBrightness, 40) == 5.0);

    // Speichern -> Laden identisch
    const QString path = tmp.filePath("fx.schneidi");
    QString err;
    CHECK(ProjectFile::save(project.data(), path, &err));
    ProjectData loaded;
    CHECK(ProjectFile::load(path, &loaded, &err));
    const Clip* lc = TimelineOps::findClip(loaded.timeline, vid);
    CHECK(lc && lc->effects == clip(vid)->effects);
    CHECK(lc && lc->keys == clip(vid)->keys);

    // Teilen übernimmt Effekte, Keyframes verteilt
    editor.splitAtPlayhead(50);
    const Track& t0 = project.timeline().video[0];
    CHECK(t0.clips.size() == 2 && t0.clips[1].effects == t0.clips[0].effects);
    CHECK(Keys::valueAt(t0.clips[1], AnimParam::FxBrightness, 0) == Keys::valueAt(*lc, AnimParam::FxBrightness, 50));
    project.undoStack()->undo();

    // Entfernen löscht auch die Keyframes der Effekt-Parameter
    editor.removeEffect({vid}, "color");
    CHECK(!EffectRegistry::has(*clip(vid), "color") && !Keys::animated(*clip(vid), AnimParam::FxBrightness));
    project.undoStack()->undo();
    CHECK(EffectRegistry::has(*clip(vid), "color") && Keys::animated(*clip(vid), AnimParam::FxBrightness));

    // Inspector zeichnen (Bild wird nicht gespeichert)
    Inspector insp(&editor);
    insp.resize(380, 1250);
    sel.set({vid});
    insp.setPlayhead(40);
    insp.show();
    app.processEvents();
    CHECK(!insp.grab().isNull());
    // Reihenfolge ändern (Unschärfe zuerst) -> Inspector sortiert um
    editor.modifyClips({vid}, "t", [](Clip& x) { std::swap(x.effects[0], x.effects[1]); });
    app.processEvents();
    CHECK(!insp.grab().isNull());
    // Effects Library: Open FX -> Filter
    EffectsLibrary lib;
    lib.resize(420, 260);
    auto* tree = lib.findChild<QTreeWidget*>();
    tree->setCurrentItem(tree->topLevelItem(1)->child(0));
    lib.show();
    app.processEvents();
    CHECK(!lib.grab().isNull());

    // Filter auf die Timeline ziehen: Audiospur nimmt nichts an, Videoclip bekommt den Effekt (ein Undo-Schritt)
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

    // Trim-Werkzeug: Spuren kommen während des Ziehens dazu (z. B. Tastenkürzel) -> Vorschau passt nicht mehr zu den
    // Zeilen; Zeichnen darf nicht über die Spuren der alten Vorschau hinaus lesen
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

    // Zahlenfeld ziehen: langsame Bewegung (1 px je Ereignis) und Shift (fein) ändern den Wert in beide Richtungen,
    // auch wenn ein Schritt kleiner ist als die angezeigte Genauigkeit (Rest wird gesammelt statt verworfen)
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
        ScrubField soft(0, 100, 0.5, 0); // wie Weichheit/Rand einer Wischblende
        soft.setValue(50);
        drag(soft, {40, 44}); // ab 3 px wird gezogen: +2
        CHECK_EQ(soft.value(), 52.0);
        drag(soft, pixels(44, 34)); // 10 px nach links, einzeln: -5
        CHECK_EQ(soft.value(), 47.0);
        ScrubField outline(0, 40, 0.1, 1); // wie Umrandung eines Titels
        outline.setValue(4);
        drag(outline, pixels(10, 60), Qt::ShiftModifier); // fein: 0,01 je px
        CHECK(outline.value() > 4.3);
    }
    return Check::result();
}
