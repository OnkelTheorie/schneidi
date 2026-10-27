// Test stabilization (effect "stabilize", MLT vid.stab with an analysis pass, engine/Stabilizer) and noise reduction
// (effect "denoise", afftdn): registry, editor (clip kinds, undo), project file, paste attributes; analysis of a
// synthetic shaky clip (a still image with jittering crop) -> rendered frames move less, cache reuse, speed change,
// background queue (cancel), export runs a missing analysis first; noise reduction lowers the noise of sine + noise.
#include "check.h"

#include "core/Editor.h"
#include "core/I18n.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Denoise.h"
#include "engine/Exporter.h"
#include "engine/Profiles.h"
#include "engine/Stabilizer.h"
#include "engine/TimelineBuilder.h"
#include "ui/EffectsLibrary.h"
#include "ui/Inspector.h"

#include <Mlt.h>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <clocale>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

Clip mk(int id, const QString& path, int start, int in, int out, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = out;
    c.linkId = link;
    return c;
}

Timeline videoTimeline(const Clip& c)
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(1);
    tl.video[0].clips << c;
    return tl;
}

// Mean absolute luma difference of consecutive frames in the middle of the picture (timeline frames [from, to]),
// rendered at w x h (like a smaller preview)
double jitter(const Timeline& tl, int from, int to, int w, int h)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    std::vector<uint8_t> prev;
    double sum = 0;
    int n = 0;
    for (int pos = from; pos <= to; ++pos) {
        tr->seek(pos);
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_image_format fmt = mlt_image_yuv420p;
        int ww = w, hh = h;
        const uint8_t* d = f->get_image(fmt, ww, hh);
        if (!d || ww != w || hh != h) return -1;
        std::vector<uint8_t> cur(d, d + size_t(w) * h);
        if (!prev.empty()) {
            double s = 0;
            int k = 0;
            for (int y = h / 4; y < 3 * h / 4; ++y)
                for (int x = w / 4; x < 3 * w / 4; ++x, ++k) s += std::abs(int(cur[size_t(y) * w + x]) - prev[size_t(y) * w + x]);
            sum += s / k;
            ++n;
        }
        prev = std::move(cur);
    }
    return n ? sum / n : -1;
}

// RMS level (dBFS) of the mixed sound over the frames [from, to], played from frame 0 (like the export: the noise
// reduction learns the noise as it goes)
double rmsDb(const Timeline& tl, int from, int to)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(0);
    double sum = 0;
    long count = 0;
    for (int pos = 0; pos <= to; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (!pcm || fmt != mlt_audio_float || pos < from) continue;
        for (int i = 0; i < samples; ++i) sum += double(pcm[i]) * pcm[i]; // first channel (planar)
        count += samples;
    }
    return count && sum > 0 ? 10 * std::log10(sum / count) : -200;
}

