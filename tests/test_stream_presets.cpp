// Import presets for files with several audio streams (docs/plan-0.8.md; OBS: "Mix" plus single streams plays
// everything twice): "mute the Mix stream when single streams exist" and rules per stream title (place / mute /
// leave out, volume, track name), applied wherever clips come from files (insert, F9–F12, drag from the viewer,
// multicam angles, CLI), learned from a clip in the timeline, kept in the settings, edited in a dialog.
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
    // Default presets change nothing
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
    StreamPresets p;
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
    f.ed.setStreamPresets({});
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
    StreamPresets p;
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
    const StreamPresets none = StreamPresets::load();
    CHECK(!none.muteMix && none.rules.isEmpty());
    StreamPresets p;
    p.muteMix = true;
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
    StreamPresets start;
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
    testMulticamAnglesFollowPresets();
    testLearnFromTimeline();
    testSaveLoadSettings();
    testDialogEditsRules();
    testCliFollowsPresets(dir.path());
    return Check::result();
}
