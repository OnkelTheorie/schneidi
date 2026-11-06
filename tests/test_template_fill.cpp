// SPDX-License-Identifier: AGPL-3.0-only
// Content templates, the core (0.18): placeholders on clips (ClipSlot) and the template's description in the project
// file (round trip, files without them unchanged), filling them (core/TemplateFill: every rule on synthetic projects
// with faked media - trim, gap, ripple, speed, freeze, loop, refill, linked sound, reframe, text shrink, music trim and
// fit, logo width, several timelines), the Editor ops (mark, fill, one undo step each) and the template folders
// (core/TemplateLibrary: save with/without bundled media, list, find, rename, remove, built-in ones from their root).
// Everything stays in the test's own folders.

#include "check.h"

#include "core/Editor.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/ProjectFormat.h"
#include "core/Selection.h"
#include "core/StartTemplates.h"
#include "core/TemplateFill.h"
#include "core/TemplateLibrary.h"
#include "core/TimelineOps.h"
#include "engine/RenderCache.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>

QDebug operator<<(QDebug dbg, const ClipSlot& s)
{
    return dbg << "slot(" << s.id << int(s.kind) << int(s.length) << s.minLen << s.maxLen << int(s.fit) << s.reframe
               << s.maxChars << s.shrink << s.fitMusic << s.snapCuts << s.label << s.hint << s.nominalLen << ")";
}
QDebug operator<<(QDebug dbg, const TemplateInfo& t)
{
    return dbg << "info(" << t.name << t.description << t.tags << t.aspect << ")";
}

namespace {

using namespace TemplateFill;

const QString kTmpl = "/m/tmpl.mp4", kLong = "/m/long.mp4", kShort = "/m/short.mp4", kSilent = "/m/silent.mp4",
              kVert = "/m/vert.mp4", kSong = "/m/song.wav", kSquare = "/m/logo.png", kWide = "/m/wide.png";

MediaInfo media(const QString& path, int length, bool video, bool audio, bool image = false)
{
    return {path, QFileInfo(path).fileName(), length, video, audio, image};
}

Context context()
{
    Context ctx;
    ctx.aspect = [](const QString& p) {
        if (p == kVert) return 9.0 / 16;
        if (p == kSquare) return 1.0;
        if (p == kWide) return 2.0;
        return 16.0 / 9;
    };
    return ctx;
}

Clip clip(int id, const QString& path, int start, int in, int len, int link = 0)
{
    Clip c;
    c.id = id;
    c.mediaPath = path;
    c.start = start;
    c.in = in;
    c.out = in + len - 1;
    c.linkId = link;
    return c;
}

ClipSlot slot(const QString& id, SlotKind kind = SlotKind::Clip)
{
    ClipSlot s;
    s.id = id;
    s.kind = kind;
    return s;
}

// V1: clip1 (slot "clip1", 0-100, linked A1) | plain clip 100-200 (linked A1). V2: title "HOOK" 0-50 (slot "hook").
// V3: logo 0-200 (slot "logo", zoom 0.2). A2: music 0-200 (slot "music").
ProjectData project()
{
    ProjectData d;
    d.format = ProjectFormat{1920, 1080, {25, 1}};
    d.media = {media(kTmpl, 500, true, true),     media(kLong, 300, true, true),    media(kShort, 50, true, true),
               media(kSilent, 300, true, false),  media(kVert, 300, true, true),    media(kSong, 1500, false, true),
               media(kSquare, 125, true, false, true), media(kWide, 125, true, false, true)};
    Timeline& tl = d.timeline;
    tl.video.resize(3);
    tl.audio.resize(2);
    for (Track& t : tl.audio) t.kind = TrackKind::Audio;
    Clip c1 = clip(1, kTmpl, 0, 0, 100, 1);
    c1.slot = slot("clip1");
    tl.video[0].clips = {c1, clip(2, kTmpl, 100, 200, 100, 2)};
    tl.audio[0].clips = {clip(3, kTmpl, 0, 0, 100, 1), clip(4, kTmpl, 100, 200, 100, 2)};
    Clip title;
    title.id = 5;
    title.kind = ClipKind::Title;
    title.title.text = "HOOK";
    title.start = 0;
    title.out = 49;
    title.slot = slot("hook", SlotKind::Text);
    tl.video[1].clips = {title};
    Clip logo = clip(6, kSquare, 0, 0, 200);
    logo.transform.zoomX = logo.transform.zoomY = 0.2;
    logo.slot = slot("logo", SlotKind::Logo);
    tl.video[2].clips = {logo};
    Clip music = clip(7, kSong, 0, 0, 200);
    music.slot = slot("music", SlotKind::Music);
    tl.audio[1].clips = {music};
    d.lastClipId = 7;
    d.lastLinkId = 2;
    return d;
}

ClipSlot& slotOf(ProjectData& d, int clipId) { return TimelineOps::findClip(d.timeline, clipId)->slot; }
const Clip* find(const ProjectData& d, int id) { return TimelineOps::findClip(d.timeline, id); }

SlotReport only(const Report& r)
{
    CHECK_EQ(int(r.results.size()), 1);
    return r.results.value(0);
}

void testFileRoundTrip()
{
    ProjectData d = project();
    ClipSlot& s = slotOf(d, 1);
    s.label = "Best kill";
    s.length = SlotLength::Flexible;
    s.minLen = 25;
    s.maxLen = 250;
    s.fit = SlotFit::Loop;
    s.reframe = false;
    s.hint = "the loudest moment";
    ClipSlot& t = slotOf(d, 5);
    t.maxChars = 40;
    t.shrink = false;
    ClipSlot& m = slotOf(d, 7);
    m.fitMusic = false;
    m.snapCuts = true;
    d.templateInfo = TemplateInfo{"Gaming short", "Hook, three kills, logo", {"gaming", "short"}, "9:16"};
    const QByteArray json = ProjectFile::toJson(d, "/x/p.schneidi");
    ProjectData back;
    QString err;
    CHECK(ProjectFile::fromJson(json, "/x/p.schneidi", &back, &err));
    CHECK_EQ(find(back, 1)->slot, s);
    CHECK_EQ(find(back, 5)->slot, t);
    CHECK_EQ(find(back, 7)->slot, m);
    CHECK_EQ(find(back, 6)->slot, slot("logo", SlotKind::Logo));
    CHECK(!find(back, 2)->slot.isSet());
    CHECK(back.templateInfo.has_value());
    if (back.templateInfo) CHECK_EQ(*back.templateInfo, *d.templateInfo);
    // Written once more: the same file
    CHECK_EQ(ProjectFile::toJson(back, "/x/p.schneidi"), json);

    // Without placeholders and description: nothing of them in the file, it loads as before
    ProjectData plain = project();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& tr : plain.timeline.tracks(k))
            for (Clip& c : tr.clips) c.slot = {};
    const QByteArray pj = ProjectFile::toJson(plain, "/x/p.schneidi");
    CHECK(!pj.contains("\"slot\""));
    CHECK(!pj.contains("templateInfo"));
    ProjectData pb;
    CHECK(ProjectFile::fromJson(pj, "/x/p.schneidi", &pb, &err));
    CHECK(!pb.templateInfo);
    CHECK_EQ(ProjectFile::toJson(pb, "/x/p.schneidi"), pj);
    // A placeholder needs no newer format: the version stays what the project needs without it
    CHECK_EQ(QJsonDocument::fromJson(json).object().value("version").toInt(),
             QJsonDocument::fromJson(pj).object().value("version").toInt());
    // A slot of a newer kind or broken values (hand edits): an ordinary default, no crash
    QByteArray odd = json;
    odd.replace("\"fit\": \"loop\"", "\"fit\": \"warp\"");
    CHECK(ProjectFile::fromJson(odd, "/x/p.schneidi", &back, &err));
    CHECK(find(back, 1)->slot.fit == SlotFit::Trim);
    // The render cache key ignores the mark (it does not change the picture)
    CHECK(!RenderCache::normalized(*find(d, 1)).slot.isSet());
}

