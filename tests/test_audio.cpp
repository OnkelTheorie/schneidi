// Test Audio: normalizing (editor logic and peak level measurement), master limiter and level meter in the render,
// limiter settings in the project file. Test media are created with ffmpeg.
#include "check.h"

#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

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

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

// Peak level (dBFS) of the mixed audio of the timeline over frames [from, to]; meterDb = value of the master level meter
double renderPeak(const Timeline& tl, int from, int to, double* meterDb = nullptr)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    MixerHooks hooks;
    auto tr = b.build(tl, meterDb ? &hooks : nullptr);
    tr->seek(from);
    float peak = 0.f;
    double meter = -200;
    for (int pos = from; pos <= to; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (!pcm || fmt != mlt_audio_float) continue;
        for (int i = 0; i < samples * channels; ++i) peak = std::max(peak, std::abs(pcm[i]));
        if (meterDb && hooks.master.meter) meter = std::max(meter, hooks.master.meter->get_double("_audio_level.0"));
    }
    if (meterDb) *meterDb = meter;
    return peak > 1e-10f ? 20 * std::log10(peak) : -200;
}

// First sample (from frame 0) above 0.2 in the mixed audio, -1 = none
long firstClick(const Timeline& tl, int frames)
{
    auto prof = makeProfile(ProjectFormat{});
    TimelineBuilder b(*prof);
    auto tr = b.build(tl, nullptr);
    tr->seek(0);
    long at = 0;
    for (int pos = 0; pos < frames; ++pos) {
        std::unique_ptr<Mlt::Frame> f(tr->get_frame());
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, channels = 2;
        int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
        const auto* pcm = static_cast<const float*>(f->get_audio(fmt, freq, channels, samples));
        if (pcm && fmt == mlt_audio_float)
            for (int i = 0; i < samples; ++i)
                if (std::abs(pcm[i]) > 0.2f) return at + i;
        at += samples;
    }
    return -1;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio");

    // --- Normalizing: editor logic (measured peaks are given)
    {
        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.addMedia({"/x/a.mp4", "a.mp4", 1000, true, true, false});
        p.addMedia({"/x/m.wav", "m.wav", 1000, false, true, false});
        p.edit("setup", [](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 3);
            tl.video[0].clips = {mk(1, "/x/a.mp4", 0, 0, 99, 1)};
            tl.audio[0].clips = {mk(11, "/x/a.mp4", 0, 0, 99, 1), mk(12, "/x/m.wav", 100, 0, 99)};
            tl.audio[1].clips = {mk(21, "/x/m.wav", 0, 0, 99)};
            tl.audio[2].clips = {mk(31, "/x/m.wav", 0, 0, 99)};
            tl.audio[2].locked = true;
        });
        auto vol = [&](int id) { return TimelineOps::findClip(p.timeline(), id)->volumeDb; };

        // Selection: video clip brings its audio along, locked track does not
        sel.set({1, 12, 31});
        CHECK_EQ(ed.selectedAudioClips(), (QVector<int>{11, 12}));

        // Independent: each clip to -9 dBFS
        const int before = p.undoStack()->index();
        ed.normalizeAudio({{11, -3.0}, {12, -20.0}, {21, -200.0}, {31, -6.0}, {1, -1.0}}, -9.0, false);
        CHECK_EQ(p.undoStack()->index(), before + 1);
        CHECK(near(vol(11), -6.0, 1e-9) && near(vol(12), 11.0, 1e-9));
        CHECK(vol(21) == 0.0 && vol(31) == 0.0); // silent/locked ones stay
        CHECK(TimelineOps::findClip(p.timeline(), 1)->volumeDb == 0.0); // video is not touched
        p.undoStack()->undo();
        CHECK(vol(11) == 0.0 && vol(12) == 0.0);

        // Relative: all by the same amount, the loudest hits the target
        ed.normalizeAudio({{11, -3.0}, {12, -20.0}}, -9.0, true);
        CHECK(near(vol(11), -6.0, 1e-9) && near(vol(12), -6.0, 1e-9));
        // Limit to +12 dB
        ed.normalizeAudio({{12, -40.0}}, -1.0, false);
        CHECK_EQ(vol(12), kMaxVolumeDb);

        // Volume keyframes: curve shifted, highest point = new value
        ed.modifyClips({21}, "k", [](Clip& c) {
            Keys::setKey(c, AnimParam::Volume, 0, -30);
            Keys::setKey(c, AnimParam::Volume, 50, -10);
        });
        ed.normalizeAudio({{21, -2.0}}, -6.0, false);
        const Clip* k = TimelineOps::findClip(p.timeline(), 21);
        CHECK(near(Keys::valueAt(*k, AnimParam::Volume, 0), -24.0, 1e-9)
              && near(Keys::valueAt(*k, AnimParam::Volume, 50), -4.0, 1e-9));

        // Nothing to do: no undo step
        const int n = p.undoStack()->index();
        ed.normalizeAudio({{21, -200.0}, {31, -3.0}}, -9.0, false);
        ed.normalizeAudio({}, -9.0, true);
        CHECK_EQ(p.undoStack()->index(), n);
    }

    // --- Limiter settings in the project file
    {
        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        d.timeline.masterLimiter = true;
        d.timeline.masterLimiterDb = -0.3;
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(d, "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
        CHECK(back.timeline.masterLimiter && near(back.timeline.masterLimiterDb, -0.3, 1e-9));
        // old file: off, -1 dBFS
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &back, &err));
        CHECK(!back.timeline.masterLimiter && back.timeline.masterLimiterDb == kDefaultLimiterDb);
    }

    // --- Fine position (sub-frame): moving in 1/100 frame, carry-over, project file
    {
        Clip c = mk(1, "/x/m.wav", 10, 0, 49);
        TimelineOps::shiftFine(c, 150);
        CHECK(c.start == 11 && c.subframe == 50);
        TimelineOps::shiftFine(c, -60);
        CHECK(c.start == 10 && c.subframe == 90);
        TimelineOps::shiftFine(c, -5000); // not before 0
        CHECK(c.start == 0 && c.subframe == 0);

        Project p;
        Selection sel;
        Editor ed(&p, &sel);
        p.addMedia({"/x/m.wav", "m.wav", 1000, false, true, false});
        p.edit("setup", [](Timeline& tl) {
            TimelineOps::ensureTracks(tl, TrackKind::Audio, 1);
            tl.audio[0].clips = {mk(1, "/x/m.wav", 10, 0, 49)};
        });
        ed.moveClipsFine({1}, 230, TrackKind::Audio, 0);
        const Clip* moved = TimelineOps::findClip(p.timeline(), 1);
        CHECK(moved && moved->start == 12 && moved->subframe == 30);
        ed.moveClipsFine({1}, 100, TrackKind::Audio, 0); // whole frames: fine position stays
        moved = TimelineOps::findClip(p.timeline(), 1);
        CHECK(moved && moved->start == 13 && moved->subframe == 30);
        p.undoStack()->undo();
        p.undoStack()->undo();
        moved = TimelineOps::findClip(p.timeline(), 1);
        CHECK(moved && moved->start == 10 && moved->subframe == 0);

        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        Clip f = mk(1, "/x/m.wav", 10, 0, 49);
        f.subframe = 37;
        d.timeline.audio[0].clips << f;
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(d, "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
        CHECK(back.timeline.audio.size() == 1 && back.timeline.audio[0].clips.size() == 1
              && back.timeline.audio[0].clips[0].subframe == 37);
    }

    // --- With real media (ffmpeg)
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // 4 s stereo sine: 0-2 s amplitude 0.1 (-20 dBFS), 2-4 s 0.5 (-6.02 dBFS)
    const QString tone = Check::makeMedia(tmp.filePath("ton.wav"),
                                          {"-f", "lavfi", "-i",
                                           "aevalsrc=if(lt(t\\,2)\\,0.1\\,0.5)*sin(2*PI*440*t)|if(lt(t\\,2)\\,0.1\\,0.5)*sin(2*PI*440*t):s=48000:d=4",
                                           "-c:a", "pcm_f32le"});
    if (!CHECK(!tone.isEmpty())) return Check::result();
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt; // 25 fps

    const double loud = 20 * std::log10(0.5), quiet = -20.0;
    auto peak = AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 99));
    CHECK(peak && near(*peak, loud, 0.1));
    peak = AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 45)); // only the quiet part (in/out counts)
    CHECK(peak && near(*peak, quiet, 0.1));
    Clip fast = mk(1, tone, 0, 0, 49); // double speed: clip frames 0-49 = whole file
    fast.speed = 2.0;
    peak = AudioAnalysis::clipPeakDb(fmt, fast);
    // Keeping pitch (default) converts and overshoots slightly (~0.4 dB) - that is how it sounds in the timeline too
    if (!CHECK(peak && near(*peak, loud, 0.6))) std::printf("       2x: %.2f\n", peak ? *peak : -999.0);
    Clip frozen = mk(1, tone, 0, 0, 49);
    frozen.freeze = true;
    CHECK(!AudioAnalysis::clipPeakDb(fmt, frozen));
    CHECK(!AudioAnalysis::clipPeakDb(fmt, mk(1, tmp.filePath("fehlt.wav"), 0, 0, 49)));
    int calls = 0; // cancel
    CHECK(!AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 99), [&](double) { return ++calls < 2; }));

    // Render: clip +12 dB on the loud part = about +6 dBFS -> level meter shows above 0 (not clipped at 0),
    // limiter holds the ceiling, in the export (without hooks) as well
    {
        auto prof = makeProfile(fmt);
        Mlt::Transition qt(*prof, "qtblend");
        if (!qt.is_valid()) {
            std::printf("Hinweis: MLT-Qt-Modul ohne Display nicht nutzbar, Render-Prüfungen übersprungen\n");
        } else {
            Timeline tl;
            tl.video.resize(1);
            tl.audio.resize(1);
            Clip c = mk(1, tone, 0, 50, 99);
            c.volumeDb = 12;
            tl.audio[0].clips << c;
            double meter = -200;
            const double open = renderPeak(tl, 10, 40, &meter);
            CHECK(near(open, loud + 12, 0.3));
            CHECK(near(meter, loud + 12, 0.3));
            tl.masterLimiter = true;
            tl.masterLimiterDb = -1.0;
            const double limited = renderPeak(tl, 10, 40, &meter);
            if (!CHECK(limited <= -0.9 && limited > -3.0)) std::printf("       Limiter: %.2f dBFS\n", limited);
            CHECK(meter <= -0.9);
            CHECK(renderPeak(tl, 10, 40) <= -0.9); // export path
            // Quiet audio stays unchanged (limiter does not raise)
            tl.audio[0].clips[0].volumeDb = -10;
            CHECK(near(renderPeak(tl, 10, 40), loud - 10, 0.3));
        }
    }

    // Fine position (sub-frame): audio later by 1/100 frames, also across section boundaries (fade in)
    {
        // Click 4 ms before 0.2 s (frame 5): with 0.5 frames (20 ms) delay it lands behind the boundary
        const QString click = Check::makeMedia(tmp.filePath("klick.wav"),
                                               {"-f", "lavfi", "-i",
                                                "aevalsrc=if(between(t\\,0.196\\,0.1965)\\,0.8\\,0)|if(between(t\\,0.196\\,0.1965)\\,0.8\\,0):s=48000:d=2",
                                                "-c:a", "pcm_f32le"});
        if (CHECK(!click.isEmpty())) {
            Timeline tl;
            tl.video.resize(1);
            tl.audio.resize(1);
            tl.audio[0].clips << mk(1, click, 10, 0, 40);
            const long base = firstClick(tl, 30);
            if (!CHECK(base > 0 && std::abs(base - (10 * 1920 + 9408)) < 48)) std::printf("       Klick: %ld\n", base);
            tl.audio[0].clips[0].subframe = 50;
            const long half = firstClick(tl, 30);
            if (!CHECK(half - base == 960)) std::printf("       Sub-Frame 50: %ld (ohne %ld)\n", half, base);
            tl.audio[0].clips[0].transIn = 5; // section fade in [10, 15), click falls behind it
            const long faded = firstClick(tl, 30);
            if (!CHECK(faded - base == 960)) std::printf("       mit Einblenden: %ld (ohne %ld)\n", faded, base);
        }
    }
    // Seek while playing (SeekFade at the master meter): the old sound still coming fades out and stays silent,
    // the target fades in from 0 (no step in the wave = no click); other frames stay untouched
    {
        Timeline tl;
        tl.video.resize(1);
        tl.audio.resize(1);
        tl.audio[0].clips << mk(1, tone, 0, 0, 99);
        auto prof = makeProfile(fmt);
        TimelineBuilder b(*prof);
        MixerHooks hooks;
        auto tr = b.build(tl, &hooks);
        SeekFade fade;
        if (CHECK(hooks.master.meter)) hooks.master.meter->set("_seek_fade", &fade, 0);
        const auto frameAudio = [&](int pos) {
            std::unique_ptr<Mlt::Frame> f(tr->get_frame());
            mlt_audio_format af = mlt_audio_float;
            int freq = 48000, channels = 2;
            int samples = mlt_audio_calculate_frame_samples(float(prof->fps()), freq, pos);
            const auto* pcm = static_cast<const float*>(f->get_audio(af, freq, channels, samples));
            return std::vector<float>(pcm, pcm + (pcm && af == mlt_audio_float ? samples : 0)); // channel 0 (planar)
        };
        const auto peakOf = [](const std::vector<float>& v, size_t from, size_t to) {
            float p = 0.f;
            for (size_t i = from; i < std::min(to, v.size()); ++i) p = std::max(p, std::abs(v[i]));
            return p;
        };
        tr->seek(60);
        CHECK(peakOf(frameAudio(60), 0, 1920) > 0.45f); // untouched
        fade.start(70, 1.0);
        const std::vector<float> stale = frameAudio(61); // still the old position
        CHECK(stale.size() == 1920 && peakOf(stale, 0, 10) > 0.05f && peakOf(stale, 240, 1920) == 0.f);
        CHECK(peakOf(frameAudio(62), 0, 1920) == 0.f);
        tr->seek(70);
        const std::vector<float> in = frameAudio(70);
        CHECK(in.size() == 1920 && peakOf(in, 0, 5) < 0.01f && peakOf(in, 480, 1920) > 0.45f);
        CHECK_EQ(fade.target.load(), -1);
        CHECK(peakOf(frameAudio(71), 0, 1920) > 0.45f);
        // Playback starts: fade-in at the start frame only
        tr->seek(80);
        fade.fadeIn(80, 1.0);
        const std::vector<float> start = frameAudio(80);
        CHECK(start.size() == 1920 && peakOf(start, 0, 5) < 0.01f && peakOf(start, 480, 1920) > 0.45f);
        CHECK_EQ(fade.target.load(), -1);
        // Dragging the playhead (seeks in quick succession): silent even at the target until release(), then the
        // next sound fades in
        fade.start(90, 1.0);
        fade.hold = true;
        tr->seek(90);
        CHECK(peakOf(frameAudio(90), 240, 1920) == 0.f);
        CHECK_EQ(fade.target.load(), -1);
        CHECK(peakOf(frameAudio(91), 0, 1920) == 0.f);
        fade.release();
        const std::vector<float> resumed = frameAudio(92);
        CHECK(resumed.size() == 1920 && peakOf(resumed, 0, 5) < 0.01f && peakOf(resumed, 480, 1920) > 0.45f);
        CHECK(peakOf(frameAudio(93), 0, 1920) > 0.45f);
    }
    return Check::result();
}
