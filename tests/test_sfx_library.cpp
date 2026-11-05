// SPDX-License-Identifier: AGPL-3.0-only
// Effects Library › "Klänge" (0.17, package I in the app): the built-in sounds (recordings and computed ones), the
// installed packs and the user's own folder are listed; an entry drags as op 'sfx' fields and plays as a file; a
// double-click only plays it (nothing is placed); a sound dropped into the timeline goes through op 'sfx' (its moment
// where it was dropped, own SFX track) as one undo step named after it, undo takes it out again; one dropped onto it
// is left out with the reason in the status bar and changes nothing, one 1 s after it is placed (no sparing rules
// by hand). Nothing is ever placed by itself.
// SFX_LIBRARY_DUMP=<file.png>: picture of the library on "Klänge".
#include "check.h"

#include "app/MainWindow.h"
#include "cli/Live.h"
#include "core/EffectFolders.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/SfxSynth.h"
#include "core/TimelineOps.h"
#include "engine/Engine.h"
#include "ui/EffectsLibrary.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMimeData>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QUndoStack>

namespace {

QTreeWidgetItem* category(QTreeWidget* tree, const QString& text, QTreeWidgetItem* in = nullptr)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->text(0) == text && (!in || (*it)->parent() == in)) return *it;
    return nullptr;
}

QStringList names(QListWidget* list)
{
    QStringList out;
    for (int i = 0; i < list->count(); ++i) out << list->item(i)->text();
    return out;
}

QListWidgetItem* entry(QListWidget* list, const QString& text)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == text) return list->item(i);
    return nullptr;
}

// What the list hands a drag of this entry
QByteArray dragData(QListWidget* list, QListWidgetItem* it)
{
    // Through the model (it asks the list's own mimeData), not a cast to a helper type the list is not
    std::unique_ptr<QMimeData> m(list->model()->mimeData({list->model()->index(list->row(it), 0)}));
    if (!m) return {};
    return m->data(EffectsLibrary::SoundMimeType);
}

