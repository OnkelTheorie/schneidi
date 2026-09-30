// Test mehrere Timelines pro Projekt + Compound Clips: Sequenzen anlegen/umbenennen/duplizieren/löschen, Undo über
// Timelines hinweg, Compound Clip erstellen/auflösen, Schleifenschutz, Projektdatei (auch alte Dateien), Bins,
// Rendern verschachtelter Timelines (Pixel: Inhalt, Transparenz, Ausschnitt, Export-Skalierung).
#include "check.h"

#include "core/Editor.h"
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
#include <QUndoStack>
#include <clocale>
#include <cstring>

namespace {

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

QVector<int> allIds(const Timeline& tl)
{
    QVector<int> ids;
    for (const auto* tracks : {&tl.video, &tl.audio})
        for (const Track& t : *tracks)
            for (const Clip& c : t.clips) ids << c.id;
    return ids;
}

const Clip* clipOn(const Timeline& tl, TrackKind kind, int track, int index)
{
    const auto& tracks = tl.tracks(kind);
    if (track >= tracks.size() || index >= tracks[track].clips.size()) return nullptr;
    return &tracks[track].clips[index];
}

void testSequences()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();
    CHECK_EQ(p.sequences().size(), 1);
    const int first = p.currentSequence();
    CHECK_EQ(p.sequenceName(first), QString("Timeline 1"));
    CHECK(!p.canRemoveSequence(first)); // letzte Timeline bleibt

    p.addMedia(media("/x/a.mp4", 100));
    ed.addMediaAt({"/x/a.mp4"}, 0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  V2:  A1: a[0-100|0-99]  A2:"));

    // Neue Timeline: leer, geöffnet, Standardname mit nächster Nummer; Undo öffnet wieder die erste
    const int second = p.addTimeline();
    CHECK_EQ(p.currentSequence(), second);
    CHECK_EQ(p.sequenceName(second), QString("Timeline 2"));
    CHECK(TimelineOps::endFrame(p.timeline()) == 0);
    ed.addMediaAt({"/x/a.mp4"}, 50);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[50-150|0-99]  V2:  A1: a[50-150|0-99]  A2:"));

    // Zur ersten wechseln (kein Undo-Schritt), dort ändern; Undo nimmt erst das zurück, dann in Timeline 2
    const int steps = undo->count();
    p.setCurrentSequence(first);
    CHECK_EQ(undo->count(), steps);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  V2:  A1: a[0-100|0-99]  A2:"));
    ed.selectAll();
    ed.deleteSelection();
    CHECK(TimelineOps::endFrame(p.timeline()) == 0);
    undo->undo();
    CHECK_EQ(p.currentSequence(), first);
    CHECK(TimelineOps::endFrame(p.timeline()) == 100);
    undo->undo(); // Clip in Timeline 2 -> öffnet Timeline 2
    CHECK_EQ(p.currentSequence(), second);
    CHECK(TimelineOps::endFrame(p.timeline()) == 0);
    undo->redo();
    CHECK(TimelineOps::endFrame(p.timeline()) == 150);
    CHECK(TimelineOps::endFrame(p.sequence(first)->timeline) == 100); // andere Timeline unberührt

    // Umbenennen, Duplizieren (eigene Clip-ids), Löschen
    p.renameSequence(second, "  Schnitt B ");
    CHECK_EQ(p.sequenceName(second), QString("Schnitt B"));
    const int dup = p.duplicateSequence(second);
    CHECK_EQ(p.sequenceName(dup), QString("Schnitt B Kopie"));
    const QVector<int> a = allIds(p.sequence(second)->timeline), b = allIds(p.sequence(dup)->timeline);
    CHECK(a.size() == 2 && b.size() == 2);
    for (int id : b) CHECK(!a.contains(id));
    CHECK(clipOn(p.sequence(dup)->timeline, TrackKind::Video, 0, 0)->linkId
          == clipOn(p.sequence(dup)->timeline, TrackKind::Audio, 0, 0)->linkId);
    CHECK(clipOn(p.sequence(dup)->timeline, TrackKind::Video, 0, 0)->linkId
          != clipOn(p.sequence(second)->timeline, TrackKind::Video, 0, 0)->linkId);
    CHECK(p.removeSequence(second)); // geöffnet -> nächste normale Timeline öffnet sich
    CHECK(!p.sequence(second));
    CHECK_EQ(p.currentSequence(), first);
    undo->undo();
    CHECK(p.sequence(second) && p.currentSequence() == second);
    undo->undo(); // Duplizieren
    CHECK(!p.sequence(dup));
    CHECK_EQ(p.sequences().size(), 2);

    // Bins: Timeline in Bin, Bin löschen -> zurück in den Eltern-Bin (ein Undo-Schritt je)
    const int bin = p.addBin(0, "Schnitte");
    p.moveSequencesToBin({second}, bin);
    CHECK_EQ(p.sequence(second)->bin, bin);
    p.removeBin(bin);
    CHECK_EQ(p.sequence(second)->bin, 0);
    undo->undo();
    CHECK_EQ(p.sequence(second)->bin, bin);

    // Framerate bleibt gesperrt, solange irgendeine Timeline Clips hat
    p.setCurrentSequence(first);
    ed.selectAll();
    ed.deleteSelection();
    CHECK(p.frameRateLocked()); // Timeline 2 hat noch Clips
}

void testCompound()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    QUndoStack* undo = p.undoStack();
    p.addMedia(media("/x/a.mp4", 100));
    p.addMedia(media("/x/b.mp4", 100));
    p.addMedia(media("/x/t.png", 50, true, false));
    ed.addMediaAt({"/x/a.mp4", "/x/b.mp4"}, 10); // a 10-110, b 110-210 auf V1/A1
    ed.addMediaAt({"/x/t.png"}, 60, 1);          // t 60-110 auf V2
    const QString before = Check::dump(p.timeline());
    const int parent = p.currentSequence();

