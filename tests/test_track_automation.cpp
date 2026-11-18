// SPDX-License-Identifier: AGPL-3.0-only
// Track volume automation (0.23, core/Automation.h, Track::volumeAuto, docs/plan-0.23.md "Spur-Automation"):
// - model: value between/before/after points, fader + curve clamped, normalising, replacing a range (old values kept
//   just outside), ripple (time taken out / put in, frame by frame against the definition), frame rate change;
// - edits: ripple delete, insert, ripple trim (end and head), retime move the curve with the time; a plain move, a
//   locked track leave it; an empty track's curve moves along with the others;
// - editor: one undo step per pen stroke (also over a gap between clips), stored on top of the fader (the fader
//   trims the whole curve), locked tracks refused, CLI path setTrackAutomation;
// - mixer: without a curve bit-identical; a constant 0 dB curve bit-identical too; curve -6 = fader -6; a ramp
//   sample-accurate (gains at frame boundaries, linear in between) and without steps, also a 1-frame jump; any block
//   size gives the same samples; a live change (updateMix) glides instead of stepping;
// - export: a WAV export plays the curve (second half 20 dB down);
// - project file: without automation no new field and an older format version (0.22 opens it as before); with it
//   format 8 (0.22 refuses) and a lossless roundtrip;
// - CLI: op track_volume (points, from/to, clear, errors), `info` lists volume_automation.

#include "check.h"

#include "core/Automation.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/VolumeDraw.h"
#include "engine/Exporter.h"
#include "engine/RenderQueue.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUndoStack>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFps = 25;
const ProjectFormat kFmt{320, 180, {kFps, 1}};

double gainOf(double db) { return db <= kMinVolumeDb ? 1e-10 : std::pow(10.0, db / 20.0); }

Clip audioClip(int id, int start, int length, const QString& path = {})
{
    Clip c;
    c.id = id;
    c.mediaPath = path.isEmpty() ? QStringLiteral("/fixtures/media/a.wav") : path;
    c.start = start;
    c.in = 0;
    c.out = length - 1;
    return c;
}

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

// The curve after a ripple, frame by frame against what it should be
bool sameAs(const AutoCurve& c, const std::function<double(int)>& expected, int upTo, const char* what)
{
    for (int f = 0; f <= upTo; ++f)
        if (!near(Automation::valueAt(c, f), expected(f), 1e-9)) {
            Check::report(false, what, __FILE__, __LINE__,
                          QString("frame %1: %2, expected %3").arg(f).arg(Automation::valueAt(c, f)).arg(expected(f)));
            return false;
        }
    Check::report(true, what, __FILE__, __LINE__);
    return true;
}

