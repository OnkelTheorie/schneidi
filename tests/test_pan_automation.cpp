// SPDX-License-Identifier: AGPL-3.0-only
// Track pan automation and write mode (0.23, core/Automation.h Track::panAuto, core/AutomationWriter.h,
// docs/plan-0.23.md "Pan-Automation + Schreibmodus"):
// - model: knob + curve clamped to -100..+100, pan values beyond the dB range survive normalising and ripple;
// - ripple: ripple delete / insert move the pan curve exactly like the volume curve, a locked track keeps it;
// - editor: pen stroke in pan mode = one undo step "Spur-Pan zeichnen", stored on top of the knob, locked refused;
// - mixer: no pan curve = bit-identical; a constant curve = the knob at that value (bit-identical gains), a ramp
//   left -> right sample-accurate (balance gains at frame boundaries, linear in between), any block size the same,
//   a live change glides; 5.1: the curve moves the surround panner's left/right;
// - project file: field panAuto, format 8, lossless roundtrip;
// - write mode (AutomationWriter, driven like the Studio mixer does): thinned keys (<= 30/s, only real changes),
//   glide back to the old curve on release, the curve outside the touch untouched, a whole pass = one undo step;
// - CLI: op track_pan (points, from/to, clear, errors), `info` lists pan_automation, help text.

#include "check.h"

#include "core/Automation.h"
#include "core/AutomationWriter.h"
#include "core/ChannelLayout.h"
#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/audio/Mixer.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFps = 25;
const ProjectFormat kFmt{320, 180, {kFps, 1}};

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

void testModel()
{
    Track t{TrackKind::Audio};
    t.pan = 50;
    t.panAuto = {{0, 80}};
    CHECK_EQ(Automation::panAt(t, 10), 100.0); // clamped
    t.panAuto = {{0, -150}};
    CHECK_EQ(Automation::panAt(t, 10), -100.0);
    t.panAuto = {{0, -50}, {10, 0}};
    CHECK(near(Automation::panAt(t, 5), 25));
    CHECK(near(Automation::heardAt(t, AutoTarget::Pan, 5), 25) && near(Automation::base(t, AutoTarget::Pan), 50));
    // Pan values beyond the dB range (+-72) stay: normalising with the pan range, ripple and retime
    const AutoCurve n = Automation::normalized({{10, 150}, {0, -300}}, AutoTarget::Pan);
    CHECK(n == AutoCurve({{0, Automation::kMinPan}, {10, 150}}));
    AutoCurve r{{0, -150}, {100, 150}};
    Automation::ripple(r, {{50, -10}});
    CHECK(near(Automation::valueAt(r, 0), -150) && near(Automation::valueAt(r, 90), 150));
    AutoCurve range{{0, -150}, {100, 150}};
    Automation::replaceRange(range, 40, 60, {{40, 120}, {60, 120}}, AutoTarget::Pan);
    CHECK(near(Automation::valueAt(range, 50), 120) && near(Automation::valueAt(range, 100), 150));
    // The volume default still clamps to the dB range
    CHECK(Automation::normalized({{0, 150}}) == AutoCurve({{0, Automation::kMaxDb}}));
}