void testList()
{
    ProjectData d = project();
    const QVector<SlotRef> all = listSlots(d);
    CHECK_EQ(int(all.size()), 4);
    CHECK_EQ(slotIds(d), (QStringList{"clip1", "hook", "logo", "music"}));
    CHECK_EQ(all[0].length, 100);
    CHECK_EQ(all[0].media, kTmpl);
    CHECK_EQ(all[1].text, QString("HOOK"));
    CHECK(all[3].track == TrackRef{TrackKind::Audio, 1});
}

void testCheck()
{
    ProjectData d = project();
    const Context ctx = context();
    const auto codes = [&](const Values& v) {
        QStringList out;
        for (const Problem& p : check(d, v, ctx)) out << p.code;
        return out;
    };
    CHECK(codes({{"clip1", Value::ofFile(kLong)}, {"hook", Value::ofText("Hi")}}).isEmpty());
    CHECK_EQ(codes({{"nope", Value::ofFile(kLong)}}), QStringList{"UNKNOWN_SLOT"});
    CHECK_EQ(codes({{"clip1", Value::ofFile("/does/not/exist.mp4")}}), QStringList{"FILE_MISSING"});
    CHECK_EQ(codes({{"clip1", Value::ofFile(kSong)}}), QStringList{"WRONG_TYPE"});    // no picture
    CHECK_EQ(codes({{"music", Value::ofFile(kSilent)}}), QStringList{"WRONG_TYPE"});  // no sound
    CHECK_EQ(codes({{"clip1", Value::ofFile(kLong, 400)}}), QStringList{"BAD_RANGE"});
    CHECK_EQ(codes({{"clip1", {}}}), QStringList{"MISSING_VALUE"});
    CHECK_EQ(codes({{"hook", Value::ofText("  ")}}), QStringList{"EMPTY_TEXT"});
    slotOf(d, 5).maxChars = 10;
    slotOf(d, 5).shrink = false;
    CHECK_EQ(codes({{"hook", Value::ofText("far more than ten")}}), QStringList{"TOO_LONG"});
    slotOf(d, 5).shrink = true; // shrinking: only a warning when filling
    CHECK(codes({{"hook", Value::ofText("far more than ten")}}).isEmpty());
    d.timeline.video[0].locked = true;
    CHECK_EQ(codes({{"clip1", Value::ofFile(kLong)}}), QStringList{"LOCKED"});
    // A file outside the project: the probe knows it
    ProjectData e = project();
    Context probing = ctx;
    probing.probe = [](const QString& p) -> std::optional<MediaInfo> {
        if (p == "/new/clip.mp4") return media(p, 80, true, true);
        return std::nullopt;
    };
    CHECK(check(e, {{"clip1", Value::ofFile("/new/clip.mp4")}}, probing).isEmpty());
    const Report r = apply(e, {{"clip1", Value::ofFile("/new/clip.mp4")}}, probing);
    CHECK(r.ok());
    CHECK(std::any_of(e.media.begin(), e.media.end(), [](const MediaInfo& m) { return m.path == "/new/clip.mp4"; }));
    // Failing values change nothing
    ProjectData f = project();
    const QByteArray before = ProjectFile::toJson(f, "/x/p.schneidi");
    const Report bad = apply(f, {{"clip1", Value::ofFile(kSong)}, {"nope", Value::ofText("x")}}, ctx);
    CHECK(!bad.ok());
    CHECK_EQ(int(bad.errors.size()), 2);
    CHECK_EQ(ProjectFile::toJson(f, "/x/p.schneidi"), before);
    CHECK_EQ(bad.unfilled, (QStringList{"hook", "logo", "music"}));
}