    // b und t (mit Audio-Partner von b) zusammenfassen -> Spanne 60..210
    const Clip* b = clipOn(p.timeline(), TrackKind::Video, 0, 1);
    const Clip* t = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    sel.set({b->id, t->id});
    const int seq = ed.createCompoundClip("Mein Compound");
    if (!CHECK(seq > 0)) return;
    CHECK_EQ(p.currentSequence(), parent); // bleibt in der Timeline
    CHECK_EQ(p.sequenceName(seq), QString("Mein Compound"));
    CHECK(p.sequence(seq)->compound);
    // Spanne 60–210 überschneidet a auf V1/A1 -> Compound Clip auf die nächste freie Spur (a bleibt ganz)
    CHECK_EQ(Check::dump(p.timeline()),
             QString("V1: a[10-110|0-99]  V2: C%1[60-210|0-149]  A1: a[10-110|0-99]  A2: C%1[60-210|0-149]").arg(seq));
    const Timeline& inner = p.sequence(seq)->timeline;
    CHECK_EQ(Check::dump(inner), QString("V1: b[50-150|0-99]  V2: t[0-50|0-49]  A1: b[50-150|0-99]  A2:"));
    const Clip* cv = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    const Clip* ca = clipOn(p.timeline(), TrackKind::Audio, 1, 0);
    if (!CHECK(cv && ca && cv->isCompound() && ca->isCompound())) return;
    CHECK(cv->linkId && cv->linkId == ca->linkId);
    CHECK_EQ(p.clipName(*cv), QString("Mein Compound"));
    CHECK(sel.contains(cv->id) && sel.contains(ca->id));
    CHECK(p.sequenceUsed(seq) && !p.canRemoveSequence(seq));
    undo->undo();
    CHECK_EQ(Check::dump(p.timeline()), before);
    CHECK(!p.sequence(seq));
    undo->redo();
    CHECK(p.sequence(seq));

