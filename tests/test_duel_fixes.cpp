// SPDX-License-Identifier: AGPL-3.0-only
// What AI agents tripped over in the editing duels of 18.11.2026 (docs/testdaten/opus-duell-2026-11-17):
//  - a file with a single audio stream never lands on a muted track (a start template's muted "Mix" made a podcast
//    silent); `info` and `check` say when the whole mix is silent
//  - whisper's made-up lines ("Untertitelung des ZDF, 2020") are filtered; silence is told apart
//  - `check`: "21 characters/s, at most 21" no more; loudness range over 15 LU; no black bars for a transparent PNG
//  - track names in remove_track/track/mixer (a track named "Stimme 1" is no subtitle track ST…)
//  - a ripple head trim moves the clips of other tracks over the trimmed clip with its content (sfx on a kill)
//  - audio_streams on keep/append; the game streams of a recording for `short`
//  - the colour grade of a clip reaches its kill feed copy on a locked track
//  - render format "mp4" = h264
#include "check.h"
#include "timeline_guard.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/StreamPresets.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "core/Transcript.h"
#include "engine/BarDetect.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("%s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return QJsonObject{{"failed", e.code}, {"message", e.message}};
    }
}

QStringList checksOf(const QJsonObject& r)
{
    QStringList out;
    for (const QJsonValue& v : r["issues"].toArray()) out << v["check"].toString();
    return out;
}

QString joined(const QJsonArray& a)
{
    QStringList out;
    for (const QJsonValue& v : a) out << v.toString();
    return out.join(" | ");
}

Transcript::Word word(qint64 from, qint64 to, const QString& text) { return Transcript::Word{from, to, text}; }

Transcript::Levels levels(int ms, float db)
{
    Transcript::Levels l;
    l.db = QVector<float>(ms / l.frameMs, db);
    return l;
}

// --- whisper: made-up lines, silence
void testHallucinations()
{
    CHECK(Transcript::isSilent(levels(3000, -90)));
    CHECK(Transcript::isSilent({}));
    CHECK(!Transcript::isSilent(levels(3000, -30)));

    // The TV subtitle credits go wherever they are, with the year after them
    QVector<Transcript::Word> w{word(0, 400, "Hallo"),          word(400, 800, "zusammen."),
                                word(1000, 1500, "Untertitelung"), word(1500, 1700, "des"),
                                word(1700, 1900, "ZDF,"),          word(1900, 2200, "2020")};
    CHECK_EQ(Transcript::dropHallucinations(w, levels(3000, -20)), 4);
    CHECK_EQ(w.size(), 2);
    QVector<Transcript::Word> amara{word(0, 300, "Untertitel"), word(300, 500, "der"),
                                    word(500, 1200, "Amara.org-Community")};
    CHECK_EQ(Transcript::dropHallucinations(amara, levels(2000, -20)), 3);
    // The polite ending only where it is quiet: said for real it stays
    QVector<Transcript::Word> thanks{word(0, 300, "Thanks"), word(300, 500, "for"), word(500, 900, "watching!")};
    QVector<Transcript::Word> said = thanks;
    CHECK_EQ(Transcript::dropHallucinations(said, levels(2000, -20)), 0);
    CHECK_EQ(Transcript::dropHallucinations(thanks, levels(2000, -70)), 3);
    // Words that only look alike stay ("des ZDF" alone, "Untertitel" in a sentence)
    QVector<Transcript::Word> normal{word(0, 300, "Die"), word(300, 700, "Untertitel"), word(700, 900, "des"),
                                     word(900, 1200, "Films")};
    CHECK_EQ(Transcript::dropHallucinations(normal, levels(2000, -70)), 0);
}

