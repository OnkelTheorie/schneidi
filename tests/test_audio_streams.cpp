// Test mehrere Ton-Streams pro Datei (OBS: Desktop-Ton, Mikro … getrennt): Einfügen (ein Audio-Clip pro Stream,
// verknüpft, eigene Spuren, gesperrte Spuren, ein Undo-Schritt), Quell-Viewer-Bearbeitungen, Projektdatei (auch alte
// Dateien ohne Streams) und gerenderter Ton (welcher Stream klingt, auch mit Geschwindigkeit und Speed Ramp).
#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/RampProducer.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QApplication>
#include <QTemporaryDir>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

MediaInfo obsMedia(const QString& path, int length, int streams)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = true;
    m.hasAudio = streams > 0;
    for (int k = 0; k < streams; ++k) m.audioStreams << AudioStreamInfo{2, k == 1 ? "Mikro" : QString()};
    return m;
}

// Audio-Clips als "A1:s0 A2:s1 …" (Spur:Stream), mit gleicher linkId wie der Videoclip
QString streams(const Timeline& tl, int* linked = nullptr)
{
    QString s;
    int link = tl.video.isEmpty() || tl.video[0].clips.isEmpty() ? -1 : tl.video[0].clips[0].linkId;
    int same = 0;
    for (int i = 0; i < tl.audio.size(); ++i)
        for (const Clip& c : tl.audio[i].clips) {
            s += QString("A%1:s%2 ").arg(i + 1).arg(c.audioStream);
            if (c.linkId != 0 && c.linkId == link) ++same;
        }
    if (linked) *linked = same;
    return s.trimmed();
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();
    p.addMedia(obsMedia("/x/obs.mkv", 100, 3));
    CHECK_EQ(p.mediaInfo("/x/obs.mkv")->audioStreamCount(), 3);
    CHECK_EQ(p.mediaInfo("/x/obs.mkv")->audioStreamName(0), T("Spur %1").arg(1));
    CHECK_EQ(p.mediaInfo("/x/obs.mkv")->audioStreamName(1), QString("Mikro"));

    // Ablegen: drei Audio-Clips auf A1–A3 (fehlende Spuren entstehen), alle mit dem Bild verknüpft, ein Undo-Schritt
    const int steps = undo->count();
    ed.addMediaAt({"/x/obs.mkv"}, 0);
    CHECK_EQ(undo->count(), steps + 1);
    int linked = 0;
    CHECK_EQ(streams(p.timeline(), &linked), QString("A1:s0 A2:s1 A3:s2"));
    CHECK_EQ(linked, 3);
    CHECK_EQ(TimelineOps::linkedGroup(p.timeline(), p.timeline().video[0].clips[0].id).size(), 4);
    undo->undo();
    CHECK_EQ(streams(p.timeline()), QString());

    // Ablegen auf Spur 2: Streams auf A2–A4
    ed.addMediaAt({"/x/obs.mkv"}, 0, 1);
    CHECK_EQ(streams(p.timeline()), QString("A2:s0 A3:s1 A4:s2"));
    undo->undo();

    // gesperrte Spur A2: Stream 1 entfällt, die anderen bleiben verknüpft
    ed.addMediaAt({"/x/obs.mkv"}, 200); // legt A1–A3 an
    undo->undo();
    Timeline tl = p.timeline();
    TimelineOps::ensureTracks(tl, TrackKind::Audio, 3);
    tl.audio[1].locked = true;
    p.edit("sperren", [&](Timeline& t) { t = tl; });
    ed.addMediaAt({"/x/obs.mkv"}, 0);
    CHECK_EQ(streams(p.timeline(), &linked), QString("A1:s0 A3:s2"));
    CHECK_EQ(linked, 2);
    undo->undo();
    undo->undo();

    // Quell-Viewer: Überschreiben legt ebenfalls alle Streams ab Zielspur ab
    ed.sourceEdit(Editor::SourceEditMode::Overwrite, "/x/obs.mkv", 0, 0);
    CHECK_EQ(streams(p.timeline(), &linked), QString("A1:s0 A2:s1 A3:s2"));
    CHECK_EQ(linked, 3);
    // Einfügen: alle Spuren rücken mit, neue Clips wieder auf A1–A3
    ed.sourceEdit(Editor::SourceEditMode::Insert, "/x/obs.mkv", 0, 0);
    CHECK_EQ(Check::dump(p.timeline()).count("obs[0-100|0-99]"), 4);
    CHECK_EQ(Check::dump(p.timeline()).count("obs[100-200|0-99]"), 4);
    // Ersetzen am Playhead: gleiche Spuren wie der ersetzte Clip samt Partnern
    ed.sourceEdit(Editor::SourceEditMode::Replace, "/x/obs.mkv", 10, 150);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(p.timeline().audio[i].clips.size(), 2);
        for (const Clip& c : p.timeline().audio[i].clips) CHECK_EQ(c.audioStream, i);
    }

    // Teilen behält den Stream
    const int a2 = p.timeline().audio[1].clips[0].id;
    sel.set({a2});
    ed.splitAtPlayhead(50);
    for (const Clip& c : p.timeline().audio[1].clips) CHECK_EQ(c.audioStream, 1);

    // Datei mit einem Stream: wie bisher ein Audio-Clip; ältere Einträge ohne Liste zählen als 1 Stream
    MediaInfo old = obsMedia("/x/alt.mp4", 50, 0);
    old.hasAudio = true;
    CHECK_EQ(old.audioStreamCount(), 1);
}