    // Trimmen begrenzt auf die Länge der Sequenz
    const int vid = clipOn(p.timeline(), TrackKind::Video, 1, 0)->id;
    CHECK_EQ(ed.clampTrim(vid, TimelineOps::Edge::End, 50), 0);
    ed.trimClip(vid, TimelineOps::Edge::Start, 20); // Inhalt ab Frame 20
    const Clip* cc = TimelineOps::findClip(p.timeline(), vid);
    CHECK(cc && cc->start == 80 && cc->in == 20);

    // Schleifenschutz: Compound in sich selbst geht nicht, Timeline in ihren Compound auch nicht
    CHECK(!p.canNest(seq, seq));
    CHECK(!p.canNest(parent, seq));
    CHECK(p.canNest(seq, parent));
    p.setCurrentSequence(seq);
    CHECK(!ed.addSequenceAt(seq, 0));
    CHECK(!ed.addSequenceAt(parent, 0));
    p.setCurrentSequence(parent);
    sel.set({vid});
    ed.copySelection();
    p.setCurrentSequence(seq);
    const QString innerBefore = Check::dump(p.timeline());
    ed.paste(0); // Compound Clip in seinen eigenen Inhalt -> nichts
    CHECK_EQ(Check::dump(p.timeline()), innerBefore);
    p.setCurrentSequence(parent);

    // Timeline aus dem Media Pool verschachteln
    const int other = p.addTimeline("Nest");
    CHECK(ed.addSequenceAt(parent, 5));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: C%1[5-215|0-209]  V2:  A1: C%1[5-215|0-209]  A2:").arg(parent));
    CHECK(!p.canNest(other, seq));    // other enthält (über parent) seq -> nicht in seq legen
    CHECK(!p.canNest(other, parent)); // other enthält parent -> Schleife
    CHECK(p.canNest(seq, other));
    p.setCurrentSequence(parent);

    // Auflösen: Inhalt im benutzten Ausschnitt (ab Frame 20) an dieselbe Stelle, Spuren ab der des Compound Clips
    sel.set({vid});
    CHECK(ed.decomposeCompoundClips({vid}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[10-110|0-99]  V2: b[110-210|0-99]  V3: t[80-110|20-49]  "
                                                "A1: a[10-110|0-99]  A2: b[110-210|0-99]"));
    const Clip* vb = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    const Clip* ab = clipOn(p.timeline(), TrackKind::Audio, 1, 0);
    CHECK(vb && ab && vb->linkId && vb->linkId == ab->linkId);
    CHECK(p.sequence(seq)); // bleibt im Media Pool
    undo->undo();
    CHECK(clipOn(p.timeline(), TrackKind::Video, 1, 0)->isCompound());
}

