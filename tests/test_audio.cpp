// Test Audio: Normalisieren (Editor-Logik und Spitzenpegel-Messung), Master-Limiter und Pegelmesser im Render,
// Limiter-Einstellungen in der Projektdatei. Testmedien werden per ffmpeg erzeugt.
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

// Spitzenpegel (dBFS) des gemischten Tons der Timeline über die Frames [from, to]; meterDb = Wert des Master-Pegelmessers
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

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio");

    // --- Normalisieren: Editor-Logik (gemessene Spitzen werden vorgegeben)
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

        // Auswahl: Videoclip bringt seinen Ton mit, gesperrte Spur nicht
        sel.set({1, 12, 31});
        CHECK_EQ(ed.selectedAudioClips(), (QVector<int>{11, 12}));

        // Unabhängig: jeder Clip auf -9 dBFS
        const int before = p.undoStack()->index();
        ed.normalizeAudio({{11, -3.0}, {12, -20.0}, {21, -200.0}, {31, -6.0}, {1, -1.0}}, -9.0, false);
        CHECK_EQ(p.undoStack()->index(), before + 1);
        CHECK(near(vol(11), -6.0, 1e-9) && near(vol(12), 11.0, 1e-9));
        CHECK(vol(21) == 0.0 && vol(31) == 0.0); // Stille / gesperrt bleiben
        CHECK(TimelineOps::findClip(p.timeline(), 1)->volumeDb == 0.0); // Video wird nicht angefasst
        p.undoStack()->undo();
        CHECK(vol(11) == 0.0 && vol(12) == 0.0);

        // Relativ: alle um denselben Betrag, der lauteste trifft das Ziel
        ed.normalizeAudio({{11, -3.0}, {12, -20.0}}, -9.0, true);
        CHECK(near(vol(11), -6.0, 1e-9) && near(vol(12), -6.0, 1e-9));
        // Begrenzung auf +12 dB
        ed.normalizeAudio({{12, -40.0}}, -1.0, false);
        CHECK_EQ(vol(12), kMaxVolumeDb);

        // Lautstärke-Keyframes: Kurve verschoben, höchster Punkt = neuer Wert
        ed.modifyClips({21}, "k", [](Clip& c) {
            Keys::setKey(c, AnimParam::Volume, 0, -30);
            Keys::setKey(c, AnimParam::Volume, 50, -10);
        });
        ed.normalizeAudio({{21, -2.0}}, -6.0, false);
        const Clip* k = TimelineOps::findClip(p.timeline(), 21);
        CHECK(near(Keys::valueAt(*k, AnimParam::Volume, 0), -24.0, 1e-9)
              && near(Keys::valueAt(*k, AnimParam::Volume, 50), -4.0, 1e-9));

        // Nichts zu tun: kein Undo-Schritt
        const int n = p.undoStack()->index();
        ed.normalizeAudio({{21, -200.0}, {31, -3.0}}, -9.0, false);
        ed.normalizeAudio({}, -9.0, true);
        CHECK_EQ(p.undoStack()->index(), n);
    }

    // --- Limiter-Einstellungen in der Projektdatei
    {
        ProjectData d;
        TimelineOps::ensureTracks(d.timeline, TrackKind::Audio, 1);
        d.timeline.masterLimiter = true;
        d.timeline.masterLimiterDb = -0.3;
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(d, "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
        CHECK(back.timeline.masterLimiter && near(back.timeline.masterLimiterDb, -0.3, 1e-9));
        // alte Datei: aus, -1 dBFS
        CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[],"timeline":{"video":[],"audio":[]}})",
                                    "/x/p.schneidi", &back, &err));
        CHECK(!back.timeline.masterLimiter && back.timeline.masterLimiterDb == kDefaultLimiterDb);
    }

    // --- Mit echten Medien (ffmpeg)
    if (!Check::haveFfmpeg()) return Check::skip("ffmpeg nicht gefunden");
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    // 4 s Stereo-Sinus: 0-2 s Amplitude 0.1 (-20 dBFS), 2-4 s 0.5 (-6.02 dBFS)
    const QString tone = Check::makeMedia(tmp.filePath("ton.wav"),
                                          {"-f", "lavfi", "-i",
                                           "aevalsrc=if(lt(t\\,2)\\,0.1\\,0.5)*sin(2*PI*440*t)|if(lt(t\\,2)\\,0.1\\,0.5)*sin(2*PI*440*t):s=48000:d=4",
                                           "-c:a", "pcm_f32le"});
    if (!CHECK(!tone.isEmpty())) return Check::result();
    Mlt::Factory::init();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt; // 25 fps

    const double loud = 20 * std::log10(0.5), quiet = -20.0;
    auto peak = AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 99));
    CHECK(peak && near(*peak, loud, 0.1));
    peak = AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 45)); // nur leiser Teil (In/Out zählt)
    CHECK(peak && near(*peak, quiet, 0.1));
    Clip fast = mk(1, tone, 0, 0, 49); // doppelte Geschwindigkeit: Clip-Frames 0-49 = ganze Datei
    fast.speed = 2.0;
    peak = AudioAnalysis::clipPeakDb(fmt, fast);
    // Tonhöhe halten (Standard) rechnet um und schwingt etwas über (~0,4 dB) – so klingt es auch in der Timeline
    if (!CHECK(peak && near(*peak, loud, 0.6))) std::printf("       2x: %.2f\n", peak ? *peak : -999.0);
    Clip frozen = mk(1, tone, 0, 0, 49);
    frozen.freeze = true;
    CHECK(!AudioAnalysis::clipPeakDb(fmt, frozen));
    CHECK(!AudioAnalysis::clipPeakDb(fmt, mk(1, tmp.filePath("fehlt.wav"), 0, 0, 49)));
    int calls = 0; // Abbrechen
    CHECK(!AudioAnalysis::clipPeakDb(fmt, mk(1, tone, 0, 0, 99), [&](double) { return ++calls < 2; }));

    // Render: Clip +12 dB auf den lauten Teil = etwa +6 dBFS -> Pegelmesser zeigt über 0 (nicht bei 0 abgeschnitten),
    // Limiter hält die Ceiling, im Export (ohne hooks) ebenso
    {
        auto prof = makeProfile(fmt);
        Mlt::Transition qt(*prof, "qtblend");
        Mlt::Filter lim(*prof, "avfilter.alimiter");
        if (!qt.is_valid()) {
            std::printf("Hinweis: MLT-Qt-Modul ohne Display nicht nutzbar, Render-Prüfungen übersprungen\n");
        } else if (!lim.is_valid()) {
            std::printf("Hinweis: avfilter.alimiter fehlt, Limiter-Prüfungen übersprungen\n");
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
            CHECK(renderPeak(tl, 10, 40) <= -0.9); // Export-Weg
            // Leiser Ton bleibt unverändert (Limiter hebt nicht an)
            tl.audio[0].clips[0].volumeDb = -10;
            CHECK(near(renderPeak(tl, 10, 40), loud - 10, 0.3));
        }
    }
    return Check::result();
}