QVector<Clip> sfxClips(Project* p)
{
    QVector<Clip> out;
    for (const Track& t : p->timeline().audio)
        if (t.name == "SFX") out += t.clips;
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("sfx_library");
    I18n::install("de");
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    Check::initEngine();
    const QString picture = Check::makeMedia(tmp.filePath("picture.mkv"), {"-f", "lavfi", "-i", "color=c=0x305070:s=320x180:d=20:r=25",
                                                                           "-c:v", "ffv1", "-pix_fmt", "bgr0"});
    if (picture.isEmpty()) return Check::skip("ffmpeg could not make the media");
    // A sound of the user's own in the effects folder ("Sounds")
    EffectFolders::ensure();
    const QString own = QDir(EffectFolders::soundDir()).filePath("Gong.wav");
    CHECK(SfxSynth::writeWav(own, SfxSynth::make(SfxSynth::Kind::Click, 1).samples));

    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return Check::result();
    MainWindow w(&engine);
    w.disableAutosave();
    w.resize(1400, 900);
    w.show();
    w.setUserRunner([&w](const QString& command, const QJsonObject& args, const QString& undoText) {
        return Cli::Live::runForUser(&w, command, args, undoText);
    });
    Project* p = w.liveProject();
    {
        ProjectData d;
        d.format.width = 320;
        d.format.height = 180;
        d.format.rate = {25, 1};
        TimelineOps::ensureTracks(d.timeline, TrackKind::Video, 1);
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        Clip v;
        v.id = 1;
        v.mediaPath = picture;
        v.in = 0;
        v.out = 499; // 20 s
        d.timeline.video[0].clips << v;
        d.lastClipId = 1;
        p->load(d);
        p->addMedia(Engine::probe(p->format(), picture));
    }

    auto* library = w.findChild<EffectsLibrary*>();
    if (!CHECK(library)) return Check::result();
    auto* tree = library->findChild<QTreeWidget*>();
    auto* list = library->findChild<QListWidget*>();
    if (!CHECK(tree && list)) return Check::result();
    QTreeWidgetItem* sounds = category(tree, T("Klänge"));
    if (!CHECK(sounds)) return Check::result();
    tree->setCurrentItem(sounds);
    const QStringList all = names(list);
    // Built in: recordings (whoosh) and computed ones (riser), the groups as headers, the own file
    CHECK(all.contains(T("Mitgeliefert")) && all.contains(T("Eigene")));
    CHECK(all.contains(T("%1 %2 (Aufnahme)").arg(T("Whoosh")).arg(1)));
    CHECK(all.contains(T("%1 %2 (berechnet)").arg(T("Riser")).arg(1)));
    CHECK(all.contains("Gong"));
    if (const QString dump = qEnvironmentVariable("SFX_LIBRARY_DUMP"); !dump.isEmpty()) {
        // The Effects panel is closed by default: the library on its own, at a panel's size
        EffectsLibrary shown;
        shown.resize(560, 520);
        shown.show();
        if (QTreeWidgetItem* s = category(shown.findChild<QTreeWidget*>(), T("Klänge"))) shown.findChild<QTreeWidget*>()->setCurrentItem(s);
        QApplication::processEvents();
        CHECK(shown.grab().save(dump));
    }
    // Packs: none installed - its subcategory says so
    if (QTreeWidgetItem* packs = category(tree, T("Pakete"), sounds); CHECK(packs)) {
        tree->setCurrentItem(packs);
        CHECK_EQ(names(list), QStringList{T("Noch keine Klangpakete installiert")});
        tree->setCurrentItem(sounds);
    }

    // Drag data: the op's fields; the file it plays
    QListWidgetItem* whoosh = entry(list, T("%1 %2 (Aufnahme)").arg(T("Whoosh")).arg(1));
    QListWidgetItem* riser = entry(list, T("%1 %2 (berechnet)").arg(T("Riser")).arg(2));
    QListWidgetItem* gong = entry(list, "Gong");
    if (!CHECK(whoosh && riser && gong)) return Check::result();
    const QByteArray whooshData = dragData(list, whoosh), riserData = dragData(list, riser), gongData = dragData(list, gong);
    CHECK_EQ(QJsonDocument::fromJson(whooshData)["kind"].toString(), QString("whoosh"));
    CHECK_EQ(QJsonDocument::fromJson(riserData)["seed"].toInt(), 2);
    CHECK(QJsonDocument::fromJson(riserData)["length"].toString().endsWith("s"));
    CHECK_EQ(QJsonDocument::fromJson(gongData)["kind"].toString(), own);
    CHECK(QFileInfo(EffectsLibrary::soundFile(whooshData)).size() > 0);
    CHECK(SfxSynth::parseCachedFile(EffectsLibrary::soundFile(riserData)).has_value()); // computed, in the cache
    CHECK_EQ(EffectsLibrary::soundFile(gongData), own);

    // Double-click: heard only (the Viewer gets the file), nothing placed
    QString heard;
    QObject::connect(library, &EffectsLibrary::soundPreviewRequested, [&heard](const QString& f) { heard = f; });
    const int undo0 = p->undoStack()->index();
    emit list->itemDoubleClicked(gong);
    CHECK_EQ(heard, own);
    CHECK_EQ(p->undoStack()->index(), undo0);
    CHECK(sfxClips(p).isEmpty());

    // Dropped into the timeline at 8 s: op sfx, the whoosh's peak on 8 s, one undo step named after it
    auto* tv = w.findChild<TimelineView*>();
    if (!CHECK(tv)) return Check::result();
    emit tv->soundDropRequested(whooshData, 200);
    CHECK(sfxClips(p).isEmpty()); // after the drop event, not inside it
    QApplication::processEvents();
    QVector<Clip> placed = sfxClips(p);
    if (CHECK_EQ(placed.size(), 1)) {
        const double anchor = SfxSynth::anchorOf(SfxSynth::Kind::Whoosh, 1, 0);
        CHECK_EQ(placed[0].start, int(std::lround(200 - anchor * 25)));
    }
    CHECK_EQ(p->undoStack()->index(), undo0 + 1);
    CHECK_EQ(p->undoStack()->undoText(), T("Klang einfügen"));
    // One more on the whoosh itself: a sound is there, left out with the reason, nothing changed
    emit tv->soundDropRequested(gongData, 200);
    QApplication::processEvents();
    CHECK_EQ(sfxClips(p).size(), 1);
    CHECK_EQ(p->undoStack()->index(), undo0 + 1);
    CHECK(w.statusBar()->currentMessage().contains(T("an dieser Stelle liegt schon ein Klang")));
    // ... and one 1 s later, after the whoosh's end: placed - the sparing rules of automatic placing (1.5 s apart,
    // a few per minute) are not for a sound put there by hand
    emit tv->soundDropRequested(gongData, 225);
    QApplication::processEvents();
    CHECK_EQ(sfxClips(p).size(), 2);
    CHECK_EQ(p->undoStack()->index(), undo0 + 2);
    // Undo twice: gone again
    p->undoStack()->undo();
    p->undoStack()->undo();
    CHECK(sfxClips(p).isEmpty());
    // The user's own file 4 s later: placed from its start (a file's moment is its start)
    emit tv->soundDropRequested(gongData, 300);
    QApplication::processEvents();
    placed = sfxClips(p);
    if (CHECK_EQ(placed.size(), 1)) CHECK_EQ(placed[0].start, 300);
    return Check::result();
}
