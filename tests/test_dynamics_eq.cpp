// Compressor and EQ per audio track (docs/archiv/plan-0.8.md, Fairlight-like, Opus finding 4: game sound with extreme
// peaks): the DSP (static curve with knee and make-up, attack/release, stereo-linked, block size and sample rate
// independent, silence and extremes; EQ bands with known responses, flat = transparent, stable at the edges), the
// model (defaults, Editor + undo, project file with clamping), rendering in the order EQ -> compressor -> fader,
// live changes without rebuilding, the loudness measurement, the Mixer buttons, the EQ curve and the dynamics
// dialog, and the CLI mixer op.
#include "check.h"

#include "app/DynamicsDialog.h"
#include "app/EqDialog.h"
#include "cli/Commands.h"
#include "core/Dynamics.h"
#include "core/Editor.h"
#include "core/Equalizer.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "engine/AudioAnalysis.h"
#include "engine/Engine.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"
#include "ui/EqCurve.h"
#include "ui/Mixer.h"

#include <Mlt.h>
#include <QApplication>
#include <QCheckBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <algorithm>
#include <clocale>
#include <cmath>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }
double db(double x) { return x > 0 ? 20 * std::log10(x) : -200; }

// Planar stereo sine: amplitude per channel, `seconds` long
std::vector<float> sine(double hz, double ampL, double ampR, double seconds, int rate)
{
    const int n = int(seconds * rate);
    std::vector<float> v(size_t(n) * 2);
    for (int i = 0; i < n; ++i) {
        const double s = std::sin(2 * kPi * hz * i / rate);
        v[size_t(i)] = float(ampL * s);
        v[size_t(n + i)] = float(ampR * s);
    }
    return v;
}

// Peak of channel `ch` of planar data (n samples per channel) in [from, to)
double peakDb(const std::vector<float>& v, int n, int ch, int from, int to)
{
    double p = 0;
    for (int i = from; i < to; ++i) p = std::max(p, double(std::abs(v[size_t(ch * n + i)])));
    return db(p);
}

TrackDynamics comp(double threshold, double ratio, double knee = 0, double makeup = 0, double attack = 10, double release = 150)
{
    TrackDynamics d;
    d.on = true;
    d.thresholdDb = threshold;
    d.ratio = ratio;
    d.kneeDb = knee;
    d.makeupDb = makeup;
    d.attackMs = attack;
    d.releaseMs = release;
    return d;
}

// ---- Compressor DSP ----

void testCompressorStaticCurve()
{
    const TrackDynamics hard = comp(-20, 4);
    CHECK(near(Compressor::curveDb(hard, -30), -30, 1e-9));
    CHECK(near(Compressor::curveDb(hard, -20), -20, 1e-9));
    CHECK(near(Compressor::curveDb(hard, -8), -17, 1e-9));
    CHECK(near(Compressor::curveDb(hard, 0), -15, 1e-9));
    // Soft knee (6 dB): rounded around the threshold, the same as the hard curve outside the knee
    const TrackDynamics soft = comp(-20, 4, 6);
    CHECK(near(Compressor::curveDb(soft, -20), -20.5625, 0.01));
    CHECK(near(Compressor::curveDb(soft, -24), -24, 1e-9));
    CHECK(near(Compressor::curveDb(soft, -16), -19, 1e-9));
    double last = -1000;
    bool monotonic = true, gentle = true;
    for (double x = -60; x <= 6; x += 0.1) {
        const double y = Compressor::curveDb(soft, x);
        monotonic = monotonic && y >= last;
        if (last > -1000) gentle = gentle && y - last <= 0.1 + 1e-9;
        last = y;
    }
    CHECK(monotonic && gentle);
    // Make-up gain lifts everything; ratio 1 changes nothing
    CHECK(near(Compressor::curveDb(comp(-20, 4, 0, 5), 0), -10, 1e-9));
    CHECK(near(Compressor::curveDb(comp(-20, 1), -3), -3, 1e-9));
}

