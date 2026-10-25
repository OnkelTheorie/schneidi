// Import presets for files with several audio streams (docs/archiv/plan-0.8.md; OBS: "Mix" plus single streams plays
// everything twice): "mute the Mix stream when single streams exist" (off by default: nothing is muted unless the user
// says so) and rules per stream title
// (place / mute / leave out, volume, track name), applied wherever clips come from files (insert, F9–F12, drag from
// the viewer, multicam angles, CLI), learned from a clip in the timeline, kept in the settings, edited in a dialog.
// A further copy of such a file (picture-in-picture, blurred background) never puts a stream on a track named after
// another stream and does not overwrite the first copy's sound; sound that already plays there comes in disabled.
#include "check.h"

#include "app/StreamPresetsDialog.h"
#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/Multicam.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/StreamPresets.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>

namespace {

using Action = StreamRule::Action;

MediaInfo obs(const QString& path, int length, const QStringList& titles, bool video = true)
{
    MediaInfo m;
    m.path = path;
    m.name = QFileInfo(path).fileName();
    m.length = length;
    m.hasVideo = video;
    m.hasAudio = !titles.isEmpty();
    for (const QString& t : titles) m.audioStreams << AudioStreamInfo{2, t};
    return m;
}

// The user's OBS layout (docs/dev/testen.md "Untertitel aus Audio (Whisper) testen": Mix, Spotify, Discord, System, Mikrofon)
const QStringList kObs{"Mix", "Spotify", "Discord", "System", "Mikrofon"};

StreamRule rule(const QString& title, Action a, double db = 0, const QString& track = {})
{
    StreamRule r;
    r.title = title;
    r.action = a;
    r.volumeDb = db;
    r.trackName = track;
    return r;
}

QString actions(const QVector<StreamRule>& plan)
{
    QString s;
    for (const StreamRule& r : plan)
        s += QString("%1%2:%3").arg(s.isEmpty() ? "" : " ", r.title,
                                    r.action == Action::Place ? "place" : r.action == Action::Mute ? "mute" : "skip");
    return s;
}

// Audio clips as "A1:s0 A2:s2(off) …" (track:stream, (off) = disabled, volume if not 0)
QString audio(const Timeline& tl)
{
    QString s;
    for (int i = 0; i < tl.audio.size(); ++i)
        for (const Clip& c : tl.audio[i].clips) {
            s += QString("%1A%2:s%3").arg(s.isEmpty() ? "" : " ").arg(i + 1).arg(c.audioStream);
            if (!c.enabled) s += "(off)";
            if (c.volumeDb != 0) s += QString("(%1dB)").arg(c.volumeDb);
        }
    return s;
}

// Audio clips that play at `frame`, with their track names: "A1 Mix:s0(off) A2 Spotify:s1 …"
QString audioAt(const Timeline& tl, int frame)
{
    QString s;
    for (int i = 0; i < tl.audio.size(); ++i)
        for (const Clip& c : tl.audio[i].clips)
            if (c.start <= frame && frame < c.end())
                s += QString("%1A%2 %3:s%4%5").arg(s.isEmpty() ? "" : " ").arg(i + 1).arg(tl.audio[i].name)
                         .arg(c.audioStream).arg(c.enabled ? "" : "(off)");
    return s;
}

void testMixTitleDetection()
{
    for (const char* yes : {"Mix", "mix", " MIX ", "Mix 1", "mix (alles)", "Mix-Spur", "Mix2"})
        CHECK(StreamPresets::isMixTitle(yes));
    for (const char* no : {"Mixer", "Remix", "Mikrofon", "", "Discord Mix"})
        CHECK(!StreamPresets::isMixTitle(no));
}

void testPlanSingleStreamAndUntitled()
{
    StreamPresets p;
    p.muteMix = true;
    p.rules << rule("Mix", Action::Skip);
    // One stream: never touched (it would be the only sound)
    CHECK_EQ(actions(p.plan(obs("/x/a.mp4", 10, {"Mix"}))), QString("Mix:place"));
    // Untitled streams: no title to match
    CHECK_EQ(actions(p.plan(obs("/x/b.mkv", 10, {"", ""}))), QString(":place :place"));
    // No audio: empty plan
    CHECK(p.plan(obs("/x/c.mp4", 10, {})).isEmpty());
    // No presets change nothing
    CHECK_EQ(actions(StreamPresets::none().plan(obs("/x/o.mkv", 10, kObs))),
             QString("Mix:place Spotify:place Discord:place System:place Mikrofon:place"));
    // The defaults mute nothing either (user decision 2026-10-25: muting is the user's own rule)
    CHECK(StreamPresets{} == StreamPresets::none());
    CHECK_EQ(actions(StreamPresets{}.plan(obs("/x/o.mkv", 10, kObs))),
             QString("Mix:place Spotify:place Discord:place System:place Mikrofon:place"));
}

void testMuteMixWhenSeparate()
{
    StreamPresets p;
    p.muteMix = true;
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, kObs))),
             QString("Mix:mute Spotify:place Discord:place System:place Mikrofon:place"));
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"Discord", "mix"}))), QString("Discord:place mix:mute"));
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"Mix", "Mix 2", "Mikro"}))), QString("Mix:mute Mix 2:mute Mikro:place"));
    // Only mixes, no single streams: nothing is muted
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"Mix", "Mix 2"}))), QString("Mix:place Mix 2:place"));
}