void testModel()
{
    using Automation::valueAt;
    CHECK_EQ(valueAt({}, 10), 0.0);
    const AutoCurve one{{20, -6}};
    CHECK(near(valueAt(one, 0), -6) && near(valueAt(one, 100), -6));
    const AutoCurve ramp{{10, 0}, {20, -20}};
    CHECK(near(valueAt(ramp, 0), 0) && near(valueAt(ramp, 15), -10) && near(valueAt(ramp, 17.5), -15)
          && near(valueAt(ramp, 99), -20));
    // Fader + curve, clamped; the bottom of the range is silent
    Track t{TrackKind::Audio};
    t.volumeDb = -6;
    t.volumeAuto = {{0, -72}};
    CHECK_EQ(Automation::levelAt(t, 5), kMinVolumeDb);
    t.volumeAuto = {{0, 30}};
    CHECK_EQ(Automation::levelAt(t, 5), kMaxVolumeDb);
    t.volumeAuto = {{0, -3}};
    CHECK(near(Automation::levelAt(t, 5), -9));

    // Normalised: sorted, one point per frame (the last given wins), no negative frames, values in range
    const AutoCurve n = Automation::normalized({{30, 1}, {10, 2}, {30, 3}, {-5, 0}, {40, 500}});
    CHECK(n == AutoCurve({{10, 2}, {30, 3}, {40, Automation::kMaxDb}}));

    // Replace a range on an empty curve: the fader level (0) held outside
    AutoCurve c;
    Automation::replaceRange(c, 10, 20, {{10, -12}, {20, -12}, {50, -40}}); // a point outside the range is dropped
    CHECK(c == AutoCurve({{9, 0}, {10, -12}, {20, -12}, {21, 0}}));
    CHECK(near(valueAt(c, 5), 0) && near(valueAt(c, 15), -12) && near(valueAt(c, 40), 0));
    // ... over an old curve: the old values right outside stay, points inside go
    AutoCurve old{{0, 0}, {100, -20}};
    Automation::replaceRange(old, 40, 60, {{40, -30}, {60, -30}});
    CHECK(near(valueAt(old, 39), -7.8) && near(valueAt(old, 50), -30) && near(valueAt(old, 61), -12.2)
          && near(valueAt(old, 100), -20) && near(valueAt(old, 0), 0));
    // ... from frame 0: nothing before it
    AutoCurve start;
    Automation::replaceRange(start, 0, 5, {{0, -6}, {5, -6}});
    CHECK(start == AutoCurve({{0, -6}, {5, -6}, {6, 0}}));

    // Ripple: time taken out [40, 60)
    const AutoCurve base{{0, 0}, {100, -20}};
    const auto oldAt = [&](double f) { return valueAt(base, f); };
    AutoCurve r = base;
    Automation::ripple(r, {{60, -20}});
    sameAs(r, [&](int f) { return f < 40 ? oldAt(f) : oldAt(f + 20); }, 150, "ripple: time taken out");
    // Time put in at 50 (30 frames): the value there holds
    r = base;
    Automation::ripple(r, {{50, 30}});
    sameAs(r, [&](int f) { return f < 50 ? oldAt(f) : f < 80 ? oldAt(50) : oldAt(f - 30); }, 160, "ripple: time put in");
    // Two pieces taken out (a ripple delete of two clips): [20, 30) and [70, 80)
    r = base;
    Automation::ripple(r, {{30, -10}, {80, -10}});
    sameAs(r, [&](int f) { return f < 20 ? oldAt(f) : f < 60 ? oldAt(f + 10) : oldAt(f + 20); }, 150, "ripple: two cuts");
    // A cut over a point: the point goes, both sides stay
    AutoCurve peak{{0, 0}, {50, -30}, {100, 0}};
    const AutoCurve peakOld = peak;
    Automation::ripple(peak, {{60, -20}});
    sameAs(peak, [&](int f) { return f < 40 ? valueAt(peakOld, f) : valueAt(peakOld, f + 20); }, 150, "ripple: cut over a point");
    // A flat curve stays one point (the helpers change nothing and go again)
    AutoCurve flat{{0, -6}};
    Automation::ripple(flat, {{60, -20}});
    CHECK(flat == AutoCurve({{0, -6}}));
    Automation::ripple(flat, {{10, 5}});
    CHECK(flat == AutoCurve({{0, -6}}));
    // Only points after the shift move when the time around them is plain (a gap closed before a point)
    AutoCurve later{{100, -10}, {120, 0}};
    Automation::ripple(later, {{50, -20}});
    CHECK(later == AutoCurve({{80, -10}, {100, 0}}));

    // Frame rate change: frames scaled
    AutoCurve rate{{25, -6}, {50, 0}};
    Automation::retime(rate, [](int f) { return f * 2; });
    CHECK(rate == AutoCurve({{50, -6}, {100, 0}}));

    // JSON
    CHECK(Automation::fromJson(Automation::toJson({{3, -1.5}, {9, 2}})) == AutoCurve({{3, -1.5}, {9, 2}}));
}

Timeline rippleTimeline()
{
    Timeline tl;
    tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}, Track{TrackKind::Audio}};
    tl.audio[0].clips << audioClip(1, 0, 50) << audioClip(2, 50, 50) << audioClip(3, 100, 50);
    tl.audio[0].volumeAuto = {{0, 0}, {150, -30}};
    tl.audio[1].volumeAuto = {{0, 0}, {150, -15}}; // no clips: still the same timeline time
    tl.audio[2].clips << audioClip(4, 160, 20);
    tl.audio[2].locked = true;
    tl.audio[2].volumeAuto = {{0, 0}, {150, -30}};
    return tl;
}