void testCompressorSteadyLevels()
{
    // A 1 kHz sine at -6 dBFS peak settles on the static curve (peak detector); below the threshold untouched
    const int rate = 48000;
    Compressor c;
    c.setParams(comp(-20, 4));
    std::vector<float> loud = sine(1000, 0.5, 0.5, 2, rate);
    c.processPlanar(loud.data(), 2 * rate, 2, rate);
    CHECK(near(peakDb(loud, 2 * rate, 0, rate, 2 * rate), -16.5, 0.5));
    CHECK(near(c.gainReductionDb(), -10.5, 0.5));
    Compressor q;
    q.setParams(comp(-20, 4));
    std::vector<float> quiet = sine(1000, 0.0316, 0.0316, 1, rate); // -30 dBFS
    const std::vector<float> in = quiet;
    q.processPlanar(quiet.data(), rate, 2, rate);
    CHECK(near(peakDb(quiet, rate, 0, rate / 2, rate), peakDb(in, rate, 0, rate / 2, rate), 0.05));
    CHECK(near(q.gainReductionDb(), 0, 0.05));
}

void testCompressorAttackRelease()
{
    // Quiet -> loud step: after the attack time a good part of the final reduction, after 5x nearly all;
    // loud -> quiet: after the release time a good part recovered
    const int rate = 48000;
    Compressor c;
    c.setParams(comp(-20, 4, 0, 0, 20, 200));
    std::vector<float> quiet = sine(1000, 0.01, 0.01, 0.5, rate);
    c.processPlanar(quiet.data(), rate / 2, 2, rate);
    const double final = Compressor::curveDb(comp(-20, 4), -4) - (-4); // -12 dB
    std::vector<float> block = sine(1000, 0.63, 0.63, 0.02, rate);     // 20 ms at -4 dBFS
    c.processPlanar(block.data(), int(0.02 * rate), 2, rate);
    const double atAttack = c.gainReductionDb() / final;
    CHECK(atAttack > 0.3 && atAttack < 0.9);
    std::vector<float> more = sine(1000, 0.63, 0.63, 0.08, rate);
    c.processPlanar(more.data(), int(0.08 * rate), 2, rate);
    CHECK(c.gainReductionDb() / final > 0.9);
    std::vector<float> after = sine(1000, 0.01, 0.01, 0.2, rate);
    c.processPlanar(after.data(), int(0.2 * rate), 2, rate);
    const double left = c.gainReductionDb() / final;
    CHECK(left > 0.1 && left < 0.7);
}

void testCompressorStereoLinked()
{
    // Loud left, quiet right: the right channel gets the same gain change (no wandering stereo image)
    const int rate = 48000;
    Compressor c;
    c.setParams(comp(-20, 4));
    std::vector<float> v = sine(1000, 0.63, 0.0316, 1, rate);
    c.processPlanar(v.data(), rate, 2, rate);
    const double gainL = peakDb(v, rate, 0, rate / 2, rate) - db(0.63);
    const double gainR = peakDb(v, rate, 1, rate / 2, rate) - db(0.0316);
    CHECK(gainL < -8);
    CHECK(near(gainL, gainR, 0.1));
}

void testCompressorBlockSizeAndRate()
{
    const int rate = 48000;
    const std::vector<float> src = [&] {
        std::vector<float> v = sine(800, 0.1, 0.1, 1, rate);
        for (int i = rate / 4; i < rate / 2; ++i) v[size_t(i)] = v[size_t(rate + i)] = float(0.8 * std::sin(2 * kPi * 800 * i / rate));
        return v;
    }();
    std::vector<float> whole = src;
    Compressor a;
    a.setParams(comp(-20, 4, 6));
    a.processPlanar(whole.data(), rate, 2, rate);
    for (int block : {1, 64, 1000}) {
        Compressor b;
        b.setParams(comp(-20, 4, 6));
        std::vector<float> parts = src;
        std::vector<float> tmp;
        for (int i = 0; i < rate; i += block) {
            const int n = std::min(block, rate - i);
            tmp.assign(size_t(2 * n), 0.f);
            for (int k = 0; k < n; ++k) {
                tmp[size_t(k)] = parts[size_t(i + k)];
                tmp[size_t(n + k)] = parts[size_t(rate + i + k)];
            }
            b.processPlanar(tmp.data(), n, 2, rate);
            for (int k = 0; k < n; ++k) {
                parts[size_t(i + k)] = tmp[size_t(k)];
                parts[size_t(rate + i + k)] = tmp[size_t(n + k)];
            }
        }
        double diff = 0;
        for (size_t i = 0; i < parts.size(); ++i) diff = std::max(diff, double(std::abs(parts[i] - whole[i])));
        CHECK(diff < 1e-5);
    }
    // Times are in ms, not samples: 44.1 kHz reaches the same reduction after the same time
    auto reductionAfter = [](int r) {
        Compressor c;
        c.setParams(comp(-20, 4, 0, 0, 20, 200));
        std::vector<float> v = sine(1000, 0.63, 0.63, 0.02, r);
        c.processPlanar(v.data(), int(0.02 * r), 2, r);
        return c.gainReductionDb();
    };
    CHECK(near(reductionAfter(44100), reductionAfter(48000), 0.3));
}