void testRipple()
{
    Timeline tl;
    tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}};
    tl.audio[0].clips << audioClip(1, 0, 50) << audioClip(2, 50, 50) << audioClip(3, 100, 50);
    tl.audio[0].volumeAuto = {{0, 0}, {150, -30}};
    tl.audio[0].panAuto = {{0, -100}, {150, 100}};
    tl.audio[1].clips << audioClip(4, 160, 20);
    tl.audio[1].locked = true;
    tl.audio[1].panAuto = {{0, -100}, {150, 100}};
    const Timeline orig = tl;
    TimelineOps::rippleDelete(tl, {2});
    const AutoCurve& p0 = orig.audio[0].panAuto;
    bool same = true, likeVolume = true;
    for (int f = 0; f <= 200; ++f) {
        const double want = f < 50 ? Automation::valueAt(p0, f) : Automation::valueAt(p0, f + 50);
        same &= near(Automation::valueAt(tl.audio[0].panAuto, f), want);
        // The same time moves as with the volume curve: pan and volume stay in step (the same relative position)
        const double vp = (Automation::valueAt(tl.audio[0].panAuto, f) + 100) / 200;
        const double vv = Automation::valueAt(tl.audio[0].volumeAuto, f) / -30;
        likeVolume &= near(vp, vv, 1e-9);
    }
    Check::report(same, "ripple delete moves the pan curve", __FILE__, __LINE__);
    Check::report(likeVolume, "pan and volume curve ripple alike", __FILE__, __LINE__);
    CHECK(tl.audio[1].panAuto == orig.audio[1].panAuto); // locked
    // Insert 25 frames at 50: the pan at 50 holds over the new time
    Timeline ins = orig;
    int nextId = 100;
    const IdGen newId = [&] { return ++nextId; };
    TimelineOps::insertGap(ins, {TrackRef{TrackKind::Audio, 0}}, 50, 25, newId, newId);
    CHECK(near(Automation::valueAt(ins.audio[0].panAuto, 60), Automation::valueAt(p0, 50))
          && near(Automation::valueAt(ins.audio[0].panAuto, 100), Automation::valueAt(p0, 75)));
    // A plain move leaves it
    Timeline mv = orig;
    TimelineOps::moveClips(mv, {3}, 30, TrackKind::Audio, 0, newId);
    CHECK(mv.audio[0].panAuto == p0);
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.edit("setup", [](Timeline& tl) {
        tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}};
        tl.audio[0].clips << audioClip(1, 0, 150);
        tl.audio[1].locked = true;
    });
    QUndoStack* undo = p.undoStack();
    const int idx = undo->index();
    ed.setTrackPan(0, 20);
    CHECK(ed.drawTrackPan(0, {{30, -80}, {120, -80}}, 0));
    CHECK_EQ(undo->index(), idx + 2);
    CHECK_EQ(undo->undoText(), QString("Spur-Pan zeichnen"));
    const Track& t = p.timeline().audio[0];
    CHECK(near(Automation::panAt(t, 75), -80) && near(Automation::valueAt(t.panAuto, 75), -100)); // on top of the knob
    CHECK(near(Automation::panAt(t, 10), 20) && near(Automation::panAt(t, 140), 20));
    CHECK(t.volumeAuto.isEmpty()); // the volume is untouched
    // A pan stroke is not clamped to the dB range (-60..+12): +90 is +90
    CHECK(ed.drawTrackPan(0, {{40, 90}, {60, 90}}, 0));
    CHECK(near(Automation::panAt(p.timeline().audio[0], 50), 90));
    undo->undo();
    undo->undo();
    CHECK(p.timeline().audio[0].panAuto.isEmpty());
    CHECK(!ed.drawTrackPan(1, {{10, -50}, {50, -50}}, 0)); // locked
    CHECK(!ed.setTrackAutomation(1, {{0, 50}}, {}, {}, AutoTarget::Pan));
    CHECK(ed.setTrackAutomation(0, {{50, 150}, {10, -20}}, {}, {}, AutoTarget::Pan));
    CHECK(p.timeline().audio[0].panAuto == AutoCurve({{10, -20}, {50, 150}}));
    CHECK(p.timeline().audio[0].volumeAuto.isEmpty());
}

struct Stereo {
    std::vector<float> l, r;
};

Stereo render(const Timeline& tl, const std::vector<int>& sizes = {4096}, int channels = 2)
{
    Audio::Mixer::Options o;
    o.fileLength = [](const QString&) { return 4 * kFps; };
    o.channels = channels;
    o.outputChannels = channels;
    Audio::Mixer m(tl, FrameRate{kFps, 1}, o);
    std::vector<std::vector<float>> ch(size_t(channels), std::vector<float>(size_t(m.length())));
    size_t k = 0;
    for (int64_t pos = 0; pos < m.length();) {
        const int n = int(std::min<int64_t>(m.length() - pos, sizes[k++ % sizes.size()]));
        std::vector<float*> buf;
        for (auto& c : ch) buf.push_back(c.data() + pos);
        m.render(pos, n, buf.data());
        pos += n;
    }
    if (channels == 2) return {ch[0], ch[1]};
    return {ch[ChannelLayout::FL], ch[ChannelLayout::FR]};
}