// --- readable subtitles: the default is ~17 characters per second, given or not
void testReadableDefault()
{
    // Words at about 20 characters per second with short breaths between the sentences (under the 0.7 s pause)
    QVector<SubtitleCue> words;
    const QStringList text = QString("Also eine künstliche Intelligenz, die schneidet, ist das Ziel. Keine schwarze "
                                     "Kiste, sondern ein Kollege. Das klingt gut. Wir zeigen heute, wie es geht.")
                                 .split(' ');
    int at = 0;
    for (const QString& t : text) {
        SubtitleCue c;
        c.start = at;
        c.end = at + std::max(4, int(t.size() * 30 / 20)); // 30 fps
        c.text = t;
        c.words = {CueWord{0, c.end - c.start}};
        at = c.end + (t.endsWith('.') ? 12 : 1);
        words << c;
    }
    Subtitles::ReadableRules given;
    given.cps = 17;
    const QVector<SubtitleCue> a = Subtitles::readable(words, 30), b = Subtitles::readable(words, 30, given);
    CHECK_EQ(a.size(), b.size());
    for (int i = 0; i < std::min(a.size(), b.size()); ++i) CHECK_EQ(a[i].text, b[i].text);
    // Regrouping the result again keeps it (as the `subtitles` op on the track does)
    const QVector<SubtitleCue> again = Subtitles::readable(a, 30, given);
    if (!CHECK_EQ(again.size(), a.size()))
        for (const SubtitleCue& c : again) std::printf("   again: %s\n", qPrintable(c.text.simplified()));
}

// --- check: the speed of a subtitle says more than the rounded limit
void testSubtitleSpeedWording()
{
    Timeline tl;
    tl.video.resize(1);
    Clip v;
    v.id = 1;
    v.mediaPath = "/x/a.mp4";
    v.out = 249;
    tl.video[0].clips << v;
    SubtitleTrack st;
    SubtitleCue cue;
    cue.id = 2;
    cue.start = 0;
    cue.end = 49; // 1.96 s at 25 fps
    cue.text = QString(42, 'a');      // 21.4 characters per second: rounded it is the limit
    st.cues << cue;
    tl.subtitles << st;
    const ProjectFormat f;
    QString message;
    for (const ExportCheck::Issue& i : ExportCheck::timeline(tl, 0, 250, f))
        if (i.check == "subtitle_fast") message = i.message;
    CHECK(message.contains("21.4") || message.contains("21,4"));
    if (!CHECK(!message.contains("(21 ")) ) std::printf("   %s\n", qPrintable(message));
}

// --- placing: the only sound of a file onto an unmuted track
void testSingleStreamNotMuted()
{
    Project p;
    TimelineGuard guard{p};
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/podcast.mp4", "podcast.mp4", 400, true, true, false});
    p.edit("tracks", [](Timeline& tl) {
        tl.audio.resize(3);
        tl.audio[0].name = "Mix";
        tl.audio[0].muted = true;
        tl.audio[1].name = "Spotify";
    });
    ed.placeSourceRange("/x/podcast.mp4", 0, 99, 0, 0, 1.0, Editor::PlaceParts::Both);
    const Timeline& tl = p.timeline();
    CHECK(tl.audio[0].clips.isEmpty());
    // An unnamed unmuted track first (the named ones belong to other sound)
    CHECK_EQ(tl.audio[2].clips.size(), 1);
    CHECK(Cli::detail::silentMixNote(tl).isEmpty());
    // All tracks muted: a new one below
    p.edit("mute", [](Timeline& t) {
        for (Track& tr : t.audio) tr.muted = true;
    });
    CHECK(Cli::detail::silentMixNote(p.timeline()).contains("A3"));
    ed.placeSourceRange("/x/podcast.mp4", 0, 99, 200, 0, 1.0, Editor::PlaceParts::Both);
    CHECK_EQ(p.timeline().audio.size(), 4);
    if (p.timeline().audio.size() == 4) CHECK_EQ(p.timeline().audio[3].clips.size(), 1);
    // Disabled clips on an audible track: silent too, the note says so
    Timeline off = p.timeline();
    for (Track& t : off.audio) {
        t.muted = false;
        for (Clip& c : t.clips) c.enabled = false;
    }
    CHECK(Cli::detail::silentMixNote(off).contains("disabled"));
    // A solo track that holds nothing: the rest is not heard
    Timeline solo = p.timeline();
    for (Track& t : solo.audio) t.muted = false;
    solo.audio[0].solo = true;
    CHECK(Cli::detail::silentMixNote(solo).contains("solo"));
}

