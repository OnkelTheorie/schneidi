// SPDX-License-Identifier: AGPL-3.0-only
// Test tracks: add/delete, lock, rename, color, ripple across all unlocked tracks, track header operation (offscreen)
#include "check.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "app/Theme.h"
#include "engine/Engine.h"
#include "ui/Mixer.h"
#include "ui/timeline/TimelineView.h"

#include <QApplication>
#include <QLabel>
#include <QRegularExpression>
#include <QContextMenuEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

static Clip mk(int id, int start, int len, int link = 0, const QString& path = "/x/a.mp4")
{
    Clip c;
    c.id = id; c.start = start; c.in = 0; c.out = len - 1; c.linkId = link; c.mediaPath = path;
    return c;
}

static const Clip* F(const Project& p, int id) { return TimelineOps::findClip(p.timeline(), id); }

// V1: 1[0,100) 2[100,200) 3[200,300); A1 Partner 11,12,13; A2: 21[0,50) 22[250,350)
static void setup(Project& p)
{
    p.reset();
    p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
    p.addMedia({"/x/m.mp3", "m.mp3", 1000, false, true, false});
    p.edit("setup", [](Timeline& tl) {
        tl.video[0].clips = {mk(1, 0, 100, 1), mk(2, 100, 100, 2), mk(3, 200, 100, 3)};
        tl.audio[0].clips = {mk(11, 0, 100, 1), mk(12, 100, 100, 2), mk(13, 200, 100, 3)};
        tl.audio[1].clips = {mk(21, 0, 50, 0, "/x/m.mp3"), mk(22, 250, 100, 0, "/x/m.mp3")};
    });
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("tracks");
    Project p;
    Selection sel;
    Editor ed(&p, &sel);

    // --- Ripple delete across all tracks
    setup(p);
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 3)->start == 100 && F(p, 13)->start == 100);
    CHECK(F(p, 22)->start == 150);
    // locked A2 stays put
    setup(p);
    ed.toggleTrackLock({TrackKind::Audio, 1});
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 3)->start == 100 && F(p, 22)->start == 250);
    // Track on which moving up would overwrite stays put
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.audio[1].clips = {mk(21, 0, 150, 0, "/x/m.mp3"), mk(22, 160, 100, 0, "/x/m.mp3")}; });
    sel.set({2, 12});
    ed.rippleDeleteSelection();
    CHECK(F(p, 22)->start == 160);

    // --- Trim mode Ripple
    using namespace TimelineOps;
    setup(p);
    auto e = ed.trimEdit(TrimKind::Ripple, 1, Edge::End);
    CHECK(e.ids.size() == 2);
    ed.applyTrimEdit(e, -30);
    CHECK(F(p, 1)->end() == 70 && F(p, 2)->start == 70 && F(p, 12)->start == 70 && F(p, 22)->start == 220);
    // Room on A2 limits the shortening
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.audio[1].clips = {mk(21, 0, 120, 0, "/x/m.mp3"), mk(22, 130, 100, 0, "/x/m.mp3")}; });
    e = ed.trimEdit(TrimKind::Ripple, 1, Edge::End);
    CHECK(ed.clampTrimEdit(e, -30) == -10);
    ed.toggleTrackLock({TrackKind::Audio, 1}); // locked -> no longer limits, stays put
    CHECK(ed.clampTrimEdit(e, -30) == -30);
    ed.applyTrimEdit(e, -30);
    CHECK(F(p, 22)->start == 130 && F(p, 2)->start == 70);
    // Start (clip stays, content moves) extended: everything moves right
    setup(p);
    p.edit("x", [](Timeline& tl) { tl.video[0].clips[1].in = 50; tl.video[0].clips[1].out = 149; tl.audio[0].clips[1].in = 50; tl.audio[0].clips[1].out = 149; });
    e = ed.trimEdit(TrimKind::Ripple, 2, Edge::Start);
    ed.applyTrimEdit(e, -20);
    CHECK(F(p, 2)->length() == 120 && F(p, 3)->start == 220 && F(p, 22)->start == 270);

    // --- Speed with ripple
    setup(p);
    ed.setClipSpeed({1}, {0.5, false, false, true}, true);
    CHECK(F(p, 1)->length() == 200 && F(p, 2)->start == 200 && F(p, 22)->start == 350);

    // --- Lock: nothing changeable, not selectable
    setup(p);
    ed.toggleTrackLock({TrackKind::Video, 0});
    CHECK(ed.withLinked({11}) == QVector<int>{11});
    ed.selectAll();
    CHECK(!sel.contains(1) && sel.contains(11));
    sel.set({1, 2});
    ed.deleteSelection();
    CHECK(F(p, 1) && F(p, 2));
    ed.modifyClips({1}, "x", [](Clip& c) { c.enabled = false; });
    CHECK(F(p, 1)->enabled);
    ed.bladeAt(1, 50);
    CHECK(F(p, 1)->length() == 100);
    ed.setClipFade(1, Edge::Start, 10);
    CHECK(F(p, 1)->fadeIn == 0);
    ed.addTransitionAt(1, 2, {});
    CHECK(F(p, 1)->transOut == 0);
    ed.trimClip(1, Edge::End, -10);
    CHECK(F(p, 1)->length() == 100);
    // Partner on A1 can be trimmed alone
    ed.trimClip(11, Edge::End, -10);
    CHECK(F(p, 11)->length() == 90 && F(p, 1)->length() == 100);
    // Title: locked V1 is skipped
    ed.addTitle(500);
    CHECK(p.timeline().video[0].clips.size() == 3 && p.timeline().video[1].clips.size() == 1);
    // Moving from V2 onto locked V1 does not work
    const int title = p.timeline().video[1].clips[0].id;
    ed.moveClips({title}, 0, TrackKind::Video, -1);
    CHECK(p.timeline().video[1].clips.size() == 1);
    // Do not paste onto a locked track
    sel.set({2});
    ed.copySelection(); // V1 locked -> copying works (only reads)
    ed.paste(600);
    CHECK(p.timeline().video[0].clips.size() == 3);
    // Place media onto locked V1: only audio lands on A1
    ed.addMediaAt({"/x/a.mp4"}, 700, 0);
    CHECK(p.timeline().video[0].clips.size() == 3 && p.timeline().audio[0].clips.size() == 4);
    // Locking a track deselects the clips of the track
    ed.toggleTrackLock({TrackKind::Video, 0});
    sel.set({1, 11});
    ed.toggleTrackLock({TrackKind::Video, 0});
    CHECK(sel.ids() == QSet<int>{11});
    // Undo reverts the lock
    p.undoStack()->undo();
    CHECK(!p.timeline().video[0].locked);

    // --- Lock key: the tracks of the selection, without one the target tracks; one undo step each
    {
        const ProjectData before = p.data(); // the rest of the test goes on from here
        setup(p);
        const auto locked = [&] {
            QString s;
            for (const Track& t : p.timeline().video) s += t.locked ? 'V' : '-';
            for (const Track& t : p.timeline().audio) s += t.locked ? 'A' : '-';
            for (const SubtitleTrack& t : p.timeline().subtitles) s += t.locked ? 'S' : '-';
            return s;
        };
        QUndoStack* undo = p.undoStack();
        int steps = undo->count();
        sel.set({2, 12}); // linked pair on V1 + A1
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V-A-"));
        CHECK(sel.isEmpty()); // the clips left the selection with their tracks
        CHECK_EQ(undo->count(), steps + 1);
        CHECK_EQ(undo->undoText(), T("Spuren sperren"));
        // No selection: target tracks (V1 + A2) - one of them unlocked -> both locked, then all locked -> unlocked
        ed.setTargetTracks(0, 1);
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V-AA"));
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("--A-"));
        CHECK_EQ(undo->undoText(), T("Spuren entsperren"));
        undo->undo();
        CHECK_EQ(locked(), QString("V-AA"));
        undo->undo();
        undo->undo();
        CHECK_EQ(locked(), QString("----"));
        // One track: the text of the lock button; a selected transition counts as selection
        sel.set({21});
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("---A"));
        CHECK_EQ(undo->undoText(), T("Spur sperren"));
        sel.setTransition({1, 2});
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V--A"));
        CHECK(sel.transition().isNull());
        // Target tracks that do not exist: nothing (no undo step)
        ed.setTargetTracks(5, 5);
        sel.clear();
        steps = undo->count();
        ed.toggleSelectionTracksLock();
        CHECK_EQ(undo->count(), steps);
        ed.setTargetTracks(0, 0);
        // A selected subtitle: its subtitle track (with clips: theirs too)
        ed.addSubtitleTrack();
        const int cue = ed.addSubtitle(10, 0);
        CHECK(sel.ids() == QSet<int>{cue});
        sel.set({cue, 12});
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V-AAS"));
        CHECK(sel.isEmpty());
        // The lock button stays one track (shares the code): one step, its own text
        ed.toggleTrackLock({TrackKind::Audio, 0});
        CHECK_EQ(locked(), QString("V--AS"));
        CHECK_EQ(undo->undoText(), T("Spur entsperren"));
        ed.toggleSubtitleTrackLock(0);
        CHECK_EQ(locked(), QString("V--A-"));
        p.load(before);
        sel.clear();
    }

    // --- Lock key twice: it locks the selection's tracks (the selection empties with them), pressed again right away
    // it unlocks those tracks - not the target tracks (before, the second press locked V1 + A1 as well)
    {
        const ProjectData before = p.data();
        setup(p);
        const auto locked = [&] {
            QString s;
            for (const Track& t : p.timeline().video) s += t.locked ? 'V' : '-';
            for (const Track& t : p.timeline().audio) s += t.locked ? 'A' : '-';
            return s;
        };
        QUndoStack* undo = p.undoStack();
        ed.setTargetTracks(0, 0);
        sel.set({21}); // on A2, not a target track
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("---A"));
        CHECK(sel.isEmpty());
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("----"));
        CHECK_EQ(undo->undoText(), T("Spur entsperren"));
        // Once more without a selection: nothing to toggle back any more -> the target tracks
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V-A-"));
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("----"));
        // Something else changed in between (another undo step, other target tracks): the target tracks
        sel.set({21});
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("---A"));
        ed.toggleTrackMute({TrackKind::Audio, 0});
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("V-AA"));
        undo->undo();
        undo->undo();
        CHECK_EQ(locked(), QString("---A"));
        ed.setTargetTracks(1, 0);
        ed.toggleSelectionTracksLock();
        CHECK_EQ(locked(), QString("-VAA"));
        ed.setTargetTracks(0, 0);
        p.load(before);
        sel.clear();
    }

    // --- Rename / color
    ed.renameTrack({TrackKind::Audio, 1}, "  Musik  ");
    CHECK(p.timeline().audio[1].name == "Musik");
    CHECK(trackDisplayName(p.timeline().audio[0], {TrackKind::Audio, 0}) == "Audio 1");
    ed.renameTrack({TrackKind::Audio, 1}, "Audio 2"); // default name = automatic
    CHECK(p.timeline().audio[1].name.isEmpty());
    ed.renameTrack({TrackKind::Audio, 1}, "Musik");
    ed.setTrackColor({TrackKind::Audio, 1}, "violet");
    ed.setTrackColor({TrackKind::Video, 0}, "nix");
    CHECK(p.timeline().audio[1].color == "violet" && p.timeline().video[0].color.isEmpty());
    ed.toggleTrackLock({TrackKind::Video, 1});

    // --- Save/load
    ProjectData d = p.data();
    const QByteArray json = ProjectFile::toJson(d, "/x/projekt.schneidi");
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/projekt.schneidi", &back, &err));
    CHECK(back.timeline.audio[1].name == "Musik" && back.timeline.audio[1].color == "violet");
    CHECK(back.timeline.video[1].locked && !back.timeline.video[0].locked && back.timeline.video[0].name.isEmpty());
    CHECK(json.contains("\"name\": \"A2\"")); // older versions see the abbreviation
    // old file: only "name": "V1"
    QByteArray old = json;
    old.replace("\"label\": \"Musik\",", "");
    CHECK(ProjectFile::fromJson(old, "/x/projekt.schneidi", &back, &err) && back.timeline.audio[1].name.isEmpty());

    // --- Add/delete tracks (right-click on the track header)
    {
        Engine engine;
        Mixer mixer(&p, &engine);
        // Mixer channel strips: abbreviation and track name (tooltip) in track order
        auto stripNames = [&] {
            QStringList out;
            for (QLabel* l : mixer.findChildren<QLabel*>())
                if (QRegularExpression("^A\\d+$").match(l->text()).hasMatch()) out << l->text() + "=" + l->toolTip().section(" – ", 0, 0); // without solo hint
            out.sort(); // A1, A2, A3 (findChildren order is not defined)
            return out;
        };
        setup(p);
        ed.renameTrack({TrackKind::Audio, 1}, "Musik");
        p.edit("x", [](Timeline& tl) { tl.audio[1].volumeDb = -6; tl.audio[1].solo = true; tl.audio[0].muted = true; });
        ed.setTargetTracks(1, 1);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        // Audio: above A1 = index 0 -> old tracks move up by one
        ed.addTrack(TrackKind::Audio, 0);
        const Timeline& t1 = p.timeline();
        CHECK(t1.audio.size() == 3 && t1.audio[0].clips.isEmpty() && t1.audio[0].name.isEmpty() && !t1.audio[0].muted);
        CHECK(t1.audio[1].muted && t1.audio[1].clips.size() == 3 && t1.audio[1].clips[0].id == 11);
        CHECK(t1.audio[2].name == "Musik" && t1.audio[2].volumeDb == -6 && t1.audio[2].solo);
        CHECK(ed.targetAudioTrack() == 2 && ed.targetVideoTrack() == 1);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Audio 2", "A3=Musik"}));
        for (QLabel* l : mixer.findChildren<QLabel*>()) // new A1 is muted by the solo of A3
            if (l->text() == "A1") CHECK(l->toolTip().contains("Solo"));
        // one undo step
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() == 2 && p.timeline().audio[1].name == "Musik"
              && p.timeline().audio[0].clips.size() == 3);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        p.undoStack()->redo();
        CHECK(p.timeline().audio.size() == 3 && p.timeline().audio[2].name == "Musik");
        // Video above V2 = very top (index 2); target track V2 stays
        ed.addTrack(TrackKind::Video, 2);
        CHECK(p.timeline().video.size() == 3 && p.timeline().video[0].clips.size() == 3 && ed.targetVideoTrack() == 1);
        // Project file round trip
        ProjectData back2;
        QString err2;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &back2, &err2));
        CHECK(back2.timeline.video.size() == 3 && back2.timeline.audio.size() == 3);
        CHECK(back2.timeline.audio[2].name == "Musik" && back2.timeline.audio[2].solo && back2.timeline.audio[1].muted);

        // Delete: clips of the track go with it, selection on it is cleared
        sel.set({1, 11, 21});
        ed.removeTrack({TrackKind::Audio, 1}); // the track with 11,12,13
        CHECK(p.timeline().audio.size() == 2 && !F(p, 11) && F(p, 1) && p.timeline().audio[1].name == "Musik");
        CHECK(sel.ids() == QSet<int>({1, 21}));
        CHECK(ed.targetAudioTrack() == 1); // follows "Musik"
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik"}));
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() == 3 && F(p, 11) && p.timeline().audio[1].muted);
        p.undoStack()->redo();
        CHECK(!F(p, 11));
        // locked track and last track of a type cannot be deleted
        ed.toggleTrackLock({TrackKind::Audio, 1});
        CHECK(!ed.canRemoveTrack({TrackKind::Audio, 1}));
        ed.removeTrack({TrackKind::Audio, 1});
        CHECK(p.timeline().audio.size() == 2);
        ed.toggleTrackLock({TrackKind::Audio, 1});
        ed.removeTrack({TrackKind::Audio, 1});
        CHECK(p.timeline().audio.size() == 1 && !ed.canRemoveTrack({TrackKind::Audio, 0}));
        ed.removeTrack({TrackKind::Audio, 0});
        CHECK(p.timeline().audio.size() == 1 && ed.targetAudioTrack() == 0);
        CHECK(!ed.canRemoveTrack({TrackKind::Video, 5}));

        // --- Reorder tracks (dragging at the track header)
        setup(p);
        ed.renameTrack({TrackKind::Audio, 1}, "Musik");
        p.edit("x", [](Timeline& tl) { tl.audio[1].volumeDb = -6; tl.audio[1].pan = 30; tl.audio[0].muted = true; });
        ed.addTrack(TrackKind::Audio, 2);
        ed.setTargetTracks(0, 0);
        const int steps = p.undoStack()->count();
        ed.moveTrack(TrackKind::Audio, 0, 2); // A1 to the very bottom
        const Timeline& t2 = p.timeline();
        CHECK(t2.audio.size() == 3 && t2.audio[0].name == "Musik" && t2.audio[0].volumeDb == -6 && t2.audio[0].pan == 30);
        CHECK(t2.audio[1].clips.isEmpty() && t2.audio[2].muted && t2.audio[2].clips.size() == 3 && F(p, 22)->start == 250);
        CHECK(ed.targetAudioTrack() == 2);
        CHECK(stripNames() == QStringList({"A1=Musik", "A2=Audio 2", "A3=Audio 3"}));
        CHECK(ed.withLinked({1}).contains(11)); // link stays
        CHECK(p.undoStack()->count() == steps + 1);
        p.undoStack()->undo();
        CHECK(p.timeline().audio[1].name == "Musik" && p.timeline().audio[0].muted && p.timeline().audio[0].clips.size() == 3);
        CHECK(stripNames() == QStringList({"A1=Audio 1", "A2=Musik", "A3=Audio 3"}));
        p.undoStack()->redo();
        CHECK(p.timeline().audio[0].name == "Musik");
        // Video: V1 up
        ed.setTargetTracks(1, 2);
        ed.moveTrack(TrackKind::Video, 0, 1);
        CHECK(p.timeline().video[1].clips.size() == 3 && p.timeline().video[0].clips.isEmpty() && ed.targetVideoTrack() == 0);
        // same position / invalid: no undo step
        ed.moveTrack(TrackKind::Video, 1, 1);
        ed.moveTrack(TrackKind::Audio, 5, 0);
        CHECK(p.undoStack()->count() == steps + 2);
        // Project file round trip
        ProjectData back3;
        QString err3;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(p.data(), "/x/p.schneidi"), "/x/p.schneidi", &back3, &err3));
        CHECK(back3.timeline.audio[0].name == "Musik" && back3.timeline.audio[0].pan == 30 && back3.timeline.audio[2].muted);
        CHECK(back3.timeline.audio[2].clips.size() == 3 && back3.timeline.video[1].clips.size() == 3);
    }

    // --- UI: track header, clicks
    setup(p);
    ed.setTrackColor({TrackKind::Audio, 1}, "violet");
    ed.renameTrack({TrackKind::Video, 1}, "Titel und Grafik");
    sel.clear();
    TimelineView view(&ed);
    view.resize(1400, 320);
    view.show();
    QApplication::processEvents();
    auto click = [&](QPoint pt) {
        QMouseEvent pr(QEvent::MouseButtonPress, pt, view.mapToGlobal(pt), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(&view, &pr);
        QMouseEvent rl(QEvent::MouseButtonRelease, pt, view.mapToGlobal(pt), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(&view, &rl);
    };
    const int kH = TimelineView::kHeaderW;
    // V1 row y 94..158, lock at x kH-54..kH-34, y+6
    click({kH - 44, 94 + 12});
    CHECK(p.timeline().video[0].locked);
    click({kH + 50 * 4, 120}); // clip 1 on V1
    CHECK(sel.isEmpty());
    click({kH + 50 * 4, 190}); // clip 11 on A1
    CHECK(sel.ids() == QSet<int>{11});
    // Double-click on the name of A1 (y 164)
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPoint(60, 164 + 12), view.mapToGlobal(QPoint(60, 176)), Qt::LeftButton, Qt::LeftButton, {});
    QApplication::sendEvent(&view, &dbl);
    QLineEdit* le = view.findChild<QLineEdit*>();
    if (!CHECK(le && le->isVisible() && le->text() == "Audio 1")) return Check::result();
    le->setText("Dialog");
    QTest::keyClick(le, Qt::Key_Return);
    CHECK(p.timeline().audio[0].name == "Dialog" && !le->isVisible());
    // Esc cancels
    QApplication::sendEvent(&view, &dbl);
    le->setText("Quatsch");
    QTest::keyClick(le, Qt::Key_Escape);
    CHECK(p.timeline().audio[0].name == "Dialog" && !le->isVisible());

    // Right-click on the track header: trigger menu item as soon as the menu is open
    auto headerMenu = [&](QPoint pt, const QString& text) {
        bool found = false, enabled = false;
        QTimer::singleShot(0, [&] {
            if (auto* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : m->actions())
                    if (a->text() == text) {
                        found = true;
                        enabled = a->isEnabled();
                        if (enabled) a->trigger();
                    }
                m->close();
            }
        });
        QContextMenuEvent ce(QContextMenuEvent::Mouse, pt, view.mapToGlobal(pt));
        QApplication::sendEvent(&view, &ce);
        return found && enabled;
    };
    // A1 (y 164): below = new A2, "Musik" moves to A3 (audio counts from the top)
    ed.renameTrack({TrackKind::Audio, 1}, "Musik");
    CHECK(headerMenu({60, 170}, "Spur unterhalb hinzufügen"));
    CHECK(p.timeline().audio.size() == 3 && p.timeline().audio[0].name == "Dialog" && p.timeline().audio[1].clips.isEmpty()
          && p.timeline().audio[2].name == "Musik");
    // V1 (y 94): above = new V2, "Titel und Grafik" moves to V3 (video counts from the bottom)
    CHECK(headerMenu({60, 100}, "Spur oberhalb hinzufügen"));
    CHECK(p.timeline().video.size() == 3 && p.timeline().video[1].clips.isEmpty()
          && p.timeline().video[2].name == "Titel und Grafik");
    // Delete track: V1 is locked -> menu item off
    CHECK(!headerMenu({60, 94 + 64 + 10}, "Spur löschen"));
    CHECK(p.timeline().video.size() == 3);

    // Drag track at the track header: insertion marker while dragging, one undo step on release
    auto drag = [&](QPoint from, QPoint to, const QString& shot = {}) {
        QMouseEvent pr(QEvent::MouseButtonPress, from, view.mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, {});
        QApplication::sendEvent(&view, &pr);
        for (int i = 1; i <= 4; ++i) {
            const QPoint pt = from + (to - from) * i / 4;
            QMouseEvent mv(QEvent::MouseMove, pt, view.mapToGlobal(pt), Qt::NoButton, Qt::LeftButton, {});
            QApplication::sendEvent(&view, &mv);
        }
        if (!shot.isEmpty()) view.grab().save(shot);
        QMouseEvent rl(QEvent::MouseButtonRelease, to, view.mapToGlobal(to), Qt::LeftButton, Qt::NoButton, {});
        QApplication::sendEvent(&view, &rl);
    };
    view.resize(1400, 480); // all tracks visible
    QApplication::processEvents();
    const int vh = view.view().videoTrackHeight, ah = view.view().audioTrackHeight;
    const int audioTop = TimelineView::kRulerH + 3 * vh + TimelineView::kSeparator;
    // A1 "Dialog" below A3
    int steps = p.undoStack()->count();
    drag({60, audioTop + 10}, {60, audioTop + 3 * ah - 3}, qEnvironmentVariable("SCHNEIDI_TEST_SHOT"));
    CHECK(p.timeline().audio[2].name == "Dialog" && p.timeline().audio[2].clips.size() == 3
          && p.timeline().audio[1].name == "Musik" && p.undoStack()->count() == steps + 1);
    // V3 "Titel und Grafik" (top) all the way down -> V1; locked old V1 becomes V2
    drag({60, TimelineView::kRulerH + 10}, {60, TimelineView::kRulerH + 3 * vh - 3});
    CHECK(p.timeline().video[0].name == "Titel und Grafik" && p.timeline().video[1].locked
          && p.timeline().video[1].clips.size() == 3);
    // short jitter (below the drag threshold) changes nothing
    steps = p.undoStack()->count();
    drag({60, audioTop + 10}, {60, audioTop + 12});
    CHECK(p.undoStack()->count() == steps);
    p.undoStack()->undo();
    CHECK(p.timeline().video[2].name == "Titel und Grafik");
    // Free area below the tracks (header and track area): new audio/video track at the end
    view.resize(1400, 900);
    QApplication::processEvents();
    const int bottom = view.height() - 30;
    CHECK(headerMenu({60, bottom}, "Audiospur hinzufügen"));
    CHECK(p.timeline().audio.size() == 4 && p.timeline().audio[3].clips.isEmpty());
    CHECK(headerMenu({view.width() - 40, bottom}, "Videospur hinzufügen"));
    CHECK(p.timeline().video.size() == 4 && p.timeline().video[3].clips.isEmpty());

    return Check::result();
}