void testRipple()
{
    const Timeline orig = rippleTimeline();
    const AutoCurve a1 = orig.audio[0].volumeAuto, a2 = orig.audio[1].volumeAuto;
    const auto at = [](const AutoCurve& c) { return [&c](double f) { return Automation::valueAt(c, f); }; };
    int nextId = 100;
    const IdGen newId = [&] { return ++nextId; };

    // Ripple delete of the middle clip: [50, 100) goes on every unlocked track, the locked one stays
    {
        Timeline tl = orig;
        TimelineOps::rippleDelete(tl, {2});
        CHECK_EQ(TimelineOps::findClip(tl, 3)->start, 50);
        const auto o1 = at(a1), o2 = at(a2);
        sameAs(tl.audio[0].volumeAuto, [&](int f) { return f < 50 ? o1(f) : o1(f + 50); }, 200, "ripple delete: own track");
        sameAs(tl.audio[1].volumeAuto, [&](int f) { return f < 50 ? o2(f) : o2(f + 50); }, 200, "ripple delete: empty track");
        CHECK(tl.audio[2].volumeAuto == orig.audio[2].volumeAuto);
    }
    // Insert 25 frames at 50 on A1 (the tracks given): the value at 50 holds over the new time
    {
        Timeline tl = orig;
        TimelineOps::insertGap(tl, {TrackRef{TrackKind::Audio, 0}}, 50, 25, newId, newId);
        const auto o1 = at(a1);
        sameAs(tl.audio[0].volumeAuto, [&](int f) { return f < 50 ? o1(f) : f < 75 ? o1(50) : o1(f - 25); }, 200, "insert");
        CHECK(tl.audio[1].volumeAuto == a2); // not a track of the insert
    }
    // A plain move: the curve stays at its time
    {
        Timeline tl = orig;
        TimelineOps::moveClips(tl, {3}, 30, TrackKind::Audio, 0, newId);
        CHECK_EQ(TimelineOps::findClip(tl, 3)->start, 130);
        CHECK(tl.audio[0].volumeAuto == a1 && tl.audio[1].volumeAuto == a2);
    }
    // Ripple trim of clip 1's end by -10: [40, 50) goes (the other unlocked tracks follow by the same shift)
    {
        Timeline tl = orig;
        TimelineOps::TrimEdit e;
        e.kind = TimelineOps::TrimKind::Ripple;
        e.ids = {1};
        e.edge = TimelineOps::Edge::End;
        TimelineOps::applyTrimEdit(tl, e, -10, [](const Clip&) { return 1000; });
        CHECK_EQ(TimelineOps::findClip(tl, 2)->start, 40);
        const auto o1 = at(a1), o2 = at(a2);
        sameAs(tl.audio[0].volumeAuto, [&](int f) { return f < 40 ? o1(f) : o1(f + 10); }, 200, "ripple trim end");
        sameAs(tl.audio[1].volumeAuto, [&](int f) { return f < 40 ? o2(f) : o2(f + 10); }, 200, "ripple trim end: other track");
    }
    // Ripple trim of clip 2's head by +10: its first 10 frames go - on its own track at its start [50, 60)
    {
        Timeline tl = orig;
        TimelineOps::TrimEdit e;
        e.kind = TimelineOps::TrimKind::Ripple;
        e.ids = {2};
        e.edge = TimelineOps::Edge::Start;
        TimelineOps::applyTrimEdit(tl, e, 10, [](const Clip&) { return 1000; });
        CHECK_EQ(TimelineOps::findClip(tl, 2)->in, 10);
        CHECK_EQ(TimelineOps::findClip(tl, 3)->start, 90);
        const auto o1 = at(a1);
        sameAs(tl.audio[0].volumeAuto, [&](int f) { return f < 50 ? o1(f) : o1(f + 10); }, 200, "ripple trim head");
    }
    // Closing a gap (ripple on every track) through Editor, frame rate change through the project
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.edit("setup", [](Timeline& tl) {
            tl.audio = {Track{TrackKind::Audio}};
            tl.audio[0].clips << audioClip(1, 0, 50) << audioClip(2, 80, 50);
            tl.audio[0].volumeAuto = {{60, 0}, {100, -20}};
        });
        sel.set({1});
        ed.rippleDeleteSelection();
        CHECK_EQ(TimelineOps::findClip(p.timeline(), 2)->start, 30);
        CHECK(p.timeline().audio[0].volumeAuto == AutoCurve({{10, 0}, {50, -20}}));
    }
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.edit("setup", [](Timeline& tl) {
        tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}};
        tl.audio[0].clips << audioClip(1, 0, 50) << audioClip(2, 100, 50); // a gap 50..100
        tl.audio[1].clips << audioClip(3, 0, 100);
        tl.audio[1].locked = true;
    });
    QUndoStack* undo = p.undoStack();
    const int idx = undo->index();
    // A dip from 30 to 120 at -20 (over the gap), no smoothing
    CHECK(ed.drawTrackVolume(0, {{30, -20}, {120, -20}}, 0));
    CHECK_EQ(undo->index(), idx + 1);
    CHECK_EQ(undo->undoText(), QString("Spur-Lautstärke zeichnen"));
    const Track& t = p.timeline().audio[0];
    CHECK(near(Automation::levelAt(t, 75), -20) && near(Automation::levelAt(t, 30), -20) && near(Automation::levelAt(t, 120), -20));
    CHECK(near(Automation::levelAt(t, 10), 0) && near(Automation::levelAt(t, 140), 0));
    CHECK(near(Automation::levelAt(t, 29), 0) && near(Automation::levelAt(t, 121), 0));
    // The clips are not touched
    CHECK(TimelineOps::findClip(p.timeline(), 1)->keys.isEmpty());
    // The fader trims the whole curve; drawing again is relative to the fader (what the lane shows is what is drawn)
    ed.setTrackVolume(0, -6);
    CHECK(near(Automation::levelAt(p.timeline().audio[0], 75), -26) && near(Automation::levelAt(p.timeline().audio[0], 10), -6));
    CHECK(ed.drawTrackVolume(0, {{60, -10}, {90, -10}}, 0));
    CHECK(near(Automation::levelAt(p.timeline().audio[0], 75), -10));
    CHECK(near(Automation::valueAt(p.timeline().audio[0].volumeAuto, 75), -4));
    CHECK(near(Automation::levelAt(p.timeline().audio[0], 59), -26)); // the old curve right before the stroke
    // Undo: one stroke at a time
    undo->undo();
    CHECK(near(Automation::levelAt(p.timeline().audio[0], 75), -26));
    undo->undo();
    undo->undo();
    CHECK(p.timeline().audio[0].volumeAuto.isEmpty() && p.timeline().audio[0].volumeDb == 0);
    // Locked, too short, a click: nothing
    const int at = undo->index();
    CHECK(!ed.drawTrackVolume(1, {{10, -20}, {50, -20}}, 0));
    CHECK(!ed.drawTrackVolume(0, {{10, -20}, {11, -20}}, 0));
    CHECK(!ed.setTrackAutomation(1, {{0, -6}}));
    CHECK_EQ(undo->index(), at);
    // setTrackAutomation (CLI): normalised, one step, clear
    CHECK(ed.setTrackAutomation(0, {{50, -6}, {10, 0}}));
    CHECK(p.timeline().audio[0].volumeAuto == AutoCurve({{10, 0}, {50, -6}}));
    CHECK(!ed.setTrackAutomation(0, {{10, 0}, {50, -6}})); // the same: no step
    CHECK(ed.setTrackAutomation(0, {}));
    CHECK(p.timeline().audio[0].volumeAuto.isEmpty());
}