void testRulesByTitle()
{
    StreamPresets p = StreamPresets::none();
    p.rules << rule(" discord ", Action::Mute) << rule("MIKROFON", Action::Place, 3, "Mic") << rule("Spotify", Action::Skip);
    const QVector<StreamRule> plan = p.plan(obs("/x/o.mkv", 10, kObs));
    CHECK_EQ(actions(plan), QString("Mix:place Spotify:skip Discord:mute System:place Mikrofon:place"));
    CHECK_EQ(plan.value(4).volumeDb, 3.0);
    CHECK_EQ(plan.value(4).trackName, QString("Mic"));
    CHECK_EQ(plan.value(2).title, QString("Discord")); // the file's title, not the rule's spelling
    // A rule and the Mix switch together: the explicit rule wins
    p.muteMix = true;
    p.rules << rule("Mix", Action::Place, -6);
    const QVector<StreamRule> both = p.plan(obs("/x/o.mkv", 10, kObs));
    CHECK(both.value(0).action == Action::Place && both.value(0).volumeDb == -6);
    // Same title twice in a file: the rule applies to both
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"Discord", "Discord", "Mix"}))), QString("Discord:mute Discord:mute Mix:place"));
}

void testSkipNeverDropsAllStreams()
{
    StreamPresets p;
    p.rules << rule("A", Action::Skip) << rule("B", Action::Skip);
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"A", "B"}))), QString("A:mute B:mute"));
    CHECK_EQ(actions(p.plan(obs("/x/o.mkv", 10, {"A", "B", "C"}))), QString("A:skip B:skip C:place"));
}

struct Fixture {
    Project p;
    Selection sel;
    Editor ed{&p, &sel};
    Fixture()
    {
        p.addMedia(obs("/x/obs.mkv", 100, kObs));
        p.addMedia(obs("/x/plain.mp4", 100, {""}));
    }
    void usePresets()
    {
        StreamPresets s;
        s.muteMix = true;
        s.rules << rule("Spotify", Action::Skip) << rule("Mikrofon", Action::Place, 3, "Mic");
        ed.setStreamPresets(s);
    }
};