void testClipFixed()
{
    const Context ctx = context();
    { // too long: trimmed from `from`, the linked sound follows
        ProjectData d = project();
        const Report r = apply(d, {{"clip1", Value::ofFile(kLong, 50)}}, ctx);
        const SlotReport s = only(r);
        CHECK(r.ok());
        CHECK(s.adjusted.contains("trimmed"));
        const Clip* v = find(d, 1);
        CHECK_EQ(v->mediaPath, kLong);
        CHECK_EQ(v->in, 50);
        CHECK_EQ(v->length(), 100);
        const Clip* a = find(d, 3);
        CHECK_EQ(a->mediaPath, kLong);
        CHECK_EQ(a->in, 50);
        CHECK_EQ(a->length(), 100);
        CHECK_EQ(a->linkId, v->linkId);
        CHECK_EQ(find(d, 2)->start, 100); // nothing moved
        CHECK(s.text.startsWith("trimmed 10.0s → 4.0s"));
        CHECK_EQ(s.details.value("value_s").toDouble(), 10.0);
    }
    { // the Media Pool's source in/out when the value gives no range
        ProjectData d = project();
        for (MediaInfo& m : d.media)
            if (m.path == kLong) m.markIn = 120, m.markOut = 269;
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(find(d, 1)->in, 120);
    }
    { // too short, Trim, fixed: left shorter, a gap stays (warning)
        ProjectData d = project();
        const Report r = apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx);
        const SlotReport s = only(r);
        CHECK(s.adjusted.contains("shortened"));
        CHECK_EQ(find(d, 1)->length(), 50);
        CHECK_EQ(find(d, 3)->length(), 50);
        CHECK_EQ(find(d, 2)->start, 100);
        CHECK_EQ(int(s.warnings.size()), 1);
    }
    { // Speed: slowed down to fill it, the sound too
        ProjectData d = project();
        slotOf(d, 1).fit = SlotFit::Speed;
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx));
        const Clip* v = find(d, 1);
        CHECK_EQ(v->speed, 0.5);
        CHECK_EQ(v->length(), 100);
        CHECK(v->out * v->speed < 50); // never beyond the file
        CHECK_EQ(find(d, 3)->speed, 0.5);
        CHECK_EQ(s.details.value("speed_pct").toDouble(), 50.0);
        CHECK(s.text.contains("slowed to 50 %"));
    }
    { // Speed below 25 %: at 25 %, shorter (warning)
        ProjectData d = project();
        slotOf(d, 1).fit = SlotFit::Speed;
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kShort, 0, 10)}}, ctx));
        CHECK_EQ(find(d, 1)->speed, 0.25);
        CHECK_EQ(find(d, 1)->length(), 40);
        CHECK_EQ(int(s.warnings.size()), 1);
    }
    { // Freeze: the last frame holds for the rest (no sound there); filled again it is one clip again
        ProjectData d = project();
        slotOf(d, 1).fit = SlotFit::Freeze;
        d.timeline.audio[0].clips[0].fadeOut = 10; // the sound's own fade-out stays at its (earlier) end
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx));
        CHECK_EQ(find(d, 3)->fadeOut, 10);
        CHECK(s.adjusted.contains("frozen"));
        CHECK_EQ(int(d.timeline.video[0].clips.size()), 3);
        const Clip& a = d.timeline.video[0].clips[0];
        const Clip& b = d.timeline.video[0].clips[1];
        CHECK_EQ(a.length(), 50);
        CHECK(b.freeze);
        CHECK_EQ(b.stillFrame(), 49);
        CHECK_EQ(b.start, 50);
        CHECK_EQ(b.length(), 50);
        CHECK_EQ(b.slot.id, QString("clip1"));
        CHECK_EQ(find(d, 3)->length(), 50);
        CHECK_EQ(int(d.timeline.audio[0].clips.size()), 2);
        const QVector<SlotRef> all = listSlots(d);
        CHECK_EQ(all[0].parts.size(), 2);
        CHECK_EQ(all[0].length, 100);
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(int(d.timeline.video[0].clips.size()), 2);
        CHECK_EQ(find(d, 1)->length(), 100);
        CHECK(!find(d, 1)->freeze);
    }
    { // Loop: repeated, each repeat with its own sound
        ProjectData d = project();
        slotOf(d, 1).fit = SlotFit::Loop;
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kShort, 10, 40)}}, ctx));
        CHECK_EQ(s.details.value("repeats").toInt(), 4);
        const Track& v1 = d.timeline.video[0];
        CHECK_EQ(int(v1.clips.size()), 5);
        int sum = 0;
        QSet<int> links;
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(v1.clips[i].start, sum);
            CHECK_EQ(v1.clips[i].in, 10);
            sum += v1.clips[i].length();
            links.insert(v1.clips[i].linkId);
        }
        CHECK_EQ(sum, 100);
        CHECK_EQ(v1.clips[3].length(), 10);
        CHECK_EQ(links.size(), 4);
        CHECK_EQ(int(d.timeline.audio[0].clips.size()), 5);
        for (int i = 0; i < 4; ++i) CHECK_EQ(d.timeline.audio[0].clips[i].linkId, v1.clips[i].linkId);
    }
    { // a file without sound: the linked sound goes
        ProjectData d = project();
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kSilent)}}, ctx));
        CHECK(s.adjusted.contains("no_sound"));
        CHECK(!find(d, 3));
        CHECK(find(d, 4));
    }
}

void testClipFlexible()
{
    const Context ctx = context();
    { // longer: the slot grows, later clips on all tracks move, a clip reaching over the slot's end stays
        ProjectData d = project();
        slotOf(d, 1).length = SlotLength::Flexible;
        d.timeline.markers = {50, 150};
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx));
        CHECK(s.adjusted.contains("rippled"));
        CHECK_EQ(find(d, 1)->length(), 300);
        CHECK_EQ(find(d, 3)->length(), 300);
        CHECK_EQ(find(d, 2)->start, 300);
        CHECK_EQ(find(d, 4)->start, 300);
        CHECK_EQ(find(d, 7)->start, 0); // the music bed reaches over it: stays
        CHECK_EQ(find(d, 7)->length(), 200);
        CHECK_EQ(d.timeline.markers, (QVector<int>{50, 350}));
        CHECK_EQ(s.details.value("ripple_s").toDouble(), 8.0);
    }
    { // shorter: later clips move up, a clip reaching over the slot's end gets shorter at its end
        ProjectData d = project();
        slotOf(d, 1).length = SlotLength::Flexible;
        apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 50);
        CHECK_EQ(find(d, 2)->start, 50);
        CHECK_EQ(find(d, 4)->start, 50);
        CHECK_EQ(find(d, 7)->start, 0);
        CHECK_EQ(find(d, 7)->length(), 150);
        CHECK_EQ(find(d, 7)->in, 0);
        CHECK_EQ(find(d, 6)->length(), 150); // the logo too
    }
    { // within [minLen, maxLen]
        ProjectData d = project();
        slotOf(d, 1).length = SlotLength::Flexible;
        slotOf(d, 1).maxLen = 150;
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 150);
        CHECK_EQ(find(d, 2)->start, 150);
        ProjectData e = project();
        slotOf(e, 1).length = SlotLength::Flexible;
        slotOf(e, 1).minLen = 75;
        slotOf(e, 1).fit = SlotFit::Freeze;
        apply(e, {{"clip1", Value::ofFile(kShort)}}, ctx);
        CHECK_EQ(listSlots(e)[0].length, 75);
        CHECK_EQ(find(e, 2)->start, 75);
    }
}