void testCompressorSilenceAndExtremes()
{
    const int rate = 48000;
    Compressor c;
    c.setParams(comp(-60, 100, 0, 12, 0.1, 5000));
    std::vector<float> zeros(size_t(2 * rate), 0.f);
    c.processPlanar(zeros.data(), rate, 2, rate);
    bool silent = true;
    for (float x : zeros) silent = silent && x == 0.f;
    CHECK(silent);
    std::vector<float> hot = sine(50, 10.0, -10.0, 1, rate); // far above 0 dBFS (float, before the master)
    c.processPlanar(hot.data(), rate, 2, rate);
    bool finite = true;
    for (float x : hot) finite = finite && std::isfinite(x);
    CHECK(finite);
    // Off = untouched
    Compressor off;
    TrackDynamics d = comp(-40, 10);
    d.on = false;
    off.setParams(d);
    std::vector<float> v = sine(1000, 0.9, 0.9, 0.5, rate);
    const std::vector<float> in = v;
    off.processPlanar(v.data(), rate / 2, 2, rate);
    CHECK(v == in);
}

// ---- EQ DSP ----

TrackEq eqWith(int band, double freq, double gain, double q = 0.7, bool on = true)
{
    TrackEq e;
    e.on = true;
    EqBand& b = e.bands[band];
    b.on = on;
    b.freq = freq;
    b.gainDb = gain;
    b.q = q;
    return e;
}

void testEqDefaults()
{
    const TrackEq e;
    CHECK(!e.on);
    if (!CHECK_EQ(e.bands.size(), 6)) return;
    using K = EqBand::Kind;
    CHECK(e.bands[0].kind == K::HighPass && !e.bands[0].on);
    CHECK(e.bands[1].kind == K::LowShelf && e.bands[1].gainDb == 0);
    CHECK(e.bands[2].kind == K::Bell && e.bands[3].kind == K::Bell);
    CHECK(e.bands[4].kind == K::HighShelf);
    CHECK(e.bands[5].kind == K::LowPass && !e.bands[5].on);
    for (int i = 1; i < 6; ++i) CHECK(e.bands[i].freq > e.bands[i - 1].freq);
    const TrackDynamics d;
    CHECK(!d.on && d.thresholdDb == -20 && d.ratio == 4 && d.attackMs == 10 && d.releaseMs == 150 && d.kneeDb == 6
          && d.makeupDb == 0);
    // Tracks start without both
    const Track t;
    CHECK(!t.eq.on && !t.dynamics.on);
}

void testEqResponse()
{
    const int rate = 48000;
    // Flat EQ (defaults switched on) and an EQ that is off: 0 dB everywhere
    TrackEq flat;
    flat.on = true;
    for (double hz : {20.0, 100.0, 1000.0, 10000.0, 20000.0}) CHECK(near(Equalizer::responseDb(flat, hz, rate), 0, 1e-3));
    TrackEq off = eqWith(2, 1000, 12);
    off.on = false;
    CHECK(near(Equalizer::responseDb(off, 1000, rate), 0, 1e-9));
    // Bell +6 dB at 1 kHz
    const TrackEq bell = eqWith(2, 1000, 6, 1);
    CHECK(near(Equalizer::responseDb(bell, 1000, rate), 6, 0.05));
    CHECK(Equalizer::responseDb(bell, 100, rate) < 0.5);
    CHECK(Equalizer::responseDb(bell, 10000, rate) < 0.5);
    // Low shelf +12 dB at 100 Hz
    const TrackEq shelf = eqWith(1, 100, 12);
    CHECK(near(Equalizer::responseDb(shelf, 10, rate), 12, 0.5));
    CHECK(near(Equalizer::responseDb(shelf, 5000, rate), 0, 0.2));
    // High pass / low pass: -3 dB at the corner (Butterworth), steep outside
    const TrackEq hp = eqWith(0, 200, 0, 0.707);
    CHECK(near(Equalizer::responseDb(hp, 200, rate), -3, 0.3));
    CHECK(Equalizer::responseDb(hp, 40, rate) < -20);
    CHECK(Equalizer::responseDb(hp, 2000, rate) > -0.2);
    const TrackEq lp = eqWith(5, 5000, 0, 0.707);
    CHECK(near(Equalizer::responseDb(lp, 5000, rate), -3, 0.3));
    CHECK(Equalizer::responseDb(lp, 20000, rate) < -18);
    // A band that is off counts nothing
    CHECK(near(Equalizer::responseDb(eqWith(2, 1000, 6, 1, false), 1000, rate), 0, 1e-6));
}