void testAddMediaAtAppliesPresets()
{
    {
        Fixture f; // without presets: every stream as before
        f.ed.setStreamPresets(StreamPresets::none());
        f.ed.addMediaAt({"/x/obs.mkv"}, 0);
        CHECK_EQ(audio(f.p.timeline()), QString("A1:s0 A2:s1 A3:s2 A4:s3 A5:s4"));
    }
    {
        Fixture f; // the defaults (an Editor without settings, a new user): nothing muted
        f.ed.addMediaAt({"/x/obs.mkv"}, 0);
        CHECK_EQ(audio(f.p.timeline()), QString("A1:s0 A2:s1 A3:s2 A4:s3 A5:s4"));
    }
    Fixture f;
    f.usePresets();
    const int steps = f.p.undoStack()->count();
    f.ed.addMediaAt({"/x/obs.mkv"}, 0);
    CHECK_EQ(f.p.undoStack()->count(), steps + 1);
    // Mix placed but off, Spotify left out (the others move up), Mikrofon +3 dB on a track named "Mic"
    CHECK_EQ(audio(f.p.timeline()), QString("A1:s0(off) A2:s2 A3:s3 A4:s4(3dB)"));
    const Timeline& tl = f.p.timeline();
    CHECK_EQ(tl.audio.value(0).name, QString("Mix"));
    CHECK_EQ(tl.audio.value(1).name, QString("Discord"));
    CHECK_EQ(tl.audio.value(3).name, QString("Mic"));
    const int link = tl.video.value(0).clips.value(0).linkId;
    for (int i = 0; i < 4; ++i) CHECK(link != 0 && tl.audio.value(i).clips.value(0).linkId == link);
    // Files with one stream are not touched
    f.ed.addMediaAt({"/x/plain.mp4"}, 200);
    CHECK(f.p.timeline().audio.value(0).clips.value(1).enabled);
    // Changing the presets later leaves the timeline alone
    const QString before = audio(f.p.timeline());
    f.ed.setStreamPresets(StreamPresets::none());
    CHECK_EQ(audio(f.p.timeline()), before);
}

void testSourceEditsApplyPresets()
{
    Fixture f;
    f.usePresets();
    // F10 from the source viewer
    CHECK(f.ed.sourceEdit(Editor::SourceEditMode::Overwrite, "/x/obs.mkv", 0, 0) > 0);
    CHECK_EQ(audio(f.p.timeline()), QString("A1:s0(off) A2:s2 A3:s3 A4:s4(3dB)"));
    // Dragging a range out of the viewer
    Fixture g;
    g.usePresets();
    g.ed.placeSourceRange("/x/obs.mkv", 10, 59, 100, 0);
    CHECK_EQ(audio(g.p.timeline()), QString("A1:s0(off) A2:s2 A3:s3 A4:s4(3dB)"));
    CHECK_EQ(g.p.timeline().audio.value(1).clips.value(0).in, 10);
}

void testSecondCopyKeepsTracksTrue()
{
    // A second copy of the same recording on V2 over the first one (blurred background, picture-in-picture; AI test
    // run 2026-10-25): "V2 -> A2" put its Mix on the first copy's "Spotify" track and overwrote the first copy's
    // single streams. Now it gets its own tracks named like its streams, the first copy stays as it was, and its
    // sound (the same moment again) comes in disabled. Same for the viewer drag/CLI and the media pool drop.
    const QString first = "A1 Mix:s0 A2 Spotify:s1 A3 Discord:s2 A4 System:s3 A5 Mikrofon:s4"; // defaults: all play
    for (bool pool : {false, true}) {
        Fixture f;
        const auto place = [&](int frame, int track) {
            if (pool) f.ed.addMediaAt({"/x/obs.mkv"}, frame, track);
            else f.ed.placeSourceRange("/x/obs.mkv", 0, 99, frame, track);
        };
        place(0, 0);
        CHECK_EQ(audioAt(f.p.timeline(), 50), first);
        const MediaInfo& m = *f.p.mediaInfo("/x/obs.mkv");
        CHECK_EQ(f.ed.streamTrackStart(f.p.timeline(), m, 1, 0, 100), 5);  // what the drop preview shows
        CHECK_EQ(f.ed.streamTrackStart(f.p.timeline(), m, 0, 0, 100), 0);  // over itself: the same tracks
        CHECK_EQ(f.ed.streamTrackStart(f.p.timeline(), m, 1, 200, 50), 0); // later: its named tracks are free there
        place(0, 1);
        const Timeline& tl = f.p.timeline();
        CHECK_EQ(audioAt(tl, 50), first + " A6 Mix:s0(off) A7 Spotify:s1(off) A8 Discord:s2(off) A9 System:s3(off) "
                                          "A10 Mikrofon:s4(off)");
        const Clip& copy = tl.video.value(1).clips.value(0);
        CHECK(copy.enabled && copy.linkId != 0 && copy.linkId != tl.video.value(0).clips.value(0).linkId);
        CHECK_EQ(TimelineOps::linkedGroup(tl, copy.id).size(), 6);
        CHECK_EQ(TimelineOps::linkedGroup(tl, tl.video.value(0).clips.value(0).id).size(), 6); // first copy whole
        // The same recording later on V2: the tracks named like its streams are free there, so they take it
        place(200, 1);
        CHECK_EQ(audioAt(f.p.timeline(), 250), first);
        CHECK_EQ(f.p.timeline().audio.size(), 10);
        // Overwriting the first copy with itself on V1 stays on its own tracks, with sound
        f.ed.placeSourceRange("/x/obs.mkv", 0, 49, 0, 0);
        CHECK_EQ(audioAt(f.p.timeline(), 20), first + " A6 Mix:s0(off) A7 Spotify:s1(off) A8 Discord:s2(off) "
                                                      "A9 System:s3(off) A10 Mikrofon:s4(off)");
    }
    // Only names of the file's own streams move it: a track the user named stays the target
    Fixture f;
    f.ed.renameTrack({TrackKind::Audio, 1}, "Voice");
    f.ed.placeSourceRange("/x/obs.mkv", 0, 99, 0, 0);
    CHECK_EQ(audioAt(f.p.timeline(), 50), QString("A1 Mix:s0 A2 Voice:s1 A3 Discord:s2 A4 System:s3 A5 Mikrofon:s4"));
}