void testReframe()
{
    Context ctx = context();
    { // a 9:16 clip in a 16:9 timeline: zoomed to fill it
        ProjectData d = project();
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kVert)}}, ctx));
        CHECK(s.adjusted.contains("reframed"));
        CHECK_EQ(s.details.value("reframe").toString(), QString("center"));
        CHECK(find(d, 1)->transform.zoomX > 3);
    }
    { // switched off: as it is
        ProjectData d = project();
        slotOf(d, 1).reframe = false;
        apply(d, {{"clip1", Value::ofFile(kVert)}}, ctx);
        CHECK_EQ(find(d, 1)->transform.zoomX, 1.0);
    }
    { // a camera path from the caller: Auto
        ProjectData d = project();
        int asked = 0;
        ctx.reframePath = [&](const QString& file, int, int length) {
            ++asked;
            CHECK_EQ(file, kVert);
            return QVector<Reframe::Key>{{0, 0.5, 0.3}, {length / 2, 0.5, 0.7}};
        };
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kVert)}}, ctx));
        CHECK_EQ(asked, 1);
        CHECK_EQ(s.details.value("reframe").toString(), QString("auto"));
        CHECK(find(d, 1)->keys.contains(AnimParam::PosY));
        // filled again with a 16:9 file: the reframed transform follows the new shape (back to the whole picture)
        const SlotReport t = only(apply(d, {{"clip1", Value::ofFile(kLong)}}, context()));
        CHECK(t.adjusted.contains("reshaped"));
        CHECK(std::abs(find(d, 1)->transform.zoomX - 1.0) < 1e-6);
    }
    { // shape unknown: a warning
        ProjectData d = project();
        Context blind;
        blind.aspect = [](const QString&) { return 0.0; };
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kVert)}}, blind));
        CHECK(s.warnings.contains("picture shape unknown: not reframed"));
    }
}

void testText()
{
    const Context ctx = context();
    { // wider than the placeholder's text: smaller, fits its width
        ProjectData d = project();
        const TitleStyle old = find(d, 5)->title;
        const SlotReport s = only(apply(d, {{"hook", Value::ofText("HOOKED")}}, ctx));
        const TitleStyle& t = find(d, 5)->title;
        CHECK_EQ(t.text, QString("HOOKED"));
        CHECK(s.adjusted.contains("shrunk"));
        CHECK(t.size < old.size);
        CHECK(titleWidth(t, t.size) <= titleWidth(old, old.size) * 1.01);
        CHECK_EQ(s.details.value("size_px").toDouble(), t.size);
    }
    { // narrower: the size stays; shrink off: the size stays
        ProjectData d = project();
        apply(d, {{"hook", Value::ofText("Hi")}}, ctx);
        CHECK_EQ(find(d, 5)->title.size, 80.0);
        ProjectData e = project();
        slotOf(e, 5).shrink = false;
        apply(e, {{"hook", Value::ofText("A much longer hook")}}, ctx);
        CHECK_EQ(find(e, 5)->title.size, 80.0);
    }
    { // at most down to 40 %, over maxChars with shrink: a warning
        ProjectData d = project();
        slotOf(d, 5).maxChars = 10;
        const SlotReport s = only(apply(d, {{"hook", Value::ofText("This hook is far too long for it")}}, ctx));
        CHECK_EQ(find(d, 5)->title.size, 32.0);
        CHECK(s.warnings.contains("too long: 32/10 chars"));
    }
}

void testMusic()
{
    Context ctx = context();
    { // without beats: trimmed with a 1 s fade-out
        ProjectData d = project();
        const SlotReport s = only(apply(d, {{"music", Value::ofFile(kSong)}}, ctx));
        CHECK(s.adjusted.contains("trimmed"));
        CHECK(s.adjusted.contains("faded"));
        CHECK_EQ(find(d, 7)->length(), 200);
        CHECK_EQ(find(d, 7)->fadeOut, 25);
    }
    { // with beats: cut on its phrases to the slot's length (core/MusicFit), all parts one slot
        ProjectData d = project();
        Beats::Result b;
        b.found = true;
        b.bpm = 120;
        b.confidence = 0.9;
        b.period = 0.5;
        b.phase = 0.5;
        b.beats = 112;
        b.downbeatConfidence = 0.9;
        b.phraseBars = 4;
        b.phraseConfidence = 0.9;
        b.barEnergyDb.assign(28, -20);
        b.barBassDb.assign(28, -30);
        int asked = 0;
        ctx.beats = [&](const QString& path, int) -> std::optional<Beats::Result> {
            ++asked;
            CHECK_EQ(path, kSong);
            return b;
        };
        const SlotReport s = only(apply(d, {{"music", Value::ofFile(kSong)}}, ctx));
        CHECK_EQ(asked, 1);
        CHECK(s.adjusted.contains("fit_music"));
        const QVector<SlotRef> all = listSlots(d);
        CHECK_EQ(all.last().slot.id, QString("music"));
        CHECK(std::abs(all.last().length - 200) <= 1);
        CHECK(all.last().parts.size() >= 2);
        CHECK_EQ(all.last().clip, 7);
    }
}

