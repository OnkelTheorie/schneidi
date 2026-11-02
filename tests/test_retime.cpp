// SPDX-License-Identifier: AGPL-3.0-only
// Test speed ramps (DaVinci Retime Controls): RetimeMap (hard, soft, reversed, inversion), editor edits with
// ripple/partners/keyframes/undo, project file, rendered frames and sound (which source picture is shown).
#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/ClipFormat.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>
#include <cstring>

namespace {

bool close(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

MediaInfo media(const QString& path, int length, bool video = true, bool audio = true)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = audio;
    return m;
}

const Clip* clipOn(const Timeline& tl, TrackKind kind, int track, int index)
{
    const auto& tracks = tl.tracks(kind);
    if (track >= tracks.size() || index >= tracks[track].clips.size()) return nullptr;
    return &tracks[track].clips[index];
}

void testMap()
{
    Clip c;
    c.speed = 0.5;
    {
        const RetimeMap m(c, 100);
        CHECK(close(m.sourceAt(10), 5));
        CHECK(close(m.materialAt(5), 10));
        CHECK_EQ(m.length(), 200);
        CHECK_EQ(c.retimedLength(100), 200);
    }
    // hard speed point: normal up to source 50, twice as fast afterwards
    c.speed = 1.0;
    c.ramp = {SpeedPoint{50, 2.0, 0}};
    {
        const RetimeMap m(c, 100);
        CHECK(close(m.sourceAt(40), 40));
        CHECK(close(m.sourceAt(60), 70));
        CHECK(close(m.speedAt(60), 2.0));
        CHECK_EQ(m.length(), 75);
        CHECK_EQ(c.retimedLength(100), 75);
        CHECK_EQ(m.pointMaterial().size(), 1);
        CHECK(close(m.pointMaterial()[0], 50));
    }
    // soft transition over 10 frames: same outside, continuous and invertible inside
    c.ramp[0].smooth = 10;
    {
        const RetimeMap m(c, 100);
        CHECK(close(m.sourceAt(45), 45));
        CHECK(close(m.sourceAt(55), 60));
        CHECK(close(m.sourceAt(70), 90));
        CHECK(close(m.sourceAt(50), 51.25)); // 45 + 5 + (2 − 1) · 25 / 20
        CHECK(close(m.speedAt(50), 1.5));
        for (double x : {0.0, 44.5, 46.0, 50.0, 53.3, 55.0, 70.0})
            CHECK(close(m.materialAt(m.sourceAt(x)), x, 1e-6));
        CHECK_EQ(m.length(), 75);
    }
    // too long a transition is limited to the neighboring sections (no overlap)
    c.ramp = {SpeedPoint{20, 2.0, 100}, SpeedPoint{40, 1.0, 100}};
    {
        const RetimeMap m(c, 100);
        double prev = -1;
        for (int x = 0; x < m.length(); ++x) {
            const double s = m.sourceAt(x);
            CHECK(s > prev);
            prev = s;
        }
        CHECK(close(m.sourceAt(90), 100 - (m.length() - 90) * 1.0, 1.0));
    }
    // reversed: file frame mirrored
    c.ramp = {SpeedPoint{50, 2.0, 0}};
    c.reverse = true;
    {
        const RetimeMap m(c, 100);
        CHECK(close(m.fileFrameAt(0), 99));
        CHECK(close(m.fileFrameAt(60), 99 - 70));
        CHECK(close(m.materialAtFile(29), 60));
    }
    // Tidy up: sorted, outside/too close discarded, speed limited
    QVector<SpeedPoint> r{{80, 3, 0}, {10, 0, -5}, {10.5, 2, 0}, {150, 2, 0}, {-3, 2, 0}};
    Retime::normalize(r, 100);
    CHECK_EQ(r.size(), 2);
    CHECK(close(r[0].source, 10) && close(r[0].speed, 1.0) && r[0].smooth == 0);
    CHECK(close(r[1].source, 80) && close(r[1].speed, 3));

}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();
    p.addMedia(media("/x/a.mp4", 100));
    p.addMedia(media("/x/b.mp4", 100));
    ed.addMediaAt({"/x/a.mp4"}, 0);
    ed.addMediaAt({"/x/b.mp4"}, 100);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99] b[100-200|0-99]  V2:  A1: a[0-100|0-99] b[100-200|0-99]  A2:"));
    const int v = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    CHECK(ed.canRetime(v));
    // Keyframe at source position 80
    ed.modifyClips({v}, "key", [](Clip& c) { c.keys[AnimParam::Opacity] = {Keyframe{80, 0.5}}; });

    // Add point: length stays, partner (audio) gets the same point
    const int steps = undo->count();
    ed.addSpeedPoint(v, 50);
    CHECK_EQ(undo->count(), steps + 1);
    const Clip* cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    const Clip* ca = clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    CHECK(cv->ramp.size() == 1 && close(cv->ramp[0].source, 50) && close(cv->ramp[0].speed, 1.0));
    CHECK(ca->ramp == cv->ramp);
    CHECK_EQ(cv->length(), 100);
    ed.addSpeedPoint(v, 50); // same point: nothing
    CHECK_EQ(undo->count(), steps + 1);

    // Double the speed of the second section: clip 75 long, b moves up, keyframe stays at its source position
    ed.setSegmentSpeed(v, 1, 2.0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-75|0-74] b[75-175|0-99]  V2:  A1: a[0-75|0-74] b[75-175|0-99]  A2:"));
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(cv->keys.value(AnimParam::Opacity).value(0).frame, 65); // 50 + 30 / 2

    // Point 10 frames to the right: before it speed 1 continues -> source 60, length 60 + 40 / 2 = 80
    ed.moveSpeedPoint(v, 0, 10);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(close(cv->ramp[0].source, 60));
    CHECK_EQ(cv->length(), 80);
    CHECK_EQ(clipOn(p.timeline(), TrackKind::Video, 0, 1)->start, 80);

    // Soft transition does not change the length (centered around the point)
    ed.setSpeedPointSmooth(v, 0, 12);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(cv->ramp[0].smooth, 12);
    CHECK_EQ(cv->length(), 80);

    // Trimming: material reaches up to the file end (length of the ramp)
    CHECK_EQ(cv->retimedLength(100), 80);

    // Remove: normal length again
    ed.removeSpeedPoint(v, 0);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(cv->ramp.isEmpty());
    CHECK_EQ(cv->length(), 100);

    // Undo chain back to before the point
    while (undo->index() > steps) undo->undo();
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99] b[100-200|0-99]  V2:  A1: a[0-100|0-99] b[100-200|0-99]  A2:"));
    CHECK(clipOn(p.timeline(), TrackKind::Video, 0, 0)->ramp.isEmpty());
    undo->redo();
    undo->redo();
    CHECK_EQ(clipOn(p.timeline(), TrackKind::Video, 0, 0)->length(), 75);

    // Constant speed (Change Clip Speed) replaces the ramp; length from the source span
    ed.setClipSpeed({v}, Editor::Retime{1.0, false, false, true}, true);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(cv->ramp.isEmpty());
    CHECK_EQ(cv->length(), 100);

    // Freeze frame/title: no ramp
    ed.setClipSpeed({v}, Editor::Retime{1.0, false, true, true}, true);
    CHECK(!ed.canRetime(v));
}