double gainOf(const TrackEq& e, double hz, int rate)
{
    Equalizer eq;
    eq.setEq(e);
    std::vector<float> v = sine(hz, 0.25, 0.25, 1, rate);
    eq.processPlanar(v.data(), rate, 2, rate);
    double s = 0;
    for (int i = rate / 2; i < rate; ++i) s += double(v[size_t(i)]) * v[size_t(i)];
    return db(std::sqrt(s / (rate / 2))) - db(0.25 / std::sqrt(2.0));
}

void testEqProcessingMatchesResponse()
{
    TrackEq e = eqWith(1, 150, 9);
    e.bands[3].gainDb = -6;
    e.bands[3].freq = 3000;
    e.bands[3].q = 2;
    for (int rate : {44100, 48000})
        for (double hz : {60.0, 150.0, 1000.0, 3000.0, 8000.0})
            CHECK(near(gainOf(e, hz, rate), Equalizer::responseDb(e, hz, rate), 0.3));
}

void testEqFlatIsTransparentAndStable()
{
    const int rate = 48000;
    TrackEq flat;
    flat.on = true;
    Equalizer eq;
    eq.setEq(flat);
    std::vector<float> v = sine(440, 0.7, 0.3, 0.5, rate);
    const std::vector<float> in = v;
    eq.processPlanar(v.data(), rate / 2, 2, rate);
    double diff = 0;
    for (size_t i = 0; i < v.size(); ++i) diff = std::max(diff, double(std::abs(v[i] - in[i])));
    CHECK(diff < 1e-6);
    // Extremes: corners near 0 and above Nyquist, tiny and huge Q, all bands +12 dB at once – finite and bounded
    for (int rate2 : {44100, 48000})
        for (double hz : {5.0, 20.0, 23900.0, 30000.0})
            for (double q : {0.05, 20.0}) {
                TrackEq e;
                e.on = true;
                for (EqBand& b : e.bands) {
                    b.on = true;
                    b.freq = hz;
                    b.q = q;
                    b.gainDb = 12;
                }
                Equalizer x;
                x.setEq(e);
                std::vector<float> noise(size_t(rate2) * 2);
                uint32_t s = 1;
                for (float& f : noise) {
                    s = s * 1664525u + 1013904223u;
                    f = float(s) / 4294967296.f - 0.5f;
                }
                x.processPlanar(noise.data(), rate2, 2, rate2);
                bool ok = true;
                for (float f : noise) ok = ok && std::isfinite(f) && std::abs(f) < 1e4f;
                CHECK(ok);
                CHECK(std::isfinite(Equalizer::responseDb(e, 1000, rate2)));
            }
}

// ---- Model ----

void testEditorTrackEqDynamicsUndo()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();
    const int steps = undo->count();
    ed.setTrackDynamics(0, comp(-24, 3));
    CHECK_EQ(undo->count(), steps + 1);
    CHECK(p.timeline().audio[0].dynamics == comp(-24, 3));
    ed.setTrackDynamics(0, comp(-24, 3)); // same: no step
    CHECK_EQ(undo->count(), steps + 1);
    // Dragging a control: one step
    for (double t = -25; t >= -30; t -= 1) ed.setTrackDynamics(0, comp(t, 3), "drag-threshold");
    CHECK_EQ(undo->count(), steps + 2);
    CHECK_EQ(p.timeline().audio[0].dynamics.thresholdDb, -30.0);
    const TrackEq e = eqWith(2, 800, 4);
    ed.setTrackEq(1, e);
    CHECK(p.timeline().audio[1].eq == e);
    CHECK_EQ(undo->count(), steps + 3);
    ed.setTrackEq(7, e); // no such track
    ed.setTrackEq(-1, e);
    CHECK_EQ(undo->count(), steps + 3);
    undo->undo();
    CHECK(!p.timeline().audio[1].eq.on);
    undo->undo();
    CHECK_EQ(p.timeline().audio[0].dynamics.thresholdDb, -24.0);
}