void testLogo()
{
    ProjectData d = project();
    const SlotReport s = only(apply(d, {{"logo", Value::ofFile(kWide)}}, context()));
    // 1:1 fits 1080 px wide at zoom 1, 2:1 fits 1920 px: the zoom keeps 0.2 x 1080 px on screen
    CHECK(s.adjusted.contains("scaled"));
    CHECK(std::abs(find(d, 6)->transform.zoomX - 0.2 * 1080 / 1920) < 1e-9);
    CHECK_EQ(find(d, 6)->length(), 200);
    CHECK_EQ(find(d, 6)->mediaPath, kWide);
}

void testSequences()
{
    ProjectData d = project();
    Sequence open{1, "Timeline 1"};
    Sequence other{2, "Short"};
    other.width = 1080;
    other.height = 1920;
    Clip c = clip(20, kTmpl, 0, 0, 60);
    c.slot = slot("clip1");
    other.timeline.video.resize(1);
    other.timeline.video[0].clips = {c};
    d.sequences = {open, other};
    d.currentSequence = 1;
    d.lastClipId = 20;
    const Report r = apply(d, {{"clip1", Value::ofFile(kLong)}}, context());
    CHECK_EQ(int(r.results.size()), 2);
    CHECK_EQ(r.results[1].sequence, 2);
    const Clip* there = TimelineOps::findClip(d.sequences[1].timeline, 20);
    CHECK_EQ(there->mediaPath, kLong);
    CHECK(there->transform.zoomX > 3); // 16:9 into the 9:16 one: reframed
    CHECK_EQ(find(d, 1)->transform.zoomX, 1.0);
    CHECK(d.sequences[0].timeline == d.timeline); // the open one in its sequence too
    const QJsonObject j = r.toJson();
    CHECK(j.value("ok").toBool());
    CHECK_EQ(j.value("results").toArray().size(), 2);
}

// A fixed slot keeps its length after a value that left it shorter: the next, longer value fills all of it again
// (review A3) - Trim with a gap, Speed at its 25 % limit, a shortened song; a gap someone filled in the meantime ends it
void testFixedKeepsLength()
{
    const Context ctx = context();
    { // Trim
        ProjectData d = project();
        apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 50);
        CHECK_EQ(listSlots(d)[0].length, 50); // what is on the timeline
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx));
        CHECK_EQ(find(d, 1)->length(), 100);
        CHECK_EQ(find(d, 3)->length(), 100);
        CHECK(s.adjusted.contains("trimmed"));
        CHECK_EQ(find(d, 2)->start, 100);
        // Through the project file: the same
        ProjectData e = project();
        apply(e, {{"clip1", Value::ofFile(kShort)}}, ctx);
        ProjectData back;
        QString err;
        CHECK(ProjectFile::fromJson(ProjectFile::toJson(e, "/x/p.schneidi"), "/x/p.schneidi", &back, &err));
        apply(back, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(find(back, 1)->length(), 100);
    }
    { // Speed at its limit
        ProjectData d = project();
        slotOf(d, 1).fit = SlotFit::Speed;
        apply(d, {{"clip1", Value::ofFile(kShort, 0, 10)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 40);
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 100);
    }
    { // a song shorter than the music slot
        ProjectData d = project();
        apply(d, {{"music", Value::ofFile(kSong, 0, 50)}}, ctx);
        CHECK_EQ(find(d, 7)->length(), 50);
        apply(d, {{"music", Value::ofFile(kSong)}}, ctx);
        CHECK_EQ(find(d, 7)->length(), 200);
    }
    { // the gap was used for something else: the slot reaches up to it only
        ProjectData d = project();
        apply(d, {{"clip1", Value::ofFile(kShort)}}, ctx);
        d.timeline.video[0].clips.insert(1, clip(50, kTmpl, 70, 0, 10));
        d.lastClipId = 50;
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(find(d, 1)->length(), 70);
        CHECK_EQ(find(d, 50)->start, 70);
        CHECK_EQ(find(d, 50)->length(), 10);
    }
}