void testRepeatedSound()
{
    // TimelineOps::repeatedSound: the same stream of the same file at the same source time elsewhere, over the whole
    // clip (pieces of a cut original count together)
    const auto clip = [](int id, const QString& path, int start, int in, int len, int stream = 0) {
        Clip c;
        c.id = id;
        c.mediaPath = path;
        c.start = start;
        c.in = in;
        c.out = in + len - 1;
        c.audioStream = stream;
        return c;
    };
    Timeline tl;
    tl.audio.resize(3);
    tl.audio[0].clips << clip(1, "/x/a.mkv", 0, 0, 50) << clip(2, "/x/a.mkv", 50, 50, 50);
    tl.audio[1].clips << clip(3, "/x/a.mkv", 10, 10, 80)    // the same moment, under 1 and 2
                      << clip(4, "/x/a.mkv", 100, 100, 20); // after the original
    tl.audio[2].clips << clip(5, "/x/a.mkv", 0, 5, 40)      // another moment of the file
                      << clip(6, "/x/a.mkv", 40, 40, 20, 1) // another stream
                      << clip(7, "/x/b.mkv", 60, 60, 20);   // another file
    CHECK_EQ(TimelineOps::repeatedSound(tl, {3, 4, 5, 6, 7}), QVector<int>({3}));
    CHECK(TimelineOps::repeatedSound(tl, {1, 2, 3}).isEmpty()); // the new clips never count for each other
    Timeline part = tl;
    part.audio[0].clips.removeLast(); // only half of it plays already: the rest would be missing
    CHECK(TimelineOps::repeatedSound(part, {3}).isEmpty());
    Timeline off = tl;
    off.audio[0].clips[1].enabled = false;
    CHECK(TimelineOps::repeatedSound(off, {3}).isEmpty());
    Timeline muted = tl;
    muted.audio[0].muted = true;
    CHECK(TimelineOps::repeatedSound(muted, {3}).isEmpty());
    Timeline fast = tl;
    fast.audio[1].clips[0].speed = 2.0;
    CHECK(TimelineOps::repeatedSound(fast, {3}).isEmpty());

    // In the editor: a one-stream video placed again over itself (V2) plays once
    Fixture f;
    f.ed.addMediaAt({"/x/plain.mp4"}, 0, 0);
    f.ed.placeSourceRange("/x/plain.mp4", 0, 99, 0, 1);
    f.ed.placeSourceRange("/x/plain.mp4", 20, 59, 0, 2); // another moment over it: both play
    CHECK_EQ(audioAt(f.p.timeline(), 10), QString("A1 :s0 A2 :s0(off) A3 :s0"));
    CHECK(f.p.timeline().video.value(1).clips.value(0).enabled);
    // F12 (place on top) of the same moment as well
    f.p.setMediaMarks("/x/plain.mp4", 30, 39);
    f.ed.sourceEdit(Editor::SourceEditMode::PlaceOnTop, "/x/plain.mp4", 0, 30);
    CHECK_EQ(audioAt(f.p.timeline(), 35), QString("A1 :s0 A2 :s0(off) A3 :s0 A4 :s0(off)"));
}

