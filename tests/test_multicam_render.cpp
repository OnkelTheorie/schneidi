// SPDX-License-Identifier: AGPL-3.0-only
// Multicam rendering (docs/archiv/plan-0.8.md): a segment shows picture and sound of its active angle (also video-only
// and audio-only switches, several streams of one angle, an angle's muted stream, gaps of a split recording), the sync
// offset is respected frame-exactly and follows a resync, the sound runs on across a through edit. The grid for the
// viewer: all angles of a page in their cells (auto and fixed layout, pages, aspect ratio kept), at the right sync
// position, sound = only the active angle, switched live without rebuilding, fine detail of a large angle smoothed
// into its small cell instead of dropped.
#include "check.h"

#include "core/Editor.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QImage>
#include <QTemporaryDir>
#include <clocale>
#include <cmath>
#include <cstring>

namespace {

using Mode = Multicam::Mode;
constexpr double kPi = 3.14159265358979323846;
const ProjectFormat kFmt{320, 180, {25, 1}};

QString g_dir;

// Colored angle with its own tone (lossless sound: exact frequencies)
QString angleFile(const QString& name, const QString& color, int hz, const QString& size = "320x180")
{
    return Check::makeMedia(g_dir + "/" + name + ".mkv",
                            {"-f", "lavfi", "-i", QString("color=%1:size=%2:rate=25:duration=8").arg(color, size),
                             "-f", "lavfi", "-i", QString("sine=frequency=%1:duration=8:sample_rate=48000").arg(hz),
                             "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le",
                             "-shortest"});
}

MediaInfo info(const QString& path, int length, bool video = true, int streams = 1)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = streams > 0;
    for (int k = 0; streams > 1 && k < streams; ++k) m.audioStreams << AudioStreamInfo{2, QString("s%1").arg(k)};
    return m;
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    Fixture()
    {
        ProjectData d;
        d.format = kFmt;
        d.timeline = emptyTimeline();
        p.load(d);
    }
};

QImage image(Mlt::Producer& tr, int pos)
{
    tr.seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr.get_frame());
    f->set("consumer.rescale", "bilinear"); // as the preview consumer (Engine) and the export scale
    mlt_image_format ifmt = mlt_image_rgba;
    int w = kFmt.width, h = kFmt.height;
    const uint8_t* data = f->get_image(ifmt, w, h);
    if (!data) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    return img.copy();
}

QImage render(const Timeline& tl, int pos)
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    return image(*tr, pos);
}

// First channel of the mixed sound of frames [pos, pos + frames), after two settling frames
std::vector<float> sound(Mlt::Producer& tr, int pos, int frames)
{
    std::vector<float> out;
    tr.seek(std::max(0, pos - 2));
    for (int i = std::max(0, pos - 2); i < pos + frames; ++i) {
        std::unique_ptr<Mlt::Frame> f(tr.get_frame());
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(25.f, freq, i);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (pcm && i >= pos) out.insert(out.end(), pcm, pcm + samples);
    }
    return out;
}

std::vector<float> sound(const Timeline& tl, int pos, int frames = 4)
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    return sound(*tr, pos, frames);
}

// Level of one frequency (Goertzel, dB of the amplitude)
double toneDb(const std::vector<float>& x, double hz)
{
    if (x.empty()) return -200;
    const double w = 2 * kPi * hz / 48000, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) {
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double mag = std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / double(x.size());
    return mag > 0 ? 20 * std::log10(mag) : -200;
}

// Which of the angle tones sounds loudest
int dominant(const std::vector<float>& x, const QVector<int>& tones = {440, 880, 1320, 2000})
{
    int best = 0;
    double bestDb = -150;
    for (int hz : tones)
        if (const double db = toneDb(x, hz); db > bestDb) {
            bestDb = db;
            best = hz;
        }
    return best;
}

double rms(const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to && i < x.size(); ++i) s += double(x[i]) * x[i];
    return to > from ? std::sqrt(s / double(to - from)) : 0;
}

bool near(QRgb c, int r, int g, int b)
{
    return std::abs(qRed(c) - r) < 40 && std::abs(qGreen(c) - g) < 40 && std::abs(qBlue(c) - b) < 40;
}

// Frame number of the luma ramp (frame n: Y = 16 + 2n) in a region
int frameOf(const QImage& img, const QRect& r)
{
    double sum = 0;
    int n = 0;
    for (int y = r.top() + r.height() / 4; y < r.bottom() - r.height() / 4; y += 3)
        for (int x = r.left() + r.width() / 4; x < r.right() - r.width() / 4; x += 3) {
            sum += qGray(img.pixel(x, y));
            ++n;
        }
    return n ? int(std::lround(sum / n * 219.0 / 255.0 / 2.0)) : -1;
}

