// SPDX-License-Identifier: AGPL-3.0-only
// Test TimelineOps basic cases (pure functions on the model): overwriting (placeClip/clearRange), splitting,
// ripple delete, insert gap, trimming (normal and trim mode ripple/roll/slip/slide), moving,
// effective transitions at edges, fade/transition curves
#include "check.h"
#include "timeline_guard.h"

#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <random>

using namespace TimelineOps;
using Check::dump;

namespace {

// Clip from file name (path /x/<name>.mp4), position [start, start+len), source from in
Clip mk(int id, const char* name, int start, int len, int in = 0, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = QString("/x/%1.mp4").arg(name);
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.linkId = link;
    return c;
}

Timeline tracks(int video, int audio)
{
    Timeline tl;
    ensureTracks(tl, TrackKind::Video, video);
    ensureTracks(tl, TrackKind::Audio, audio);
    return tl;
}

// All files are 200 frames long, titles unlimited
const SourceLength kSrc = [](const Clip& c) { return c.isTitle() ? 0 : 200; };

struct Ids {
    int next = 100;
    IdGen gen() { return [this] { return ++next; }; }
};

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("timelineops");

    // --- placeClip / clearRange (overwriting, nothing moves up)
    {
        Ids ids;
        Track t;
        placeClip(t, mk(1, "a", 0, 100), ids.gen(), 25);
        CHECK_EQ(t.clips.size(), 1);
        // into the middle: a is split, right part new ID and provisional link (-old, see resolvePendingLinks),
        // transition/fade at the cut edges gone
        t.clips[0].linkId = 7;
        t.clips[0].transIn = t.clips[0].transOut = 10;
        t.clips[0].fadeIn = t.clips[0].fadeOut = 5;
        placeClip(t, mk(2, "b", 40, 20), ids.gen(), 25);
        Timeline tl = tracks(1, 0);
        tl.video[0] = t;
        CHECK_EQ(dump(tl), QString("V1: a[0-40|0-39] b[40-60|0-19] a[60-100|60-99]"));
        CHECK_TIMELINE(tl);
        CHECK(t.clips[0].id == 1 && t.clips[2].id == 101 && t.clips[2].linkId == -7 && t.clips[0].linkId == 7);
        {
            Timeline pending = tl;
            int next = 20;
            resolvePendingLinks(pending, [&] { return next++; });
            CHECK_EQ(pending.video[0].clips[2].linkId, 20);
            CHECK_EQ(pending.video[0].clips[0].linkId, 7);
        }
        CHECK(t.clips[0].transIn == 10 && t.clips[0].transOut == 0 && t.clips[0].fadeIn == 5 && t.clips[0].fadeOut == 0);
        CHECK(t.clips[2].transIn == 0 && t.clips[2].transOut == 10 && t.clips[2].fadeIn == 0 && t.clips[2].fadeOut == 5);
        // Cut off end and start, remove completely covered clip
        clearRange(tl.video[0], 30, 70, ids.gen(), 25);
        CHECK_EQ(dump(tl), QString("V1: a[0-30|0-29] a[70-100|70-99]"));
        CHECK_TIMELINE(tl);
        clearRange(tl.video[0], 60, 60, ids.gen(), 25); // empty range: nothing
        clearRange(tl.video[0], 0, 30, ids.gen(), 25);
        CHECK_EQ(dump(tl), QString("V1: a[70-100|70-99]"));
        CHECK_TIMELINE(tl);
        placeClip(tl.video[0], mk(3, "c", 60, 50), ids.gen(), 25); // covers a entirely
        CHECK_EQ(dump(tl), QString("V1: c[60-110|0-49]"));
        CHECK_TIMELINE(tl);
        CHECK(removeClip(tl, 3) && !removeClip(tl, 3) && tl.video[0].clips.isEmpty());
    }