void testMulticamAnglesFollowPresets()
{
    Fixture f;
    f.usePresets();
    f.p.addMedia(obs("/x/pov.mp4", 100, {""}));
    const int seq = f.ed.createMulticamClip("M", {{"OBS", {{"/x/obs.mkv", 0}}}, {"POV", {{"/x/pov.mp4", 0}}}});
    const Sequence* s = f.p.sequence(seq);
    if (!CHECK(s)) return;
    CHECK(s->angles.value(0) == (MulticamAngle{"OBS", 0, {0, 1, 2, 3}}));
    CHECK(s->angles.value(1) == (MulticamAngle{"POV", 1, {4}}));
    CHECK_EQ(audio(s->timeline), QString("A1:s0(off) A2:s2 A3:s3 A4:s4(3dB) A5:s0"));
}

void testLearnFromTimeline()
{
    // The user sets up one OBS clip by hand, then "remember these track settings"
    Fixture f;
    f.ed.setStreamPresets(StreamPresets::none());
    f.ed.addMediaAt({"/x/obs.mkv"}, 0);
    const Timeline& tl = f.p.timeline();
    const int link = tl.video.value(0).clips.value(0).linkId;
    f.ed.modifyClips({tl.audio.value(0).clips.value(0).id}, "off", [](Clip& c) { c.enabled = false; });
    f.ed.setLinkedSelection(false);
    f.ed.setClipVolume(f.p.timeline().audio.value(2).clips.value(0).id, -6);
    f.ed.renameTrack({TrackKind::Audio, 3}, "Game");
    f.sel.set({f.p.timeline().audio.value(1).clips.value(0).id});
    f.ed.deleteSelection(); // Spotify deleted = not wanted
    const QVector<StreamRule> learned = StreamPresets::learn(f.p.timeline(), *f.p.mediaInfo("/x/obs.mkv"), link);
    QHash<QString, StreamRule> by;
    for (const StreamRule& r : learned) by[r.title] = r;
    CHECK(by.value("Mix").action == Action::Mute);
    CHECK(by.value("Spotify").action == Action::Skip);
    CHECK(by.value("Discord").action == Action::Place && by.value("Discord").volumeDb == -6);
    CHECK(by.value("System").trackName == "Game");
    CHECK(by.value("Mikrofon").action == Action::Place && by.value("Mikrofon").trackName.isEmpty());

    // Merge: same title (any case) replaced, others kept
    StreamPresets p = StreamPresets::none();
    p.rules << rule("mix", Action::Place) << rule("Kamera", Action::Mute);
    p.merge(learned);
    int mixes = 0;
    for (const StreamRule& r : p.rules) mixes += r.title.compare("mix", Qt::CaseInsensitive) == 0;
    CHECK_EQ(mixes, 1);
    CHECK_EQ(p.rules.size(), 6);
    // Applying the learned rules to the next file reproduces the setup
    Fixture g;
    g.ed.setStreamPresets(p);
    g.ed.addMediaAt({"/x/obs.mkv"}, 0);
    CHECK_EQ(audio(g.p.timeline()), QString("A1:s0(off) A2:s2(-6dB) A3:s3 A4:s4"));
    CHECK_EQ(g.p.timeline().audio.value(2).name, QString("Game"));
}