// A Clip slot whose placeholder had no sound (a still): the new file brings its sound, linked, on a free audio track
// (review A6)
void testStillGetsSound()
{
    const Context ctx = context();
    const auto stillSlot = [](ProjectData& d) {
        Clip& v = d.timeline.video[0].clips[0];
        v.mediaPath = kSquare;
        v.in = 0;
        v.out = 99;
        v.linkId = 0;
        d.timeline.audio[0].clips.removeFirst(); // clip 3, its old sound
    };
    { // A1 is free there
        ProjectData d = project();
        stillSlot(d);
        const SlotReport s = only(apply(d, {{"clip1", Value::ofFile(kLong, 20)}}, ctx));
        const Clip* v = find(d, 1);
        CHECK(v->linkId != 0);
        const Track& a1 = d.timeline.audio[0];
        CHECK_EQ(int(a1.clips.size()), 2);
        const Clip& a = a1.clips[0];
        CHECK_EQ(a.mediaPath, kLong);
        CHECK_EQ(a.start, 0);
        CHECK_EQ(a.in, 20);
        CHECK_EQ(a.length(), 100);
        CHECK_EQ(a.linkId, v->linkId);
        CHECK(a.linkId != find(d, 4)->linkId);
        CHECK(!a.slot.isSet());
        CHECK(s.adjusted.contains("sound_added"));
    }
    { // A1 and A2 are taken there: a new track
        ProjectData d = project();
        stillSlot(d);
        d.timeline.audio[0].clips.prepend(clip(40, kSong, 0, 0, 50));
        d.lastClipId = 40;
        apply(d, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(int(d.timeline.audio.size()), 3);
        if (d.timeline.audio.size() == 3) {
            CHECK_EQ(int(d.timeline.audio[2].clips.size()), 1);
            CHECK_EQ(d.timeline.audio[2].clips.value(0).linkId, find(d, 1)->linkId);
        }
    }
    { // Loop: each repeat with its sound
        ProjectData d = project();
        stillSlot(d);
        slotOf(d, 1).fit = SlotFit::Loop;
        apply(d, {{"clip1", Value::ofFile(kShort, 0, 40)}}, ctx);
        CHECK_EQ(int(d.timeline.video[0].clips.size()), 4);
        CHECK_EQ(int(d.timeline.audio[0].clips.size()), 4);
        for (int i = 0; i < 3; ++i)
            CHECK_EQ(d.timeline.audio[0].clips.value(i).linkId, d.timeline.video[0].clips[i].linkId);
    }
    { // a file without sound, or a video placeholder whose sound was taken away on purpose: no sound
        ProjectData d = project();
        stillSlot(d);
        apply(d, {{"clip1", Value::ofFile(kSilent)}}, ctx);
        CHECK_EQ(int(d.timeline.audio[0].clips.size()), 1);
        ProjectData e = project();
        e.timeline.video[0].clips[0].linkId = 0;
        e.timeline.audio[0].clips.removeFirst();
        apply(e, {{"clip1", Value::ofFile(kLong)}}, ctx);
        CHECK_EQ(int(e.timeline.audio[0].clips.size()), 1);
    }
}

// A flexible music slot with beats: cut to the song's length within [minLen, maxLen] (review A9)
void testMusicFlexible()
{
    Context ctx = context();
    Beats::Result b;
    b.found = true;
    b.bpm = 120;
    b.confidence = 0.9;
    b.period = 0.5;
    b.phase = 0.5;
    b.beats = 112;
    b.downbeatConfidence = 0.9;
    b.phraseBars = 4;
    b.phraseConfidence = 0.9;
    b.barEnergyDb.assign(28, -20);
    b.barBassDb.assign(28, -30);
    ctx.beats = [&](const QString&, int) -> std::optional<Beats::Result> { return b; };
    ProjectData d = project();
    slotOf(d, 7).length = SlotLength::Flexible;
    slotOf(d, 7).maxLen = 400;
    const SlotReport s = only(apply(d, {{"music", Value::ofFile(kSong)}}, ctx));
    CHECK(s.adjusted.contains("fit_music"));
    CHECK(std::abs(listSlots(d).last().length - 400) <= 1);
}

// A text slot cut in two: every part gets the text (review A10)
void testTextSplit()
{
    ProjectData d = project();
    Clip& t = d.timeline.video[1].clips[0];
    t.out = 24;
    Clip second = t;
    second.id = 8;
    second.start = 25;
    d.timeline.video[1].clips << second;
    d.lastClipId = 8;
    const SlotReport s = only(apply(d, {{"hook", Value::ofText("HOOKED")}}, context()));
    CHECK_EQ(s.clips, (QVector<int>{5, 8}));
    for (int id : {5, 8}) {
        CHECK_EQ(find(d, id)->title.text, QString("HOOKED"));
        CHECK(find(d, id)->title.size < 80);
    }
    CHECK_EQ(find(d, 5)->title.size, find(d, 8)->title.size);
}

// A flexible slot inside a compound clip: the compound clips that show it follow, their timeline ripples (review A13)
void testCompound()
{
    ProjectData d = project();
    Sequence open{1, "Timeline 1"};
    Sequence inner{2, "Compound"};
    inner.compound = true;
    inner.timeline.video.resize(1);
    Clip c = clip(20, kTmpl, 0, 0, 60);
    c.slot = slot("inner");
    c.slot.length = SlotLength::Flexible;
    inner.timeline.video[0].clips = {c, clip(21, kTmpl, 60, 300, 60)};
    Clip whole;
    whole.id = 30;
    whole.kind = ClipKind::Compound;
    whole.sequenceId = 2;
    whole.start = 200;
    whole.out = 119;
    Clip tail = whole; // shows the compound's second half
    tail.id = 31;
    tail.start = 0;
    tail.in = 60;
    tail.out = 119;
    Clip head = whole; // shows its first 30 frames: before the slot's end
    head.id = 32;
    head.start = 100;
    head.out = 29;
    d.timeline.video[0].clips << whole << clip(33, kTmpl, 320, 0, 50);
    d.timeline.video.resize(5);
    d.timeline.video[3].clips = {tail};
    d.timeline.video[4].clips = {head};
    d.sequences = {open, inner};
    d.currentSequence = 1;
    d.lastClipId = 33;
    const SlotReport s = only(apply(d, {{"inner", Value::ofFile(kLong)}}, context()));
    CHECK_EQ(s.details.value("ripple_s").toDouble(), 9.6);
    CHECK_EQ(TimelineOps::endFrame(d.sequences[1].timeline), 360);
    CHECK_EQ(find(d, 30)->start, 200);
    CHECK_EQ(find(d, 30)->in, 0);
    CHECK_EQ(find(d, 30)->length(), 360);
    CHECK_EQ(find(d, 33)->start, 560); // the parent rippled
    CHECK_EQ(find(d, 31)->in, 300);    // the same picture as before
    CHECK_EQ(find(d, 31)->length(), 60);
    CHECK_EQ(find(d, 31)->start, 0);
    CHECK_EQ(find(d, 32)->in, 0);
    CHECK_EQ(find(d, 32)->length(), 30);
    CHECK(s.adjusted.contains("compound"));
    CHECK(d.sequences[0].timeline == d.timeline);
}

// Inspector edits reach every part of a slot instance (frozen tail, loop repeats, razor cut) (review A5)
void testEditorParts()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    ProjectData d = project();
    slotOf(d, 1).fit = SlotFit::Freeze;
    apply(d, {{"clip1", Value::ofFile(kShort)}}, context());
    p.load(d);
    const QVector<int> parts = listSlots(p.data())[0].parts;
    CHECK_EQ(int(parts.size()), 2);
    ClipSlot renamed = TimelineOps::findClip(p.timeline(), parts[1])->slot;
    renamed.id = "best";
    CHECK(ed.setClipSlot(parts[1], renamed).isEmpty());
    CHECK_EQ(p.undoStack()->count(), 1);
    for (int id : parts) CHECK_EQ(TimelineOps::findClip(p.timeline(), id)->slot.id, QString("best"));
    CHECK_EQ(listSlots(p.data())[0].parts, parts);
    CHECK(ed.setClipSlot(parts[0], {}).isEmpty());
    CHECK_EQ(p.undoStack()->count(), 2);
    for (int id : parts) CHECK(!TimelineOps::findClip(p.timeline(), id)->slot.isSet());
    // A kind one of the parts cannot hold: refused for all
    ProjectData e = project();
    Clip t = e.timeline.video[1].clips[0]; // a title right after clip1 on V1, the same slot
    t.id = 9;
    t.start = 100;
    t.out = 49;
    t.slot = slot("clip1");
    e.timeline.video[0].clips[1].start = 150;
    e.timeline.video[0].clips.insert(1, t);
    e.lastClipId = 9;
    p.load(e);
    CHECK_EQ(listSlots(p.data())[0].parts, (QVector<int>{1, 9}));
    CHECK(!ed.setClipSlot(9, slot("clip1", SlotKind::Text)).isEmpty());
    CHECK(TimelineOps::findClip(p.timeline(), 9)->slot.kind == SlotKind::Clip);
}