// Left channel of `tl`, rendered in the given block sizes (cycled)
std::vector<float> renderLeft(const Timeline& tl, const std::vector<int>& sizes = {4096})
{
    Audio::Mixer::Options o;
    o.fileLength = [](const QString&) { return 4 * kFps; };
    Audio::Mixer m(tl, FrameRate{kFps, 1}, o);
    std::vector<float> l(size_t(m.length())), r(size_t(m.length()));
    size_t k = 0;
    for (int64_t pos = 0; pos < m.length();) {
        const int n = int(std::min<int64_t>(m.length() - pos, sizes[k++ % sizes.size()]));
        float* buf[2] = {l.data() + pos, r.data() + pos};
        m.render(pos, n, buf);
        pos += n;
    }
    return l;
}

Timeline toneTimeline(const QString& tone)
{
    Timeline tl;
    tl.audio = {Track{TrackKind::Audio}};
    tl.audio[0].clips << audioClip(1, 0, 100, tone);
    return tl;
}

void testMixer(const QString& tone)
{
    const Timeline plain = toneTimeline(tone);
    const std::vector<float> ref = renderLeft(plain);
    if (!CHECK(ref.size() >= size_t(4 * kRate - 10))) return;
    // A constant 0 dB curve: the same samples, bit for bit
    Timeline zero = plain;
    zero.audio[0].volumeAuto = {{0, 0}};
    CHECK(renderLeft(zero) == ref);
    // Curve -6 on a 0 dB fader = fader -6 without curve (the same gain on every sample)
    Timeline fader = plain;
    fader.audio[0].volumeDb = -6;
    Timeline curve = plain;
    curve.audio[0].volumeAuto = {{0, -6}};
    CHECK(renderLeft(curve) == renderLeft(fader));
    // ... and fader -6 with curve +6 = nothing
    Timeline trimmed = fader;
    trimmed.audio[0].volumeAuto = {{0, 6}};
    {
        const std::vector<float> x = renderLeft(trimmed);
        double worst = 0;
        for (size_t i = 0; i < x.size(); ++i) worst = std::max(worst, double(std::abs(x[i] - ref[i])));
        Check::report(worst < 1e-6, "fader -6 + curve +6 = 0 dB", __FILE__, __LINE__, QString::number(worst));
    }

    // A ramp 0 -> -20 dB from frame 25 to 75, and a jump over one frame (-40 dB from 85 to 86): the gain per
    // sample is the frame-boundary values with straight lines between, so it never steps
    Timeline ramp = plain;
    ramp.audio[0].volumeAuto = {{25, 0}, {75, -20}, {85, -20}, {86, -40}};
    const std::vector<float> out = renderLeft(ramp);
    const std::vector<float> odd = renderLeft(ramp, {1, 333, 1919, 7, 4096, 256});
    CHECK(odd == out); // any block size: the same samples
    const auto frameStart = [](int64_t f) { return f * kRate / kFps; };
    double worst = 0, worstStep = 0, prevG = -1;
    int64_t checked = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (std::abs(ref[i]) < 0.05) {
            prevG = -1;
            continue;
        }
        const double g = double(out[i]) / ref[i];
        const int64_t f = int64_t(i) * kFps / kRate;
        const double g0 = gainOf(Automation::valueAt(ramp.audio[0].volumeAuto, double(f)));
        const double g1 = gainOf(Automation::valueAt(ramp.audio[0].volumeAuto, double(f + 1)));
        const double want = g0 + (g1 - g0) * double(int64_t(i) - frameStart(f)) / double(frameStart(f + 1) - frameStart(f));
        worst = std::max(worst, std::abs(g - want));
        if (prevG >= 0) worstStep = std::max(worstStep, std::abs(g - prevG));
        prevG = g;
        ++checked;
    }
    CHECK(checked > kRate);
    Check::report(worst < 2e-5, "ramp sample-accurate", __FILE__, __LINE__, QString::number(worst));
    // Steepest part: the 1-frame jump from 0.1 to 0.01 over 1920 samples = 4.7e-5 per sample (plus float noise)
    Check::report(worstStep < 1e-4, "no step in the gain", __FILE__, __LINE__, QString::number(worstStep));

    // Live change (preview): the curve changes while playing - it glides there instead of stepping
    {
        Audio::Mixer::Options o;
        o.fileLength = [](const QString&) { return 4 * kFps; };
        Timeline a = plain;
        a.audio[0].volumeAuto = {{0, 0}};
        Audio::Mixer m(a, FrameRate{kFps, 1}, o);
        std::vector<float> l(256), r(256), all;
        float* buf[2] = {l.data(), r.data()};
        int64_t pos = 0;
        for (int b = 0; b < 40; ++b, pos += 256) {
            m.render(pos, 256, buf);
            all.insert(all.end(), l.begin(), l.end());
        }
        Timeline b = plain;
        b.audio[0].volumeAuto = {{0, -20}};
        CHECK(m.updateMix(b)); // no new mixer
        for (int k = 0; k < 40; ++k, pos += 256) {
            m.render(pos, 256, buf);
            all.insert(all.end(), l.begin(), l.end());
        }
        // Removing the curve again glides back to the fader alone
        CHECK(m.updateMix(plain));
        for (int k = 0; k < 40; ++k, pos += 256) {
            m.render(pos, 256, buf);
            all.insert(all.end(), l.begin(), l.end());
        }
        double step = 0, prev = -1;
        for (size_t i = 0; i < all.size(); ++i) {
            if (std::abs(ref[i]) < 0.05) {
                prev = -1;
                continue;
            }
            const double g = double(all[i]) / ref[i];
            if (prev >= 0) step = std::max(step, std::abs(g - prev));
            prev = g;
        }
        Check::report(step < 0.01, "live change glides", __FILE__, __LINE__, QString::number(step));
        const size_t after = 40 * 256 + 2000, back = 80 * 256 + 2000;
        CHECK(std::abs(ref[after]) < 0.05 || near(all[after] / ref[after], 0.1, 1e-4));
        CHECK(std::abs(ref[back]) < 0.05 || near(all[back] / ref[back], 1.0, 1e-4));
    }
}