void testSaveLoadSettings()
{
    QSettings().remove("import/streamPresets");
    const StreamPresets defaults = StreamPresets::load();
    CHECK(!defaults.muteMix && defaults.rules.isEmpty());
    // Switched on by the user: stays on
    StreamPresets on;
    on.muteMix = true;
    on.save();
    CHECK(QSettings().contains("import/streamPresets"));
    CHECK(StreamPresets::load() == on);
    CHECK(!StreamPresets::fromJson({}).muteMix); // a missing value is the default
    StreamPresets p;
    p.rules << rule("Discord", Action::Mute) << rule("Mikrofon", Action::Place, 2.5, "Mic") << rule("Spotify", Action::Skip);
    p.save();
    CHECK(StreamPresets::load() == p);
    // Broken settings: defaults instead of a crash
    QSettings().setValue("import/streamPresets", "{not json");
    CHECK(StreamPresets::load() == StreamPresets{});
    QSettings().remove("import/streamPresets");
}

void testDialogEditsRules()
{
    StreamPresets start = StreamPresets::none();
    start.rules << rule("Discord", Action::Mute);
    StreamPresetsDialog dlg(start);
    auto* mix = dlg.findChild<QCheckBox*>("muteMix");
    auto* table = dlg.findChild<QTableWidget*>("rules");
    auto* add = dlg.findChild<QPushButton*>("addRule");
    auto* remove = dlg.findChild<QPushButton*>("removeRule");
    if (!CHECK(mix && table && add && remove)) return;
    CHECK(!mix->isChecked());
    CHECK_EQ(table->rowCount(), 1);
    CHECK_EQ(table->item(0, 0)->text(), QString("Discord"));
    mix->setChecked(true);
    add->click();
    CHECK_EQ(table->rowCount(), 2);
    table->item(1, 0)->setText(" Spotify ");
    auto* action = qobject_cast<QComboBox*>(table->cellWidget(1, 1));
    if (!CHECK(action)) return;
    action->setCurrentIndex(2); // place / mute / leave out
    add->click(); // empty row: dropped
    const StreamPresets out = dlg.presets();
    CHECK(out.muteMix);
    CHECK_EQ(out.rules.size(), 2);
    CHECK(out.rules.value(1).title == "Spotify" && out.rules.value(1).action == Action::Skip);
    table->setCurrentCell(0, 0);
    remove->click();
    CHECK_EQ(dlg.presets().rules.size(), 1);
    CHECK_EQ(dlg.presets().rules.value(0).title, QString("Spotify"));
}