void testProjectFile()
{
    Project p;
    p.addMedia(media("/x/a.mp4", 100));
    Selection sel;
    Editor ed(&p, &sel);
    ed.addMediaAt({"/x/a.mp4"}, 0);
    const int v = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    ed.addSpeedPoint(v, 30);
    ed.setSegmentSpeed(v, 1, 0.5);
    ed.setSpeedPointSmooth(v, 0, 8);
    const ProjectData d = p.data();
    const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
    CHECK_EQ(ProjectFile::toJson(back, "/x/p.schneidi"), json);
    const Clip* c = clipOn(back.timeline, TrackKind::Video, 0, 0);
    CHECK(c && c->ramp.size() == 1 && c->ramp[0].smooth == 8 && close(c->ramp[0].speed, 0.5));
}

// ---- Rendering: brightness encodes the source frame number ----

int frameAt(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    const QImage img = Check::renderFrame(tl, fmt, pos);
    if (img.isNull()) return -1;
    const int g = qGreen(img.pixel(img.width() / 2, img.height() / 2));
    return int(std::lround(g / 2.0)); // source: gray = 2 * frame
}

// The mix of `tl` frame by frame: left channel of frames [0, frames)
std::vector<std::vector<float>> soundFrames(const Timeline& tl, int frames)
{
    Audio::Mixer mixer(tl, FrameRate{25, 1});
    std::vector<std::vector<float>> out;
    for (int f = 0; f < frames; ++f) {
        const int64_t a = mixer.frameStart(f), b = mixer.frameStart(f + 1);
        std::vector<float> l(static_cast<size_t>(b - a)), r(static_cast<size_t>(b - a));
        float* planes[] = {l.data(), r.data()};
        mixer.render(a, int(b - a), planes);
        out.push_back(std::move(l));
    }
    return out;
}

