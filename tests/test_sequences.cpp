// Test several timelines per project + compound clips: create/rename/duplicate/delete sequences, undo across
// timelines, create/dissolve compound clip, loop protection, project file (also old files), bins,
// rendering of nested timelines (pixels: content, transparency, section, export scaling).
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
    CHECK(!p.canRemoveSequence(first)); // last timeline stays

    p.addMedia(media("/x/a.mp4", 100));
    ed.addMediaAt({"/x/a.mp4"}, 0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  V2:  A1: a[0-100|0-99]  A2:"));

    // New timeline: empty, opened, default name with next number; undo opens the first again
    const int second = p.addTimeline();
    CHECK_EQ(p.currentSequence(), second);
    CHECK_EQ(p.sequenceName(second), QString("Timeline 2"));
    CHECK(TimelineOps::endFrame(p.timeline()) == 0);
    ed.addMediaAt({"/x/a.mp4"}, 50);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[50-150|0-99]  V2:  A1: a[50-150|0-99]  A2:"));

    // Switch to the first (no undo step), change there; undo reverts that first, then in timeline 2
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
    undo->undo(); // clip in timeline 2 -> opens timeline 2
    CHECK_EQ(p.currentSequence(), second);
    CHECK(TimelineOps::endFrame(p.timeline()) == 0);
    undo->redo();
    CHECK(TimelineOps::endFrame(p.timeline()) == 150);
    CHECK(TimelineOps::endFrame(p.sequence(first)->timeline) == 100); // other timeline untouched

    // Rename, duplicate (own clip ids), delete
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
    CHECK(p.removeSequence(second)); // opened -> next normal timeline opens
    CHECK(!p.sequence(second));
    CHECK_EQ(p.currentSequence(), first);
    undo->undo();
    CHECK(p.sequence(second) && p.currentSequence() == second);
    undo->undo(); // duplicate
    CHECK(!p.sequence(dup));
    CHECK_EQ(p.sequences().size(), 2);

    // Bins: timeline into bin, delete bin -> back into the parent bin (one undo step each)
    const int bin = p.addBin(0, "Schnitte");
    p.moveSequencesToBin({second}, bin);
    CHECK_EQ(p.sequence(second)->bin, bin);
    p.removeBin(bin);
    CHECK_EQ(p.sequence(second)->bin, 0);
    undo->undo();
    CHECK_EQ(p.sequence(second)->bin, bin);

    // Frame rate stays locked as long as any timeline has clips
    p.setCurrentSequence(first);
    ed.selectAll();
    ed.deleteSelection();
    CHECK(p.frameRateLocked()); // timeline 2 still has clips
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
    ed.addMediaAt({"/x/a.mp4", "/x/b.mp4"}, 10); // a 10-110, b 110-210 on V1/A1
    ed.addMediaAt({"/x/t.png"}, 60, 1);          // t 60-110 on V2
    const QString before = Check::dump(p.timeline());
    const int parent = p.currentSequence();

    // Combine b and t (with audio partner of b) -> span 60..210
    const Clip* b = clipOn(p.timeline(), TrackKind::Video, 0, 1);
    const Clip* t = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    sel.set({b->id, t->id});
    const int seq = ed.createCompoundClip("Mein Compound");
    if (!CHECK(seq > 0)) return;
    CHECK_EQ(p.currentSequence(), parent); // stays in the timeline
    CHECK_EQ(p.sequenceName(seq), QString("Mein Compound"));
    CHECK(p.sequence(seq)->compound);
    // Span 60-210 overlaps a on V1/A1 -> compound clip onto the next free track (a stays whole)
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

    // Trimming limited to the length of the sequence
    const int vid = clipOn(p.timeline(), TrackKind::Video, 1, 0)->id;
    CHECK_EQ(ed.clampTrim(vid, TimelineOps::Edge::End, 50), 0);
    ed.trimClip(vid, TimelineOps::Edge::Start, 20); // content from frame 20
    const Clip* cc = TimelineOps::findClip(p.timeline(), vid);
    CHECK(cc && cc->start == 80 && cc->in == 20);

    // Loop protection: compound into itself is not possible, timeline into its compound neither
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
    ed.paste(0); // compound clip into its own content -> nothing
    CHECK_EQ(Check::dump(p.timeline()), innerBefore);
    p.setCurrentSequence(parent);

    // Nest timeline from the Media Pool
    const int other = p.addTimeline("Nest");
    CHECK(ed.addSequenceAt(parent, 5));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: C%1[5-215|0-209]  V2:  A1: C%1[5-215|0-209]  A2:").arg(parent));
    CHECK(!p.canNest(other, seq));    // other contains (via parent) seq -> do not put into seq
    CHECK(!p.canNest(other, parent)); // other contains parent -> loop
    CHECK(p.canNest(seq, other));
    p.setCurrentSequence(parent);

    // Dissolve: content in the used section (from frame 20) at the same position, tracks from that of the compound clip
    sel.set({vid});
    CHECK(ed.decomposeCompoundClips({vid}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[10-110|0-99]  V2: b[110-210|0-99]  V3: t[80-110|20-49]  "
                                                "A1: a[10-110|0-99]  A2: b[110-210|0-99]"));
    const Clip* vb = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    const Clip* ab = clipOn(p.timeline(), TrackKind::Audio, 1, 0);
    CHECK(vb && ab && vb->linkId && vb->linkId == ab->linkId);
    CHECK(p.sequence(seq)); // stays in the Media Pool
    undo->undo();
    CHECK(clipOn(p.timeline(), TrackKind::Video, 1, 0)->isCompound());

    // Linked selection off: only the video part is dissolved, the audio part stays -> no duplicate audio
    ed.setLinkedSelection(false);
    CHECK(ed.decomposeCompoundClips({vid}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[10-110|0-99]  V2: b[110-210|0-99]  V3: t[80-110|20-49]  "
                                                "A1: a[10-110|0-99]  A2: C%1[80-210|20-149]").arg(seq));
    ed.setLinkedSelection(true);
}