// Überschreiben mitten in einen verknüpften Clip: die Reststücke rechts bleiben untereinander verknüpft
// (eigene Gruppe), die linken behalten die alte Verknüpfung
void testOverwriteKeepsLinks()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(obsMedia("/x/lang.mkv", 200, 2));
    p.addMedia(obsMedia("/x/kurz.mkv", 20, 2));
    ed.addMediaAt({"/x/lang.mkv"}, 0);
    ed.addMediaAt({"/x/kurz.mkv"}, 50);
    const Timeline& tl = p.timeline();
    auto piece = [&](TrackKind k, int track, int start) -> const Clip* {
        for (const Clip& c : tl.tracks(k)[track].clips)
            if (c.start == start) return &c;
        return nullptr;
    };
    const Clip* vl = piece(TrackKind::Video, 0, 0);
    const Clip* vr = piece(TrackKind::Video, 0, 70);
    if (!CHECK(vl && vr)) return;
    CHECK(vr->linkId > 0 && vr->linkId != vl->linkId);
    for (int a = 0; a < 2; ++a) {
        const Clip* l = piece(TrackKind::Audio, a, 0);
        const Clip* r = piece(TrackKind::Audio, a, 70);
        if (CHECK(l && r)) {
            CHECK_EQ(l->linkId, vl->linkId);
            CHECK_EQ(r->linkId, vr->linkId);
        }
    }
    CHECK_EQ(TimelineOps::linkedGroup(tl, vr->id).size(), 3);
}

void testProjectFile()
{
    QTemporaryDir tmp;
    const QString path = tmp.filePath("p.schneidi");
    ProjectData d;
    d.media << obsMedia("/x/obs.mkv", 100, 3);
    Clip v;
    v.id = 1;
    v.mediaPath = "/x/obs.mkv";
    v.out = 99;
    v.linkId = 1;
    d.timeline.video.resize(1);
    d.timeline.video[0].clips << v;
    d.timeline.audio.resize(3);
    for (int k = 0; k < 3; ++k) {
        Clip a = v;
        a.id = 2 + k;
        a.audioStream = k;
        d.timeline.audio[k].clips << a;
    }
    d.lastClipId = 4;
    d.lastLinkId = 1;
    QString err;
    CHECK(ProjectFile::save(d, path, &err));
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    CHECK_EQ(back.media.size(), 1);
    if (!back.media.isEmpty()) CHECK(back.media[0].audioStreams == d.media[0].audioStreams);
    for (int k = 0; k < 3 && k < back.timeline.audio.size(); ++k) CHECK_EQ(back.timeline.audio[k].clips[0].audioStream, k);
    CHECK(ProjectFile::toJson(back, path) == ProjectFile::toJson(d, path));

    // ältere Datei ohne "audioStreams"/"audioStream": ein Stream, Clip auf Stream 0
    ProjectData old;
    CHECK(ProjectFile::fromJson(R"({"app":"schneidi","version":1,"media":[
        {"path":"/x/a.mp4","name":"a.mp4","length":50,"hasVideo":true,"hasAudio":true}],
        "timeline":{"video":[],"audio":[{"clips":[{"id":1,"media":0,"start":0,"in":0,"out":49}]}]}})",
                                path, &old, &err));
    if (CHECK_EQ(old.media.size(), 1)) CHECK_EQ(old.media[0].audioStreamCount(), 1);
    if (CHECK(!old.timeline.audio.isEmpty() && !old.timeline.audio[0].clips.isEmpty()))
        CHECK_EQ(old.timeline.audio[0].clips[0].audioStream, 0);
}