// A clip alone on an audio track from frame 0
Timeline soundOf(const Clip& c)
{
    Timeline tl;
    tl.masterLimiter = false;
    tl.audio.resize(1);
    Clip at = c;
    at.start = 0;
    tl.audio[0].clips << at;
    return tl;
}

// Reversed at 100 %: audio stays in sync with the picture (beep in file frame 100 of 200 -> material frame ~98.5),
// also with "keep pitch" (Rubberband would only contribute its delay of ~3 frames)
void testReverseAudioSync(const QString& dir)
{
    const QString file = Check::makeMedia(dir + "/beep.mov",
                                          {"-f", "lavfi", "-i", "aevalsrc='if(between(t,4,4.04),sin(2*PI*1000*t),0)':s=48000:d=8",
                                           "-c:a", "pcm_s16le"});
    if (!CHECK(!file.isEmpty())) return;
    auto loudest = [&](const Clip& c) {
        const auto frames = soundFrames(soundOf(c), c.out + 1);
        int best = -1;
        double bestPeak = 0;
        for (int pos = 0; pos < int(frames.size()); ++pos)
            for (const float v : frames[size_t(pos)])
                if (std::abs(v) > bestPeak) {
                    bestPeak = std::abs(v);
                    best = pos;
                }
        return best;
    };
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.reverse = true;
    c.out = 199;
    for (bool pitch : {false, true}) {
        c.keepPitch = pitch;
        c.ramp.clear();
        const int got = loudest(c);
        CHECK(std::abs(got - 98.5) <= 1.5);
        // Speed ramp reversed: beep in the section at 100 %
        c.ramp = {SpeedPoint{150, 2.0, 0}};
        c.out = RetimeMap(c, 200).length() - 1;
        const int gotRamp = loudest(c);
        CHECK(std::abs(gotRamp - 98.5) <= 1.5);
        c.out = 199;
    }
}

// "Keep pitch" (the time stretcher) delays the audio; the mixer compensates its delay. Click in the timeline: with
// pitch at most ~1/2 frame off from without.
void testPitchAudioSync(const QString& dir)
{
    const QString file = Check::makeMedia(dir + "/click.wav",
                                          {"-f", "lavfi", "-i", "aevalsrc='if(between(t,4,4.002),0.9,0)':s=48000:d=12"});
    if (!CHECK(!file.isEmpty())) return;
    // first click sample in the timeline output
    auto clickAt = [&](const Clip& c) -> long {
        Timeline tl = emptyTimeline();
        tl.audio[0].clips << c;
        long at = 0;
        for (const std::vector<float>& f : soundFrames(tl, c.end())) {
            for (size_t i = 0; i < f.size(); ++i)
                if (std::abs(f[i]) > 0.2f) return at + long(i);
            at += long(f.size());
        }
        return -1;
    };
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    for (double speed : {0.5, 1.5, 2.0, 3.0}) {
        c.speed = speed;
        c.out = int(200 / speed) - 1; // file longer than the clip: room for the pre-roll
        c.keepPitch = false;
        const long plain = clickAt(c);
        c.keepPitch = true;
        const long pitched = clickAt(c);
        CHECK(plain > 0 && std::abs(pitched - plain) <= 1100); // ~1/2 frame at 25 fps
    }
    // up to the file end (pre-roll reads beyond the end) and reversed (timewarp computes from the end)
    for (bool reverse : {false, true}) {
        c.speed = 2.0;
        c.reverse = reverse;
        c.out = 300 / 2 - 1;
        c.keepPitch = false;
        const long plain = clickAt(c);
        c.keepPitch = true;
        const long pitched = clickAt(c);
        CHECK(plain > 0 && std::abs(pitched - plain) <= 1100);
    }
    c.reverse = false;
    // Speed ramp: click in the section at 150 % (fixed, with pitch)
    c.speed = 1.0;
    c.ramp = {SpeedPoint{50, 1.5, 0}};
    c.out = RetimeMap(c, 200).length() - 1;
    c.keepPitch = false;
    const long plain = clickAt(c);
    c.keepPitch = true;
    const long pitched = clickAt(c);
    CHECK(plain > 0 && std::abs(pitched - plain) <= 1100);
}