// Auflösen mit belegten Spuren darüber: Inhalt weicht auf freie Spuren aus, der Titel darüber bleibt
void testDecomposeKeepsClipsAbove()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media("/x/a.mp4", 100, true, false));
    p.addMedia(media("/x/b.mp4", 100, true, false));
    ed.addMediaAt({"/x/a.mp4"}, 0, 0);
    ed.addMediaAt({"/x/b.mp4"}, 0, 1);
    ed.selectAll();
    const int seq = ed.createCompoundClip(); // zwei Videospuren innen, Compound Clip auf V1
    CHECK(seq > 0);
    ed.addTitle(10, 1); // Titel auf V2 über dem Compound Clip
    CHECK_EQ(Check::dump(p.timeline()).count("T["), 1);
    const Clip* cc = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    if (!CHECK(cc && cc->isCompound())) return;
    CHECK(ed.decomposeCompoundClips({cc->id}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  V2: T[10-135|0-124]  V3: b[0-100|0-99]  A1:  A2:"));
}

void testProjectFile()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media("/x/a.mp4", 100));
    ed.addMediaAt({"/x/a.mp4"}, 0);
    ed.selectAll();
    const int seq = ed.createCompoundClip();
    CHECK_EQ(p.sequenceName(seq), QString("Compound Clip 1"));
    const int bin = p.addBin(0);
    const int second = p.addTimeline("Zweite", bin);
    CHECK(ed.addSequenceAt(seq, 30));
    RenderJob job;
    job.id = 1;
    job.path = "/x/out.mp4";
    job.sequence = second;
    p.setRenderQueue({job});

    const ProjectData d = p.data();
    const QByteArray json = ProjectFile::toJson(d, "/x/projekt.schneidi");
    ProjectData back;
    QString err;
    if (!CHECK(ProjectFile::fromJson(json, "/x/projekt.schneidi", &back, &err))) return;
    CHECK_EQ(back.sequences.size(), 3);
    CHECK_EQ(back.currentSequence, second);
    Project q;
    q.load(back);
    CHECK_EQ(q.currentSequence(), second);
    CHECK_EQ(q.sequences().size(), 3);
    for (const Sequence& s : p.sequences()) {
        const Sequence* o = q.sequence(s.id);
        if (!CHECK(o)) continue;
        CHECK_EQ(o->name, s.name);
        CHECK_EQ(o->compound, s.compound);
        CHECK_EQ(o->bin, s.bin);
        CHECK_EQ(Check::dump(o->timeline), Check::dump(s.timeline));
    }
    CHECK_EQ(q.renderQueue().value(0).sequence, second);
    // Speichern -> Laden -> Speichern identisch
    CHECK_EQ(ProjectFile::toJson(q.data(), "/x/projekt.schneidi"), json);
    // Ältere Versionen lesen nur "timeline": dort steht die geöffnete Timeline
    CHECK(json.contains("\"timeline\"") && json.contains("\"sequences\""));

    // Alte Datei ohne Sequenzen: eine Timeline „Timeline 1“
    ProjectData old;
    old.timeline = p.sequence(p.currentSequence())->timeline;
    old.timeline.video[0].clips.clear();
    old.timeline.audio[0].clips.clear();
    Project r;
    r.load(old);
    CHECK_EQ(r.sequences().size(), 1);
    CHECK_EQ(r.sequenceName(r.currentSequence()), QString("Timeline 1"));

    // Kaputte Datei: Compound Clip auf fehlende Sequenz fliegt raus, Selbstverweis auch
    ProjectData broken = back;
    broken.sequences.removeIf([&](const Sequence& s) { return s.id == seq; });
    Project b2;
    b2.load(broken);
    for (const Sequence& s : b2.sequences())
        for (const Clip* c : {clipOn(s.timeline, TrackKind::Video, 0, 0)})
            CHECK(!c || !c->isCompound());
}

// ---- Rendern ----

QImage render(const Timeline& tl, const ProjectFormat& fmt, int pos)
{
    auto prof = makeProfile(fmt);
    TimelineBuilder b(*prof);
    auto tr = b.build(tl);
    tr->seek(pos);
    std::unique_ptr<Mlt::Frame> f(tr->get_frame());
    mlt_image_format ifmt = mlt_image_rgba;
    int w = prof->width(), h = prof->height();
    const uint8_t* data = f->get_image(ifmt, w, h);
    if (!data) return {};
    QImage img(w, h, QImage::Format_RGBA8888);
    std::memcpy(img.bits(), data, size_t(w) * h * 4);
    return img.copy();
}

bool near(QRgb c, int r, int g, int b)
{
    return std::abs(qRed(c) - r) < 40 && std::abs(qGreen(c) - g) < 40 && std::abs(qBlue(c) - b) < 40;
}