// Dissolve with occupied tracks above: content moves to free tracks, the title above stays
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
    const int seq = ed.createCompoundClip(); // two video tracks inside, compound clip on V1
    CHECK(seq > 0);
    ed.addTitle(10, 1); // title on V2 above the compound clip
    CHECK_EQ(Check::dump(p.timeline()).count("T["), 1);
    const Clip* cc = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    if (!CHECK(cc && cc->isCompound())) return;
    CHECK(ed.decomposeCompoundClips({cc->id}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  V2: T[10-135|0-124]  V3: b[0-100|0-99]  A1:  A2:"));
}

// Dissolving keeps what was set on the compound clip: transform, effects, volume/pan, keyframes, fades at its edges
// (the content's own fades and dissolves inside stay)
void testDecomposeKeepsSettings()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media("/x/a.mp4", 100));
    p.addMedia(media("/x/b.mp4", 100));
    ed.addMediaAt({"/x/a.mp4", "/x/b.mp4"}, 0); // a 0-100, b 100-200 on V1/A1
    ed.selectAll();
    const int parent = p.currentSequence();
    const int seq = ed.createCompoundClip();
    if (!CHECK(seq > 0)) return;
    // Inside: a fade-out of a at its end (not an edge of the compound clip) stays
    p.setCurrentSequence(seq);
    const int innerA = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    p.edit("inner", [&](Timeline& tl) { TimelineOps::findClip(tl, innerA)->fadeOut = 5; });
    p.setCurrentSequence(parent);
    const Clip* cv = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    const Clip* ca = clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    if (!CHECK(cv && ca && cv->isCompound())) return;
    const int vid = cv->id, aid = ca->id;
    ed.trimClip(vid, TimelineOps::Edge::Start, 20); // compound shows sequence frames 20..199 from timeline frame 20
    p.edit("outer", [&](Timeline& tl) {
        Clip* v = TimelineOps::findClip(tl, vid);
        v->transform.opacity = 0.5;
        v->transform.zoomX = v->transform.zoomY = 1.5;
        v->effects << EffectInstance{"color", {}, true};
        v->fadeIn = 10;
        v->fadeOut = 15;
        Keyframe k1, k2;
        k1.frame = 20;
        k1.value = 1.0;
        k2.frame = 150;
        k2.value = 2.0;
        v->keys[AnimParam::ZoomX] = {k1, k2};
        Clip* a = TimelineOps::findClip(tl, aid);
        a->volumeDb = -6;
        a->pan = 30;
    });
    CHECK(ed.decomposeCompoundClips({vid}));
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[20-100|20-99]  b[100-200|0-99]  A1: a[20-100|20-99]  b[100-200|0-99]"));
    const Clip* va = clipOn(p.timeline(), TrackKind::Video, 0, 0);
    const Clip* vb = clipOn(p.timeline(), TrackKind::Video, 0, 1);
    const Clip* aa = clipOn(p.timeline(), TrackKind::Audio, 0, 0);
    const Clip* ab = clipOn(p.timeline(), TrackKind::Audio, 0, 1);
    if (!CHECK(va && vb && aa && ab)) return;
    for (const Clip* c : {va, vb}) {
        CHECK_EQ(c->transform.opacity, 0.5);
        CHECK_EQ(c->transform.zoomX, 1.5);
        CHECK(c->effects.size() == 1 && c->effects[0].effectId == "color");
    }
    CHECK_EQ(va->fadeIn, 10); // the compound clip's edges
    CHECK_EQ(vb->fadeOut, 15);
    CHECK_EQ(va->fadeOut, 5); // inside: a's own
    CHECK_EQ(vb->fadeIn, 0);
    // Keys at sequence frames 20 and 150: a's source = sequence frame, b's = sequence frame - 100
    if (CHECK_EQ(va->keys.value(AnimParam::ZoomX).size(), 2)) CHECK_EQ(va->keys[AnimParam::ZoomX][1].frame, 150);
    if (CHECK_EQ(vb->keys.value(AnimParam::ZoomX).size(), 2)) CHECK_EQ(vb->keys[AnimParam::ZoomX][1].frame, 50);
    for (const Clip* c : {aa, ab}) {
        CHECK_EQ(c->volumeDb, -6.0);
        CHECK_EQ(c->pan, 30.0);
        CHECK(c->effects.isEmpty() && c->transform == ClipTransform{}); // video settings stay off the sound
    }
}

