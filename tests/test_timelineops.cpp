// SPDX-License-Identifier: AGPL-3.0-only
// Test TimelineOps basic cases (pure functions on the model): overwriting (placeClip/clearRange), splitting,
// ripple delete, insert gap, trimming (normal and trim mode ripple/roll/slip/slide), moving,
// effective transitions at edges, fade/transition curves
#include "check.h"

#include "core/GradeWindows.h"
#include "core/Keyframes.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>

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
        placeClip(t, mk(1, "a", 0, 100), ids.gen());
        CHECK_EQ(t.clips.size(), 1);
        // into the middle: a is split, right part new ID and provisional link (-old, see resolvePendingLinks),
        // transition/fade at the cut edges gone
        t.clips[0].linkId = 7;
        t.clips[0].transIn = t.clips[0].transOut = 10;
        t.clips[0].fadeIn = t.clips[0].fadeOut = 5;
        placeClip(t, mk(2, "b", 40, 20), ids.gen());
        Timeline tl = tracks(1, 0);
        tl.video[0] = t;
        CHECK_EQ(dump(tl), QString("V1: a[0-40|0-39] b[40-60|0-19] a[60-100|60-99]"));
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
        clearRange(tl.video[0], 30, 70, ids.gen());
        CHECK_EQ(dump(tl), QString("V1: a[0-30|0-29] a[70-100|70-99]"));
        clearRange(tl.video[0], 60, 60, ids.gen()); // empty range: nothing
        clearRange(tl.video[0], 0, 30, ids.gen());
        CHECK_EQ(dump(tl), QString("V1: a[70-100|70-99]"));
        placeClip(tl.video[0], mk(3, "c", 60, 50), ids.gen()); // covers a entirely
        CHECK_EQ(dump(tl), QString("V1: c[60-110|0-49]"));
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
        const Clip* v = findClip(tl, right[0]);
        const Clip* a = findClip(tl, right[1]);
        CHECK(v && a && v->linkId == 51 && a->linkId == 51 && findClip(tl, 1)->linkId == 5);
        CHECK(findClip(tl, 1)->transOut == 0 && findClip(tl, 1)->fadeIn == 4 && v->transOut == 12 && v->fadeIn == 0);
        CHECK_EQ(linkedGroup(tl, 1), (QVector<int>{1, 2}));
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
        // Ripple start: clip stays put, content moves, rest moves up
        tl = base();
        e = TrimEdit{TrimKind::Ripple, {2}, {}, Edge::Start};
        applyTrimEdit(tl, e, 10, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-50|50-99] b[50-90|60-99] c[90-140|50-99]  A1: m[110-130|0-19]"));
        // Ripple with two (linked) clips of the same track: the rear one moves along, nothing overlaps
        // (order of ids does not matter; before, the rear one stayed put and c slid under it)
        for (const QVector<int>& order : {QVector<int>{1, 2}, QVector<int>{2, 1}}) {
            tl = base();
            applyTrimEdit(tl, TrimEdit{TrimKind::Ripple, order, {}, Edge::End}, -10, kSrc);
            CHECK_EQ(dump(tl), QString("V1: a[0-40|50-89] b[40-80|50-89] c[80-130|50-99]  A1: m[100-120|0-19]"));
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
        // Slip: position stays, content shifted (in >= 0, out <= 199)
        tl = base();
        e = TrimEdit{TrimKind::Slip, {2}, {}, Edge::End};
        CHECK_EQ(clampTrimEdit(tl, e, -80, kSrc), -50);
        CHECK_EQ(clampTrimEdit(tl, e, 200, kSrc), 100);
        applyTrimEdit(tl, e, 30, kSrc);
        CHECK_EQ(dump(tl), QString("V1: a[0-50|50-99] b[50-100|80-129] c[100-150|50-99]  A1: m[120-140|0-19]"));
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
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Video, -1), 0);
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Video, 1), 0); // V2 does not exist (yet)
        CHECK_EQ(clampTrackDelta(tl, {1, 3}, TrackKind::Audio, 1), 0);
        tl.video << Track{};
        moveClips(tl, {1, 3}, 0, TrackKind::Video, 1, ids.gen());
        CHECK_EQ(dump(tl), QString("V1: b[30-70|0-39]  V2: a[0-30|0-29]  A1:  A2: a[0-50|0-49]"));
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

    CHECK_EQ(endFrame(Timeline{}), 0);
    return Check::result();
}