int testRender(const QString& dir)
{
    // Frame n has the gray value 2n (full range, lossless)
    const QString file = Check::makeMedia(dir + "/ramp.mkv",
                                          {"-f", "lavfi", "-i", "nullsrc=size=64x64:rate=25:duration=4,format=gray,geq=lum='2*N'",
                                           "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
                                           "-c:v", "ffv1", "-pix_fmt", "rgb24", "-c:a", "pcm_s16le", "-shortest"});
    if (!CHECK(!file.isEmpty())) return 1;
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt{64, 64, {25, 1}};

    Project p;
    p.load([&] {
        ProjectData d;
        d.format = fmt;
        d.timeline = emptyTimeline();
        return d;
    }());
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media(file, 100));
    ed.addMediaAt({file}, 0);
    const int v = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, 30), 30);
    ed.addSpeedPoint(v, 20);
    ed.setSegmentSpeed(v, 1, 2.0);
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, 10), 10);
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, 30), 40); // 20 + 10 · 2
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, 45), 70);
    // trimmed at the start: material keeps counting from the source
    const int id = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    ed.trimClip(id, TimelineOps::Edge::Start, 25);
    const Clip* c = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(c->in, 25);
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, c->start), 30);
    // reversed with ramp
    Timeline tl = p.renderTimeline();
    Clip& r = tl.video[0].clips[0];
    r.in = 0;
    r.out = 59;
    r.start = 0;
    r.reverse = true;
    CHECK_EQ(frameAt(tl, fmt, 0), 99);
    CHECK_EQ(frameAt(tl, fmt, 30), 99 - 40);

    // Split a freeze frame/trim at the start: both parts keep showing the same picture
    {
        Project q;
        q.load([&] {
            ProjectData d;
            d.format = fmt;
            d.timeline = emptyTimeline();
            return d;
        }());
        Selection qs;
        Editor qe(&q, &qs);
        q.addMedia(media(file, 100));
        qe.addMediaAt({file}, 0);
        const int still = clipOn(q.timeline(), TrackKind::Video, 0, 0)->id;
        qe.trimClip(still, TimelineOps::Edge::Start, 20); // clip starts at source 20
        qe.setClipSpeed({still}, Editor::Retime{1.0, false, true, true}, false);
        const Clip* s0 = clipOn(q.timeline(), TrackKind::Video, 0, 0);
        if (CHECK(s0 && s0->freeze)) {
            const int start = s0->start;
            CHECK_EQ(frameAt(q.renderTimeline(), fmt, start + 5), 20);
            qs.set({s0->id});
            qe.splitAtPlayhead(start + 30);
            CHECK_EQ(frameAt(q.renderTimeline(), fmt, start + 10), 20);
            CHECK_EQ(frameAt(q.renderTimeline(), fmt, start + 40), 20);
            const Clip* right = clipOn(q.timeline(), TrackKind::Video, 0, 1);
            if (CHECK(right)) {
                qe.trimClip(right->id, TimelineOps::Edge::Start, 10);
                CHECK_EQ(frameAt(q.renderTimeline(), fmt, clipOn(q.timeline(), TrackKind::Video, 0, 1)->start + 2), 20);
            }
        }
    }

    // Audio: in the fast section not silent (sine from the file); played through: the 100 % section without a gap,
    // in the 200 % section only a short settling (keep pitch)
    auto silentFrames = [&](const Clip& clip) {
        int silent = 0;
        for (const std::vector<float>& f : soundFrames(soundOf(clip), clip.length())) {
            float peak = 0;
            for (const float v : f) peak = std::max(peak, std::abs(v));
            if (peak < 0.04f) ++silent; // ffmpeg sine: ~0.09
        }
        return silent;
    };
    Clip a = *clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    CHECK(silentFrames(a) <= 2);
    // soft transition: steps without pitch correction -> no gaps
    ed.setSpeedPointSmooth(v, 0, 12);
    a = *clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    CHECK(silentFrames(a) <= 2);
    return 0;
}