double rms(const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to && i < x.size(); ++i) s += double(x[i]) * x[i];
    return std::sqrt(s / double(std::max<size_t>(1, to - from)));
}

void testExport(const QTemporaryDir& tmp, const QString& tone)
{
    Timeline tl = toneTimeline(tone);
    tl.audio[0].volumeAuto = {{49, 0}, {50, -20}};
    RenderJob job;
    job.id = 1;
    job.path = tmp.filePath("auto.wav");
    job.settings.format = "wav";
    Exporter ex;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&ex, &Exporter::finished, &loop, [&](bool success, const QString&) {
        ok = success;
        loop.quit();
    });
    QString error;
    if (!CHECK(ex.start(tl, RenderQueue::exportSettings(job, kFmt), &error))) {
        std::printf("       %s\n", qPrintable(error));
        return;
    }
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!CHECK(ok)) return;
    QProcess p;
    p.start("ffmpeg", {"-v", "error", "-nostdin", "-i", job.path, "-ac", "1", "-f", "f32le", "-"});
    p.waitForFinished(60000);
    const QByteArray raw = p.readAllStandardOutput();
    std::vector<float> x(size_t(raw.size()) / sizeof(float));
    std::memcpy(x.data(), raw.constData(), x.size() * sizeof(float));
    if (!CHECK(x.size() > size_t(3 * kRate))) return;
    const double first = rms(x, kRate / 2, kRate * 3 / 2), second = rms(x, kRate * 5 / 2, kRate * 7 / 2);
    const double diff = 20 * std::log10(first / second);
    Check::report(std::abs(diff - 20) < 0.2, "export plays the automation", __FILE__, __LINE__, QString("%1 dB").arg(diff));
}