void testProjectFileRoundTrip()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    ed.setTrackDynamics(0, comp(-18, 2.5, 3, 4, 5, 300));
    TrackEq e = eqWith(0, 90, 0);
    e.bands[4].gainDb = 2.5;
    ed.setTrackEq(1, e);
    const QByteArray json = ProjectFile::toJson(p.data(), "/x/p.schneidi");
    ProjectData back;
    QString err;
    if (!CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err))) return;
    CHECK(back.timeline.audio[0].dynamics == comp(-18, 2.5, 3, 4, 5, 300));
    CHECK(back.timeline.audio[1].eq == e);
    CHECK(!back.timeline.audio[1].dynamics.on && !back.timeline.audio[0].eq.on);
    Project q;
    q.load(back);
    CHECK_EQ(ProjectFile::toJson(q.data(), "/x/p.schneidi"), json);
    // Tracks without EQ/compressor write nothing (old versions and small files)
    Project plain;
    const QJsonObject tl = QJsonDocument::fromJson(ProjectFile::toJson(plain.data(), "/x/p.schneidi")).object();
    CHECK(!QJsonDocument(tl).toJson().contains("\"eq\"") && !QJsonDocument(tl).toJson().contains("\"dynamics\""));
    // Values out of range are clamped when loading (hand-edited or broken files)
    QByteArray bad = json;
    bad.replace("\"ratio\": 2.5", "\"ratio\": 0.2");
    bad.replace("\"thresholdDb\": -18", "\"thresholdDb\": 40");
    if (!CHECK(ProjectFile::fromJson(bad, "/x/p.schneidi", &back, &err))) return;
    CHECK_EQ(back.timeline.audio[0].dynamics.ratio, 1.0);
    CHECK_EQ(back.timeline.audio[0].dynamics.thresholdDb, 0.0);
}

// ---- Rendering ----

QString g_dir;
QString g_tone;    // 1 kHz at -6 dBFS
QString g_twoTone; // 100 Hz + 5 kHz, each -12 dBFS

Timeline toneTimeline(const QString& file, int frames = 100)
{
    Timeline tl = emptyTimeline();
    Clip c;
    c.id = 1;
    c.mediaPath = file;
    c.out = frames - 1;
    tl.audio[0].clips << c;
    return tl;
}

std::vector<float> renderSound(const Timeline& tl, int from, int to, MixerHooks* hooks = nullptr,
                               std::unique_ptr<Mlt::Tractor>* keep = nullptr)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl, hooks);
    std::vector<float> out;
    tr->seek(0);
    for (int pos = 0; pos < to; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (pcm && pos >= from) out.insert(out.end(), pcm, pcm + samples);
    }
    if (keep) *keep = std::move(tr);
    return out;
}

double peak(const std::vector<float>& x)
{
    double p = 0;
    for (float v : x) p = std::max(p, double(std::abs(v)));
    return db(p);
}

double toneDb(const std::vector<float>& x, double hz)
{
    const double w = 2 * kPi * hz / 48000, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (float v : x) {
        const double s0 = v + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return db(std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) * 2 / double(x.size()));
}