// The one picture mapping (RetimeMap::pictureFrame) and its inverse: by the sound's time, no frame before the cut
void testPictureFrame()
{
    Clip c;
    c.speed = 0.5;
    {
        const RetimeMap m(c, 50);
        // material frame m starts at file time m / 2: frames 6 and 7 both show file frame 3
        CHECK_EQ(m.pictureFrame(6), 3);
        CHECK_EQ(m.pictureFrame(7), 3);
        CHECK_EQ(m.pictureFrame(8), 4);
        CHECK_EQ(m.pictureFrame(500), 49); // clamped to the file
        // first material frame showing file frame 3, and it really shows 3 (rounding gave 2 at times: a flash)
        CHECK_EQ(m.materialForPicture(3), 6);
        for (int f = 0; f < 50; ++f) CHECK_EQ(m.pictureFrame(m.materialForPicture(f)), f);
    }
    c.reverse = true;
    {
        const RetimeMap m(c, 50);
        CHECK_EQ(m.pictureFrame(0), 49);
        CHECK_EQ(m.pictureFrame(7), 46); // file time 50 - 3.5 = 46.5
        for (int f = 0; f < 50; ++f) CHECK_EQ(m.pictureFrame(m.materialForPicture(f)), f);
    }
    c.reverse = false;
    c.speed = 2.0;
    {
        const RetimeMap m(c, 50);
        CHECK_EQ(m.pictureFrame(7), 14);
        CHECK_EQ(m.materialForPicture(15), 8); // skipped: the next frame shows 16
    }
    // speeds that are not exact in binary: still no frame before
    for (double speed : {0.3, 0.7, 1.0 / 3, 0.9}) {
        c.speed = speed;
        const RetimeMap m(c, 300);
        for (int f = 0; f < 300; ++f) CHECK_EQ(m.pictureFrame(m.materialForPicture(f)), f);
        CHECK_EQ(RetimeMap::materialForPicture(120, speed), m.materialForPicture(120));
    }
    // freeze frame: the still frame through the same mapping
    c.speed = 0.5;
    c.freeze = true;
    c.freezeFrame = 7;
    CHECK_EQ(RetimeMap(c, 50).pictureFrame(c.stillFrame()), 3);
}

// Changing a clip's speed keeps the picture at its start (it could show the frame before: a flash at the cut)
void testSpeedChangeKeepsFirstPicture()
{
    for (double speed : {0.5, 0.3, 0.7, 0.25, 2.0}) {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.addMedia(media("/x/a.mp4", 300));
        ed.addMediaAt({"/x/a.mp4"}, 0);
        const int v = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
        ed.trimClip(v, TimelineOps::Edge::Start, 37); // starts at file frame 37
        const Clip* before = clipOn(p.timeline(), TrackKind::Video, 0, 0);
        CHECK_EQ(before->in, 37);
        Editor::Retime r;
        r.speed = speed;
        ed.setClipSpeed({v}, r, true);
        const Clip* c = TimelineOps::findClip(p.timeline(), v);
        if (!CHECK(c)) continue;
        // faster than 1: frame 37 may be skipped, then the next shown one (never the one before)
        const int first = RetimeMap(*c, 300).pictureFrame(c->in);
        if (speed <= 1) CHECK_EQ(first, 37);
        else CHECK(first >= 37 && first < 37 + speed);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("retime");
    testMap();
    testPictureFrame();
    testSpeedChangeKeepsFirstPicture();
    testEditor();
    testProjectFile();
    if (!Check::haveFfmpeg()) {
        Check::result();
        return Check::skip("ffmpeg nicht gefunden (Render-Teil)");
    }
    QTemporaryDir tmp;
    if (CHECK(tmp.isValid()))
        if (const int r = testRender(tmp.path()); r == Check::kSkip) {
            Check::result();
            return r;
        }
    if (tmp.isValid()) testReverseAudioSync(tmp.path());
    if (tmp.isValid()) testPitchAudioSync(tmp.path());
    return Check::result();
}