// Nulldurchgänge im Ton ab Frame `at` (über mehrere Frames, nach dem Einschwingen) -> Frequenz in Hz
double frequencyAt(Mlt::Producer& p, int at, double fps)
{
    p.seek(at);
    int zc = 0, total = 0;
    float prev = 0;
    for (int n = 0; n < 8; ++n) {
        std::unique_ptr<Mlt::Frame> f(p.get_frame());
        if (!f) return 0;
        mlt_audio_format fmt = mlt_audio_float;
        int freq = 48000, ch = 1, samples = mlt_audio_calculate_frame_samples(float(fps), freq, at + n);
        const auto* s = static_cast<const float*>(f->get_audio(fmt, freq, ch, samples));
        if (!s || n < 3) continue; // Einschwingen nach dem Springen
        for (int i = 0; i < samples; ++i) {
            if ((prev < 0) != (s[i] < 0)) ++zc;
            prev = s[i];
        }
        total += samples;
    }
    return total ? zc / 2.0 * 48000.0 / total : 0;
}

bool near(double hz, double want) { return std::abs(hz - want) < want * 0.1; }

int testRender(const QString& dir)
{
    // Drei Ton-Streams mit 440 / 880 / 220 Hz (Stream 2 als Mikro benannt, Stream 3 mono)
    const QString file = Check::makeMedia(
        dir + "/obs.mkv",
        {"-f", "lavfi", "-i", "testsrc2=size=64x64:rate=25:duration=4", "-f", "lavfi", "-i", "sine=frequency=440:duration=4",
         "-f", "lavfi", "-i", "sine=frequency=880:duration=4", "-f", "lavfi", "-i", "sine=frequency=220:duration=4",
         "-map", "0", "-map", "1", "-map", "2", "-map", "3", "-c:v", "ffv1", "-c:a", "pcm_s16le", "-ac:a:2", "1",
         "-metadata:s:a:1", "title=Mikro", "-shortest"});
    if (!CHECK(!file.isEmpty())) return 1;
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt{64, 64, {25, 1}};
    auto prof = makeProfile(fmt);

    // Streams erkennen (wie Engine::probe)
    {
        Mlt::Producer probe(*prof, file.toUtf8().constData());
        const QVector<AudioStreamInfo> s = audioStreamsOf(probe);
        if (CHECK_EQ(s.size(), 3)) {
            CHECK_EQ(s[0].title, QString());
            CHECK_EQ(s[1].title, QString("Mikro"));
            CHECK_EQ(s[2].channels, 1);
        }
    }

    const double want[] = {440, 880, 220};
    TimelineBuilder builder(*prof);
    for (int k = 0; k < 3; ++k) {
        Clip c;
        c.id = 1;
        c.mediaPath = file;
        c.out = 99;
        c.audioStream = k;
        // normales Tempo, halbes Tempo mit Tonhöhe (timewarp) und Speed Ramp (eigener Producer)
        if (Mlt::Producer* p = builder.clipAudioSource(c); CHECK(p))
            CHECK(near(frequencyAt(*p, 20, fmt.rate.fps()), want[k]));
        Clip slow = c;
        slow.speed = 0.5;
        slow.keepPitch = false;
        slow.out = 199;
        if (Mlt::Producer* p = builder.clipAudioSource(slow); CHECK(p))
            CHECK(near(frequencyAt(*p, 20, fmt.rate.fps()), want[k] / 2));
        Clip ramp = c;
        ramp.ramp = {SpeedPoint{50, 2.0, 0}};
        auto rp = RampProducer::audio(*prof, file, ramp);
        if (CHECK(rp && rp->is_valid())) CHECK(near(frequencyAt(*rp, 20, fmt.rate.fps()), want[k]));
    }

    // ganze Timeline: nur der Clip mit Stream 2 (880 Hz) klingt; zwei Streams derselben Datei gleichzeitig
    Timeline tl;
    tl.masterLimiter = false;
    tl.audio.resize(2);
    Clip a;
    a.id = 1;
    a.mediaPath = file;
    a.out = 99;
    a.audioStream = 1;
    tl.audio[0].clips << a;
    auto tractor = builder.build(tl);
    if (CHECK(tractor)) CHECK(near(frequencyAt(*tractor, 20, fmt.rate.fps()), 880));
    // Stream fehlt (Datei ersetzt): still statt falscher Ton
    tl.audio[0].clips[0].audioStream = 7;
    tractor = builder.build(tl);
    if (CHECK(tractor)) CHECK_EQ(frequencyAt(*tractor, 20, fmt.rate.fps()), 0.0);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("audio-streams");
    testEditor();
    testOverwriteKeepsLinks();
    testProjectFile();
    if (!Check::haveFfmpeg()) {
        Check::result();
        return Check::skip("ffmpeg nicht gefunden (Render-Teil)");
    }
    QTemporaryDir tmp;
    if (CHECK(tmp.isValid())) testRender(tmp.path());
    return Check::result();
}