// --- ripple head trim: what lies over the clip on other tracks moves with its content
void testRippleHeadTrim()
{
    Timeline tl;
    tl.video.resize(1);
    tl.audio.resize(3);
    Clip v;
    v.id = 1;
    v.mediaPath = "/x/a.mp4";
    v.in = 0;
    v.out = 199; // 0..200
    tl.video[0].clips << v;
    Clip next = v;
    next.id = 2;
    next.start = 200;
    next.in = 300;
    next.out = 349;
    tl.video[0].clips << next;
    Clip sfx;
    sfx.id = 3;
    sfx.mediaPath = "/x/whoosh.wav";
    sfx.start = 120; // on a kill inside clip 1
    sfx.out = 9;
    tl.audio[2].clips << sfx;
    Clip later = sfx;
    later.id = 4;
    later.start = 230;
    tl.audio[2].clips << later;
    TimelineOps::TrimEdit e;
    e.kind = TimelineOps::TrimKind::Ripple;
    e.edge = TimelineOps::Edge::Start;
    e.ids = {1};
    CHECK_EQ(TimelineOps::clampTrimEdit(tl, e, 60, {}), 60);
    Timeline t = tl;
    TimelineOps::applyTrimEdit(t, e, 60, {});
    const Clip* c1 = TimelineOps::findClip(t, 1);
    if (CHECK(c1)) {
        CHECK_EQ(c1->start, 0);
        CHECK_EQ(c1->in, 60);
    }
    CHECK_EQ(TimelineOps::findClip(t, 2)->start, 140);
    CHECK_EQ(TimelineOps::findClip(t, 3)->start, 60); // with its kill (frame 120 of the file is at 60 now)
    CHECK_EQ(TimelineOps::findClip(t, 4)->start, 170);
    // A tail trim leaves what lies over the clip where it is
    e.edge = TimelineOps::Edge::End;
    t = tl;
    TimelineOps::applyTrimEdit(t, e, -60, {});
    CHECK_EQ(TimelineOps::findClip(t, 3)->start, 120);
    CHECK_EQ(TimelineOps::findClip(t, 4)->start, 170);
}

// --- the grade of a clip reaches its kill feed copy (on a locked track)
void testCopyGrade()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/cs2/a.mkv", "a.mkv", 400, true, true, false});
    int lead = 0, copy = 0;
    p.edit("setup", [&](Timeline& tl) {
        tl.video.resize(2);
        Clip v;
        v.id = lead = p.newClipId();
        v.mediaPath = "/x/cs2/a.mkv";
        v.out = 99;
        v.linkId = p.newLinkId();
        tl.video[0].clips << v;
        Clip k = v;
        k.id = copy = p.newClipId();
        k.keepRegion = "killfeed";
        tl.video[1].clips << k;
        tl.video[1].locked = true;
    });
    ed.setGradeLut({lead}, "/x/kino.cube");
    const auto lut = [&](int id) {
        const EffectInstance* e = EffectRegistry::instance(*TimelineOps::findClip(p.timeline(), id), "grade");
        return e ? e->params.value("lut").toString() : QString();
    };
    CHECK_EQ(lut(lead), QString("/x/kino.cube"));
    CHECK_EQ(lut(copy), QString("/x/kino.cube"));
    ed.setGradeValues({lead}, {{AnimParam::GradeSaturation, 62}}, 0, "Color");
    const Clip* k = TimelineOps::findClip(p.timeline(), copy);
    CHECK(k && Keys::staticValue(*k, AnimParam::GradeSaturation) == 62);
    ed.setGradeEnabled({lead}, false);
    CHECK(!EffectRegistry::instance(*TimelineOps::findClip(p.timeline(), copy), "grade")->enabled);
    // The copy alone on its locked track stays as it is (like every edit leaves it)
    ed.setGradeLut({copy}, "/x/other.cube");
    CHECK_EQ(lut(copy), QString("/x/kino.cube"));
}