// The id of a deleted timeline is not given again after saving and opening (a render job queued for it would render
// the new timeline)
void testSequenceIdNotReused()
{
    Project p;
    const int second = p.addTimeline("Zwei");
    RenderJob job;
    job.id = 1;
    job.path = "/x/out.mp4";
    job.sequence = second;
    p.setRenderQueue({job});
    CHECK(p.removeSequence(second));
    const QByteArray json = ProjectFile::toJson(p.data(), "/x/p.schneidi");
    ProjectData d;
    QString err;
    if (!CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &d, &err))) return;
    CHECK_EQ(d.lastSequenceId, second);
    Project again;
    again.load(d);
    const int third = again.addTimeline("Drei");
    CHECK(third > second);
    CHECK(!again.sequence(again.renderQueue().value(0).sequence));
    // Nothing deleted: the file stays as before (no field)
    Project plain;
    plain.addTimeline();
    CHECK(!ProjectFile::toJson(plain.data(), "/x/p.schneidi").contains("lastSequenceId"));
}

// The clipboard after opening another project: compound clips (they point at a sequence of the old project) are left
// out, media clips bring their Media Pool entry along; another frame rate empties it
void testClipboardAcrossProjects()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia(media("/x/a.mp4", 100));
    p.addMedia(media("/x/b.mp4", 100));
    ed.addMediaAt({"/x/a.mp4"}, 0);
    ed.addMediaAt({"/x/b.mp4"}, 100);
    sel.set({clipOn(p.timeline(), TrackKind::Video, 0, 1)->id, clipOn(p.timeline(), TrackKind::Audio, 0, 1)->id});
    const int seq = ed.createCompoundClip();
    CHECK(seq > 0);
    ed.selectAll(); // a (V1/A1) and the compound clip
    ed.copySelection();

    // Other project: a timeline 2 of its own with other content
    ProjectData other;
    other.format = p.format();
    other.timeline.video.resize(1);
    other.timeline.audio.resize(1);
    Sequence s2;
    s2.id = seq;
    s2.name = "Fremd";
    s2.timeline.video.resize(1);
    s2.timeline.audio.resize(1);
    other.sequences << s2;
    p.load(other);
    CHECK(!p.mediaInfo("/x/a.mp4"));
    ed.paste(0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1: a[0-100|0-99]  A1: a[0-100|0-99]"));
    const MediaInfo* m = p.mediaInfo("/x/a.mp4");
    CHECK(m && m->length == 100 && m->bin == 0);
    CHECK(!p.mediaInfo("/x/b.mp4")); // only inside the compound clip, which stays behind

    // New project at another frame rate: nothing to paste (times count in the old rate)
    ed.selectAll();
    ed.copySelection();
    ProjectData fast;
    fast.format = p.format();
    fast.format.rate = {50, 1};
    fast.timeline.video.resize(1);
    fast.timeline.audio.resize(1);
    p.load(fast);
    ed.paste(0);
    CHECK_EQ(Check::dump(p.timeline()), QString("V1:  A1:"));
    CHECK_EQ(ed.pasteAttributesAvailable(), 0);
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
    // Save -> load -> save identical
    CHECK_EQ(ProjectFile::toJson(q.data(), "/x/projekt.schneidi"), json);
    // Older versions read only "timeline": the opened timeline is in there
    CHECK(json.contains("\"timeline\"") && json.contains("\"sequences\""));

    // Old file without sequences: one timeline "Timeline 1"
    ProjectData old;
    old.timeline = p.sequence(p.currentSequence())->timeline;
    old.timeline.video[0].clips.clear();
    old.timeline.audio[0].clips.clear();
    Project r;
    r.load(old);
    CHECK_EQ(r.sequences().size(), 1);
    CHECK_EQ(r.sequenceName(r.currentSequence()), QString("Timeline 1"));

    // Broken file: compound clip pointing to a missing sequence is dropped, self-reference too
    ProjectData broken = back;
    broken.sequences.removeIf([&](const Sequence& s) { return s.id == seq; });
    Project b2;
    b2.load(broken);
    for (const Sequence& s : b2.sequences())
        for (const Clip* c : {clipOn(s.timeline, TrackKind::Video, 0, 0)})
            CHECK(!c || !c->isCompound());
}