int testRender(const QString& dir)
{
    // rot 0–2 s, dann grün 2–4 s (Ausschnitt prüfbar); blau als Hintergrund
    const QString rg = Check::makeMedia(dir + "/rg.mp4", {"-f", "lavfi", "-i", "color=red:size=320x180:rate=25:duration=2",
                                                          "-f", "lavfi", "-i", "color=lime:size=320x180:rate=25:duration=2",
                                                          "-filter_complex", "[0][1]concat=n=2:v=1:a=0", "-c:v", "libx264",
                                                          "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    const QString blue = Check::makeMedia(dir + "/blue.mp4", {"-f", "lavfi", "-i", "color=blue:size=320x180:rate=25:duration=4",
                                                              "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p"});
    if (!CHECK(!rg.isEmpty() && !blue.isEmpty())) return 1;
    Check::initMlt();
    std::setlocale(LC_NUMERIC, "C");
    const ProjectFormat fmt{320, 180, {25, 1}};
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
    p.addMedia(media(rg, 100, true, false));
    p.addMedia(media(blue, 100, true, false));
    ed.addMediaAt({blue}, 0, 0); // V1 blau
    ed.addMediaAt({rg}, 0, 1);   // V2 rot/grün
    // V2-Clip auf 0–60 kürzen (danach sieht man V1)
    const int rgId = clipOn(p.timeline(), TrackKind::Video, 1, 0)->id;
    ed.trimClip(rgId, TimelineOps::Edge::End, -40);
    sel.set({rgId});
    const int seq = ed.createCompoundClip();
    if (!CHECK(seq > 0)) return 1;
    const Clip* c = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    if (!CHECK(c && c->isCompound())) return 1;

    // Inhalt: rot bei 10; ohne verschachtelte Timelines (Timeline::nested) bleibt nur V1 blau
    QImage img = render(p.renderTimeline(), fmt, 10);
    CHECK(near(img.pixel(160, 90), 255, 0, 0));
    img = render(p.timeline(), fmt, 10);
    CHECK(near(img.pixel(160, 90), 0, 0, 255));

    // Ausschnitt: Compound Clip beginnt mit Inhalt-Frame 52 (Inhalt: 0–49 rot, 50–59 grün)
    const int cid = c->id;
    ed.trimClip(cid, TimelineOps::Edge::Start, 52);
    img = render(p.renderTimeline(), fmt, 55); // Inhalt Frame 55 -> grün
    CHECK(near(img.pixel(160, 90), 0, 255, 0));

    // Transparenz: Compound Clip mit herausgezoomtem Inhalt zeigt die Spur darunter
    p.setCurrentSequence(seq);
    const int innerId = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    ed.modifyClips({innerId}, "zoom", [](Clip& x) { x.transform.zoomX = x.transform.zoomY = 0.5; });
    p.setCurrentSequence(p.sequences().first().id);
    img = render(p.renderTimeline(), fmt, 55);
    CHECK(near(img.pixel(160, 90), 0, 255, 0)); // Mitte: grün
    CHECK(near(img.pixel(10, 10), 0, 0, 255));  // Rand: V1 blau scheint durch

    // Export in anderer Größe: Inhalt der Compound Clips mitskaliert (Position in Pixeln)
    p.setCurrentSequence(seq);
    ed.modifyClips({innerId}, "pos", [](Clip& x) { x.transform.posX = 80; }); // halbe Breite nach rechts (320er Bild)
    p.setCurrentSequence(p.sequences().first().id);
    Timeline big = p.renderTimeline();
    scaleTimeline(big, {320, 180}, {640, 360});
    img = render(big, ProjectFormat{640, 360, {25, 1}}, 55);
    CHECK(near(img.pixel(320 + 160, 180), 0, 255, 0)); // um 160 (= 80 · 2) nach rechts
    CHECK(near(img.pixel(200, 180), 0, 0, 255));

    // Kein Absturz bei Schleife (kaputte Daten): Compound verweist auf sich selbst
    Timeline loop = p.renderTimeline();
    auto nested = std::make_shared<NestedTimelines>(*loop.nested);
    (*nested)[seq].video[0].clips[0].kind = ClipKind::Compound;
    (*nested)[seq].video[0].clips[0].sequenceId = seq;
    loop.nested = nested;
    img = render(loop, fmt, 55);
    CHECK(!img.isNull());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("sequences");
    testSequences();
    testCompound();
    testDecomposeKeepsClipsAbove();
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
