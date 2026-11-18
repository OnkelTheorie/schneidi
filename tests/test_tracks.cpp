// SPDX-License-Identifier: AGPL-3.0-only
// Test tracks: add/delete, lock, rename, color, ripple across all unlocked tracks, track header operation (offscreen)
#include "check.h"
#include "timeline_guard.h"

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
    while (p.newClipId() < 100) {} // the clips below get their ids by hand: new ones (titles, cues) above them
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
    TimelineGuard guard_p{p};
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
    // Two unlinked clips on different tracks at different times twice as fast: every track moves up by both (V1 by
    // its own clip only took x apart from its sound on A1)
    setup(p);
    p.edit("x", [](Timeline& tl) {
        tl.video[0].clips = {mk(31, 0, 100), mk(32, 100, 100, 5), mk(33, 500, 100, 6)};
        tl.audio[0].clips = {mk(41, 100, 100, 5), mk(42, 300, 100, 0, "/x/m.mp3"), mk(43, 500, 100, 6)};
        tl.audio[1].clips = {mk(51, 700, 100, 0, "/x/m.mp3")};
    });
    ed.setClipSpeed({31, 42}, {2.0, false, false, true}, true);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-50|0-49] a[50-150|0-99] a[400-500|0-99]  V2:  "
                                                "A1: a[50-150|0-99] m[250-300|0-49] a[400-500|0-99]  A2: m[600-700|0-99]"));

    // A dissolve on a retimed track that the speed change pushes apart (A stays, B moves with V1's longer clip): it is
    // released, not left as a fade out and a fade in
    setup(p);
    p.edit("x", [](Timeline& tl) {
        tl.video[0].clips = {mk(31, 0, 100)};
        while (tl.video.size() < 2) tl.video << Track{};
        Clip a = mk(32, 60, 40), b = mk(33, 100, 50, 0), q = mk(34, 200, 50);
        a.in = b.in = 300;
        a.out = 339;
        b.out = 349;
        a.transOut = b.transIn = 10;
        tl.video[1].clips = {a, b, q};
        tl.audio[0].clips.clear();
        tl.audio[1].clips.clear();
    });
    ed.setClipSpeed({31, 34}, {0.5, false, false, true}, true);
    CHECK_EQ(F(p, 33)->start, 200);
    CHECK(F(p, 32)->transOut == 0 && F(p, 33)->transIn == 0);
    CHECK_EQ(F(p, 34)->start, 300);

    // --- A ripple that would take linked picture and sound apart is refused with a reason (nothing changes, no undo
    // step): A2 cannot move up (the music lies across the edit point) while V2 can, so x on V2 and its sound on A2
    // came apart. Ripple delete, speed with ripple, one side deleted alone; locked: also ripple trim.
    {
        const auto layout = [&](bool lockA2, bool music) {
            setup(p);
            p.edit("x", [&](Timeline& tl) {
                while (tl.video.size() < 2) tl.video << Track{};
                tl.video[0].clips = {mk(1, 0, 100, 1), mk(2, 100, 100, 2)};
                tl.audio[0].clips = {mk(11, 0, 100, 1), mk(12, 100, 100, 2)};
                tl.video[1].clips = {mk(31, 300, 100, 3)};
                tl.audio[1].clips = {mk(22, 300, 100, 3)};
                if (music) tl.audio[1].clips.prepend(mk(21, 50, 250, 0, "/x/m.mp3"));
                tl.audio[1].locked = lockA2;
            });
            ed.takeNotice();
        };
        const auto refused = [&](const char* what, const QString& says) {
            const Editor::Notice n = ed.takeNotice();
            const bool ok = n.refused && n.text.contains(says);
            if (!ok) std::printf("       %s: %s\n", what, qPrintable(n.text));
            return ok;
        };
        layout(false, true);
        Timeline was = p.timeline();
        int undo = p.undoStack()->index();
        sel.set({1, 11});
        ed.rippleDeleteSelection();
        CHECK(p.timeline() == was && p.undoStack()->index() == undo);
        CHECK(refused("ripple delete", "A2"));
        CHECK(sel.contains(1)); // the selection stays
        ed.setClipSpeed({1}, {2.0, false, false, true}, true);
        CHECK(p.timeline() == was && p.undoStack()->index() == undo);
        CHECK(refused("speed", "A2"));
        ed.setClipSpeed({1}, {2.0, false, false, true}, false); // without ripple nothing moves: fine
        CHECK_EQ(F(p, 1)->speed, 2.0);
        CHECK(ed.takeNotice().text.isEmpty());
        // Only the picture of a linked clip: its sound stays in the deleted time, A1 cannot move up
        layout(false, false);
        was = p.timeline();
        sel.set({1});
        ed.rippleDeleteSelection();
        CHECK(p.timeline() == was);
        CHECK(refused("picture alone", "A1"));
        // A locked track with a partner: locking is the way to move one side only - done, a note says how far apart
        layout(true, false);
        sel.set({1, 11});
        guard_p.allowSlip({31, 22});
        ed.rippleDeleteSelection();
        CHECK(F(p, 31)->start == 200 && F(p, 22)->start == 300);
        const auto lockedNote = [&](const char* what, const QString& by) {
            const Editor::Notice n = ed.takeNotice();
            const bool ok = !n.refused && n.text.contains("A2") && n.text.contains(by);
            if (!ok) std::printf("       %s: %s\n", what, qPrintable(n.text));
            return ok;
        };
        CHECK(lockedNote("locked", "100"));
        layout(true, false);
        const auto e = ed.trimEdit(TimelineOps::TrimKind::Ripple, 1, TimelineOps::Edge::End);
        guard_p.allowSlip({31, 22});
        ed.applyTrimEdit(e, -30);
        CHECK(F(p, 31)->start == 270 && F(p, 22)->start == 300);
        CHECK(lockedNote("ripple trim, locked", "30"));
        // The refusal names the reason that holds: a clip across the edit point (only there), otherwise picture and
        // sound moving by different amounts (P on V2 starts before the trimmed clip's end, its sound after it)
        setup(p);
        p.edit("x", [&](Timeline& tl) {
            while (tl.video.size() < 2) tl.video << Track{};
            tl.video[0].clips = {mk(1, 0, 100)};
            tl.audio[0].clips.clear();
            Clip v = mk(31, 90, 60, 3), a = mk(22, 110, 40, 3);
            a.in = 20;
            a.out = 59;
            tl.video[1].clips = {v};
            tl.audio[1].clips = {a};
        });
        ed.takeNotice();
        was = p.timeline();
        const auto lengthen = ed.trimEdit(TimelineOps::TrimKind::Ripple, 1, TimelineOps::Edge::End);
        CHECK(!ed.trimEditRefusal(lengthen, 20).isEmpty()); // the drag preview holds still
        CHECK(ed.trimEditRefusal(lengthen, 0).isEmpty());
        ed.applyTrimEdit(lengthen, 20);
        CHECK(p.timeline() == was);
        CHECK(refused("ripple trim across", "V2"));
        // Nothing linked on the track that stays: as before (A2 stays, the rest moves up)
        layout(false, true);
        p.edit("x", [](Timeline& tl) { tl.audio[1].clips.removeLast(); tl.video[1].clips[0].linkId = 0; });
        sel.set({1, 11});
        ed.rippleDeleteSelection();
        CHECK(F(p, 2)->start == 0 && F(p, 31)->start == 200 && F(p, 21)->start == 50);
        CHECK(ed.takeNotice().text.isEmpty());
    }
    // --- Ripple delete of a linked pair with its own L-cut tail: the tail stays as a gap, no warning (it is not
    // time meant to go)
    {
        setup(p);
        p.edit("x", [](Timeline& tl) {
            tl.video[0].clips = {mk(1, 0, 100, 1), mk(2, 100, 100, 2)};
            Clip xa = mk(11, 0, 120, 1), ya = mk(12, 120, 80, 2);
            ya.in = 20;
            ya.out = 99;
            tl.audio[0].clips = {xa, ya};
            tl.audio[1].clips.clear();
        });
        ed.takeNotice();
        sel.set({1, 11});
        ed.rippleDeleteSelection();
        CHECK(F(p, 2)->start == 0 && F(p, 12)->start == 20);
        const Editor::Notice n = ed.takeNotice();
        if (!CHECK(n.text.isEmpty())) std::printf("       %s\n", qPrintable(n.text));
    }
    // --- Ripple delete taking out less than was deleted (each deleted clip lies under a clip that stays on the other
    // track): done, and says why it moved up less (not like a broken command)
    {
        setup(p);
        p.edit("x", [](Timeline& tl) {
            tl.video[0].clips = {mk(1, 0, 50), mk(2, 50, 50)};
            tl.audio[0].clips.clear();
            tl.audio[1].clips = {mk(21, 0, 50, 0, "/x/m.mp3"), mk(22, 50, 50, 0, "/x/m.mp3")};
        });
        ed.takeNotice();
        sel.set({1, 22});
        ed.rippleDeleteSelection();
        CHECK(!F(p, 1) && !F(p, 22) && F(p, 2)->start == 50);
        const Editor::Notice n = ed.takeNotice();
        CHECK(!n.refused && n.text.contains(T("Nicht nachgerückt")));
        p.edit("x", [](Timeline& tl) {
            tl.video[0].clips = {mk(1, 0, 50), mk(2, 50, 50), mk(3, 200, 10)};
            tl.audio[1].clips = {mk(21, 0, 40, 0, "/x/m.mp3"), mk(22, 150, 10, 0, "/x/m.mp3")};
        });
        sel.set({1, 2, 22});
        ed.rippleDeleteSelection(); // 110 frames deleted, [0,40) under the music that stays: 70 frames out
        CHECK_EQ(F(p, 3)->start, 130);
        const Editor::Notice m = ed.takeNotice();
        CHECK(!m.refused && m.text.contains("70") && m.text.contains("110"));
    }

    // --- Speed on an L-cut pair (the sound starts later, other in points): both new in points come from the same
    // place, so picture and sound stay in sync to the frame (each rounded alone, they could end one frame apart)
    for (const double speed : {0.33, 0.7, 1.5, 2.37}) {
        for (const bool reverse : {false, true}) {
            for (const int vIn : {37, 41, 113}) {
                setup(p);
                p.edit("x", [&](Timeline& tl) {
                    Clip v = mk(61, 100, 200, 9), a = mk(62, 160, 140, 9);
                    v.in = vIn;
                    v.out = vIn + 199;
                    a.in = vIn + 60;
                    a.out = a.in + 139;
                    tl.video[0].clips = {v};
                    tl.audio[0].clips = {a};
                    tl.audio[1].clips.clear();
                });
                ed.setClipSpeed({61}, {speed, reverse, false, true}, true);
                const Clip* v = F(p, 61);
                const Clip* a = F(p, 62);
                if (!CHECK(v && a && v->speed == speed && v->start - v->in == a->start - a->in))
                    std::printf("       speed %g reverse %d in %d\n", speed, int(reverse), vIn);
                if (reverse) continue;
                ed.addSpeedPoint(61, 190);
                ed.setSegmentSpeed(61, 1, speed * 1.7);
                v = F(p, 61);
                a = F(p, 62);
                if (!CHECK(v && a && v->ramp == a->ramp && v->start - v->in == a->start - a->in))
                    std::printf("       ramp after speed %g in %d\n", speed, vIn);
            }
        }
    }

    // --- Speed on razor-cut pieces of one take (same file, each piece its own link): every piece keeps its own content
    // and length (one offset for all squeezed the later pieces)
    {
        setup(p);
        p.edit("x", [](Timeline& tl) {
            QVector<Clip> v, a;
            for (int k = 0; k < 4; ++k) {
                Clip c = mk(70 + k, k * 100, 100, 20 + k);
                c.in = k * 100;
                c.out = c.in + 99;
                v << c;
                c.id = 80 + k;
                a << c;
            }
            tl.video[0].clips = v;
            tl.audio[0].clips = a;
            tl.audio[1].clips.clear();
        });
        ed.selectAll();
        ed.setClipSpeed(ed.selection()->ids().values().toVector(), {1.1, false, false, true}, true);
        int at = 0;
        for (int k = 0; k < 4; ++k) {
            const Clip* v = F(p, 70 + k);
            const Clip* a = F(p, 80 + k);
            if (!CHECK(v && a)) continue;
            CHECK(std::abs(v->length() - 91) <= 1 && std::abs(v->in - int(k * 100 / 1.1)) <= 1);
            CHECK(v->start == at && a->start == v->start && a->in == v->in && a->length() == v->length());
            at = v->end();
        }
    }
    // --- Speed with ripple on an L-cut and a J-cut (picture and sound end at different points): what follows moves by
    // one amount on all tracks - in sync, nothing refused (the changes of picture and sound were added up)
    for (const bool lcut : {true, false})
        for (const double speed : {0.5, 2.0}) {
            setup(p);
            p.edit("x", [&](Timeline& tl) {
                // L: the sound runs 20 longer; J: the next sound starts 20 early
                Clip x = mk(1, 0, 200, 1), xa = mk(11, 0, lcut ? 220 : 180, 1);
                Clip y = mk(2, 200, 100, 2), ya = mk(12, lcut ? 220 : 180, lcut ? 80 : 120, 2);
                y.in = 300;
                y.out = 399;
                ya.in = lcut ? 320 : 280;
                ya.out = (lcut ? 320 + 79 : 280 + 119);
                tl.video[0].clips = {x, y};
                tl.audio[0].clips = {xa, ya};
                tl.audio[1].clips = {mk(21, 700, 10, 0, "/x/m.mp3")};
            });
            ed.takeNotice();
            ed.setClipSpeed({1}, {speed, false, false, true}, true);
            const Clip *x = F(p, 1), *xa = F(p, 11), *y = F(p, 2), *ya = F(p, 12), *m = F(p, 21);
            const Editor::Notice n = ed.takeNotice();
            if (!CHECK(!n.refused && x->speed == speed)) std::printf("       %s\n", qPrintable(n.text));
            CHECK(y->start - y->in == ya->start - ya->in);              // in sync
            CHECK(y->start >= x->end() && ya->start >= xa->end());      // nothing overlaps
            CHECK(y->start == x->end() || ya->start == xa->end());      // no more room than needed
            CHECK_EQ(m->start - 700, y->start - 200);                   // the others: by the same amount
        }

    // --- Two separate clips ending together, their changes apart by rounding (+100, +99): what follows moves by the
    // larger one, not by both
    {
        setup(p);
        p.edit("x", [&](Timeline& tl) {
            while (tl.video.size() < 2) tl.video << Track{};
            tl.video[0].clips = {mk(1, 0, 100), mk(2, 100, 50)};
            tl.video[1].clips = {mk(31, 1, 99), mk(32, 100, 50)};
            tl.audio[0].clips.clear();
            tl.audio[1].clips = {mk(21, 300, 10, 0, "/x/m.mp3")};
        });
        ed.setClipSpeed({1, 31}, {0.5, false, false, true}, true);
        CHECK(F(p, 1)->length() == 200 && F(p, 31)->length() == 198);
        CHECK(F(p, 2)->start == 200 && F(p, 32)->start == 200 && F(p, 21)->start == 400);
    }
    // --- A linked sound that starts where its picture ends moves up with it on speed-up; an unselected clip before it
    // on its track would be run into: refused (it overlapped)
    {
        setup(p);
        p.edit("x", [&](Timeline& tl) {
            tl.video[0].clips = {mk(1, 0, 100, 7)};
            Clip a = mk(12, 100, 100, 7);
            a.in = 100;
            a.out = 199;
            tl.audio[0].clips = {mk(11, 0, 100, 0, "/x/m.mp3"), a};
            tl.audio[1].clips.clear();
        });
        ed.takeNotice();
        const Timeline was = p.timeline();
        ed.setClipSpeed({1}, {2.0, false, false, true}, true);
        CHECK(p.timeline() == was);
        const Editor::Notice n = ed.takeNotice();
        if (!CHECK(n.refused && n.text.contains("A1"))) std::printf("       %s\n", qPrintable(n.text));
    }
    // --- The ripple-trim drag stops at the furthest allowed delta (here: none - P on V2 starts before the edit point,
    // its sound after it), says why; an allowed one is kept as it is
    {
        setup(p);
        p.edit("x", [&](Timeline& tl) {
            while (tl.video.size() < 2) tl.video << Track{};
            tl.video[0].clips = {mk(1, 0, 100)};
            tl.audio[0].clips.clear();
            Clip v = mk(31, 90, 60, 3), a = mk(22, 110, 40, 3);
            a.in = 20;
            a.out = 59;
            tl.video[1].clips = {v};
            tl.audio[1].clips = {a};
        });
        const auto e = ed.trimEdit(TimelineOps::TrimKind::Ripple, 1, TimelineOps::Edge::End);
        QString why;
        Timeline preview;
        CHECK_EQ(ed.allowedTrimEdit(e, 20, &why, &preview), 0);
        CHECK(!why.isEmpty() && preview == p.timeline());
        p.edit("x", [](Timeline& tl) { tl.audio[1].clips[0].start = 90; tl.audio[1].clips[0].in = 0; });
        CHECK_EQ(ed.allowedTrimEdit(e, 20, &why, &preview), 20);
        CHECK(why.isEmpty() && TimelineOps::findClip(preview, 1)->length() == 120);
    }

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

    // --- Target tracks follow undo/redo of track add/delete/move (before: they stayed where the step had put them,
    //     e.g. on a track that undo had removed again)
    {
        setup(p); // V1 V2, A1 A2
        using TP = QPair<int, int>;
        const auto targets = [&] { return TP(ed.targetVideoTrack(), ed.targetAudioTrack()); };
        // Add below the target: target moves up with its track; undo -> back on the same track
        ed.setTargetTracks(1, 1);
        ed.addTrack(TrackKind::Audio, 0);
        CHECK(targets() == TP(1, 2));
        p.undoStack()->undo();
        CHECK(p.timeline().audio.size() == 2);
        CHECK(targets() == TP(1, 1));
        p.undoStack()->redo();
        CHECK(targets() == TP(1, 2));
        p.undoStack()->undo();
        // Add on top, the user targets the new track; undo removes it -> the target goes back to an existing one
        ed.addTrack(TrackKind::Video, 2);
        ed.setTargetTracks(2, 1);
        p.undoStack()->undo();
        CHECK(p.timeline().video.size() == 2);
        CHECK(targets() == TP(1, 1));
        p.undoStack()->redo();
        CHECK(p.timeline().video.size() == 3 && targets().first < 3);
        p.undoStack()->undo();
        // Delete the target track: undo puts the target back on it; delete a track below: undo -> same track again
        ed.setTargetTracks(1, 1);
        ed.removeTrack({TrackKind::Audio, 1});
        CHECK(targets() == TP(1, 0));
        p.undoStack()->undo();
        CHECK(targets() == TP(1, 1));
        p.undoStack()->redo();
        CHECK(targets() == TP(1, 0));
        p.undoStack()->undo();
        ed.removeTrack({TrackKind::Video, 0});
        CHECK(targets() == TP(0, 1));
        p.undoStack()->undo();
        CHECK(targets() == TP(1, 1));
        // Move: target follows its track, also back on undo
        ed.setTargetTracks(0, 0);
        ed.moveTrack(TrackKind::Audio, 0, 1);
        CHECK(targets() == TP(0, 1));
        p.undoStack()->undo();
        CHECK(targets() == TP(0, 0));
        p.undoStack()->redo();
        CHECK(targets() == TP(0, 1));
        // Changed by hand after the step: undo keeps the chosen track (it moves back with it) instead of resetting
        ed.setTargetTracks(0, 0); // the old A2, now at index 0
        p.undoStack()->undo();    // ... back at index 1
        CHECK(targets() == TP(0, 1));
        // Undo never leaves a target beyond the tracks, still one undo step per operation
        const int steps = p.undoStack()->index();
        ed.addTrack(TrackKind::Audio, 2);
        CHECK(p.undoStack()->index() == steps + 1);
        ed.setTargetTracks(0, 2);
        p.undoStack()->undo();
        CHECK(ed.targetAudioTrack() < p.timeline().audio.size());
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