void testEditor()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    ProjectData d = project();
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (Track& t : d.timeline.tracks(k))
            for (Clip& c : t.clips) c.slot = {};
    p.load(d);
    const Timeline plain = p.timeline();
    // Mark: one undo step each; a clip that cannot hold the kind is refused
    CHECK(ed.setClipSlot(1, slot("clip1")).isEmpty());
    CHECK_EQ(p.undoStack()->count(), 1);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), 1)->slot.id, QString("clip1"));
    CHECK(!ed.setClipSlot(5, slot("hook", SlotKind::Music)).isEmpty()); // a title is no music
    CHECK(!ed.setClipSlot(3, slot("clip1", SlotKind::Music)).isEmpty()); // the id is a clip slot already
    CHECK(ed.setClipSlot(5, slot("hook", SlotKind::Text)).isEmpty());
    CHECK_EQ(p.undoStack()->count(), 2);
    CHECK(ed.setClipSlot(5, {}).isEmpty()); // unmark
    CHECK(!TimelineOps::findClip(p.timeline(), 5)->slot.isSet());
    p.undoStack()->undo();
    CHECK_EQ(TimelineOps::findClip(p.timeline(), 5)->slot.id, QString("hook"));

    // Fill: one undo step, a new file comes into the pool, ids stay unique
    const Timeline marked = p.timeline();
    const int steps = p.undoStack()->index();
    Context ctx = context();
    ctx.probe = [](const QString& path) -> std::optional<MediaInfo> { return media(path, 40, true, true); };
    TemplateFill::Report r = ed.fillSlot("clip1", Value::ofFile("/new/a.mp4"), ctx);
    CHECK(r.ok());
    CHECK_EQ(p.undoStack()->index(), steps + 1);
    CHECK(p.mediaInfo("/new/a.mp4"));
    CHECK_EQ(TimelineOps::findClip(p.timeline(), 1)->mediaPath, QString("/new/a.mp4"));
    const int next = p.newClipId();
    CHECK(next > 7);
    p.undoStack()->undo();
    CHECK(p.timeline() == marked);
    p.undoStack()->redo();
    CHECK_EQ(TimelineOps::findClip(p.timeline(), 1)->mediaPath, QString("/new/a.mp4"));
    // Several at once, one step; nothing fillable: no step
    const int before = p.undoStack()->count();
    r = ed.fillSlots({{"clip1", Value::ofFile(kLong)}, {"hook", Value::ofText("Go")}}, ctx);
    CHECK(r.ok());
    CHECK_EQ(p.undoStack()->count(), before + 1);
    CHECK_EQ(TimelineOps::findClip(p.timeline(), 5)->title.text, QString("Go"));
    r = ed.fillSlot("nope", Value::ofText("x"), ctx);
    CHECK(!r.ok());
    CHECK_EQ(p.undoStack()->count(), before + 1);
    while (p.undoStack()->canUndo()) p.undoStack()->undo();
    CHECK(p.timeline() == plain);
}

QString touch(const QString& path, const QByteArray& bytes = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
    return path;
}

void copyTree(const QString& from, const QString& to)
{
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = QDir(to).filePath(QDir(from).relativeFilePath(src));
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile::copy(src, dst);
    }
}

