// Test Speed Ramps (DaVinci Retime Controls): RetimeMap (hart, weich, rückwärts, Umkehrung), Editor-Bearbeitungen mit
// Ripple/Partnern/Keyframes/Undo, Projektdatei, Ton-Stufen und gerenderte Frames (welches Quellbild wird gezeigt).
#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Retime.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/RampProducer.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
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
    // harter Speed-Punkt: bis Quelle 50 normal, danach doppelt so schnell
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
    // weicher Übergang über 10 Frames: außerhalb gleich, innen stetig und umkehrbar
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
    // zu langer Übergang wird auf die Nachbarabschnitte begrenzt (kein Überlappen)
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
    // rückwärts: Datei-Frame gespiegelt
    c.ramp = {SpeedPoint{50, 2.0, 0}};
    c.reverse = true;
    {
        const RetimeMap m(c, 100);
        CHECK(close(m.fileFrameAt(0), 99));
        CHECK(close(m.fileFrameAt(60), 99 - 70));
        CHECK(close(m.materialAtFile(29), 60));
    }
    // Aufräumen: sortiert, außerhalb/zu dicht verworfen, Tempo begrenzt
    QVector<SpeedPoint> r{{80, 3, 0}, {10, 0, -5}, {10.5, 2, 0}, {150, 2, 0}, {-3, 2, 0}};
    Retime::normalize(r, 100);
    CHECK_EQ(r.size(), 2);
    CHECK(close(r[0].source, 10) && close(r[0].speed, 1.0) && r[0].smooth == 0);
    CHECK(close(r[1].source, 80) && close(r[1].speed, 3));

    // Ton-Stufen: lückenlos, harte Abschnitte je eine Stufe
    Clip a;
    a.ramp = {SpeedPoint{50, 2.0, 0}};
    auto steps = RampProducer::audioSteps(a, 100);
    CHECK_EQ(steps.size(), 2);
    if (steps.size() == 2) {
        CHECK(steps[0].m0 == 0 && steps[0].m1 == 50 && close(steps[0].speed, 1.0));
        CHECK(steps[1].m0 == 50 && steps[1].m1 == 75 && close(steps[1].speed, 2.0) && close(steps[1].s0, 50));
    }
    a.ramp[0].smooth = 16;
    steps = RampProducer::audioSteps(a, 100);
    CHECK(steps.size() > 3);
    int at = 0;
    for (const auto& s : steps) {
        CHECK_EQ(s.m0, at);
        at = s.m1;
    }
    CHECK_EQ(at, 75);
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
    // Keyframe an Quellstelle 80
    ed.modifyClips({v}, "key", [](Clip& c) { c.keys[AnimParam::Opacity] = {Keyframe{80, 0.5}}; });

    // Punkt hinzufügen: Länge bleibt, Partner (Ton) bekommt denselben Punkt
    const int steps = undo->count();
    ed.addSpeedPoint(v, 50);
    CHECK_EQ(undo->count(), steps + 1);
    const Clip* cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    const Clip* ca = clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    CHECK(cv->ramp.size() == 1 && close(cv->ramp[0].source, 50) && close(cv->ramp[0].speed, 1.0));
    CHECK(ca->ramp == cv->ramp);
    CHECK_EQ(cv->length(), 100);
    ed.addSpeedPoint(v, 50); // gleicher Punkt: nichts
    CHECK_EQ(undo->count(), steps + 1);

    // Tempo des zweiten Abschnitts verdoppeln: Clip 75 lang, b rückt nach, Keyframe bleibt an seiner Quellstelle
    ed.setSegmentSpeed(v, 1, 2.0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-75|0-74] b[75-175|0-99]  V2:  A1: a[0-75|0-74] b[75-175|0-99]  A2:"));
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(cv->keys.value(AnimParam::Opacity).value(0).frame, 65); // 50 + 30 / 2

    // Punkt 10 Frames nach rechts: davor läuft Tempo 1 weiter -> Quelle 60, Länge 60 + 40 / 2 = 80
    ed.moveSpeedPoint(v, 0, 10);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(close(cv->ramp[0].source, 60));
    CHECK_EQ(cv->length(), 80);
    CHECK_EQ(clipOn(p.timeline(), TrackKind::Video, 0, 1)->start, 80);

    // Weicher Übergang ändert die Länge nicht (mittig um den Punkt)
    ed.setSpeedPointSmooth(v, 0, 12);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(cv->ramp[0].smooth, 12);
    CHECK_EQ(cv->length(), 80);

    // Trimmen: Material reicht bis zum Dateiende (Länge der Rampe)
    CHECK_EQ(cv->retimedLength(100), 80);

    // Entfernen: wieder normal lang
    ed.removeSpeedPoint(v, 0);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(cv->ramp.isEmpty());
    CHECK_EQ(cv->length(), 100);

    // Undo-Kette zurück bis vor den Punkt
    while (undo->index() > steps) undo->undo();
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99] b[100-200|0-99]  V2:  A1: a[0-100|0-99] b[100-200|0-99]  A2:"));
    CHECK(clipOn(p.timeline(), TrackKind::Video, 0, 0)->ramp.isEmpty());
    undo->redo();
    undo->redo();
    CHECK_EQ(clipOn(p.timeline(), TrackKind::Video, 0, 0)->length(), 75);

    // Konstantes Tempo (Change Clip Speed) ersetzt die Rampe; Länge aus der Quell-Spanne
    ed.setClipSpeed({v}, Editor::Retime{1.0, false, false, true}, true);
    cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK(cv->ramp.isEmpty());
    CHECK_EQ(cv->length(), 100);

    // Standbild/Titel: keine Rampe
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