void testProjectFile(const QTemporaryDir& tmp)
{
    ProjectData d;
    d.format = kFmt;
    d.timeline.audio.resize(1);
    d.timeline.audio[0].clips << audioClip(1, 0, 50);
    const QString path = tmp.filePath("p.schneidi");
    // Without automation: no new field, an older version (0.22 opens it as it always did)
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object();
    CHECK(root.value("version").toInt() < 8);
    CHECK(!root.value("timeline").toObject().value("audio").toArray().first().toObject().contains("volumeAuto"));
    // With it: format 8 (0.22 knows at most 7 and refuses it instead of playing the fader alone), a lossless roundtrip
    d.timeline.audio[0].volumeAuto = {{0, 0}, {25, -12.5}, {40, 3.25}};
    const QByteArray json = ProjectFile::toJson(d, path);
    root = QJsonDocument::fromJson(json).object();
    CHECK_EQ(root.value("version").toInt(), 8);
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(json, path, &back, &error));
    CHECK(back.timeline.audio.value(0).volumeAuto == d.timeline.audio[0].volumeAuto);
    // In a sequence too
    ProjectData seq;
    seq.format = kFmt;
    Sequence s;
    s.id = 7;
    s.timeline.audio.resize(1);
    s.timeline.audio[0].volumeAuto = {{5, -3}};
    seq.sequences << s;
    CHECK_EQ(QJsonDocument::fromJson(ProjectFile::toJson(seq, path)).object().value("version").toInt(), 8);
}