void testRenderOrderEqCompressorFader()
{
    Timeline tl = toneTimeline(g_tone);
    const double plain = peak(renderSound(tl, 25, 75));
    CHECK(near(plain, -6, 0.3));
    tl.audio[0].dynamics = comp(-20, 4, 0);
    CHECK(near(peak(renderSound(tl, 25, 75)), -16.5, 1.0));
    // The fader comes after the compressor: -10 dB on the fader = exactly 10 dB less, no change in compression
    tl.audio[0].volumeDb = -10;
    CHECK(near(peak(renderSound(tl, 25, 75)), -26.5, 1.0));
    const double withFader = peak(renderSound(tl, 25, 75));
    tl.audio[0].volumeDb = 0;
    CHECK(near(peak(renderSound(tl, 25, 75)) - withFader, 10, 0.2));
    // The EQ comes before the compressor: +6 dB at 1 kHz feeds it 0 dBFS -> -15, not -16.5 + 6
    tl.audio[0].eq = eqWith(2, 1000, 6, 1);
    CHECK(near(peak(renderSound(tl, 25, 75)), -15, 1.0));
    // Off again = as before
    tl.audio[0].eq.on = false;
    tl.audio[0].dynamics.on = false;
    CHECK(near(peak(renderSound(tl, 25, 75)), plain, 0.1));
}

void testRenderEqChangesSpectrum()
{
    Timeline tl = toneTimeline(g_twoTone);
    const std::vector<float> before = renderSound(tl, 25, 75);
    tl.audio[0].eq = eqWith(1, 300, 12);
    const std::vector<float> after = renderSound(tl, 25, 75);
    const double low = toneDb(after, 100) - toneDb(before, 100), high = toneDb(after, 5000) - toneDb(before, 5000);
    CHECK(near(low, Equalizer::responseDb(tl.audio[0].eq, 100, 48000), 0.5));
    CHECK(low > 9);
    CHECK(near(high, 0, 0.3));
}

void testLiveChangeWithoutRebuild()
{
    // Preview: a changed compressor/EQ goes into the running tractor (like the faders), no rebuild
    Timeline tl = toneTimeline(g_tone, 200);
    MixerHooks hooks;
    std::unique_ptr<Mlt::Tractor> tr;
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    tr = b.build(tl, &hooks);
    auto level = [&](int from) {
        std::vector<float> out;
        tr->seek(from);
        for (int pos = from; pos < from + 25; ++pos) {
            std::unique_ptr<Mlt::Frame> f(tr->get_frame());
            mlt_audio_format fmt = mlt_audio_float;
            int freq = 48000, channels = 2;
            int samples = mlt_audio_calculate_frame_samples(25.f, freq, pos);
            const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
            if (pcm && pos >= from + 10) out.insert(out.end(), pcm, pcm + samples);
        }
        return peak(out);
    };
    CHECK(near(level(10), -6, 0.3));
    tl.audio[0].dynamics = comp(-20, 4, 0);
    CHECK(TimelineBuilder::applyMixer(tl, hooks));
    CHECK(near(level(50), -16.5, 1.0));
    tl.audio[0].eq = eqWith(2, 1000, 6, 1);
    CHECK(TimelineBuilder::applyMixer(tl, hooks));
    CHECK(near(level(100), -15, 1.0));
}

void testLoudnessMeasurementIncludesDynamics()
{
    Timeline tl = toneTimeline(g_tone);
    const auto plain = AudioAnalysis::timelineLoudness(ProjectFormat{}, tl, 0, 100);
    const auto plainPeaks = AudioAnalysis::timelineFramePeaks(ProjectFormat{}, tl, 0, 100);
    tl.audio[0].dynamics = comp(-20, 4, 0);
    const auto compressed = AudioAnalysis::timelineLoudness(ProjectFormat{}, tl, 0, 100);
    const auto compressedPeaks = AudioAnalysis::timelineFramePeaks(ProjectFormat{}, tl, 0, 100);
    if (!CHECK(plain && compressed && plainPeaks && compressedPeaks)) return;
    // Peaks after the attack (from frame 25): without lookahead the first peaks pass before it has acted, like
    // with every compressor
    auto settledDb = [](const std::vector<float>& peaks) {
        return db(*std::max_element(peaks.begin() + std::min<size_t>(25, peaks.size() - 1), peaks.end()));
    };
    CHECK(near(settledDb(*plainPeaks) - settledDb(*compressedPeaks), 10.5, 1.0));
    CHECK(compressed->integrated < plain->integrated - 8);
}

// ---- UI ----