// ---- Rendering ----

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
    // red 0-2 s, then green 2-4 s (section checkable); blue as background
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
    ed.addMediaAt({blue}, 0, 0); // V1 blue
    ed.addMediaAt({rg}, 0, 1);   // V2 red/green
    // Shorten V2 clip to 0-60 (afterwards V1 is visible)
    const int rgId = clipOn(p.timeline(), TrackKind::Video, 1, 0)->id;
    ed.trimClip(rgId, TimelineOps::Edge::End, -40);
    sel.set({rgId});
    const int seq = ed.createCompoundClip();
    if (!CHECK(seq > 0)) return 1;
    const Clip* c = clipOn(p.timeline(), TrackKind::Video, 1, 0);
    if (!CHECK(c && c->isCompound())) return 1;

    // Content: red at 10; without nested timelines (Timeline::nested) only V1 blue remains
    QImage img = render(p.renderTimeline(), fmt, 10);
    CHECK(near(img.pixel(160, 90), 255, 0, 0));
    img = render(p.timeline(), fmt, 10);
    CHECK(near(img.pixel(160, 90), 0, 0, 255));

    // Section: compound clip starts with content frame 52 (content: 0-49 red, 50-59 green)
    const int cid = c->id;
    ed.trimClip(cid, TimelineOps::Edge::Start, 52);
    img = render(p.renderTimeline(), fmt, 55); // content frame 55 -> green
    CHECK(near(img.pixel(160, 90), 0, 255, 0));

    // Transparency: compound clip with zoomed-out content shows the track below
    p.setCurrentSequence(seq);
    const int innerId = clipOn(p.timeline(), TrackKind::Video, 0, 0)->id;
    ed.modifyClips({innerId}, "zoom", [](Clip& x) { x.transform.zoomX = x.transform.zoomY = 0.5; });
    p.setCurrentSequence(p.sequences().first().id);
    img = render(p.renderTimeline(), fmt, 55);
    CHECK(near(img.pixel(160, 90), 0, 255, 0)); // middle: green
    CHECK(near(img.pixel(10, 10), 0, 0, 255));  // edge: V1 blue shines through

    // Export at a different size: content of the compound clips scaled along (position in pixels)
    p.setCurrentSequence(seq);
    ed.modifyClips({innerId}, "pos", [](Clip& x) { x.transform.posX = 80; }); // half width to the right (320 picture)
    p.setCurrentSequence(p.sequences().first().id);
    Timeline big = p.renderTimeline();
    scaleTimeline(big, {320, 180}, {640, 360});
    img = render(big, ProjectFormat{640, 360, {25, 1}}, 55);
    CHECK(near(img.pixel(320 + 160, 180), 0, 255, 0)); // by 160 (= 80 * 2) to the right
    CHECK(near(img.pixel(200, 180), 0, 0, 255));

    // No crash on loop (broken data): compound refers to itself
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
    testDecomposeKeepsSettings();
    testClipboardAcrossProjects();
    testSequenceIdNotReused();
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