Timeline toneTimeline(const QString& tone)
{
    Timeline tl;
    tl.audio = {Track{TrackKind::Audio}};
    tl.audio[0].clips << audioClip(1, 0, 100, tone);
    return tl;
}

// Balance gains of a pan (-1..1), as the mixer's stereo pan
std::pair<double, double> sides(double pan)
{
    return {pan < 0 ? 1.0 : std::max(0.0, 1.0 - pan), pan < 0 ? std::max(0.0, pan + 1.0) : 1.0};
}

void testMixer(const QString& tone)
{
    const Timeline plain = toneTimeline(tone);
    const Stereo ref = render(plain);
    if (!CHECK(ref.l.size() >= size_t(4 * kRate - 10))) return;
    // A constant curve -40 = the knob at -40: the same balance on every sample
    Timeline knob = plain;
    knob.audio[0].pan = -40;
    Timeline curve = plain;
    curve.audio[0].panAuto = {{0, -40}};
    {
        const Stereo a = render(knob), b = render(curve);
        double worst = 0;
        for (size_t i = 0; i < a.l.size(); ++i)
            worst = std::max({worst, double(std::abs(a.l[i] - b.l[i])), double(std::abs(a.r[i] - b.r[i]))});
        Check::report(worst < 1e-6, "constant pan curve = knob", __FILE__, __LINE__, QString::number(worst));
        // ... and the knob trims it: knob +40 with curve -40 = centre
        Timeline trimmed = curve;
        trimmed.audio[0].pan = 40;
        const Stereo c = render(trimmed);
        worst = 0;
        for (size_t i = 0; i < c.l.size(); ++i) worst = std::max(worst, double(std::abs(c.r[i] - ref.r[i])));
        Check::report(worst < 1e-6, "knob + curve = centre", __FILE__, __LINE__, QString::number(worst));
    }
    // A ramp hard left -> hard right from frame 25 to 75: per sample the balance of the frame-boundary values with
    // straight lines between (sample check on both channels)
    Timeline ramp = plain;
    ramp.audio[0].panAuto = {{25, -100}, {75, 100}};
    const Stereo out = render(ramp);
    CHECK(render(ramp, {1, 333, 1919, 7, 4096, 256}).l == out.l);
    CHECK(render(ramp, {1, 333, 1919, 7, 4096, 256}).r == out.r);
    const auto frameStart = [](int64_t f) { return f * kRate / kFps; };
    double worst = 0;
    int64_t checked = 0;
    for (size_t i = 0; i < out.l.size(); ++i) {
        if (std::abs(ref.l[i]) < 0.05) continue;
        const int64_t f = int64_t(i) * kFps / kRate;
        const auto a = sides(Automation::valueAt(ramp.audio[0].panAuto, double(f)) / 100);
        const auto b = sides(Automation::valueAt(ramp.audio[0].panAuto, double(f + 1)) / 100);
        const double t = double(int64_t(i) - frameStart(f)) / double(frameStart(f + 1) - frameStart(f));
        const double wl = a.first + (b.first - a.first) * t, wr = a.second + (b.second - a.second) * t;
        worst = std::max({worst, std::abs(double(out.l[i]) / ref.l[i] - wl), std::abs(double(out.r[i]) / ref.r[i] - wr)});
        ++checked;
    }
    CHECK(checked > kRate);
    Check::report(worst < 2e-5, "pan ramp sample-accurate", __FILE__, __LINE__, QString::number(worst));
    // At the start hard left (right silent), at the end hard right
    const size_t early = size_t(kRate / 2), late = size_t(3.5 * kRate);
    CHECK(std::abs(out.r[early]) < 1e-6 && near(out.l[early], ref.l[early], 1e-6));
    CHECK(std::abs(out.l[late]) < 1e-6 && near(out.r[late], ref.r[late], 1e-6));

    // Live change: a pan curve comes in while playing - it glides there instead of jumping
    {
        Audio::Mixer::Options o;
        o.fileLength = [](const QString&) { return 4 * kFps; };
        Audio::Mixer m(plain, FrameRate{kFps, 1}, o);
        std::vector<float> l(256), r(256), all;
        float* buf[2] = {l.data(), r.data()};
        int64_t pos = 0;
        for (int b = 0; b < 40; ++b, pos += 256) {
            m.render(pos, 256, buf);
            all.insert(all.end(), r.begin(), r.end());
        }
        Timeline left = plain;
        left.audio[0].panAuto = {{0, -100}};
        CHECK(m.updateMix(left));
        for (int k = 0; k < 40; ++k, pos += 256) {
            m.render(pos, 256, buf);
            all.insert(all.end(), r.begin(), r.end());
        }
        double step = 0, prev = -1;
        for (size_t i = 0; i < all.size(); ++i) {
            if (std::abs(ref.r[i]) < 0.05) {
                prev = -1;
                continue;
            }
            const double g = double(all[i]) / ref.r[i];
            if (prev >= 0) step = std::max(step, std::abs(g - prev));
            prev = g;
        }
        Check::report(step < 0.01, "live pan change glides", __FILE__, __LINE__, QString::number(step));
        const size_t after = 40 * 256 + 2000;
        CHECK(std::abs(all[after]) < 1e-6);
    }

    // 5.1: the curve moves the surround panner's left/right (front left / front right follow the curve)
    {
        Timeline s = plain;
        s.audioChannels = 6;
        s.audio[0].panAuto = {{0, -100}, {99, -100}};
        const Stereo left = render(s, {4096}, 6);
        s.audio[0].panAuto = {{0, 100}, {99, 100}};
        const Stereo right = render(s, {4096}, 6);
        double ll = 0, lr = 0, rl = 0, rr = 0;
        for (size_t i = 0; i < left.l.size(); ++i) {
            ll += left.l[i] * left.l[i];
            lr += left.r[i] * left.r[i];
            rl += right.l[i] * right.l[i];
            rr += right.r[i] * right.r[i];
        }
        CHECK(ll > 100 * lr + 1e-3 && rr > 100 * rl + 1e-3);
    }
}