void testMixerButtons()
{
    Project p;
    Engine engine;
    Mixer mixer(&p, &engine);
    mixer.show();
    auto* eq = mixer.findChild<QToolButton*>("eq_A1");
    auto* dyn = mixer.findChild<QToolButton*>("dyn_A2");
    if (!CHECK(eq && dyn && mixer.findChild<QToolButton*>("eq_A2") && mixer.findChild<QToolButton*>("dyn_A1"))) return;
    CHECK(!mixer.findChild<QToolButton*>("dyn_master")); // the master has its limiter
    CHECK(!eq->isChecked());
    const int steps = p.undoStack()->count();
    eq->click();
    CHECK(p.timeline().audio[0].eq.on && eq->isChecked());
    CHECK_EQ(p.undoStack()->count(), steps + 1);
    dyn->click();
    CHECK(p.timeline().audio[1].dynamics.on);
    p.undoStack()->undo();
    CHECK(!p.timeline().audio[1].dynamics.on && !dyn->isChecked()); // the button follows the model
    // A new track gets its own buttons
    p.edit("track", [](Timeline& tl) { tl.audio.append(Track{TrackKind::Audio}); });
    QApplication::processEvents();
    CHECK(mixer.findChild<QToolButton*>("eq_A3"));
}

void testEqCurveDrag()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    EqCurve curve(&ed, 0);
    curve.resize(400, 200);
    curve.show();
    const int steps = p.undoStack()->count();
    const QPoint from = curve.pointFor(2).toPoint(); // bell at 500 Hz, 0 dB
    QTest::mousePress(&curve, Qt::LeftButton, Qt::NoModifier, from);
    for (int dy = 5; dy <= 40; dy += 5) QTest::mouseMove(&curve, from - QPoint(0, dy));
    QTest::mouseRelease(&curve, Qt::LeftButton, Qt::NoModifier, from - QPoint(0, 40));
    const EqBand& b = p.timeline().audio[0].eq.bands[2];
    CHECK(b.gainDb > 2);
    CHECK(near(b.freq, 500, 25));
    CHECK(p.timeline().audio[0].eq.on); // dragging switches the EQ on
    CHECK_EQ(p.undoStack()->count(), steps + 1);
    // Sideways: the frequency follows, the curve point stays under the mouse
    const QPoint at = curve.pointFor(2).toPoint();
    QTest::mousePress(&curve, Qt::LeftButton, Qt::NoModifier, at);
    QTest::mouseMove(&curve, at + QPoint(60, 0));
    QTest::mouseRelease(&curve, Qt::LeftButton, Qt::NoModifier, at + QPoint(60, 0));
    CHECK(p.timeline().audio[0].eq.bands[2].freq > 600);
    CHECK((curve.pointFor(2).toPoint() - (at + QPoint(60, 0))).manhattanLength() <= 3);
    // Double click on a point: back to 0 dB
    QTest::mouseDClick(&curve, Qt::LeftButton, Qt::NoModifier, curve.pointFor(2).toPoint());
    CHECK_EQ(p.timeline().audio[0].eq.bands[2].gainDb, 0.0);
}

void testDialogs()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    DynamicsDialog dyn(&ed, 1);
    auto* on = dyn.findChild<QCheckBox*>("dynamicsOn");
    auto* threshold = dyn.findChild<QLineEdit*>("threshold");
    auto* ratio = dyn.findChild<QLineEdit*>("ratio");
    if (!CHECK(on && threshold && ratio && dyn.findChild<QLineEdit*>("attack") && dyn.findChild<QLineEdit*>("release")
               && dyn.findChild<QLineEdit*>("knee") && dyn.findChild<QLineEdit*>("makeup")))
        return;
    on->setChecked(true);
    threshold->selectAll();
    QTest::keyClicks(threshold, "-30");
    QTest::keyClick(threshold, Qt::Key_Return);
    ratio->selectAll();
    QTest::keyClicks(ratio, "200"); // clamped to the maximum ratio
    QTest::keyClick(ratio, Qt::Key_Return);
    const TrackDynamics& d = p.timeline().audio[1].dynamics;
    CHECK(d.on && d.thresholdDb == -30);
    CHECK(d.ratio > 1 && d.ratio <= 100);
    // The EQ dialog shows the curve of the same track and an on switch
    EqDialog eq(&ed, 1);
    CHECK(eq.findChild<EqCurve*>());
    auto* eqOn = eq.findChild<QCheckBox*>("eqOn");
    if (!CHECK(eqOn)) return;
    eqOn->setChecked(true);
    CHECK(p.timeline().audio[1].eq.on);
}