void testLibrary()
{
    QDir(TemplateLibrary::dir()).removeRecursively();
    QTemporaryDir tmp;
    // A project with media of the same name in two folders, one unused file in the pool
    const QString a = touch(tmp.filePath("one/clip.mp4"), "aaa"), b = touch(tmp.filePath("two/clip.mp4"), "bbbb");
    const QString unused = touch(tmp.filePath("one/unused.mp4"));
    ProjectData d = project();
    d.media = {media(a, 100, true, true), media(b, 100, true, true), media(unused, 100, true, true)};
    d.timeline.video[0].clips[0].mediaPath = a;
    d.timeline.video[0].clips[1].mediaPath = b;
    d.timeline.audio[0].clips[0].mediaPath = a;
    d.timeline.audio[0].clips[1].mediaPath = b;
    d.timeline.video[2].clips.clear();
    d.timeline.audio[1].clips.clear();
    TemplateLibrary::SaveOptions o;
    o.description = "Two clips and a hook";
    o.tags = {"test"};
    o.preview = touch(tmp.filePath("shot.png"));
    QString err;
    std::optional<ContentTemplate> t = TemplateLibrary::save(d, "My Short", o, &err);
    if (!CHECK(t.has_value())) {
        std::printf("%s\n", qPrintable(err));
        return;
    }
    CHECK_EQ(t->id, QString("My Short"));
    CHECK_EQ(t->name, QString("My Short"));
    CHECK_EQ(t->description, QString("Two clips and a hook"));
    CHECK_EQ(t->tags, QStringList{"test"});
    CHECK_EQ(t->aspect, QString("16:9"));
    CHECK_EQ(t->slotIds, (QStringList{"clip1", "hook"}));
    CHECK_EQ(t->length, 200);
    CHECK(t->preview.endsWith("preview.png"));
    const QDir folder(t->dir);
    CHECK(folder.exists("media/clip.mp4"));
    CHECK(folder.exists("media/clip-2.mp4"));
    CHECK(!folder.exists("media/unused.mp4"));
    ProjectData loaded;
    CHECK(TemplateLibrary::load(*t, &loaded, &err));
    CHECK_EQ(int(loaded.media.size()), 2);
    for (const MediaInfo& m : loaded.media) CHECK(m.path.startsWith(folder.absoluteFilePath("media/")));
    CHECK_EQ(TimelineOps::findClip(loaded.timeline, 2)->mediaPath, folder.absoluteFilePath("media/clip-2.mp4"));
    CHECK_EQ(QFileInfo(TimelineOps::findClip(loaded.timeline, 2)->mediaPath).size(), 4);
    CHECK_EQ(TimelineOps::findClip(loaded.timeline, 1)->slot.id, QString("clip1"));
    // The folder moved (a ZIP unpacked elsewhere): its media come along by their relative paths
    QTemporaryDir elsewhere;
    const QString moved = elsewhere.filePath("unpacked");
    copyTree(t->dir, moved);
    std::optional<ContentTemplate> u = TemplateLibrary::open(moved, &err);
    CHECK(u.has_value());
    ProjectData fromZip;
    if (u) CHECK(TemplateLibrary::load(*u, &fromZip, &err));
    CHECK(TimelineOps::findClip(fromZip.timeline, 1)->mediaPath.startsWith(QDir(moved).absolutePath()));

    // Listed, found by id, name or path; start templates do not see the folder
    CHECK_EQ(int(TemplateLibrary::own().size()), 1);
    CHECK(TemplateLibrary::find("my short").has_value());
    CHECK(TemplateLibrary::find(t->dir).has_value());
    CHECK(!TemplateLibrary::find("nothing").has_value());
    CHECK(StartTemplates::own().isEmpty());
    // Saving again under the name: only with replace; the preview stays
    CHECK(!TemplateLibrary::save(d, "My Short", {}, &err));
    TemplateLibrary::SaveOptions again;
    again.replace = true;
    again.copyMedia = false;
    t = TemplateLibrary::save(d, "My Short", again, &err);
    CHECK(t.has_value());
    if (t) {
        CHECK(t->preview.endsWith("preview.png"));
        CHECK(!QDir(t->dir).exists("media"));
        ProjectData l;
        TemplateLibrary::load(*t, &l, &err);
        CHECK_EQ(TimelineOps::findClip(l.timeline, 1)->mediaPath, a); // not copied: where it was
        CHECK_EQ(int(l.media.size()), 3);
    }
    // Rename: the name and the folder; remove
    CHECK(TemplateLibrary::rename("My Short", "Gaming", &err));
    CHECK(TemplateLibrary::find("Gaming").has_value());
    CHECK_EQ(TemplateLibrary::find("Gaming")->id, QString("Gaming"));
    CHECK(!TemplateLibrary::remove("..", &err));
    CHECK(TemplateLibrary::remove("Gaming", &err));
    CHECK(TemplateLibrary::own().isEmpty());

    // Built-in ones: from their root, copied into the cache, listed first
    QTemporaryDir root;
    ProjectData bi = project();
    bi.templateInfo = TemplateInfo{"Built in", {}, {}, {}};
    QDir().mkpath(root.filePath("demo/media"));
    CHECK(ProjectFile::save(bi, root.filePath("demo/template.schneidi"), &err));
    touch(root.filePath("demo/preview.mp4"));
    TemplateLibrary::setBuiltInRoot(root.path());
    TemplateLibrary::save(d, "Zed", {}, &err);
    const QVector<ContentTemplate> list = TemplateLibrary::all();
    CHECK_EQ(int(list.size()), 2);
    if (list.size() == 2) {
        CHECK(list[0].builtIn);
        CHECK_EQ(list[0].name, QString("Built in"));
        CHECK(!list[0].dir.startsWith(root.path()));
        CHECK(QFileInfo::exists(list[0].preview));
        CHECK(!list[1].builtIn);
    }
    // A newer version of a built-in file with the same size replaces the cached copy
    bi.templateInfo->name = "Built 2x";
    CHECK(ProjectFile::save(bi, root.filePath("demo/template.schneidi"), &err));
    const QVector<ContentTemplate> updated = TemplateLibrary::builtIn();
    CHECK_EQ(int(updated.size()), 1);
    if (!updated.isEmpty()) CHECK_EQ(updated[0].name, QString("Built 2x"));
    CHECK(!TemplateLibrary::remove("demo", &err)); // not an own one
    {
        // A program update with a changed template of the very same size: the cached copy is refreshed
        const QString src = root.filePath("demo/template.schneidi");
        QFile f(src);
        CHECK(f.open(QIODevice::ReadOnly));
        QByteArray bytes = f.readAll();
        f.close();
        const qint64 size = bytes.size();
        bytes.replace("\"Built 2x\"", "\"Built on\"");
        CHECK_EQ(qint64(bytes.size()), size);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(bytes);
        f.close();
        CHECK(f.open(QIODevice::ReadWrite));
        f.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime);
        f.close();
        const QVector<ContentTemplate> again = TemplateLibrary::builtIn();
        CHECK_EQ(int(again.size()), 1);
        if (!again.isEmpty()) CHECK_EQ(again[0].name, QString("Built on"));
    }
    TemplateLibrary::setBuiltInRoot({});
    // The program's own (resources): listing them works (empty until package E fills assets/templates)
    for (const ContentTemplate& c : TemplateLibrary::builtIn()) CHECK(c.builtIn);
    QDir(TemplateLibrary::dir()).removeRecursively();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("template_fill");
    testFileRoundTrip();
    testList();
    testCheck();
    testClipFixed();
    testClipFlexible();
    testReframe();
    testText();
    testMusic();
    testLogo();
    testSequences();
    testEditor();
    testFixedKeepsLength();
    testStillGetsSound();
    testMusicFlexible();
    testTextSplit();
    testCompound();
    testEditorParts();
    testLibrary();
    return Check::result();
}