struct Files {
    QString red, lime, blue, yellow, ramp, two, narrow, part1, part2, lines;
};
Files g;

// ---- The timeline: active angle ----

void testActiveAngleVideo()
{
    Fixture f;
    for (const QString& path : {g.red, g.lime, g.blue}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip("RGB", {{"R", {{g.red, 0}}}, {"G", {{g.lime, 0}}}, {"B", {{g.blue, 0}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    f.ed.multicamCut(50, 1, Mode::Both);
    f.ed.multicamCut(120, 2, Mode::Both);
    const Timeline tl = f.p.renderTimeline();
    CHECK(near(render(tl, 25).pixel(160, 90), 255, 0, 0));
    CHECK(near(render(tl, 75).pixel(160, 90), 0, 255, 0));
    CHECK(near(render(tl, 150).pixel(160, 90), 0, 0, 255));
    // Exactly at the cut: the new angle
    CHECK(near(render(tl, 50).pixel(160, 90), 0, 255, 0));
    CHECK(near(render(tl, 49).pixel(160, 90), 255, 0, 0));
    // Changing the angle afterwards changes the picture
    f.ed.multicamSwitch(75, 2, Mode::Both);
    CHECK(near(render(f.p.renderTimeline(), 75).pixel(160, 90), 0, 0, 255));
    // Without the nested content (plain timeline) nothing of the angles is drawn
    CHECK(near(render(f.p.timeline(), 25).pixel(160, 90), 0, 0, 0));
}

void testActiveAngleAudio()
{
    Fixture f;
    for (const QString& path : {g.red, g.lime, g.blue}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip("RGB", {{"R", {{g.red, 0}}}, {"G", {{g.lime, 0}}}, {"B", {{g.blue, 0}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    f.ed.multicamCut(50, 1, Mode::Both);
    f.ed.multicamCut(120, 2, Mode::Both);
    f.ed.multicamCut(170, 0, Mode::Video); // picture back to red, sound stays blue's
    const Timeline tl = f.p.renderTimeline();
    CHECK_EQ(dominant(sound(tl, 20)), 440);
    CHECK_EQ(dominant(sound(tl, 70)), 880);
    CHECK_EQ(dominant(sound(tl, 140)), 1320);
    CHECK_EQ(dominant(sound(tl, 180)), 1320);
    CHECK(near(render(tl, 180).pixel(160, 90), 255, 0, 0));
    // Only one angle sounds at a time
    const std::vector<float> s = sound(tl, 70);
    CHECK(toneDb(s, 880) > toneDb(s, 440) + 30 && toneDb(s, 880) > toneDb(s, 1320) + 30);
    // Audio-only switch
    f.ed.multicamCut(20, 2, Mode::Audio);
    CHECK_EQ(dominant(sound(f.p.renderTimeline(), 25)), 1320);
    CHECK(near(render(f.p.renderTimeline(), 25).pixel(160, 90), 255, 0, 0));
}

void testAngleWithSeveralStreams()
{
    // An OBS-like angle with two streams (440 Hz, 2000 Hz): its audio part plays both; a stream switched off inside
    // the multicam clip stays off
    Fixture f;
    f.p.addMedia(info(g.two, 200, true, 2));
    f.p.addMedia(info(g.lime, 200));
    const int seq = f.ed.createMulticamClip("OBS", {{"OBS", {{g.two, 0}}}, {"G", {{g.lime, 0}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    std::vector<float> s = sound(f.p.renderTimeline(), 30);
    CHECK(toneDb(s, 440) > toneDb(s, 880) + 20);
    CHECK(toneDb(s, 2000) > toneDb(s, 880) + 20);
    CHECK(std::abs(toneDb(s, 440) - toneDb(s, 2000)) < 6);

    const int timeline = f.p.currentSequence();
    f.p.setCurrentSequence(seq);
    const int second = f.p.timeline().audio.value(1).clips.value(0).id; // stream 1 (2000 Hz)
    f.ed.modifyClips({second}, "off", [](Clip& c) { c.enabled = false; });
    f.p.setCurrentSequence(timeline);
    s = sound(f.p.renderTimeline(), 30);
    CHECK(toneDb(s, 440) > toneDb(s, 2000) + 30);
}

void testSyncOffsetRendered()
{
    // The ramp angle starts 10 frames into the sequence: at sequence frame 60 it shows its own frame 50
    Fixture f;
    f.p.addMedia(info(g.red, 200));
    f.p.addMedia(info(g.ramp, 100));
    const int seq = f.ed.createMulticamClip("Ramp", {{"R", {{g.red, 0}}}, {"Ramp", {{g.ramp, 10}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    f.ed.multicamCut(30, 1, Mode::Both);
    CHECK(std::abs(frameOf(render(f.p.renderTimeline(), 60), QRect(0, 0, 320, 180)) - 50) <= 1);
    CHECK(std::abs(frameOf(render(f.p.renderTimeline(), 30), QRect(0, 0, 320, 180)) - 20) <= 1);
    // Resync: the angle 5 frames later -> the same timeline frame shows 5 frames earlier
    f.ed.nudgeMulticamAngle(seq, 1, 5);
    CHECK(std::abs(frameOf(render(f.p.renderTimeline(), 60), QRect(0, 0, 320, 180)) - 45) <= 1);
    // A moved segment shows the same sequence frames at its new place
    const int seg = f.p.timeline().video[0].clips.value(1).id;
    f.ed.moveClips({seg}, 500, TrackKind::Video, 0);
    CHECK(std::abs(frameOf(render(f.p.renderTimeline(), 560), QRect(0, 0, 320, 180)) - 45) <= 1);
}

void testThroughEditSoundContinuous()
{
    // A video-only cut splits the sound part too, but it must play on without a dip or click
    Fixture f;
    for (const QString& path : {g.red, g.lime}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip("RG", {{"R", {{g.red, 0}}}, {"G", {{g.lime, 0}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    f.ed.multicamCut(100, 1, Mode::Video);
    const std::vector<float> s = sound(f.p.renderTimeline(), 96, 8); // frames 96..103, cut after 4 frames
    const size_t perFrame = 1920, cut = 4 * perFrame;
    if (!CHECK(s.size() >= 8 * perFrame)) return;
    const double before = rms(s, cut - perFrame, cut), after = rms(s, cut, cut + perFrame);
    CHECK(std::abs(20 * std::log10(after / before)) < 0.5);
    double maxStep = 0, cutStep = std::abs(double(s[cut]) - s[cut - 1]);
    for (size_t i = 1; i < s.size(); ++i)
        if (i != cut) maxStep = std::max(maxStep, std::abs(double(s[i]) - s[i - 1]));
    CHECK(cutStep <= maxStep * 1.2);
}

void testGapInAngleIsBlackAndSilent()
{
    // Split recording: angle 2 has no material between its two files
    Fixture f;
    f.p.addMedia(info(g.red, 200));
    f.p.addMedia(info(g.part1, 50));
    f.p.addMedia(info(g.part2, 50));
    const int seq = f.ed.createMulticamClip("Split", {{"R", {{g.red, 0}}}, {"P", {{g.part1, 0}, {g.part2, 100}}}});
    f.ed.addSequenceAt(seq, 0, 0);
    f.ed.multicamCut(20, 1, Mode::Both);
    const Timeline tl = f.p.renderTimeline();
    CHECK(near(render(tl, 30).pixel(160, 90), 0, 255, 0));  // part1 (lime)
    CHECK(near(render(tl, 75).pixel(160, 90), 0, 0, 0));    // gap: black, not the red angle below
    CHECK(near(render(tl, 120).pixel(160, 90), 0, 0, 255)); // part2 (blue)
    const std::vector<float> gap = sound(tl, 70);
    CHECK(rms(gap, 0, gap.size()) < 1e-4);
}

// ---- The grid of the viewer ----

QImage gridImage(const Sequence& s, const MulticamGridOptions& o, int pos, MulticamGridHooks* hooks = nullptr)
{
    auto prof = makeProfile(kFmt);
    TimelineBuilder b(*prof);
    auto tr = b.buildMulticamGrid(s.timeline, s.angles, o, hooks);
    if (!tr) return {};
    return image(*tr, pos);
}

void testGridShowsAllAnglesInCells()
{
    Fixture f;
    for (const QString& path : {g.red, g.lime, g.blue, g.yellow}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip(
        "Four", {{"R", {{g.red, 0}}}, {"G", {{g.lime, 0}}}, {"B", {{g.blue, 0}}}, {"Y", {{g.yellow, 0}}}});
    const QImage img = gridImage(*f.p.sequence(seq), {}, 10);
    if (!CHECK(!img.isNull())) return;
    // Auto layout for 4 angles: 2x2, angle order left to right, top to bottom
    CHECK(near(img.pixel(80, 45), 255, 0, 0));
    CHECK(near(img.pixel(240, 45), 0, 255, 0));
    CHECK(near(img.pixel(80, 135), 0, 0, 255));
    CHECK(near(img.pixel(240, 135), 255, 255, 0));
    // One angle only: the whole picture
    const int one = f.ed.createMulticamClip("One", {{"R", {{g.red, 0}}}});
    const QImage single = gridImage(*f.p.sequence(one), {}, 10);
    CHECK(near(single.pixel(10, 10), 255, 0, 0) && near(single.pixel(310, 170), 255, 0, 0));
}

void testGridPagesAndEmptyCells()
{
    Fixture f;
    for (const QString& path : {g.red, g.lime, g.blue, g.yellow}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip(
        "Five", {{"1", {{g.red, 0}}}, {"2", {{g.lime, 0}}}, {"3", {{g.blue, 0}}}, {"4", {{g.yellow, 0}}}, {"5", {{g.lime, 0}}}});
    const Sequence& s = *f.p.sequence(seq);
    // Auto: 5 angles -> 3x3 on one page, empty cells black
    QImage img = gridImage(s, {}, 10);
    CHECK(near(img.pixel(53, 30), 255, 0, 0));   // cell 0
    CHECK(near(img.pixel(160, 90), 0, 255, 0));  // cell 4 = angle 5 (middle)
    CHECK(near(img.pixel(267, 150), 0, 0, 0));   // cell 8: empty
    // Fixed 2x2: page 2 shows angle 5 top left, the rest empty
    MulticamGridOptions o;
    o.columns = 2;
    o.page = 1;
    img = gridImage(s, o, 10);
    CHECK(near(img.pixel(80, 45), 0, 255, 0));
    CHECK(near(img.pixel(240, 45), 0, 0, 0));
    CHECK(near(img.pixel(240, 135), 0, 0, 0));
    // Page beyond the last: clamped to the last page
    o.page = 9;
    CHECK(near(gridImage(s, o, 10).pixel(80, 45), 0, 255, 0));
}

void testGridFollowsSyncPosition()
{
    Fixture f;
    f.p.addMedia(info(g.red, 200));
    f.p.addMedia(info(g.ramp, 100));
    const int seq = f.ed.createMulticamClip("Ramp", {{"R", {{g.red, 0}}}, {"Ramp", {{g.ramp, 10}}}});
    // 2 angles -> 2x2 cells (auto), ramp in the top right cell; grid frame = sequence frame
    QImage img = gridImage(*f.p.sequence(seq), {}, 60);
    CHECK(std::abs(frameOf(img, QRect(160, 0, 160, 90)) - 50) <= 1);
    // Before the ramp starts its cell is black
    img = gridImage(*f.p.sequence(seq), {}, 5);
    CHECK(near(img.pixel(240, 45), 0, 0, 0));
}

void testGridKeepsAspect()
{
    // A 4:3 angle in a 16:9 cell: pillarboxed, not stretched
    Fixture f;
    f.p.addMedia(info(g.narrow, 200));
    f.p.addMedia(info(g.red, 200));
    const int seq = f.ed.createMulticamClip("Aspect", {{"N", {{g.narrow, 0}}}, {"R", {{g.red, 0}}}});
    const QImage img = gridImage(*f.p.sequence(seq), {}, 10);
    // Cell 0 is 160x90; a 4:3 picture of height 90 is 120 wide -> 20 px bars on each side
    CHECK(near(img.pixel(80, 45), 0, 0, 255));
    CHECK(near(img.pixel(5, 45), 0, 0, 0));
    CHECK(near(img.pixel(155, 45), 0, 0, 0));
}

void testGridSmoothsFineDetail()
{
    // 1280x720 angle with a 1-px black line every 4 px into a cell of the 4x4 grid (80x45, 16x smaller): averaged to
    // gray, not sampled away (a few source columns only -> stripes, or all white / all black)
    Fixture f;
    f.p.addMedia(info(g.lines, 200));
    f.p.addMedia(info(g.red, 200));
    const int seq = f.ed.createMulticamClip("Lines", {{"L", {{g.lines, 0}}}, {"R", {{g.red, 0}}}});
    MulticamGridOptions o;
    o.columns = 4;
    const QImage img = gridImage(*f.p.sequence(seq), o, 10);
    if (!CHECK(!img.isNull())) return;
    double sum = 0;
    int n = 0, lo = 255, hi = 0;
    for (int y = 10; y < 35; y += 3)
        for (int x = 8; x < 72; ++x) {
            const int v = qGray(img.pixel(x, y));
            sum += v;
            ++n;
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    const double mean = sum / n; // source: 3/4 white
    CHECK(mean > 160 && mean < 225);
    CHECK(hi - lo < 60); // even gray, no stripes left over
}

void testGridSoundIsActiveAngleOnlyAndLive()
{
    Fixture f;
    for (const QString& path : {g.red, g.lime, g.blue}) f.p.addMedia(info(path, 200));
    const int seq = f.ed.createMulticamClip("RGB", {{"R", {{g.red, 0}}}, {"G", {{g.lime, 0}}}, {"B", {{g.blue, 0}}}});
    const Sequence& s = *f.p.sequence(seq);
    auto prof = makeProfile(kFmt);
    TimelineBuilder b(*prof);
    MulticamGridHooks hooks;
    MulticamGridOptions o;
    o.activeAngle = 1;
    auto tr = b.buildMulticamGrid(s.timeline, s.angles, o, &hooks);
    if (!CHECK(tr)) return;
    std::vector<float> x = sound(*tr, 20, 4);
    CHECK_EQ(dominant(x), 880);
    CHECK(toneDb(x, 880) > toneDb(x, 440) + 30);
    // Switch live: same tractor, next frames have the new angle's sound
    CHECK(TimelineBuilder::applyMulticamAudio(hooks, 2));
    x = sound(*tr, 40, 4);
    CHECK_EQ(dominant(x), 1320);
    CHECK(toneDb(x, 1320) > toneDb(x, 880) + 30);
    CHECK(!TimelineBuilder::applyMulticamAudio(hooks, 7)); // no such angle: unchanged
    CHECK_EQ(dominant(sound(*tr, 60, 4)), 1320);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("multicam-render");
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg fehlt");
    QTemporaryDir dir;
    g_dir = dir.path();
    g.red = angleFile("red", "red", 440);
    g.lime = angleFile("lime", "lime", 880);
    g.blue = angleFile("blue", "blue", 1320);
    g.yellow = angleFile("yellow", "yellow", 2000);
    g.narrow = angleFile("narrow", "blue", 440, "240x180");
    g.part1 = Check::makeMedia(g_dir + "/part1.mkv", {"-f", "lavfi", "-i", "color=lime:size=320x180:rate=25:duration=2",
                                                      "-f", "lavfi", "-i", "sine=frequency=880:duration=2:sample_rate=48000",
                                                      "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                      "-c:a", "pcm_s16le", "-shortest"});
    g.part2 = Check::makeMedia(g_dir + "/part2.mkv", {"-f", "lavfi", "-i", "color=blue:size=320x180:rate=25:duration=2",
                                                      "-f", "lavfi", "-i", "sine=frequency=1320:duration=2:sample_rate=48000",
                                                      "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                                      "-c:a", "pcm_s16le", "-shortest"});
    g.ramp = Check::makeMedia(g_dir + "/ramp.mp4", {"-f", "lavfi", "-i", "color=black:size=320x180:rate=25:duration=4", "-vf",
                                                    "geq=lum='16+2*N':cb=128:cr=128", "-c:v", "libx264", "-qp", "0",
                                                    "-pix_fmt", "yuv420p"});
    g.two = Check::makeMedia(g_dir + "/two.mkv", {"-f", "lavfi", "-i", "color=red:size=320x180:rate=25:duration=8",
                                                  "-f", "lavfi", "-i", "sine=frequency=440:duration=8:sample_rate=48000",
                                                  "-f", "lavfi", "-i", "sine=frequency=2000:duration=8:sample_rate=48000",
                                                  "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "libx264",
                                                  "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "pcm_s16le",
                                                  "-shortest"});
    g.lines = Check::makeMedia(g_dir + "/lines.mp4", {"-f", "lavfi", "-i", "color=white:size=1280x720:rate=25:duration=8",
                                                      "-vf", "geq=lum='if(eq(mod(X,4),1),16,235)':cb=128:cr=128",
                                                      "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p"});
    for (const QString& file : {g.red, g.lime, g.blue, g.yellow, g.narrow, g.part1, g.part2, g.ramp, g.two, g.lines})
        if (!CHECK(!file.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    {
        auto prof = makeProfile(kFmt);
        Mlt::Filter qt(*prof, "qtext");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }

    testActiveAngleVideo();
    testActiveAngleAudio();
    testAngleWithSeveralStreams();
    testSyncOffsetRendered();
    testThroughEditSoundContinuous();
    testGapInAngleIsBlackAndSilent();
    testGridShowsAllAnglesInCells();
    testGridPagesAndEmptyCells();
    testGridFollowsSyncPosition();
    testGridKeepsAspect();
    testGridSmoothsFineDetail();
    testGridSoundIsActiveAngleOnlyAndLive();
    return Check::result();
}