// ---- CLI ----

void testCliMixerEqCompressor(const QString& dir)
{
    const QString path = dir + "/mix.schneidi";
    {
        Project p;
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    auto run = [&](const QString& ops, QString* code = nullptr) {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, {{"project", path}, {"ops", ops}}), ctx);
        } catch (const Cli::Error& e) {
            if (code) *code = e.code;
            return QJsonObject{};
        }
    };
    const QJsonObject r = run(R"([{"op":"mixer","track":"A1","compressor":{"on":true,"threshold_db":-24,"ratio":3,
                                   "attack_ms":5,"release_ms":120,"knee_db":4,"makeup_db":2},
                                   "eq":{"on":true,"bands":[{"band":3,"freq":2500,"gain_db":4,"q":1.5},{"band":1,"on":true,"freq":90}]}}])");
    ProjectData d;
    QString err;
    if (!CHECK(ProjectFile::load(path, &d, &err))) return;
    CHECK(d.timeline.audio[0].dynamics == comp(-24, 3, 4, 2, 5, 120));
    const EqBand& b3 = d.timeline.audio[0].eq.bands[2]; // bands count from 1 like in the dialog
    CHECK(d.timeline.audio[0].eq.on && b3.freq == 2500 && b3.gainDb == 4 && b3.q == 1.5);
    CHECK(d.timeline.audio[0].eq.bands[0].on && d.timeline.audio[0].eq.bands[0].freq == 90);
    // info shows them on the track
    const QJsonObject track = r["timeline"]["tracks"][2].toObject(); // V1, V2, A1
    CHECK(track["compressor"]["threshold_db"].toDouble() == -24);
    CHECK(track["eq"]["bands"].toArray().size() == 6);
    // Errors
    QString code;
    run(R"([{"op":"mixer","track":"A1","compressor":{"ratio":0.5}}])", &code);
    CHECK_EQ(code, QString("BAD_ARGUMENT"));
    code.clear();
    run(R"([{"op":"mixer","track":"A1","eq":{"bands":[{"band":7,"gain_db":3}]}}])", &code);
    CHECK_EQ(code, QString("BAD_ARGUMENT"));
    code.clear();
    run(R"([{"op":"mixer","track":"master","compressor":{"on":true}}])", &code);
    CHECK_EQ(code, QString("BAD_ARGUMENT"));
    code.clear();
    run(R"([{"op":"mixer","track":"A1","eq":{"bands":[{"band":2,"freq":-5}]}}])", &code);
    CHECK_EQ(code, QString("BAD_ARGUMENT"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("dynamics-eq");
    QTemporaryDir dir;
    g_dir = dir.path();

    testCompressorStaticCurve();
    testCompressorSteadyLevels();
    testCompressorAttackRelease();
    testCompressorStereoLinked();
    testCompressorBlockSizeAndRate();
    testCompressorSilenceAndExtremes();
    testEqDefaults();
    testEqResponse();
    testEqProcessingMatchesResponse();
    testEqFlatIsTransparentAndStable();
    testEditorTrackEqDynamicsUndo();
    testProjectFileRoundTrip();
    testMixerButtons();
    testEqCurveDrag();
    testDialogs();
    testCliMixerEqCompressor(dir.path());

    if (!Check::haveFfmpeg()) return Check::result();
    // Two channels in aevalsrc itself: "-ac 2" would upmix mono at -3 dB
    g_tone = Check::makeMedia(g_dir + "/tone.wav", {"-f", "lavfi", "-i",
                                                    "aevalsrc=0.5*sin(2*PI*1000*t)|0.5*sin(2*PI*1000*t):s=48000:d=10"});
    g_twoTone = Check::makeMedia(g_dir + "/two.wav",
                                 {"-f", "lavfi", "-i",
                                  "aevalsrc=0.25*sin(2*PI*100*t)+0.25*sin(2*PI*5000*t)|"
                                  "0.25*sin(2*PI*100*t)+0.25*sin(2*PI*5000*t):s=48000:d=10"});
    if (!CHECK(!g_tone.isEmpty() && !g_twoTone.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    testRenderOrderEqCompressorFader();
    testRenderEqChangesSpectrum();
    testLiveChangeWithoutRebuild();
    testLoudnessMeasurementIncludesDynamics();
    return Check::result();
}