// --- the game sound of a multi-stream recording
void testGameStreams()
{
    CHECK_EQ(StreamPresets::gameStreams({"Mix", "Spotify", "Discord", "System Output", "Mikrofon"}), QVector<int>{3});
    CHECK_EQ(StreamPresets::gameStreams({"Mix", "Game", "Mic"}), QVector<int>{1});
    CHECK(StreamPresets::gameStreams({"Mix", "Discord", "Mikrofon"}).isEmpty()); // no game stream: all stay
    CHECK(StreamPresets::gameStreams({"Stereo"}).isEmpty());
    CHECK(StreamPresets::gameStreams({"Game", "Desktop"}).isEmpty()); // every one is: nothing to leave out
}

// --- a transparent logo is no picture with black bars
void testTransparentLogo(const QString& dir)
{
    QImage logo(400, 400, QImage::Format_ARGB32);
    logo.fill(Qt::transparent);
    QPainter(&logo).fillRect(150, 150, 100, 100, Qt::white);
    const QString path = dir + "/logo.png";
    logo.save(path);
    const auto c = BarDetect::content(path);
    if (CHECK(c.has_value())) CHECK(!BarDetect::hasBars(*c));
    // The same picture without transparency (black around it) has them
    QImage flat(400, 400, QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter(&flat).fillRect(0, 100, 400, 200, Qt::white);
    flat.save(dir + "/flat.png");
    const auto f = BarDetect::content(dir + "/flat.png");
    if (CHECK(f.has_value())) CHECK(BarDetect::hasBars(*f));
}

// --- the commands on real (made-up) media
void testCommands(const QString& dir)
{
    const QString clip = Check::makeMedia(dir + "/podcast.mp4",
                                          {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=3", "-f", "lavfi", "-i",
                                           "sine=f=440:d=3", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
                                           "-shortest"});
    if (!CHECK(!clip.isEmpty())) return;

    // A template with a muted Mix track: the podcast's sound goes elsewhere, nothing is silent
    const QString project = dir + "/p.schneidi";
    run("new", {{"project", project}, {"media", QJsonArray{clip}}});
    QJsonObject r = run("edit", {{"project", project},
                                 {"ops", QJsonArray{QJsonObject{{"op", "track"}, {"track", "A1"}, {"name", "Mix"}, {"mute", true}},
                                                    QJsonObject{{"op", "append"}, {"media", clip}}}}});
    CHECK(!r.contains("failed"));
    r = run("info", {{"project", project}});
    CHECK(!joined(r["notes"].toArray()).contains("silent"));
    // Muted afterwards: info and check tell
    run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "track"}, {"track", "A2"}, {"mute", true}}}}});
    r = run("info", {{"project", project}});
    CHECK(joined(r["notes"].toArray()).contains("the whole mix is silent"));
    r = run("check", {{"source", project}});
    CHECK(checksOf(r).contains("mix_silent"));
    CHECK(r["errors"].toInt() >= 1);

    // Track names: mixer, track and remove_track; "Stimme 1" is no subtitle track
    r = run("edit", {{"project", project},
                     {"ops", QJsonArray{QJsonObject{{"op", "mixer"}, {"track", "mix"}, {"volume_db", -3}},
                                        QJsonObject{{"op", "track"}, {"track", "A2"}, {"name", "Stimme 1"}, {"mute", false}},
                                        QJsonObject{{"op", "track"}, {"track", "Stimme 1"}, {"color", "teal"}},
                                        QJsonObject{{"op", "remove_track"}, {"track", "Mix"}}}}});
    if (CHECK(!r.contains("failed"))) {
        r = run("info", {{"project", project}});
        const QJsonArray tracks = r["timeline"].toObject()["tracks"].toArray();
        QStringList names;
        for (const QJsonValue& t : tracks) names << t["name"].toString();
        CHECK(!names.contains("Mix"));
        CHECK(names.contains("Stimme 1"));
    }
    CHECK_EQ(run("edit", {{"project", project}, {"ops", QJsonArray{QJsonObject{{"op", "remove_track"}, {"track", "Nope"}}}}})
                 ["failed"].toString(),
             QString("NOT_FOUND"));

    // keep with audio_streams: only the chosen streams of a recording with several
    const QString game = Check::makeMedia(
        dir + "/game.mkv",
        {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=3", "-f", "lavfi", "-i", "sine=f=300:d=3", "-f", "lavfi", "-i",
         "sine=f=500:d=3", "-f", "lavfi", "-i", "sine=f=700:d=3", "-map", "0", "-map", "1", "-map", "2", "-map", "3",
         "-metadata:s:a:0", "title=Mix", "-metadata:s:a:1", "title=Discord", "-metadata:s:a:2", "title=System Output",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    if (CHECK(!game.isEmpty())) {
        const QString gp = dir + "/g.schneidi";
        run("new", {{"project", gp}, {"media", QJsonArray{game}}});
        r = run("edit", {{"project", gp},
                         {"ops", QJsonArray{QJsonObject{{"op", "keep"}, {"media", game},
                                                        {"ranges", QJsonArray{QJsonArray{"0s", "1s"}, QJsonArray{"2s", "3s"}}},
                                                        {"audio_streams", QJsonArray{"System Output"}}}}}});
        CHECK(!r.contains("failed"));
        Project p;
        ProjectData data;
        QString error;
        if (CHECK(ProjectFile::load(gp, &data, &error))) {
            p.load(data);
            int sound = 0, other = 0;
            for (const Track& t : p.timeline().audio)
                for (const Clip& c : t.clips) {
                    if (c.audioStream == 2 && c.enabled) ++sound;
                    else ++other;
                }
            CHECK_EQ(sound, 2);
            CHECK_EQ(other, 0);
        }
    }

    // render format mp4 = h264 in MP4
    const QString out = dir + "/out";
    r = run("render", {{"project", project}, {"out", out}, {"format", "mp4"}, {"to", "1s"}});
    if (CHECK(!r.contains("failed"))) {
        CHECK_EQ(r["format"].toString(), QString("h264"));
        CHECK(r["out"].toString().endsWith(".mp4"));
        CHECK(joined(r["notes"].toArray()).contains("format mp4 read as h264"));
    }

    // Loudness range: 5 s whisper, 5 s loud
    const QString wide = Check::makeMedia(dir + "/wide.wav",
                                          {"-f", "lavfi", "-i", "sine=f=440:d=10", "-af",
                                           "volume='if(lt(t,5),0.12,1)':eval=frame"});
    if (CHECK(!wide.isEmpty())) {
        r = run("check", {{"source", wide}});
        CHECK(checksOf(r).contains("loudness_range"));
    }
    r = run("check", {{"source", clip}});
    CHECK(!checksOf(r).contains("loudness_range"));
}

// --- `<command> --help` is the command's help (it said "missing value for --help")
void testHelpFlag()
{
    const QString cli = Check::cliPath();
    if (cli.isEmpty() || !QFileInfo::exists(cli)) {
        std::printf("schneidi-cli not found: --help not checked\n");
        return;
    }
    for (const QStringList& args : {QStringList{"transcribe", "--help"}, QStringList{"edit", "-h"},
                                    QStringList{"new", "x.schneidi", "--help"}, QStringList{"help", "--help"},
                                    QStringList{"template", "--help"}}) {
        QProcess p;
        p.start(cli, args);
        if (!CHECK(p.waitForFinished(60000))) continue;
        const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
        if (!CHECK(o["ok"].toBool())) std::printf("   %s: %s\n", qPrintable(args.join(' ')), qPrintable(QJsonDocument(o).toJson()));
        if (args.first() != "help") CHECK(QJsonDocument(o).toJson().contains("\"params\"") || o.contains("command"));
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("duel_fixes");
    testHallucinations();
    testReadableDefault();
    testSubtitleSpeedWording();
    testSingleStreamNotMuted();
    testRippleHeadTrim();
    testCopyGrade();
    testGameStreams();
    QTemporaryDir dir;
    qputenv("XDG_DATA_HOME", dir.filePath("data").toUtf8()); // backups of the test projects
    testTransparentLogo(dir.path());
    testHelpFlag();
    if (!Check::haveFfmpeg()) return Check::result();
    Check::initEngine();
    testCommands(dir.path());
    return Check::result();
}
