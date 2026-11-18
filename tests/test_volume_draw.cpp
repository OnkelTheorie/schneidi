// SPDX-License-Identifier: AGPL-3.0-only
// Drawing a volume curve with the pen (Studio page, core/VolumeDraw.h, Editor::drawVolume):
// - resampling one value per frame (later strokes win, back-and-forth strokes), smoothing, thinning within tolerance;
// - a stroke over part of a clip: keys only under the stroke, the old curve kept just before and after it (the static
//   volume of a clip without keys, the old keyframe curve of one with keys), values as drawn;
// - one undo step per stroke, also over two clips; undo gives the clips back as they were;
// - nothing (and no undo step) beside the clips, on a locked track, for an empty stroke;
// - the volume line's scale (posOf/dbAt) is the mixer fader's: -inf..-20 / -20..0 / 0..+12 dB.

#include "check.h"

#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/Selection.h"
#include "core/VolumeDraw.h"

#include <QCoreApplication>
#include <QUndoStack>
#include <cmath>

namespace {

using VolumeDraw::Point;

void testPure()
{
    // Scale
    CHECK(std::abs(VolumeDraw::posOf(kMinVolumeDb)) < 1e-9);
    CHECK(std::abs(VolumeDraw::posOf(-20) - 0.25) < 1e-9);
    CHECK(std::abs(VolumeDraw::posOf(0) - 0.75) < 1e-9);
    CHECK(std::abs(VolumeDraw::posOf(kMaxVolumeDb) - 1.0) < 1e-9);
    for (double db : {-55.0, -20.0, -6.5, 0.0, 3.0, 12.0}) CHECK(std::abs(VolumeDraw::dbAt(VolumeDraw::posOf(db)) - db) < 1e-9);

    // Resample: a ramp 10..20 from -20 to 0 dB, one value per frame
    int first = -1;
    QVector<double> v = VolumeDraw::resample({{10, -20}, {20, 0}}, &first);
    CHECK_EQ(first, 10);
    CHECK_EQ(int(v.size()), 11);
    CHECK(std::abs(v[0] + 20) < 1e-9 && std::abs(v[5] + 10) < 1e-9 && std::abs(v[10]) < 1e-9);
    // Back over it: what was drawn last wins (frames 15..20 at -30)
    v = VolumeDraw::resample({{10, -20}, {20, 0}, {15, -30}}, &first);
    CHECK_EQ(int(v.size()), 11);
    CHECK(std::abs(v[2] + 16) < 1e-9);  // frame 12: the first segment
    CHECK(std::abs(v[5] + 30) < 1e-9);  // frame 15: the last point
    CHECK(std::abs(v[10]) < 1e-9);      // frame 20: the turn
    // Drawn from right to left
    v = VolumeDraw::resample({{30, 0}, {20, -10}}, &first);
    CHECK_EQ(first, 20);
    CHECK(std::abs(v[0] + 10) < 1e-9 && std::abs(v[10]) < 1e-9);
    // Clamped to the volume range
    v = VolumeDraw::resample({{0, -200}, {2, 50}}, &first);
    CHECK_EQ(v.first(), kMinVolumeDb);
    CHECK_EQ(v.last(), kMaxVolumeDb);

    // Smooth: a step becomes a slope; a constant stays; the ends stay near their values
    QVector<double> step(20, -20.0);
    for (int i = 10; i < 20; ++i) step[i] = 0;
    const QVector<double> s = VolumeDraw::smooth(step, 3);
    CHECK(s[9] > -20 && s[9] < 0 && s[10] > -20 && s[10] < 0);
    CHECK(std::abs(s[0] + 20) < 1e-9 && std::abs(s[19]) < 1e-9);
    CHECK(VolumeDraw::smooth(step, 0) == step);

    // Thin: a straight line needs only its ends; a corner keeps the corner; every value within the tolerance
    QVector<double> line;
    for (int i = 0; i < 50; ++i) line << -30 + i * 0.5;
    CHECK(VolumeDraw::thin(line) == QVector<int>({0, 49}));
    QVector<double> vee;
    for (int i = 0; i < 41; ++i) vee << -std::abs(i - 20) * 1.0;
    const QVector<int> keep = VolumeDraw::thin(vee);
    CHECK(keep.contains(20) && keep.first() == 0 && keep.last() == 40);
    QVector<double> wobble;
    for (int i = 0; i < 200; ++i) wobble << 6 * std::sin(i / 9.0);
    const QVector<int> w = VolumeDraw::thin(wobble);
    CHECK(w.size() < 60);
    for (int k = 1; k < w.size(); ++k)
        for (int i = w[k - 1]; i <= w[k]; ++i) {
            const double line2 = wobble[w[k - 1]] + (wobble[w[k]] - wobble[w[k - 1]]) * double(i - w[k - 1]) / (w[k] - w[k - 1]);
            if (!CHECK(std::abs(line2 - wobble[i]) <= VolumeDraw::kTolerance + 1e-9)) break;
        }
}

Clip audioClip(int id, int start, int length, int in = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = "/x/a.wav";
    c.start = start;
    c.in = in;
    c.out = in + length - 1;
    return c;
}

const Clip& clipOf(const Project& p, int track, int i) { return p.timeline().audio[track].clips[i]; }

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.edit("setup", [](Timeline& tl) {
        tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}};
        Clip a = audioClip(1, 0, 100, 20); // source offset: keyframes are stored at in + t
        a.volumeDb = -6;
        tl.audio[0].clips << a << audioClip(2, 100, 100);
        Clip locked = audioClip(3, 0, 100);
        tl.audio[1].clips << locked;
        tl.audio[1].locked = true;
    });
    QUndoStack* undo = p.undoStack();

    // A dip over frames 40..60 of the first clip (no smoothing): -20 dB flat
    int idx = undo->index();
    CHECK(ed.drawVolume(0, {{40, -20}, {60, -20}}, 0));
    CHECK_EQ(undo->index(), idx + 1);
    CHECK_EQ(undo->undoText(), QString("Lautstärke zeichnen"));
    const Clip& a = clipOf(p, 0, 0);
    CHECK(Keys::animated(a, AnimParam::Volume));
    const KeyTrack& k = a.keys[AnimParam::Volume];
    // The old level just outside, the drawn one inside; keys stored at source frames (in = 20)
    CHECK_EQ(int(k.size()), 4);
    if (k.size() == 4) {
        CHECK_EQ(k[0].frame, 20 + 39);
        CHECK(std::abs(k[0].value + 6) < 1e-9);
        CHECK_EQ(k[1].frame, 20 + 40);
        CHECK(std::abs(k[1].value + 20) < 1e-9);
        CHECK_EQ(k[2].frame, 20 + 60);
        CHECK(std::abs(k[2].value + 20) < 1e-9);
        CHECK_EQ(k[3].frame, 20 + 61);
        CHECK(std::abs(k[3].value + 6) < 1e-9);
    }
    CHECK(std::abs(Keys::valueAt(a, AnimParam::Volume, 10) + 6) < 1e-9);
    CHECK(std::abs(Keys::valueAt(a, AnimParam::Volume, 50) + 20) < 1e-9);
    CHECK(std::abs(Keys::valueAt(a, AnimParam::Volume, 90) + 6) < 1e-9);
    CHECK(!Keys::animated(clipOf(p, 0, 1), AnimParam::Volume)); // the second clip is not under it

    // A ramp drawn over the dip replaces its keys under the stroke, the rest of the curve stays
    idx = undo->index();
    CHECK(ed.drawVolume(0, {{45, -30}, {55, 0}}, 0));
    CHECK_EQ(undo->index(), idx + 1);
    const Clip& a2 = clipOf(p, 0, 0);
    CHECK(std::abs(Keys::valueAt(a2, AnimParam::Volume, 42) + 20) < 1e-6); // old dip before the stroke
    CHECK(std::abs(Keys::valueAt(a2, AnimParam::Volume, 45) + 30) < 1e-6);
    CHECK(std::abs(Keys::valueAt(a2, AnimParam::Volume, 50) + 15) < 0.3); // the ramp (thinned within tolerance)
    CHECK(std::abs(Keys::valueAt(a2, AnimParam::Volume, 55)) < 1e-6);
    CHECK(std::abs(Keys::valueAt(a2, AnimParam::Volume, 58) + 20) < 1e-6); // old dip after it
    int inside = 0;
    for (const Keyframe& key : a2.keys[AnimParam::Volume])
        if (key.frame - a2.in >= 45 && key.frame - a2.in <= 55) ++inside;
    CHECK(inside >= 2 && inside <= 4); // a straight ramp: its ends (the thinning keeps no more)

    // One stroke over both clips: one undo step, keys on both, the cut between them follows the line
    idx = undo->index();
    CHECK(ed.drawVolume(0, {{80, -10}, {120, -10}}, 2));
    CHECK_EQ(undo->index(), idx + 1);
    CHECK(std::abs(Keys::valueAt(clipOf(p, 0, 0), AnimParam::Volume, 90) + 10) < 0.01);
    const Clip& b = clipOf(p, 0, 1);
    CHECK(Keys::animated(b, AnimParam::Volume));
    CHECK(std::abs(Keys::valueAt(b, AnimParam::Volume, 5) + 10) < 0.01);
    CHECK(std::abs(Keys::valueAt(b, AnimParam::Volume, 60)) < 1e-9); // its static 0 dB after the stroke

    // Smoothing: a jagged stroke gives fewer keys than without
    auto jagged = [] {
        QVector<Point> s;
        for (int f = 110; f <= 190; f += 2) s << Point{double(f), (f / 2) % 2 ? -10.0 : -14.0};
        return s;
    };
    CHECK(ed.drawVolume(0, jagged(), 0));
    const int rough = int(clipOf(p, 0, 1).keys[AnimParam::Volume].size());
    undo->undo();
    CHECK(ed.drawVolume(0, jagged(), 5));
    const int smooth = int(clipOf(p, 0, 1).keys[AnimParam::Volume].size());
    CHECK(smooth < rough);
    undo->undo();

    // Nothing changes beside the clips, on a locked track, for an empty stroke: no undo step
    idx = undo->index();
    CHECK(!ed.drawVolume(0, {{300, -10}, {320, -10}}, 0));
    CHECK(!ed.drawVolume(1, {{10, -10}, {50, -10}}, 0));
    CHECK(!ed.drawVolume(0, {}, 0));
    CHECK(!ed.drawVolume(5, {{10, -10}}, 0));
    // A click without dragging, a stroke straight up and down in place, one frame wide: ignored (no 1-frame dip/spike)
    CHECK(!ed.drawVolume(0, {{50, -10}}, 0));
    CHECK(!ed.drawVolume(0, {{50, -30}, {50.2, 6}, {49.9, -40}, {50, 0}}, 0));
    CHECK(!ed.drawVolume(0, {{50, -30}, {51, -30}}, 0));
    CHECK(!VolumeDraw::spansEnough({{50, -30}, {51, -30}}));
    CHECK(VolumeDraw::spansEnough({{50, -30}, {52, -30}}));
    CHECK(VolumeDraw::spansEnough({{52, -30}, {50, -30}}));
    {
        Clip c = audioClip(9, 0, 100);
        CHECK(!VolumeDraw::apply(c, {{50, -30}, {50, 6}}, 0));
        CHECK(!Keys::animated(c, AnimParam::Volume));
    }
    CHECK_EQ(undo->index(), idx);
    CHECK(!Keys::animated(clipOf(p, 1, 0), AnimParam::Volume));

    // Undo all: the clips as before
    while (undo->index() > 1) undo->undo();
    CHECK(!Keys::animated(clipOf(p, 0, 0), AnimParam::Volume));
    CHECK(!Keys::animated(clipOf(p, 0, 1), AnimParam::Volume));
    CHECK_EQ(clipOf(p, 0, 0).volumeDb, -6.0);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("volume-draw");
    testPure();
    testEditor();
    return Check::result();
}