QJsonObject cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    if (!p.waitForFinished(180000)) {
        p.kill();
        p.waitForFinished(1000);
    }
    return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

QJsonObject ops(const QString& project, const QJsonArray& list)
{
    return cli({"edit", project, "--ops", QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact))});
}

QJsonArray automationOf(const QString& project, const QString& track)
{
    for (const QJsonValue& t : cli({"info", project}).value("timeline").toObject().value("tracks").toArray())
        if (t.toObject().value("track").toString() == track) return t.toObject().value("volume_automation").toArray();
    return {};
}

void testCli(const QTemporaryDir& tmp, const QString& tone)
{
    if (Check::cliPath().isEmpty()) {
        std::printf("       schneidi-cli nicht gefunden, CLI-Teil übersprungen\n");
        return;
    }
    const QString project = tmp.filePath("cli.schneidi");
    CHECK(cli({"new", project, "--template", "neutral", "--media", QString("[\"%1\"]").arg(tone)}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "append"}, {"media", tone}}}).value("ok").toBool());
    // Points (objects and pairs, seconds and frames)
    CHECK(ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"},
                                    {"points", QJsonArray{QJsonObject{{"at", "1s"}, {"db", 0}}, QJsonArray{50, -12}}}}})
              .value("ok").toBool());
    QJsonArray a = automationOf(project, "A1");
    CHECK(a.size() == 2 && a[0].toObject().value("at").toInt() == 25 && a[1].toObject().value("db").toDouble() == -12);
    // A part: from/to keeps the rest
    CHECK(ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}, {"from", 70}, {"to", 80},
                                    {"points", QJsonArray{QJsonArray{70, -30}, QJsonArray{80, -30}}}}})
              .value("ok").toBool());
    a = automationOf(project, "A1");
    AutoCurve c;
    for (const QJsonValue& v : a) c << AutoPoint{v.toObject().value("at").toInt(), v.toObject().value("db").toDouble()};
    CHECK(near(Automation::valueAt(c, 25), 0) && near(Automation::valueAt(c, 50), -12) && near(Automation::valueAt(c, 75), -30)
          && near(Automation::valueAt(c, 69), -12) && near(Automation::valueAt(c, 81), -12));
    // The project file is format 8 now
    ProjectData d;
    QString error;
    CHECK(ProjectFile::load(project, &d, &error) && !d.timeline.audio.value(0).volumeAuto.isEmpty());
    // Errors: no track, video track, out of range, neither points nor clear, both
    CHECK(!ops(project, {QJsonObject{{"op", "track_volume"}, {"points", QJsonArray{QJsonArray{0, 0}}}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "V1"}, {"points", QJsonArray{QJsonArray{0, 0}}}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}, {"points", QJsonArray{QJsonArray{0, -100}}}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}, {"clear", true}, {"points", QJsonArray{}}}}).value("ok").toBool());
    // Clear
    CHECK(ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}, {"clear", true}}}).value("ok").toBool());
    CHECK(automationOf(project, "A1").isEmpty());
    // The op is described in `help`
    const QJsonObject help = cli({"help", "edit", "--op", "track_volume"});
    CHECK(QJsonDocument(help).toJson().contains("on top of the track fader"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("track_automation");
    testModel();
    testRipple();
    testEditor();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testProjectFile(tmp);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QString tone = Check::makeMedia(tmp.filePath("tone.wav"),
                                          {"-f", "lavfi", "-i", "aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=48000:d=4",
                                           "-c:a", "pcm_f32le"});
    if (!CHECK(!tone.isEmpty())) return Check::result();
    testMixer(tone);
    testExport(tmp, tone);
    testCli(tmp, tone);
    return Check::result();
}
