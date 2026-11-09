// SPDX-License-Identifier: AGPL-3.0-only
// Placing only the picture or only the sound of a file: Editor::addMediaAt / placeSourceRange with PlaceParts, the
// sticky "leave out sound" setting, F9 (sourceEdit) with it, and a timeline drop with Shift / Ctrl / a Viewer handle
// (offscreen). The Media Pool keeps the file's sound in every case; each placement is one undo step.
#include "check.h"

#include "app/Theme.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "ui/MediaPool.h"
#include "ui/Viewer.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QDropEvent>
#include <QMimeData>
#include <QUndoStack>

namespace {

int clipCount(const QVector<Track>& tracks)
{
    int n = 0;
    for (const Track& t : tracks) n += t.clips.size();
    return n;
}

struct Fixture {
    Project project;
    Selection sel;
    Editor editor{&project, &sel};
    Fixture()
    {
        project.addMedia(MediaInfo{"/x/a.mp4", "a.mp4", 100, true, true, false});  // picture + sound
        project.addMedia(MediaInfo{"/x/m.wav", "m.wav", 100, false, true, false}); // sound only
        project.undoStack()->clear();
    }
    int videos() const { return clipCount(project.timeline().video); }
    int sounds() const { return clipCount(project.timeline().audio); }
};

void testCore()
{
    {
        Fixture f;
        f.editor.addMediaAt({"/x/a.mp4"}, 10, 0, PlaceParts::Picture);
        CHECK_EQ(f.videos(), 1);
        CHECK_EQ(f.sounds(), 0);
        CHECK_EQ(f.project.timeline().video[0].clips[0].linkId, 0); // nothing to be linked with
        CHECK_EQ(f.project.undoStack()->count(), 1);
        CHECK(f.project.mediaInfo("/x/a.mp4")->hasAudio); // the pool keeps the sound
        f.project.undoStack()->undo();
        CHECK_EQ(f.videos() + f.sounds(), 0);
        f.editor.addMediaAt({"/x/a.mp4"}, 10, 0, PlaceParts::Sound);
        CHECK_EQ(f.videos(), 0);
        CHECK_EQ(f.sounds(), 1);
        // A sound-only file asked for its picture: nothing (and no undo step)
        const int steps = f.project.undoStack()->count();
        f.editor.addMediaAt({"/x/m.wav"}, 200, 0, PlaceParts::Picture);
        CHECK_EQ(f.project.undoStack()->count(), steps);
    }
    {
        Fixture f;
        CHECK(f.editor.placeSourceRange("/x/a.mp4", 0, 49, 0, 0, 1.0, PlaceParts::Picture) == 50);
        CHECK_EQ(f.videos(), 1);
        CHECK_EQ(f.sounds(), 0);
        CHECK_EQ(f.project.undoStack()->count(), 1);
    }
    {
        // The sticky setting: a file with a picture comes without its sound, a sound file keeps its sound,
        // an explicit Sound still places the sound
        Fixture f;
        CHECK(!f.editor.leaveOutSound()); // default: the sound comes along
        f.editor.setLeaveOutSound(true);
        f.editor.addMediaAt({"/x/a.mp4"}, 0);
        CHECK_EQ(f.videos(), 1);
        CHECK_EQ(f.sounds(), 0);
        f.editor.addMediaAt({"/x/m.wav"}, 200);
        CHECK_EQ(f.sounds(), 1);
        f.editor.addMediaAt({"/x/a.mp4"}, 400, 0, PlaceParts::Sound);
        CHECK_EQ(f.sounds(), 2);
        CHECK_EQ(f.videos(), 1);
        // F9 with it: the picture only
        const int end = f.editor.sourceEdit(Editor::SourceEditMode::Overwrite, "/x/a.mp4", 0, 600);
        CHECK(end > 600);
        CHECK_EQ(f.videos(), 2);
        CHECK_EQ(f.sounds(), 2);
        // Off again: both
        f.editor.setLeaveOutSound(false);
        f.editor.addMediaAt({"/x/a.mp4"}, 800);
        CHECK_EQ(f.videos(), 3);
        CHECK_EQ(f.sounds(), 3);
    }
}

// Dropping onto the timeline: Shift = picture only, Ctrl = sound only, a Viewer handle's part wins
void testDrop()
{
    Fixture f;
    TimelineView tv(&f.editor);
    tv.resize(1200, 400);
    tv.show();
    QApplication::processEvents();
    QVector<PlaceParts> got;
    QObject::connect(&tv, &TimelineView::dropRequested,
                     [&](const QStringList&, int, int, PlaceParts parts) { got << parts; });
    QObject::connect(&tv, &TimelineView::rangeDropRequested,
                     [&](const QString&, int, int, int, int, PlaceParts parts) { got << parts; });
    const auto drop = [&](Qt::KeyboardModifiers mods, const QByteArray& part = {}) {
        QMimeData mime;
        mime.setData(MediaPool::MimeType, "/x/a.mp4");
        if (!part.isEmpty()) {
            mime.setData(Viewer::RangeMimeType, "0 49");
            mime.setData(Viewer::PartsMimeType, part);
        }
        const QPoint pt(TimelineView::kHeaderW + 100, 60);
        QDragEnterEvent enter(pt, Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(&tv, &enter);
        QDragMoveEvent move(pt, Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(&tv, &move);
        QDropEvent dropped(QPointF(pt), Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(&tv, &dropped);
    };
    drop(Qt::NoModifier);
    drop(Qt::ShiftModifier);
    drop(Qt::ControlModifier);
    drop(Qt::NoModifier, "picture");
    drop(Qt::NoModifier, "sound");
    CHECK(got == (QVector<PlaceParts>{PlaceParts::Both, PlaceParts::Picture, PlaceParts::Sound, PlaceParts::Picture,
                                      PlaceParts::Sound}));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("place_parts");
    I18n::install("de");
    Theme::apply(app);
    testCore();
    testDrop();
    return Check::result();
}