// ---- Rendern: Helligkeit kodiert die Quell-Frame-Nummer ----

int frameAt(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof->width(), h = prof->height();
    const uint8_t* data = f->get_image(ifmt, w, h);
    if (!data) return -1;
    const int g = data[(size_t(h / 2) * w + w / 2) * 4 + 1];
    return int(std::lround(g / 2.0)); // Quelle: Grau = 2 · Frame
}

int testRender(const QString& dir)
{
    // Frame n hat den Grauwert 2n (volle Range, verlustfrei)
    const QString file = Check::makeMedia(dir + "/ramp.mkv",
                                          {"-f", "lavfi", "-i", "nullsrc=size=64x64:rate=25:duration=4,format=gray,geq=lum='2*N'",
                                           "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
                                           "-c:v", "ffv1", "-pix_fmt", "rgb24", "-c:a", "pcm_s16le", "-shortest"});
    if (!CHECK(!file.isEmpty())) return 1;
    Mlt::Factory::init();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt{64, 64, {25, 1}};
    {
        auto prof = makeProfile(fmt);
        Mlt::Filter qt(*prof, "qtext");
        if (!qt.is_valid()) return Check::skip("MLT-Qt-Modul nicht nutzbar (kein Display? z. B. xvfb-run -a ctest …)");
    }

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
    // am Anfang getrimmt: Material zählt weiter ab Quelle
    const int id = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    ed.trimClip(id, TimelineOps::Edge::Start, 25);
    const Clip* c = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    CHECK_EQ(c->in, 25);
    CHECK_EQ(frameAt(p.renderTimeline(), fmt, c->start), 30);
    // rückwärts mit Rampe
    Timeline tl = p.renderTimeline();
    Clip& r = tl.video[0].clips[0];
    r.in = 0;
    r.out = 59;
    r.start = 0;
    r.reverse = true;
    CHECK_EQ(frameAt(tl, fmt, 0), 99);
    CHECK_EQ(frameAt(tl, fmt, 30), 99 - 40);

    // Ton: Playlist so lang wie das Material
    Clip a = *clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    auto prof = makeProfile(fmt);
    auto audio = RampProducer::audio(*prof, file, a);
    if (CHECK(audio && audio->is_valid())) {
        CHECK_EQ(audio->get_length(), a.retimedLength(100));
        // Ton im schnellen Abschnitt nicht stumm (Sinus aus der Datei)
        // durchgehend abspielen: 100 %-Abschnitt ohne Lücke, im 200 %-Abschnitt nur kurz Einschwingen (Tonhöhe halten)
        audio->seek(0);
        int silent = 0;
        for (int pos = 0; pos < audio->get_length(); ++pos) {
            std::unique_ptr<Mlt::Frame> f(audio->get_frame());
            mlt_audio_format afmt = mlt_audio_float;
            int freq = 48000, ch = 2, samples = 1920;
            const auto* s = static_cast<const float*>(f->get_audio(afmt, freq, ch, samples));
            float peak = 0;
            for (int i = 0; s && i < samples * ch; ++i) peak = std::max(peak, std::abs(s[i]));
            if (peak < 0.04f) ++silent; // ffmpeg-Sinus: ~0,09
        }
        CHECK(silent <= 2);
    }
    // weicher Übergang: Stufen ohne Tonhöhenkorrektur -> keine Lücken
    ed.setSpeedPointSmooth(v, 0, 12);
    a = *clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    audio = RampProducer::audio(*prof, file, a);
    if (CHECK(audio && audio->is_valid())) {
        audio->seek(0);
        int silent = 0;
        for (int pos = 0; pos < audio->get_length(); ++pos) {
            std::unique_ptr<Mlt::Frame> f(audio->get_frame());
            mlt_audio_format afmt = mlt_audio_float;
            int freq = 48000, ch = 2, samples = 1920;
            const auto* s = static_cast<const float*>(f->get_audio(afmt, freq, ch, samples));
            float peak = 0;
            for (int i = 0; s && i < samples * ch; ++i) peak = std::max(peak, std::abs(s[i]));
            if (peak < 0.04f) ++silent;
        }
        CHECK(silent <= 2);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("retime");
    testMap();
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
    return Check::result();
}