bool waitFor(const std::function<bool()>& done, int ms)
{
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (done()) loop.quit();
    });
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    timer.start();
    if (!done()) loop.exec();
    return done();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("stabilize-denoise");

    // --- Registry: kinds, defaults, analysis parameters
    {
        const EffectDescriptor* st = EffectRegistry::find("stabilize");
        const EffectDescriptor* dn = EffectRegistry::find("denoise");
        CHECK(st && st->video && st->library);
        CHECK(dn && !dn->video && dn->library);
        if (st) {
            int analysis = 0;
            for (const EffectParam& p : st->params) analysis += p.analysis;
            CHECK_EQ(analysis, 2); // shakiness, accuracy
            for (const EffectParam& p : st->params)
                if (p.key == "border") CHECK(p.choiceLabels.size() == p.choices.size() && p.choiceText("auto") != "auto");
        }
        CHECK(Editor::effectKind("denoise") == TrackKind::Audio);
        CHECK(Editor::effectKind("stabilize") == TrackKind::Video);
        CHECK(Editor::effectKind("lut:/x/a.cube") == TrackKind::Video);
    }

    // --- Editor: effects only on clips of their kind, undo, project file, paste attributes (audio effects)
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
        p.edit("setup", [](Timeline& tl) {
            tl.video[0].clips = {mk(1, "/x/a.mp4", 0, 0, 99, 1)};
            tl.audio[0].clips = {mk(11, "/x/a.mp4", 0, 0, 99, 1), mk(12, "/x/a.mp4", 100, 0, 99)};
        });
        auto clip = [&](int id) { return TimelineOps::findClip(p.timeline(), id); };
        ed.addEffect({1, 11}, "denoise");
        CHECK(!EffectRegistry::has(*clip(1), "denoise") && EffectRegistry::has(*clip(11), "denoise"));
        ed.addEffect({1, 11}, "stabilize");
        CHECK(EffectRegistry::has(*clip(1), "stabilize") && !EffectRegistry::has(*clip(11), "stabilize"));
        p.undoStack()->undo();
        CHECK(!EffectRegistry::has(*clip(1), "stabilize"));
        p.undoStack()->redo();
        // Double click in the library without a selection: clip at the playhead of the effect's kind
        CHECK_EQ(ed.effectTargets(110, TrackKind::Audio), QVector<int>{12});
        CHECK_EQ(ed.effectTargets(10, TrackKind::Video), QVector<int>{1});
        ed.modifyClips({11}, "x", [](Clip& c) { EffectRegistry::instance(c, "denoise")->params["reduction"] = 25.0; });

        // Project file round trip
        QTemporaryDir dir;
        const QString path = dir.filePath("p.schneidi");
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
        ProjectData back;
        CHECK(ProjectFile::load(path, &back, &error));
        const Clip* a = TimelineOps::findClip(back.timeline, 11);
        const Clip* v = TimelineOps::findClip(back.timeline, 1);
        CHECK(a && EffectRegistry::value(*a, "denoise", "reduction").toDouble() == 25.0);
        CHECK(v && EffectRegistry::value(*v, "stabilize", "border").toString() == "auto");
        CHECK(a && *a == *clip(11)); // loads unchanged (types like the registry)

        // Paste attributes: audio effects from an audio clip to another one
        sel.set({11});
        ed.copySelection();
        CHECK(ed.pasteAttributesAvailable() & Editor::AttrAudioEffects);
        sel.set({12});
        ed.pasteAttributes(Editor::AttrAudioEffects);
        CHECK(EffectRegistry::value(*clip(12), "denoise", "reduction").toDouble() == 25.0);
    }

    // --- Noise reduction core: delay, quiet noise, keeps a tone, speech-like bursts
    {
        const int rate = 48000;
        Denoise::Processor p(rate);
        // Delay: an impulse comes out exactly latency() samples later, unchanged (no reduction)
        p.setParams(0, -50, false);
        std::vector<float> imp(4096, 0.f);
        imp[100] = 1.f;
        p.process(imp.data(), int(imp.size()));
        const auto peak = std::max_element(imp.begin(), imp.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
        CHECK_EQ(int(peak - imp.begin()), 100 + p.latency());
        CHECK(std::abs(*peak - 1.f) < 0.01f);
        CHECK(p.latency() <= rate / 80); // ~11 ms, well below lip sync tolerance

        // 3 s noise (-35 dBFS RMS), then 3 s noise + tone bursts (speech-like on/off)
        auto run = [&](double reduction, bool track, double floor, double* noiseDb, double* toneDb, double* rawTone) {
            Denoise::Processor d(rate);
            d.setParams(reduction, floor, track);
            std::vector<float> x(size_t(rate) * 6), clean(x.size());
            uint32_t seed = 1;
            for (size_t i = 0; i < x.size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double noise = ((seed >> 8) / double(1 << 24) * 2 - 1) * 0.0308; // uniform: RMS 0.0178 = -35 dBFS
                const double t = double(i) / rate;
                const bool on = t >= 3 && std::fmod(t, 0.5) < 0.3;
                clean[i] = on ? float(0.3 * std::sin(2 * std::numbers::pi * 440 * t) * std::sin(std::numbers::pi * std::fmod(t, 0.5) / 0.3)) : 0.f;
                x[i] = float(noise) + clean[i];
            }
            d.process(x.data(), int(x.size()));
            const int lat = d.latency();
            auto rms = [&](const std::vector<float>& v, double from, double to, int shift) {
                double s = 0;
                long n = 0;
                for (long i = long(from * rate); i < long(to * rate); ++i, ++n) s += double(v[size_t(i + shift)]) * v[size_t(i + shift)];
                return 10 * std::log10(s / double(n));
            };
            *noiseDb = rms(x, 2, 2.9, lat);
            // tone: inside the bursts only (where the clean tone is loud)
            double s = 0, c = 0;
            for (long i = 3 * rate; i < 6 * rate - lat; ++i)
                if (std::abs(clean[size_t(i)]) > 0.1f) {
                    s += double(x[size_t(i + lat)]) * x[size_t(i + lat)];
                    c += double(clean[size_t(i)]) * clean[size_t(i)];
                }
            *toneDb = 10 * std::log10(s);
            *rawTone = 10 * std::log10(c);
        };
        double noise = 0, tone = 0, raw = 0;
        run(12, true, -50, &noise, &tone, &raw);
        if (!CHECK(noise < -35 - 8)) std::printf("       tracked: noise -35 -> %.1f dB\n", noise);
        if (!CHECK(std::abs(tone - raw) < 1.0)) std::printf("       tracked: tone %.2f dB off\n", tone - raw);
        double strongNoise = 0;
        run(30, true, -50, &strongNoise, &tone, &raw);
        if (!CHECK(strongNoise < noise - 5)) std::printf("       strong: noise %.1f dB\n", strongNoise);
        run(12, false, -35, &noise, &tone, &raw); // fixed noise level
        if (!CHECK(noise < -35 - 8)) std::printf("       fixed: noise -35 -> %.1f dB\n", noise);
        if (!CHECK(std::abs(tone - raw) < 1.0)) std::printf("       fixed: tone %.2f dB off\n", tone - raw);
    }

    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt; // 1920x1080, 25 fps

    // The first qt module service (qtblend: transform/opacity) must not put LC_NUMERIC back to the user's locale:
    // vid.stab then could not read its results ("0.48" with a German locale) and exported unstabilized
    {
        Mlt::Profile profile;
        Mlt::Transition blend(profile, "qtblend");
        CHECK_EQ(QString(std::setlocale(LC_NUMERIC, nullptr)), QString("C"));
    }

    // --- Noise reduction: 0-2 s noise only, 2-4 s sine + noise
    {
        const QString noisy = Check::makeMedia(
            tmp.filePath("noisy.wav"),
            {"-f", "lavfi", "-i",
             "aevalsrc=0.03*(2*random(0)-1)+gte(t\\,2)*0.3*sin(2*PI*440*t)|0.03*(2*random(1)-1)+gte(t\\,2)*0.3*sin(2*PI*440*t)"
             ":s=48000:d=4",
             "-c:a", "pcm_f32le"});
        if (CHECK(!noisy.isEmpty())) {
            Timeline plain;
            plain.audio.resize(1);
            plain.audio[0].clips << mk(1, noisy, 0, 0, 99);
            Timeline denoised = plain;
            CHECK(EffectRegistry::add(denoised.audio[0].clips[0], "denoise"));
            const double noiseBefore = rmsDb(plain, 25, 49), noiseAfter = rmsDb(denoised, 25, 49);
            const double toneBefore = rmsDb(plain, 75, 99), toneAfter = rmsDb(denoised, 75, 99);
            std::printf("noise reduction: noise %.1f -> %.1f dB, tone %.1f -> %.1f dB\n", noiseBefore, noiseAfter,
                        toneBefore, toneAfter);
            CHECK(noiseAfter < noiseBefore - 6);
            if (!CHECK(std::abs(toneAfter - toneBefore) < 1.5))
                std::printf("       tone %.1f -> %.1f dB\n", toneBefore, toneAfter);
            // Stronger reduction lowers more; switched off = unchanged
            Timeline strong = denoised;
            EffectRegistry::instance(strong.audio[0].clips[0], "denoise")->params["reduction"] = 30.0;
            CHECK(rmsDb(strong, 25, 49) < noiseAfter - 2);
            Timeline off = denoised;
            EffectRegistry::instance(off.audio[0].clips[0], "denoise")->enabled = false;
            CHECK(std::abs(rmsDb(off, 25, 49) - noiseBefore) < 0.1);
        }
    }

    // --- Stabilization
    if (!Stabilizer::available()) {
        std::printf("vid.stab fehlt: Stabilisieren nicht geprüft\n");
        return Check::result();
    }
    QDir(Stabilizer::cacheDir()).removeRecursively(); // test paths: results of earlier runs (other temp files)
    // A still image with a jittering crop = camera shake over a static scene (slightly blurred: sub-pixel
    // interpolation of the sharp test pattern would dominate the frame differences)
    const QString still = Check::makeMedia(tmp.filePath("still.png"),
                                           {"-f", "lavfi", "-i", "testsrc2=size=1280x768:rate=1:duration=1", "-vf", "gblur=sigma=3", "-frames:v", "1"});
    const QString shaky = Check::makeMedia(tmp.filePath("shaky.mp4"),
                                           {"-loop", "1", "-framerate", "25", "-i", still, "-t", "4", "-vf",
                                            "crop=1120:630:80+25*sin(n*1.7):69+18*cos(n*2.3),scale=1920:1080",
                                            "-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "yuv420p"});
    if (!CHECK(!shaky.isEmpty())) return Check::result();

    Clip c = mk(1, shaky, 0, 10, 79);
    EffectRegistry::add(c, "stabilize");
    const auto req = Stabilizer::request(c, fmt);
    CHECK(req && req->width == 1280 && req->height == 720 && req->needFrom == 10 && req->needTo == 79);
    if (!req) return Check::result();
    CHECK(!Stabilizer::find(*req).valid());
    CHECK_EQ(Stabilizer::missing(videoTimeline(c), fmt).size(), 1);
    {
        Clip title;
        title.kind = ClipKind::Title;
        EffectRegistry::add(title, "stabilize");
        CHECK(!Stabilizer::request(title, fmt) && !Stabilizer::unsupportedReason(title).isEmpty());
    }

    // Without analysis the clip plays as it is
    Clip raw = c;
    EffectRegistry::remove(raw, "stabilize");
    const double rawJitter = jitter(videoTimeline(raw), 0, 40, 640, 360);
    CHECK(std::abs(jitter(videoTimeline(c), 0, 40, 640, 360) - rawJitter) < 0.01);

    QString error;
    std::atomic<int> progress{0};
    CHECK(Stabilizer::analyze(*req, &error, nullptr, &progress));
    CHECK_EQ(progress.load(), 1000);
    const Stabilizer::Result res = Stabilizer::find(*req);
    CHECK(res.valid() && res.from == 10 && res.to == 79);
    CHECK(Stabilizer::missing(videoTimeline(c), fmt).isEmpty());
    const double stabJitter = jitter(videoTimeline(c), 0, 40, 640, 360);
    std::printf("stabilization: frame difference %.2f -> %.2f\n", rawJitter, stabJitter);
    CHECK(stabJitter > 0 && stabJitter < rawJitter * 0.5);
    // Other sizes (preview, export) use the same analysis
    const double small = jitter(videoTimeline(c), 0, 40, 320, 180);
    CHECK(small > 0 && small < jitter(videoTimeline(raw), 0, 40, 320, 180) * 0.5);

    // Trimmed shorter / moved: same analysis; other shakiness: a new one; smoothing: none
    {
        Clip t = c;
        t.in = 20;
        t.out = 60;
        t.start = 50;
        CHECK(Stabilizer::find(*Stabilizer::request(t, fmt)).valid());
        EffectRegistry::instance(t, "stabilize")->params["smoothing"] = 40.0;
        CHECK(Stabilizer::find(*Stabilizer::request(t, fmt)).valid());
        EffectRegistry::instance(t, "stabilize")->params["shakiness"] = 8.0;
        CHECK(!Stabilizer::find(*Stabilizer::request(t, fmt)).valid());
        Clip longer = c;
        longer.in = 0;
        CHECK(!Stabilizer::find(*Stabilizer::request(longer, fmt)).valid());
        Clip off = c; // switched off: nothing needed
        EffectRegistry::instance(off, "stabilize")->enabled = false;
        off.in = 0;
        CHECK(Stabilizer::missing(videoTimeline(off), fmt).isEmpty());
    }

    // Speed change (timewarp): analyzed on the same producer the timeline reads
    {
        Clip slow = mk(1, shaky, 0, 0, 59);
        slow.speed = 0.5;
        EffectRegistry::add(slow, "stabilize");
        const auto r = Stabilizer::request(slow, fmt);
        CHECK(r && r->speed == 0.5 && r->base() != req->base());
        CHECK(Stabilizer::analyze(*r, &error));
        Clip slowRaw = slow;
        EffectRegistry::remove(slowRaw, "stabilize");
        const double before = jitter(videoTimeline(slowRaw), 0, 40, 640, 360);
        const double after = jitter(videoTimeline(slow), 0, 40, 640, 360);
        if (!CHECK(after > 0 && after < before * 0.6)) std::printf("       speed 50 %%: jitter %.2f -> %.2f\n", before, after);
    }

    // Background queue: runs, reports, can be cancelled
    {
        Stabilizer* st = Stabilizer::instance();
        Clip q = c;
        EffectRegistry::instance(q, "stabilize")->params["accuracy"] = 4.0;
        const auto r = Stabilizer::request(q, fmt);
        CHECK(st->state(*r) == Stabilizer::State::Missing);
        int ready = 0;
        QObject::connect(st, &Stabilizer::resultReady, &app, [&] { ++ready; });
        st->sync({*r});
        CHECK(st->state(*r) == Stabilizer::State::Running);
        CHECK(waitFor([&] { return st->state(*r) == Stabilizer::State::Done; }, 60000));
        CHECK(waitFor([&] { return ready == 1; }, 1000));

        EffectRegistry::instance(q, "stabilize")->params["accuracy"] = 5.0;
        const auto r2 = Stabilizer::request(q, fmt);
        st->sync({*r2});
        st->cancel(*r2);
        CHECK(waitFor([&] { return st->state(*r2) == Stabilizer::State::Cancelled; }, 10000));
        st->sync({*r2}); // cancelled ones stay cancelled until started explicitly
        CHECK(st->state(*r2) == Stabilizer::State::Cancelled);
        st->start(*r2);
        CHECK(st->state(*r2) == Stabilizer::State::Running);
        st->sync({}); // not needed any more: stops, not marked as cancelled
        CHECK(waitFor([&] { return st->state(*r2) == Stabilizer::State::Missing; }, 10000));
        CHECK(!Stabilizer::find(*r2).valid());
    }

    // Export runs a missing analysis first (never exports unstabilized)
    {
        Clip e = c;
        EffectRegistry::instance(e, "stabilize")->params["shakiness"] = 7.0;
        const auto r = Stabilizer::request(e, fmt);
        CHECK(!Stabilizer::find(*r).valid());
        Exporter ex;
        ExportSettings s;
        s.path = tmp.filePath("out.mp4");
        s.format = fmt;
        s.size = QSize(960, 540); // other size than the project: same analysis
        s.preset = "ultrafast";
        bool ok = false, done = false;
        QObject::connect(&ex, &Exporter::finished, &app, [&](bool success, const QString&) {
            ok = success;
            done = true;
        });
        CHECK(ex.start(videoTimeline(e), s, &error));
        CHECK(ex.isRunning());
        CHECK(waitFor([&] { return done; }, 120000));
        CHECK(ok && QFileInfo(s.path).size() > 0);
        CHECK(Stabilizer::find(*r).valid());

        // Cancel while analyzing
        EffectRegistry::instance(e, "stabilize")->params["shakiness"] = 9.0;
        done = false;
        CHECK(ex.start(videoTimeline(e), s, &error));
        ex.cancel();
        CHECK(done && !ok && !ex.isRunning());

        // Cancel right as the analysis ends (the window was busy meanwhile): its finished() waits in the event queue
        // when cancel() stops it; it must do nothing then (it used the deleted thread: crash) and never export
        EffectRegistry::instance(e, "stabilize")->params["shakiness"] = 8.0;
        const auto r8 = Stabilizer::request(e, fmt);
        s.path = tmp.filePath("canceled.mp4");
        done = false;
        int finishedCount = 0;
        QObject::connect(&ex, &Exporter::finished, &app, [&] { ++finishedCount; });
        CHECK(ex.start(videoTimeline(e), s, &error));
        QElapsedTimer analyzing; // no events meanwhile: the result of the thread stays queued
        analyzing.start();
        while (!Stabilizer::find(*r8).valid() && analyzing.elapsed() < 120000) QThread::msleep(20);
        CHECK(Stabilizer::find(*r8).valid());
        QThread::msleep(300); // the thread has ended
        ex.cancel();
        CHECK(done && !ok && !ex.isRunning());
        waitFor([] { return false; }, 500);
        CHECK_EQ(finishedCount, 1);
        CHECK(!ex.isRunning() && !Exporter::anyRunning());
        CHECK(!QFileInfo::exists(s.path));
    }

    // Inspector (stabilization state, border choice with labels; noise reduction in the Audio tab) and Effects
    // Library (Audio FX). Pictures: STABILIZE_DUMP=<folder> build-tests/tests/test_stabilize_denoise
    {
        const QString dump = QString::fromLocal8Bit(qgetenv("STABILIZE_DUMP"));
        Project project;
        Selection sel;
        Editor editor(&project, &sel);
        project.addMedia({shaky, "shaky.mp4", 100, true, false, false});
        project.addMedia({tmp.filePath("noisy.wav"), "noisy.wav", 100, false, true, false});
        project.edit("setup", [&](Timeline& tl) {
            tl.video[0].clips = {mk(1, shaky, 0, 10, 79)};
            tl.audio[0].clips = {mk(2, tmp.filePath("noisy.wav"), 0, 0, 69)};
        });
        Inspector insp(&editor);
        insp.resize(380, 900);
        sel.set({1});
        insp.show();
        app.processEvents();
        editor.addEffect({1}, "stabilize"); // analyzed above (same clip and values): done at once
        app.processEvents();
        QLabel* status = insp.findChild<QLabel*>("StabilizeStatus");
        CHECK(status && status->isVisibleTo(&insp) && status->text() == T("Fertig"));
        QComboBox* border = nullptr;
        for (QComboBox* c : insp.findChildren<QComboBox*>())
            if (c->findData("black") >= 0) border = c;
        if (CHECK(border)) {
            CHECK(border->currentData().toString() == "auto" && border->currentText() == T("Automatisch zoomen"));
            border->activated(border->findData("black"));
            CHECK(EffectRegistry::value(*TimelineOps::findClip(project.timeline(), 1), "stabilize", "border").toString() == "black");
        }
        if (!dump.isEmpty()) insp.grab().save(QDir(dump).filePath("inspector-video.png"));
        // Other shakiness: not analyzed yet (the preview engine would start it; here no engine runs)
        editor.modifyClips({1}, "x", [](Clip& c) { EffectRegistry::instance(c, "stabilize")->params["shakiness"] = 3.0; });
        app.processEvents();
        CHECK(status && status->text() == T("Noch nicht analysiert"));

        sel.set({2});
        editor.addEffect({2}, "denoise");
        app.processEvents();
        for (QToolButton* b : insp.findChildren<QToolButton*>())
            if (b->text().endsWith("\nAudio")) b->click();
        app.processEvents();
        bool shown = false;
        for (QToolButton* b : insp.findChildren<QToolButton*>())
            if (b->text() == T("Rauschunterdrückung") && b->isVisibleTo(&insp)) shown = true;
        CHECK(shown);
        if (!dump.isEmpty()) insp.grab().save(QDir(dump).filePath("inspector-audio.png"));

        EffectsLibrary lib;
        lib.resize(520, 420);
        auto* tree = lib.findChild<QTreeWidget*>();
        auto* list = lib.findChild<QListWidget*>();
        auto names = [&] {
            QStringList out;
            for (int i = 0; i < list->count(); ++i) out << list->item(i)->text();
            return out;
        };
        const auto audioFx = tree->findItems("Audio FX", Qt::MatchExactly | Qt::MatchRecursive);
        if (CHECK(audioFx.size() == 1)) {
            tree->setCurrentItem(audioFx.first());
            CHECK(names().contains(T("Rauschunterdrückung")) && !names().contains(T("Stabilisieren")));
            lib.show();
            app.processEvents();
            if (!dump.isEmpty()) lib.grab().save(QDir(dump).filePath("library.png"));
        }
        const auto filters = tree->findItems(T("Filter"), Qt::MatchExactly | Qt::MatchRecursive);
        if (CHECK(filters.size() == 1)) {
            tree->setCurrentItem(filters.first());
            CHECK(names().contains(T("Stabilisieren")) && !names().contains(T("Rauschunterdrückung")));
        }
    }
    return Check::result();
}