    // --- splitAt: linked pairs stay linked (new common linkId on the right)
    {
        Ids ids;
        int link = 50;
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 10, 100, 20, 5);
        tl.audio[0].clips << mk(2, "a", 10, 100, 20, 5);
        tl.video[0].clips[0].transOut = 12;
        tl.video[0].clips[0].fadeIn = 4;
        const QVector<int> right = splitAt(tl, {1, 2}, 60, ids.gen(), [&] { return ++link; });
        CHECK_EQ(right.size(), 2);
        CHECK_EQ(dump(tl), QString("V1: a[10-60|20-69] a[60-110|70-119]  A1: a[10-60|20-69] a[60-110|70-119]"));
        CHECK_TIMELINE(tl);
        const Clip* v = findClip(tl, right[0]);
        const Clip* a = findClip(tl, right[1]);
        CHECK(v && a && v->linkId == 51 && a->linkId == 51 && findClip(tl, 1)->linkId == 5);
        CHECK(findClip(tl, 1)->transOut == 0 && findClip(tl, 1)->fadeIn == 4 && v->transOut == 12 && v->fadeIn == 0);
        CHECK_EQ(linkedGroup(tl, 1), (QVector<int>{1, 2}));
        // A sound that starts after the cut (L-cut, not split): goes with the right piece it plays in sync with
        {
            Timeline l = tracks(1, 1);
            l.video[0].clips << mk(1, "a", 0, 100, 0, 5);
            l.audio[0].clips << mk(2, "a", 60, 40, 60, 5);
            const QVector<int> r = splitAt(l, {1, 2}, 50, ids.gen(), [&] { return ++link; });
            CHECK_EQ(r.size(), 1);
            CHECK_EQ(findClip(l, 2)->linkId, findClip(l, r.value(0))->linkId);
            CHECK_EQ(findClip(l, 1)->linkId, 5);
        }
        // at the edge or outside: nothing
        CHECK(splitAt(tl, {1}, 10, ids.gen(), [&] { return ++link; }).isEmpty());
        CHECK(splitAt(tl, {1}, 60, ids.gen(), [&] { return ++link; }).isEmpty());
    }

    // --- rippleDelete: affected tracks move up, other unlocked ones too, locked ones stay
    {
        Timeline tl = tracks(2, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 50, 30) << mk(3, "c", 100, 20);
        tl.audio[0].clips << mk(4, "m", 90, 10);
        tl.video[1].clips << mk(5, "t", 120, 10);
        tl.video[1].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(dump(tl), QString("V1: a[0-50|0-49] c[70-90|0-19]  V2: t[120-130|0-9]  A1: m[60-70|0-9]"));
        CHECK_TIMELINE(tl);
    }

    // --- rippleDelete on two tracks at different times (an unlinked clip each): every track moves up by all deleted
    // time, the affected ones too - otherwise V1 moved by its own gap only and the linked x on V1/A1 came apart
    {
        Timeline tl = tracks(1, 2);
        tl.video[0].clips << mk(1, "t", 0, 10) << mk(2, "x", 10, 10, 0, 5) << mk(3, "y", 50, 10, 0, 6);
        tl.audio[0].clips << mk(4, "x", 10, 10, 0, 5) << mk(5, "m", 30, 10) << mk(6, "y", 50, 10, 0, 6);
        tl.audio[1].clips << mk(7, "n", 60, 10);
        const Timeline before = tl;
        rippleDelete(tl, {1, 5});
        CHECK_EQ(dump(tl), QString("V1: x[0-10|0-9] y[30-40|0-9]  A1: x[0-10|0-9] y[30-40|0-9]  A2: n[40-50|0-9]"));
        CHECK_TIMELINE_SYNC(before, tl);
    }
    // A linked pair with an L-cut (the sound runs 20 frames longer) deleted with ripple: only the time free on both
    // tracks goes, the next pair stays in sync (y stayed at 100 with a 100-frame hole while its sound went to 0)
    {
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "x", 0, 100, 0, 1) << mk(2, "y", 100, 100, 0, 2);
        tl.audio[0].clips << mk(3, "x", 0, 120, 0, 1) << mk(4, "y", 120, 80, 20, 2);
        const Timeline before = tl;
        rippleDelete(tl, {1, 3});
        CHECK_EQ(dump(tl), QString("V1: y[0-100|0-99]  A1: y[20-100|20-99]"));
        CHECK_TIMELINE_SYNC(before, tl);
    }
    // Deleted time overlapping across tracks (V1 [0,50), A1 [40,120)): y on V1 lies in A1's deleted time, so only
    // [0,50) goes - y and its sound stay in sync
    {
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "x", 0, 50) << mk(2, "y", 50, 100, 0, 2);
        tl.audio[0].clips << mk(3, "m", 40, 80) << mk(4, "y", 120, 30, 70, 2);
        const Timeline before = tl;
        rippleDelete(tl, {1, 3});
        CHECK_EQ(dump(tl), QString("V1: y[0-100|0-99]  A1: y[70-100|70-99]"));
        CHECK_TIMELINE_SYNC(before, tl);
    }
    // A clip of an affected track lies across time deleted on another one (g on V1 in A1's [20,30)): only the part of
    // that time where nothing stays goes, every track the same - e and its sound f stay together
    {
        Timeline tl = tracks(1, 2);
        tl.video[0].clips << mk(1, "t", 0, 10) << mk(2, "g", 25, 10) << mk(3, "e", 40, 10, 0, 7);
        tl.audio[0].clips << mk(4, "b", 20, 10) << mk(5, "e", 40, 10, 0, 7);
        tl.audio[1].clips << mk(6, "n", 40, 10);
        const Timeline before = tl;
        rippleDelete(tl, {1, 4});
        CHECK_EQ(dump(tl), QString("V1: g[10-20|0-9] e[25-35|0-9]  A1: e[25-35|0-9]  A2: n[25-35|0-9]"));
        CHECK_TIMELINE_SYNC(before, tl);
    }

    // --- rippleDelete says how much time it took out: all of it, or less where a clip stays in the deleted time
    {
        Timeline tl = tracks(1, 2);
        tl.video[0].clips << mk(1, "b", 0, 50) << mk(2, "s", 50, 50);
        tl.audio[1].clips << mk(3, "s", 0, 50) << mk(4, "m", 50, 50);
        const RippleResult r = rippleDelete(tl, {1, 4}); // each covered by the clip staying on the other track
        CHECK(r.deleted == 100 && r.removed == 0);
        Timeline t2 = tracks(1, 0);
        t2.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 50, 50);
        const RippleResult r2 = rippleDelete(t2, {1});
        CHECK(r2.deleted == 50 && r2.removed == 50);
    }

    // --- The time meant to go of a linked group: its picture's time - also when its sound is shorter (a J-cut: the
    // next sound starts under it) or does not overlap it at all
    {
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "x", 0, 100, 0, 1) << mk(2, "y", 100, 100, 0, 2);
        tl.audio[0].clips << mk(3, "x", 0, 30, 0, 1) << mk(4, "y", 30, 170, 0, 2);
        const RippleResult r = rippleDelete(tl, {1, 3});
        CHECK(r.meant == 100 && r.meantRemoved == 30);
        Timeline t2 = tracks(1, 1);
        t2.video[0].clips << mk(1, "x", 0, 100, 0, 1);
        t2.audio[0].clips << mk(3, "x", 100, 100, 100, 1);
        const RippleResult r2 = rippleDelete(t2, {1, 3});
        CHECK_EQ(r2.meant, 100);
    }

    // --- linksApart: linked pairs in sync before and not after; on-purpose changes and other playback left out
    {
        Timeline a = tracks(1, 1);
        a.video[0].clips << mk(1, "x", 0, 100, 0, 5);
        a.audio[0].clips << mk(2, "x", 0, 100, 0, 5);
        Timeline b = a;
        b.audio[0].clips[0].start = 10;
        CHECK_EQ(linksApart(a, b).size(), 1);
        CHECK(linksApart(a, b, {2}).isEmpty());
        b.audio[0].clips[0].in = 10; // slipped along: still in sync
        CHECK(linksApart(a, b).isEmpty());
        Timeline c = a;
        c.audio[0].clips[0].start = 10;
        c.audio[0].clips[0].speed = 2; // plays otherwise
        CHECK(linksApart(a, c).isEmpty());
        Timeline apart = a; // apart before already: not this edit's doing
        apart.audio[0].clips[0].start = 30;
        Timeline more = apart;
        more.audio[0].clips[0].start = 40;
        CHECK(linksApart(apart, more).isEmpty());
    }

    // --- resolvePendingLinks: a range over a linked picture whose sound starts after it - the sound stays linked to the
    // right piece it plays in sync with (it went with the left piece and the right one had no partner)
    {
        Ids ids;
        int link = 50;
        const auto newLink = [&] { return ++link; };
        Timeline tl = tracks(1, 3);
        tl.video[0].clips << mk(1, "v", 1070, 400, 0, 5);
        tl.audio[0].clips << mk(2, "v", 1213, 257, 143, 5);  // starts after the range, in sync with v
        tl.audio[1].clips << mk(3, "v", 1213, 257, 143, 5);  // a second stream: the same
        tl.audio[2].clips << mk(4, "v", 1070, 400, 0, 5);    // under both pieces: stays with the left one
        clearRange(tl.video[0], 1078, 1137, ids.gen(), 25);
        resolvePendingLinks(tl, newLink);
        const Clip* left = findClip(tl, 1);
        const Clip* right = tl.video[0].clips.size() == 2 ? &tl.video[0].clips[1] : nullptr;
        if (CHECK(left && right)) {
            CHECK_EQ(left->linkId, 5);
            CHECK_EQ(right->linkId, 51);
            CHECK_EQ(findClip(tl, 2)->linkId, 51);
            CHECK_EQ(findClip(tl, 3)->linkId, 51);
            CHECK_EQ(findClip(tl, 4)->linkId, 5);
        }
        // Not in sync with the right piece (another part of the file): keeps the old link, as before
        Timeline t2 = tracks(1, 1);
        t2.video[0].clips << mk(1, "v", 1070, 400, 0, 5);
        t2.audio[0].clips << mk(2, "v", 1213, 257, 10, 5);
        clearRange(t2.video[0], 1078, 1137, ids.gen(), 25);
        resolvePendingLinks(t2, newLink);
        CHECK_EQ(findClip(t2, 2)->linkId, 5);
    }
    // A partner of another file (a recorder's sound to a camera's picture): with the piece it lies under more
    {
        Ids ids;
        int link = 70;
        Timeline tl = tracks(1, 2);
        tl.video[0].clips << mk(1, "cam", 0, 100, 0, 5);
        tl.audio[0].clips << mk(2, "rec", 20, 80, 0, 5);  // under the right piece only
        tl.audio[1].clips << mk(3, "rec2", 0, 50, 0, 5);  // more under the left piece
        clearRange(tl.video[0], 30, 40, ids.gen(), 25);
        resolvePendingLinks(tl, [&] { return ++link; });
        CHECK_EQ(findClip(tl, 2)->linkId, 71);
        CHECK_EQ(findClip(tl, 3)->linkId, 5);
    }
    // ... random ranges over linked pictures with sounds that start later (L/J offsets): an uncut sound in sync with
    // exactly one piece and under no other one ends up linked to that piece
    {
        std::mt19937 rng(11);
        const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int bad = 0;
        for (int round = 0; round < 300; ++round) {
            Ids ids;
            int link = 100;
            Timeline tl = tracks(1, 2);
            const int vs = pick(0, 50), vl = pick(20, 200);
            tl.video[0].clips << mk(1, "v", vs, vl, 0, 5);
            for (int k = 0; k < 2; ++k) {
                const int as = vs + pick(0, vl - 1), al = pick(1, vs + vl - as);
                tl.audio[k].clips << mk(2 + k, "v", as, al, as - vs, 5);
            }
            const int from = pick(0, vs + vl), to = from + pick(1, 60);
            clearRange(tl.video[0], from, to, ids.gen(), 25);
            resolvePendingLinks(tl, [&] { return ++link; });
            for (int k = 0; k < 2; ++k) {
                const Clip& a = tl.audio[k].clips[0];
                QVector<const Clip*> under;
                for (const Clip& v : tl.video[0].clips)
                    if (v.start < a.end() && a.start < v.end()) under << &v;
                if (under.size() == 1 && a.linkId != under[0]->linkId && ++bad <= 3)
                    std::printf("       range [%d,%d): %s\n", from, to, qPrintable(TimelineCheck::dumpIds(tl)));
            }
        }
        CHECK_EQ(bad, 0);
    }

    // --- insertGap / shiftFrom / clearDissolveAt
    {
        Ids ids;
        int link = 0;
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 40) << mk(2, "b", 40, 60);
        tl.video[0].clips[0].transOut = 10;
        tl.video[0].clips[1].transIn = 10;
        tl.audio[0].clips << mk(3, "m", 20, 50);
        insertGap(tl, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}}, 40, 25, ids.gen(), [&] { return ++link; });
        CHECK_EQ(dump(tl), QString("V1: a[0-40|0-39] b[65-125|0-59]  A1: m[20-40|0-19] m[65-95|20-49]"));
        CHECK_TIMELINE(tl);
        CHECK(findClip(tl, 1)->transOut == 0 && findClip(tl, 2)->transIn == 0); // dissolve at the insert point gone
        shiftFrom(tl.video[0], 50, -5);
        CHECK_EQ(tl.video[0].clips[1].start, 60);
        tl.video[0].clips[0].transOut = 5;
        tl.video[0].clips[1].transIn = 5;
        clearDissolveAt(tl.video[0], 60); // no cut there: fade out/in stay
        CHECK(tl.video[0].clips[0].transOut == 5 && tl.video[0].clips[1].transIn == 5);
    }
    // Insert next to a standalone fade out/in (to black): stays, only dissolves at the cut are dropped
    {
        Ids ids;
        Timeline tl = tracks(1, 0);
        tl.video[0].clips << mk(1, "a", 0, 40) << mk(2, "b", 40, 60) << mk(3, "c", 100, 20);
        tl.video[0].clips[0].transOut = 10;
        tl.video[0].clips[1].transIn = 10;
        tl.video[0].clips[0].transOutAlone = tl.video[0].clips[1].transInAlone = true;
        tl.video[0].clips[1].transOut = 8; // dissolve b -> c
        tl.video[0].clips[2].transIn = 8;
        insertGap(tl, {{TrackKind::Video, 0}}, 40, 25, ids.gen(), [] { return 0; });
        CHECK(findClip(tl, 1)->transOut == 10 && findClip(tl, 2)->transIn == 10);
        insertGap(tl, {{TrackKind::Video, 0}}, 125, 5, ids.gen(), [] { return 0; });
        CHECK(findClip(tl, 2)->transOut == 0 && findClip(tl, 3)->transIn == 0);
    }

    // --- trimClips / clampTrim (no ripple): source material, neighbors, frame 0, at least 1 frame
    {
        Timeline tl = tracks(1, 0);
        tl.video[0].clips << mk(1, "a", 10, 50, 5) << mk(2, "b", 80, 50, 100);
        CHECK_EQ(clampTrim(tl, {1}, Edge::Start, -20, kSrc), -5);  // only 5 frames of material before
        CHECK_EQ(clampTrim(tl, {1}, Edge::End, 100, kSrc), 20);    // up to the neighbor b
        CHECK_EQ(clampTrim(tl, {1}, Edge::End, -100, kSrc), -49);  // at least 1 frame
        CHECK_EQ(clampTrim(tl, {2}, Edge::End, 100, kSrc), 50);    // file ends at 199
        CHECK_EQ(clampTrim(tl, {2}, Edge::Start, -100, kSrc), -20); // up to the end of a
        trimClips(tl, {1}, Edge::Start, -20, kSrc);
        trimClips(tl, {2}, Edge::End, 100, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[5-60|0-54] b[80-180|100-199]"));
        CHECK_TIMELINE(tl);
        // Title: any length, may trim before the "source"
        Clip title = mk(3, "t", 300, 10);
        title.kind = ClipKind::Title;
        tl.video[0].clips << title;
        CHECK_EQ(clampTrim(tl, {3}, Edge::Start, -50, kSrc), -50);
        CHECK_EQ(clampTrim(tl, {3}, Edge::End, 5000, kSrc), 5000);
    }

    // --- Trim mode: Ripple, Roll, Slip, Slide
    {
        auto base = [] {
            Timeline tl = tracks(1, 1);
            tl.video[0].clips << mk(1, "a", 0, 50, 50) << mk(2, "b", 50, 50, 50) << mk(3, "c", 100, 50, 50);
            tl.audio[0].clips << mk(4, "m", 120, 20);
            return tl;
        };
        // Ripple end: later clips of the same and the other tracks move along
        Timeline tl = base();
        TrimEdit e{TrimKind::Ripple, {1}, {}, Edge::End};
        applyTrimEdit(tl, e, -10, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-40|50-89] b[40-90|50-99] c[90-140|50-99]  A1: m[110-130|0-19]"));
        CHECK_TIMELINE(tl);
        // Ripple start: clip stays put, content moves, rest moves up
        tl = base();
        e = TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start};
        applyTrimEdit(tl, e, 10, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-50|50-99] b[50-90|60-99] c[90-140|50-99]  A1: m[110-130|0-19]"));
        CHECK_TIMELINE(tl);
        // Ripple with two (linked) clips of the same track: the rear one moves along, nothing overlaps
        // (order of ids does not matter; before, the rear one stayed put and c slid under it)
        for (const QVector<int>& order : {QVector<int>{1, 2}, QVector<int>{2, 1}}) {
            tl = base();
            applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, order, {}, Edge::End}, -10, kSrc);
            CHECK_EQ(dump(tl), QString("V1: a[0-40|50-89] b[40-80|50-89] c[80-130|50-99]  A1: m[100-120|0-19]"));
            CHECK_TIMELINE(tl);
        }
        // Ripple shortening limited by room on the other track
        tl = base();
        tl.audio[0].clips = {mk(4, "m", 0, 45), mk(5, "n", 55, 10)};
        CHECK_EQ(clampTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -30, kSrc), -10);
        // Roll: cut a|b moves, total length stays; limited by material (b.in = 50, a has 100 frames after it)
        tl = base();
        e = TrimEdit{TrimKind::Roll, {1}, {2}, Edge::End};
        CHECK_EQ(clampTrimEdit(tl, e, -100, kSrc), -49);
        CHECK_EQ(clampTrimEdit(tl, e, 100, kSrc), 49);
        applyTrimEdit(tl, e, 20, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-70|50-119] b[70-100|70-99] c[100-150|50-99]  A1: m[120-140|0-19]"));
        CHECK_TIMELINE(tl);
        // Slip: position stays, content shifted (in >= 0, out <= 199)
        tl = base();
        e = TrimEdit{TrimKind::Slip, {2}, {}, Edge::End};
        CHECK_EQ(clampTrimEdit(tl, e, -80, kSrc), -50);
        CHECK_EQ(clampTrimEdit(tl, e, 200, kSrc), 100);
        applyTrimEdit(tl, e, 30, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-50|50-99] b[50-100|80-129] c[100-150|50-99]  A1: m[120-140|0-19]"));
        CHECK_TIMELINE(tl);
        // Keyframes stay where they are in the clip: stored as source frames they rode along with the content
        // (AI test run 2026-10-25: some ended up behind the clip's end)
        tl = base();
        Keys::setKey(tl.video[0].clips[1], AnimParam::Opacity, 0, 0);
        Keys::setKey(tl.video[0].clips[1], AnimParam::Opacity, 49, 100);
        applyTrimEdit(tl, e, 30, kSrc);
        CHECK_EQ(Keys::keyTimes(tl.video[0].clips[1]), QVector<int>({0, 49}));
        CHECK_EQ(Keys::valueAt(tl.video[0].clips[1], AnimParam::Opacity, 49), 100.0);
        applyTrimEdit(tl, e, -50, kSrc);
        CHECK_EQ(Keys::keyTimes(tl.video[0].clips[1]), QVector<int>({0, 49}));
        // ... except the place of a Color page window: it sits on the content (a tracked face) and moves with it;
        // its other keys (fill amount) stay in the clip like any animation
        tl = base();
        Clip& b = tl.video[0].clips[1];
        const AnimParam wx = GradeWindows::param(1, "x"), wAmount = GradeWindows::param(1, "amount");
        Keys::setKey(b, wx, 40, 20);
        Keys::setKey(b, wAmount, 40, 80);
        applyTrimEdit(tl, e, 30, kSrc);
        CHECK_EQ(Keys::keyTimes(tl.video[0].clips[1], {wx}), QVector<int>({10}));
        CHECK_EQ(Keys::valueAt(tl.video[0].clips[1], wx, 10), 20.0);
        CHECK_EQ(Keys::keyTimes(tl.video[0].clips[1], {wAmount}), QVector<int>({40}));
        // Slide: b moves, a gets longer, c shorter
        tl = base();
        e = TrimEdit{TrimKind::Slide, {2}, {}, Edge::End};
        applyTrimEdit(tl, e, 15, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-65|50-114] b[65-115|50-99] c[115-150|65-99]  A1: m[120-140|0-19]"));
        CHECK_TIMELINE(tl);
        CHECK_EQ(clampTrimEdit(base(), e, -100, kSrc), -49); // c may be 50 frames before its in, a only up to 1 frame
    }

    // --- moveClips: overwrites at the target, V1 -> V2 creates a missing track; not below V1
    {
        Ids ids;
        Timeline tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 60, 40);
        tl.audio[0].clips << mk(3, "a", 0, 50);
        moveClips(tl, {2}, -30, TrackKind::Video, 0, ids.gen());
        CHECK_EQ(dump(tl), QString("V1: a[0-30|0-29] b[30-70|0-39]  A1: a[0-50|0-49]"));
        CHECK_TIMELINE(tl);
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Video, -1), 0);
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Video, 1), 0); // V2 does not exist (yet)
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Audio, 1), 0);
        tl.video << Track{};
        moveClips(tl, {1, 3}, 0, TrackKind::Video, 1, ids.gen());
        CHECK_EQ(dump(tl), QString("V1: b[30-70|0-39]  V2: a[0-30|0-29]  A1:  A2: a[0-50|0-49]"));
        CHECK_TIMELINE(tl);
    }

    // --- Transitions at edges (effective length)
    {
        Timeline tl = tracks(1, 0);
        // a ends at 100 (source 0-99, 100 frames handle), b starts at 100 with in 50 (50 frames handle)
        tl.video[0].clips << mk(1, "a", 0, 100) << mk(2, "b", 100, 100, 50);
        tl.video[0].clips[0].transOut = 20;
        tl.video[0].clips[1].transIn = 20;
        auto spans = transitions(tl.video[0], kSrc);
        CHECK(spans.size() == 1 && spans[0].isDissolve() && spans[0].start == 90 && spans[0].end == 110
              && spans[0].cut == 100 && spans[0].leftId == 1 && spans[0].rightId == 2);
        // Start on Edit: entirely after the cut
        tl.video[0].clips[0].transOutStyle.align = TransitionAlign::Start;
        spans = transitions(tl.video[0], kSrc);
        CHECK(spans.size() == 1 && spans[0].start == 100 && spans[0].end == 120);
        // no handles before b (in 0): centered only 1 frame possible, End on Edit not at all
        tl.video[0].clips[0].transOutStyle.align = TransitionAlign::Center;
        tl.video[0].clips[1].in = 0;
        tl.video[0].clips[1].out = 99;
        spans = transitions(tl.video[0], kSrc);
        CHECK(spans.size() == 1 && spans[0].start == 100 && spans[0].end == 101);
        tl.video[0].clips[0].transOutStyle.align = TransitionAlign::End;
        CHECK(transitions(tl.video[0], kSrc).isEmpty());
        // Fade in/out (no neighbor at the edge), limited to the clip length
        tl = tracks(1, 0);
        tl.video[0].clips << mk(1, "a", 10, 30) << mk(2, "b", 50, 30);
        tl.video[0].clips[0].transIn = 12;
        tl.video[0].clips[0].transOut = 25; // only 18 frames fit after the fade in
        tl.video[0].clips[1].transIn = 8;
        spans = transitions(tl.video[0], kSrc);
        CHECK_EQ(spans.size(), 3);
        if (spans.size() == 3) {
            CHECK(spans[0].leftId == 0 && spans[0].rightId == 1 && spans[0].start == 10 && spans[0].end == 22);
            CHECK(spans[1].leftId == 1 && spans[1].rightId == 0 && spans[1].start == 22 && spans[1].end == 40);
            CHECK(spans[2].leftId == 0 && spans[2].rightId == 2 && spans[2].start == 50 && spans[2].end == 58);
        }
        // detachTransitions: only one side affected -> dissolve released
        tl = tracks(1, 0);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 50, 50, 50);
        tl.video[0].clips[0].transOut = tl.video[0].clips[1].transIn = 10;
        detachTransitions(tl, {1, 2});
        CHECK(tl.video[0].clips[0].transOut == 10);
        detachTransitions(tl, {2});
        CHECK(tl.video[0].clips[0].transOut == 0 && tl.video[0].clips[1].transIn == 0);
    }

    // --- Curves: fade handles and audio transitions
    {
        Clip c = mk(1, "a", 0, 100);
        c.fadeIn = 10;
        c.fadeOut = 20;
        CHECK(fadeRamp(c, 0) == 0.0 && fadeRamp(c, 5) == 0.5 && fadeRamp(c, 50) == 1.0 && fadeRamp(c, 99) == 0.0);
        CHECK(std::abs(audioFadeGain(c, 5) - std::sin(M_PI / 4)) < 1e-9);
        TransitionSpan s{1, 2, 100, 120, 110, {}};
        CHECK(audioTransitionGain(s, 2, 99) == 1.0 && audioTransitionGain(s, 3, 110) == 1.0);
        CHECK(audioTransitionGain(s, 2, 100) == 0.0);
        s.style.audio = AudioCurve::Zero; // 0 dB: linear, both sides add up to 1 (apart from 1 frame offset)
        CHECK(std::abs(audioTransitionGain(s, 2, 110) - 0.5) < 1e-9);
        CHECK(std::abs(audioTransitionGain(s, 1, 110) + audioTransitionGain(s, 2, 110) - 0.95) < 1e-9);
    }

    // --- Timeline markers follow ripple edits of the whole timeline
    {
        using M = QVector<int>;
        const M marks{10, 50, 60, 99, 100, 120, 200};
        auto three = [&] {
            Timeline tl = tracks(2, 1);
            tl.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 50, 50, 50) << mk(3, "c", 100, 50, 100);
            tl.markers = marks;
            return tl;
        };
        // Ripple delete: before stays, inside goes (the cut start stays), after moves up; equal ones merge
        Timeline tl = three();
        rippleDelete(tl, {2});
        CHECK_EQ(dump(tl).section("  ", 0, 0), QString("V1: a[0-50|0-49] c[50-100|100-149]"));
        CHECK_EQ(tl.markers, M({10, 50, 70, 150}));
        // Deleting the first clip: a marker at 0 stays, the rest moves by its length
        tl = three();
        tl.markers = {0, 25, 50};
        rippleDelete(tl, {1});
        CHECK_EQ(tl.markers, M({0}));
        // Two ranges at once (a and c, b stays): each range takes its markers, the offsets add up
        tl = three();
        tl.markers = {5, 50, 75, 110, 160};
        rippleDelete(tl, {1, 3});
        CHECK_EQ(tl.markers, M({0, 25, 60}));
        // A locked track with content after the cut: only part of the timeline moves -> the markers stay
        tl = three();
        tl.audio[0].clips << mk(9, "s", 0, 150);
        tl.audio[0].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(findClip(tl, 3)->start, 50);
        CHECK_EQ(tl.markers, marks);
        // ... content on the locked track only before the cut does not hold them
        tl = three();
        tl.audio[0].clips << mk(9, "s", 0, 40);
        tl.audio[0].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(tl.markers, M({10, 50, 70, 150}));
        // ... nor an empty locked track
        tl = three();
        tl.video[1].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(tl.markers, M({10, 50, 70, 150}));
        // An unlocked track that stays because it would overwrite: markers stay too
        tl = three();
        tl.video[1].clips << mk(7, "x", 0, 80) << mk(8, "y", 110, 20);
        rippleDelete(tl, {2});
        CHECK_EQ(findClip(tl, 8)->start, 110);
        CHECK_EQ(tl.markers, marks);

        // Ripple trim, end: the trimmed-off tail takes its markers, the rest moves
        tl = three();
        tl.markers = {40, 45, 49, 50, 70};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc);
        CHECK_EQ(findClip(tl, 2)->start, 40);
        CHECK_EQ(tl.markers, M({40, 60}));
        // ... longer: markers from the old end on move
        tl = three();
        tl.markers = {49, 50, 70};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, 10, kSrc);
        CHECK_EQ(tl.markers, M({49, 60, 80}));
        // Ripple trim, head: the trimmed-off head goes, the clip's rest keeps its marker on the same picture
        tl = three();
        tl.markers = {50, 55, 70, 100};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start}, 10, kSrc);
        CHECK_EQ(dump(tl).section("  ", 0, 0), QString("V1: a[0-50|0-49] b[50-90|60-99] c[90-140|100-149]"));
        CHECK_EQ(tl.markers, M({50, 60, 90}));
        // ... longer at the head: the clip's own markers move with its content
        tl = three();
        tl.markers = {49, 50, 70};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start}, -10, kSrc);
        CHECK_EQ(findClip(tl, 2)->in, 40);
        CHECK_EQ(tl.markers, M({49, 60, 80}));
        // Ripple trim with a locked track holding content later: markers stay
        tl = three();
        tl.markers = {45, 70};
        tl.audio[0].clips << mk(9, "s", 0, 150);
        tl.audio[0].locked = true;
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc);
        CHECK_EQ(tl.markers, M({45, 70}));

        // Taking out time by default (content removed: ripple delete, ranges): a marker in it goes, the rest moves up
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(3, "c", 80, 50);
        tl.markers = {50, 60, 80, 140};
        rippleTracks(tl, {{80, -30}}, {});
        CHECK_EQ(findClip(tl, 3)->start, 50);
        CHECK_EQ(tl.markers, M({50, 110}));
        // Closing an empty gap / a transition's trim (MarkerCut::MoveToCut): markers in the gap land on the cut point
        // and merge there, the rest moves up
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(3, "c", 80, 50);
        tl.markers = {20, 55, 60, 79, 80, 140};
        rippleTracks(tl, {{80, -30}}, {}, nullptr, MarkerCut::MoveToCut);
        CHECK_EQ(findClip(tl, 3)->start, 50);
        CHECK_EQ(tl.markers, M({20, 50, 110}));
        tl.markers = {55, 140};
        rippleAnnotations(tl, {{100, -20}, {60, -10}}, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}}, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({50, 110})); // each range to its own cut, the offsets add up
        // ... a locked track holding content later keeps them, as for every ripple
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(3, "c", 80, 50);
        tl.audio[0].clips << mk(9, "s", 0, 150);
        tl.audio[0].locked = true;
        tl.markers = {60, 140};
        rippleTracks(tl, {{80, -30}}, {}, nullptr, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({60, 140}));
        // Ripple trim with MoveToCut (a transition's trim): the trimmed-off tail's markers land on the new cut
        tl = three();
        tl.markers = {40, 45, 49, 50, 70};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({40, 60}));
        tl = three();
        tl.markers = {30, 45, 70};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({30, 40, 60}));
        // ... the head: the trimmed-off head's markers land on the clip's start
        tl = three();
        tl.markers = {50, 55, 70, 100};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start}, 10, kSrc, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({50, 60, 90}));
        tl = three();
        tl.markers = {52, 70, 100};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start}, 10, kSrc, MarkerCut::MoveToCut);
        CHECK_EQ(tl.markers, M({50, 60, 90}));

        // Insert (rippleAnnotations with the tracks that moved): from the insert point on, markers move
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 100);
        tl.markers = {49, 50, 60};
        Ids gen;
        insertGap(tl, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}}, 50, 20, gen.gen(), gen.gen());
        rippleAnnotations(tl, {{50, 20}}, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}});
        CHECK_EQ(tl.markers, M({49, 70, 80}));
        // ... a locked track with a clip across the insert point keeps them
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 100);
        tl.audio[0].clips << mk(2, "s", 0, 100);
        tl.audio[0].locked = true;
        tl.markers = {49, 50, 60};
        insertGap(tl, {{TrackKind::Video, 0}}, 50, 20, gen.gen(), gen.gen());
        rippleAnnotations(tl, {{50, 20}}, {{TrackKind::Video, 0}});
        CHECK_EQ(tl.markers, M({49, 50, 60}));
        // A moving ripple (ripple overwrite with a longer clip, retime) without markers: nothing to do
        tl = three();
        tl.markers.clear();
        rippleTracks(tl, {{100, 20}}, {});
        CHECK(tl.markers.isEmpty());
    }

    // Notes and story sections follow ripples by the markers' rules (rippleAnnotations, same code path)
    {
        using Sec = QVector<QPair<int, int>>;
        auto note = [](int id, int at) { return TimelineNote{id, at, QString("n%1").arg(id), false}; };
        auto notesAt = [](const Timeline& tl) {
            QVector<QPair<int, int>> out; // (id, at)
            for (const TimelineNote& n : tl.notes) out << qMakePair(n.id, n.at);
            return out;
        };
        auto secs = [](const Timeline& tl) {
            Sec out;
            for (const StorySection& s : tl.sections) out << qMakePair(s.from, s.to);
            return out;
        };
        auto three = [&] {
            Timeline tl = tracks(2, 1);
            tl.video[0].clips << mk(1, "a", 0, 50) << mk(2, "b", 50, 50, 50) << mk(3, "c", 100, 50, 100);
            tl.notes = {note(1, 10), note(2, 50), note(3, 60), note(4, 99), note(5, 100), note(6, 140)};
            tl.sections = {StorySection{StoryKind::Hook, {}, 0, 60}, StorySection{StoryKind::Build, "mid", 60, 90},
                           StorySection{StoryKind::Resolution, {}, 90, 150}};
            return tl;
        };
        // Ripple delete (content removed): before stays, a note at the cut start stays, inside goes, after moves up;
        // sections get shorter, one only inside goes, one starting inside starts at the cut
        Timeline tl = three();
        rippleDelete(tl, {2});
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 10}, {2, 50}, {5, 50}, {6, 90}}));
        CHECK_EQ(secs(tl), (Sec{{0, 50}, {50, 100}}));
        CHECK(tl.sections[0].kind == StoryKind::Hook && tl.sections[1].kind == StoryKind::Resolution);
        CHECK_EQ(tl.notes[3].text, QString("n6"));
        // ... a section reaching over the cut gets shorter by the time taken out
        tl = three();
        tl.sections = {StorySection{StoryKind::Build, {}, 20, 120}};
        rippleDelete(tl, {2});
        CHECK_EQ(secs(tl), (Sec{{20, 70}}));
        // Deleting after everything: nothing changes (also not the order or the ids)
        tl = three();
        tl.video[0].clips << mk(4, "d", 150, 30, 150);
        const Timeline kept = tl;
        rippleDelete(tl, {4});
        CHECK(tl.notes == kept.notes && tl.sections == kept.sections);
        // Deleting before everything: all move up, the first section starts at 0
        tl = three();
        rippleDelete(tl, {1});
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{2, 0}, {3, 10}, {4, 49}, {5, 50}, {6, 90}}));
        CHECK_EQ(secs(tl), (Sec{{0, 10}, {10, 40}, {40, 100}}));
        // A locked track with content after the cut: notes and sections stay
        tl = three();
        tl.audio[0].clips << mk(9, "s", 0, 150);
        tl.audio[0].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(findClip(tl, 3)->start, 50);
        CHECK(tl.notes == three().notes && tl.sections == three().sections);
        // ... an empty locked track does not hold them
        tl = three();
        tl.video[1].locked = true;
        rippleDelete(tl, {2});
        CHECK_EQ(secs(tl), (Sec{{0, 50}, {50, 100}}));

        // Closing an empty gap (MoveToCut): notes in the gap land on the cut point (they keep their own, no merging),
        // sections shrink to the cut, one only in the gap goes
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(3, "c", 80, 50);
        tl.notes = {note(1, 20), note(2, 55), note(3, 79), note(4, 80), note(5, 140)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 55}, StorySection{StoryKind::Free, {}, 60, 70},
                       StorySection{StoryKind::Build, {}, 70, 130}};
        rippleTracks(tl, {{80, -30}}, {}, nullptr, MarkerCut::MoveToCut);
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 20}, {2, 50}, {3, 50}, {4, 50}, {5, 110}}));
        CHECK_EQ(secs(tl), (Sec{{0, 50}, {50, 100}}));
        // ... with a locked track holding content later: all stay
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 50) << mk(3, "c", 80, 50);
        tl.audio[0].clips << mk(9, "s", 0, 150);
        tl.audio[0].locked = true;
        tl.notes = {note(1, 60)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 100}};
        rippleTracks(tl, {{80, -30}}, {}, nullptr, MarkerCut::MoveToCut);
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 60}}));
        CHECK_EQ(secs(tl), (Sec{{0, 100}}));

        // Ripple trim, end: the trimmed-off tail takes its notes and shortens its section, the rest moves
        tl = three();
        tl.notes = {note(1, 45), note(2, 50), note(3, 70)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 50}, StorySection{StoryKind::Build, {}, 50, 150}};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc);
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{2, 40}, {3, 60}}));
        CHECK_EQ(secs(tl), (Sec{{0, 40}, {40, 140}}));
        // ... longer: the section that ends there grows with the clip
        tl = three();
        tl.notes = {note(1, 49), note(2, 50)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 50}, StorySection{StoryKind::Build, {}, 50, 150}};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, 10, kSrc);
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 49}, {2, 60}}));
        CHECK_EQ(secs(tl), (Sec{{0, 60}, {60, 160}}));
        // ... a transition's trim (MoveToCut): notes of the trimmed-off tail land on the new cut
        tl = three();
        tl.notes = {note(1, 45), note(2, 70)};
        applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, {1}, {}, Edge::End}, -10, kSrc, MarkerCut::MoveToCut);
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 40}, {2, 60}}));

        // Insert: from the insert point on notes move; time inserted inside or at a section's end joins it
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 100);
        tl.notes = {note(1, 49), note(2, 50), note(3, 60)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 50}, StorySection{StoryKind::Build, {}, 50, 80},
                       StorySection{StoryKind::Resolution, {}, 80, 100}};
        Ids gen;
        insertGap(tl, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}}, 50, 20, gen.gen(), gen.gen());
        rippleAnnotations(tl, {{50, 20}}, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}});
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 49}, {2, 70}, {3, 80}}));
        CHECK_EQ(secs(tl), (Sec{{0, 70}, {70, 100}, {100, 120}}));
        // ... at 0 (an intro): everything moves back
        rippleAnnotations(tl, {{0, 30}}, {{TrackKind::Video, 0}, {TrackKind::Audio, 0}});
        CHECK_EQ(secs(tl), (Sec{{30, 100}, {100, 130}, {130, 150}}));
        // ... a locked track with a clip across the insert point keeps them
        tl = tracks(1, 1);
        tl.video[0].clips << mk(1, "a", 0, 100);
        tl.audio[0].clips << mk(2, "s", 0, 100);
        tl.audio[0].locked = true;
        tl.notes = {note(1, 60)};
        tl.sections = {StorySection{StoryKind::Hook, {}, 0, 80}};
        insertGap(tl, {{TrackKind::Video, 0}}, 50, 20, gen.gen(), gen.gen());
        rippleAnnotations(tl, {{50, 20}}, {{TrackKind::Video, 0}});
        CHECK_EQ(notesAt(tl), (QVector<QPair<int, int>>{{1, 60}}));
        CHECK_EQ(secs(tl), (Sec{{0, 80}}));
    }

    CHECK_EQ(endFrame(Timeline{}), 0);
    return Check::result();
}