void testProjectFile(const QTemporaryDir& tmp)
{
    ProjectData d;
    d.format = kFmt;
    d.timeline.audio.resize(1);
    d.timeline.audio[0].clips << audioClip(1, 0, 50);
    const QString path = tmp.filePath("p.schneidi");
    QJsonObject root = QJsonDocument::fromJson(ProjectFile::toJson(d, path)).object();
    CHECK(root.value("version").toInt() < 8);
    d.timeline.audio[0].panAuto = {{0, -100}, {25, 37.5}, {40, 150}};
    const QByteArray json = ProjectFile::toJson(d, path);
    root = QJsonDocument::fromJson(json).object();
    CHECK_EQ(root.value("version").toInt(), 8);
    CHECK(root.value("timeline").toObject().value("audio").toArray().first().toObject().contains("panAuto"));
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(json, path, &back, &error));
    CHECK(back.timeline.audio.value(0).panAuto == d.timeline.audio[0].panAuto);
    CHECK(back.timeline.audio.value(0).volumeAuto.isEmpty());
}

void testWriter()
{
    // Pure core: one value per frame (a fader moved by a hand: steps, holds, tiny jitter)
    Track t{TrackKind::Audio};
    t.volumeDb = -6;
    t.volumeAuto = {{0, 0}, {500, -10}};
    Automation::Touch touch = Automation::beginTouch(t, AutoTarget::Volume);
    for (int f = 100; f <= 200; ++f) {
        double heard = f < 120 ? -6 - (f - 100) * 0.5 : -16; // down 10 dB over 20 frames, then held
        if (f > 150) heard += (f % 2) * 0.04;                 // jitter below the step: no keys
        Automation::record(touch, f, heard);
    }
    Automation::record(touch, 150, -30); // a jump back: ignored
    const AutoCurve thin = Automation::thinned(touch, 50.0);
    CHECK(thin.size() >= 2 && thin.size() <= 6);
    bool gaps = true;
    for (int i = 1; i < thin.size(); ++i) gaps &= thin[i].frame - thin[i - 1].frame >= 2; // 50 fps: <= 30 keys/s
    CHECK(gaps);
    bool close = true;
    for (int f = 100; f <= 200; ++f) {
        const double want = f < 120 ? -(f - 100) * 0.5 : -10.0; // on top of the fader
        close &= near(Automation::valueAt(thin, f), want, 0.15);
    }
    Check::report(close, "thinned keys follow what was heard", __FILE__, __LINE__);
    // Released at 200: the old curve outside, the glide back over glideBackFrames (0.5 s)
    const AutoCurve rel = Automation::releasedCurve(touch, 200, 50.0);
    const int g = Automation::glideBackFrames(50.0);
    CHECK_EQ(g, 25);
    CHECK(near(Automation::valueAt(rel, 50), Automation::valueAt(t.volumeAuto, 50)));
    CHECK(near(Automation::valueAt(rel, 99), Automation::valueAt(t.volumeAuto, 99)));
    CHECK(near(Automation::valueAt(rel, 200), -10, 0.15));
    CHECK(near(Automation::valueAt(rel, 200 + g), Automation::valueAt(t.volumeAuto, 200 + g)));
    CHECK(near(Automation::valueAt(rel, 400), Automation::valueAt(t.volumeAuto, 400)));
    const double mid = Automation::valueAt(rel, 200 + g / 2.0);
    CHECK(mid < Automation::valueAt(t.volumeAuto, 200 + g / 2.0) && mid > -10); // gliding
    // While touching: from the last key the touched value holds (the mixer runs ahead of the playhead)
    const AutoCurve live = Automation::touchingCurve(touch, 50.0);
    CHECK(near(Automation::valueAt(live, 300), -10, 0.15) && near(Automation::valueAt(live, 90), Automation::valueAt(t.volumeAuto, 90)));

    // Through the writer (what the Studio mixer calls): two touches on two tracks in one pass = one undo step
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.edit("setup", [](Timeline& tl) {
        tl.audio = {Track{TrackKind::Audio}, Track{TrackKind::Audio}};
        tl.audio[0].clips << audioClip(1, 0, 400);
        tl.audio[1].clips << audioClip(2, 0, 400);
        tl.audio[0].volumeDb = -3;
    });
    QUndoStack* undo = p.undoStack();
    const int idx = undo->index();
    AutomationWriter w(&ed);
    CHECK(!w.inPass());
    for (int f = 25; f <= 75; ++f) w.move(0, AutoTarget::Volume, f, -3 - (f - 25) * 0.2); // -3 -> -13 dB
    CHECK(w.inPass() && w.touching(0, AutoTarget::Volume));
    CHECK(near(Automation::levelAt(p.timeline().audio[0], 200), -13, 0.15)); // live: the touched value holds
    w.release(0, AutoTarget::Volume, 80);
    CHECK(!w.touching(0, AutoTarget::Volume) && w.inPass());
    const Track& a1 = p.timeline().audio[0];
    CHECK(near(Automation::levelAt(a1, 10), -3) && near(Automation::levelAt(a1, 50), -8, 0.15)
          && near(Automation::levelAt(a1, 80), -13, 0.15) && near(Automation::levelAt(a1, 80 + 13), -3)); // 25 fps: 13 frames back
    int keys = int(a1.volumeAuto.size());
    CHECK(keys >= 3 && keys <= 2 + 51 * 30 / 25); // a straight ride thins to a few keys
    for (int f = 100; f <= 150; ++f) w.move(1, AutoTarget::Pan, f, std::min(100, (f - 100) * 4)); // to the right
    w.endPass(160); // playback stops: the open touch is released
    CHECK(!w.inPass() && !w.touching());
    CHECK(near(Automation::panAt(p.timeline().audio[1], 150), 100, 0.6) && near(Automation::panAt(p.timeline().audio[1], 50), 0));
    CHECK_EQ(undo->index(), idx + 1);
    CHECK_EQ(undo->undoText(), QString("Automation schreiben"));
    undo->undo();
    CHECK(p.timeline().audio[0].volumeAuto.isEmpty() && p.timeline().audio[1].panAuto.isEmpty());
    // The next pass is its own step
    undo->redo();
    for (int f = 200; f <= 220; ++f) w.move(0, AutoTarget::Volume, f, -20);
    w.endPass(220);
    CHECK_EQ(undo->index(), idx + 2);
    // A locked track: nothing
    p.edit("lock", [](Timeline& tl) { tl.audio[1].locked = true; });
    const int at = undo->index();
    w.move(1, AutoTarget::Volume, 300, -20);
    w.endPass(310);
    CHECK_EQ(undo->index(), at);
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

QJsonArray panOf(const QString& project, const QString& track)
{
    for (const QJsonValue& t : cli({"info", project}).value("timeline").toObject().value("tracks").toArray())
        if (t.toObject().value("track").toString() == track) return t.toObject().value("pan_automation").toArray();
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
    CHECK(ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"},
                                    {"points", QJsonArray{QJsonObject{{"at", "1s"}, {"pan", -100}}, QJsonArray{50, 100}}}}})
              .value("ok").toBool());
    QJsonArray a = panOf(project, "A1");
    CHECK(a.size() == 2 && a[0].toObject().value("at").toInt() == 25 && a[0].toObject().value("pan").toDouble() == -100
          && a[1].toObject().value("pan").toDouble() == 100);
    // A part keeps the rest; the volume op still works next to it (backward compatible)
    CHECK(ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"}, {"from", 30}, {"to", 40},
                                    {"points", QJsonArray{QJsonArray{30, 0}, QJsonArray{40, 0}}}}})
              .value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "track_volume"}, {"track", "A1"}, {"points", QJsonArray{QJsonArray{0, -6}}}}})
              .value("ok").toBool());
    a = panOf(project, "A1");
    AutoCurve c;
    for (const QJsonValue& v : a) c << AutoPoint{v.toObject().value("at").toInt(), v.toObject().value("pan").toDouble()};
    CHECK(near(Automation::valueAt(c, 25), -100) && near(Automation::valueAt(c, 35), 0) && near(Automation::valueAt(c, 50), 100));
    ProjectData d;
    QString error;
    CHECK(ProjectFile::load(project, &d, &error) && !d.timeline.audio.value(0).panAuto.isEmpty()
          && !d.timeline.audio.value(0).volumeAuto.isEmpty());
    // Errors: out of range, db instead of pan, neither points nor clear
    CHECK(!ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"}, {"points", QJsonArray{QJsonArray{0, 250}}}}}).value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"}, {"points", QJsonArray{QJsonObject{{"at", 0}, {"db", 0}}}}}})
               .value("ok").toBool());
    CHECK(!ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"}}}).value("ok").toBool());
    CHECK(ops(project, {QJsonObject{{"op", "track_pan"}, {"track", "A1"}, {"clear", true}}}).value("ok").toBool());
    CHECK(panOf(project, "A1").isEmpty());
    const QJsonObject help = cli({"help", "edit", "--op", "track_pan"});
    CHECK(QJsonDocument(help).toJson().contains("on top of the track's pan knob"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("pan_automation");
    testModel();
    testRipple();
    testEditor();
    testWriter();
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testProjectFile(tmp);
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    const QString tone = Check::makeMedia(tmp.filePath("tone.wav"),
                                          {"-f", "lavfi", "-i", "aevalsrc=0.25*sin(2*PI*1000*t)|0.25*sin(2*PI*1000*t):s=48000:d=4",
                                           "-c:a", "pcm_f32le"});
    if (!CHECK(!tone.isEmpty())) return Check::result();
    testMixer(tone);
    testCli(tmp, tone);
    return Check::result();
}