void testCliRealRecording(const QString& dir)
{
    // A recording like the user's own recorder writes it (MKV, streams titled Mix, Discord, Mikrofon; the Mix = the
    // others together), cut by the CLI like the AI test run of 2026-10-25: append into a new 9:16 project with the
    // user's presets muting the Mix (off by default: the user turns it on), then the same range again on V2 as a
    // blurred background. It must play once: the timeline is as loud as the Mix stream alone (the Mix and its parts
    // together, or both copies, would be +6 LU).
    if (!Check::haveFfmpeg()) return;
    const QString mix = "0.2*sin(2*PI*440*t)+0.2*sin(2*PI*660*t)";
    const QString file = Check::makeMedia(
        dir + "/clip.mkv",
        {"-f", "lavfi", "-i", "color=c=gray:s=64x64:r=25:d=3", "-f", "lavfi", "-i", "aevalsrc=" + mix + ":s=48000:d=3",
         "-f", "lavfi", "-i", "aevalsrc=0.2*sin(2*PI*440*t):s=48000:d=3", "-f", "lavfi", "-i",
         "aevalsrc=0.2*sin(2*PI*660*t):s=48000:d=3", "-map", "0", "-map", "1", "-map", "2", "-map", "3", "-c:v", "ffv1",
         "-c:a", "pcm_s16le", "-ac", "2", "-metadata:s:a:0", "title=Mix", "-metadata:s:a:1", "title=Discord",
         "-metadata:s:a:2", "title=Mikrofon"});
    if (!CHECK(!file.isEmpty())) return;
    Check::initMlt();
    StreamPresets user;
    user.muteMix = true; // set in the import presets dialog, like the user does
    user.save();
    const QString path = dir + "/real.schneidi";
    auto run = [&](const QString& name, const QJsonObject& raw) {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, raw), ctx);
        } catch (const Cli::Error& e) {
            qWarning().noquote() << e.code << e.message;
            return QJsonObject{};
        }
    };
    auto load = [&] {
        ProjectData d;
        QString error;
        ProjectFile::load(path, &d, &error);
        return d.timeline;
    };
    auto lufs = [&](const QJsonObject& raw) {
        const QJsonObject r = run("loudness", raw);
        return r["loudness"]["integrated_lufs"].toDouble();
    };
    run("new", {{"project", path}, {"width", 1080}, {"height", 1920}, {"fps", 25}});
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"append","media":"%1"}])").arg(file)}});
    CHECK_EQ(audioAt(load(), 10), QString("A1 Mix:s0(off) A2 Discord:s1 A3 Mikrofon:s2"));
    const double once = lufs({{"source", file}, {"audio_stream", 0}});
    CHECK(once < -5 && once > -30);
    CHECK(std::abs(lufs({{"source", path}}) - once) < 0.3);
    run("edit", {{"project", path}, {"ops", QString(R"([{"op":"place","media":"%1","at":0,"track":"V2"}])").arg(file)}});
    CHECK_EQ(audioAt(load(), 10), QString("A1 Mix:s0(off) A2 Discord:s1 A3 Mikrofon:s2 A4 Mix:s0(off) A5 Discord:s1(off) "
                                          "A6 Mikrofon:s2(off)"));
    CHECK(std::abs(lufs({{"source", path}}) - once) < 0.3);
    // presets:false: every stream as in the file (a copy of a moment that plays already stays off)
    run("edit", {{"project", path},
                 {"ops", QString(R"([{"op":"clear"},{"op":"append","media":"%1","presets":false}])").arg(file)}});
    CHECK_EQ(audioAt(load(), 10), QString("A1 Mix:s0 A2 Discord:s1 A3 Mikrofon:s2"));
    QSettings().remove("import/streamPresets");
}

void testCliFollowsPresets(const QString& dir)
{
    // The CLI (AI editing) uses the same presets as the app; presets:false places every stream
    StreamPresets s;
    s.muteMix = true;
    s.save();
    const QString path = dir + "/cli.schneidi";
    {
        Project p;
        ProjectData d;
        d.timeline = emptyTimeline();
        p.load(d);
        p.addMedia(obs("/x/obs.mkv", 100, kObs));
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    auto edit = [&](const QString& ops) {
        const Cli::Command* cmd = Cli::find("edit");
        Cli::Context ctx;
        try {
            return cmd->run(Cli::normalize(*cmd, {{"project", path}, {"ops", ops}}), ctx);
        } catch (const Cli::Error& e) {
            qWarning().noquote() << e.code << e.message;
            return QJsonObject{};
        }
    };
    auto load = [&] {
        ProjectData d;
        QString error;
        ProjectFile::load(path, &d, &error);
        return d.timeline;
    };
    edit(R"([{"op":"keep","media":"/x/obs.mkv","ranges":[[0,50]]}])");
    CHECK_EQ(audio(load()), QString("A1:s0(off) A2:s1 A3:s2 A4:s3 A5:s4"));
    edit(R"([{"op":"clear"},{"op":"keep","media":"/x/obs.mkv","ranges":[[0,50]],"presets":false}])");
    CHECK_EQ(audio(load()), QString("A1:s0 A2:s1 A3:s2 A4:s3 A5:s4"));
    QSettings().remove("import/streamPresets");
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("stream-presets");
    QSettings().remove("import/streamPresets");
    QTemporaryDir dir;

    testMixTitleDetection();
    testPlanSingleStreamAndUntitled();
    testMuteMixWhenSeparate();
    testRulesByTitle();
    testSkipNeverDropsAllStreams();
    testAddMediaAtAppliesPresets();
    testSourceEditsApplyPresets();
    testSecondCopyKeepsTracksTrue();
    testRepeatedSound();
    testMulticamAnglesFollowPresets();
    testLearnFromTimeline();
    testSaveLoadSettings();
    testDialogEditsRules();
    testCliFollowsPresets(dir.path());
    testCliRealRecording(dir.path());
    return Check::result();
}
