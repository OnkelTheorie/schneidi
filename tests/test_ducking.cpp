// SPDX-License-Identifier: AGPL-3.0-only
// Ducking in the app (docs/archiv/plan-0.8.md): the logic of the CLI op `duck` moves into the core (sound ranges from
// per-frame peaks, volume keyframes with attack/release/hold), the Editor applies it as one undo step, the analysis
// finds the voice on real files, the CLI gives the same keyframes as the app, and the dialog (music = selection,
// "lower while there is sound on" = track checkboxes, CLI defaults, remembers the last choice).
#include "check.h"

#include "app/DuckingDialog.h"
#include "cli/Commands.h"
#include "core/Ducking.h"
#include "core/Editor.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/AudioAnalysis.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <clocale>
#include <cmath>

namespace {

using Ranges = QVector<QPair<int, int>>;

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

std::vector<float> peaks(int frames, const Ranges& loud, float level = 0.3f, float floor = 0.001f)
{
    std::vector<float> v(size_t(frames), floor);
    for (const auto& [a, b] : loud)
        for (int i = a; i < b && i < frames; ++i) v[size_t(i)] = level;
    return v;
}

QString keys(const Clip& c)
{
    QString s;
    for (const Keyframe& k : c.keys.value(AnimParam::Volume))
        s += QString("%1%2:%3").arg(s.isEmpty() ? "" : " ").arg(k.frame - c.in).arg(k.value);
    return s;
}

Clip music(int start, int length, double db = 0)
{
    Clip c;
    c.id = 1;
    c.mediaPath = "/x/music.mp3";
    c.start = start;
    c.out = length - 1;
    c.volumeDb = db;
    return c;
}

void testDefaults()
{
    // Same as the CLI op: -12 dB, threshold -35 dBFS, attack 0.15 s, release 0.4 s, hold 0.5 s
    const Ducking::Params p = Ducking::defaults(25);
    CHECK_EQ(p.amountDb, -12.0);
    CHECK_EQ(p.thresholdDb, -35.0);
    CHECK_EQ(p.attack, 4);
    CHECK_EQ(p.release, 10);
    CHECK_EQ(p.hold, 13);
    const Ducking::Params q = Ducking::defaults(60);
    CHECK(q.attack == 9 && q.release == 24 && q.hold == 30);
}

void testSoundRanges()
{
    const Ducking::Params p = Ducking::defaults(25);
    // Short pause (5 frames) inside hold+attack+release is bridged, the long one is not; `first` shifts the frames
    CHECK_EQ(Ducking::soundRanges(peaks(200, {{10, 20}, {25, 30}, {100, 110}}), 0, p), Ranges({{10, 30}, {100, 110}}));
    CHECK_EQ(Ducking::soundRanges(peaks(200, {{10, 20}}), 50, p), Ranges({{60, 70}}));
    CHECK(Ducking::soundRanges(peaks(200, {}), 0, p).isEmpty());
    CHECK(Ducking::soundRanges({}, 0, p).isEmpty());
    CHECK_EQ(Ducking::soundRanges(peaks(40, {{0, 40}}), 0, p), Ranges({{0, 40}}));
    // Exactly at the threshold counts as sound; just below not
    const float at = float(std::pow(10.0, -35.0 / 20.0));
    CHECK_EQ(Ducking::soundRanges(peaks(50, {{5, 6}}, at), 0, p), Ranges({{5, 6}}));
    CHECK(Ducking::soundRanges(peaks(50, {{5, 6}}, at * 0.99f), 0, p).isEmpty());
    // Lower threshold: the quiet floor counts
    Ducking::Params low = p;
    low.thresholdDb = -70;
    CHECK_EQ(Ducking::soundRanges(peaks(30, {}), 0, low), Ranges({{0, 30}}));
}

void testApplyKeys()
{
    const Ducking::Params p = Ducking::defaults(25); // attack 4, release 10
    // Clip 100..199 at -3 dB, sound 130..149: ramp down before, back up after (clip frames)
    Clip c = music(100, 100, -3);
    CHECK(Ducking::apply(c, {{130, 150}}, p));
    CHECK_EQ(keys(c), QString("26:-3 30:-15 50:-15 60:-3"));
    CHECK(near(Keys::valueAt(c, AnimParam::Volume, 40), -15, 1e-9));
    CHECK(near(Keys::valueAt(c, AnimParam::Volume, 80), -3, 1e-9));
    // Sound already running at the clip start / still running at its end: starts / ends low
    Clip d = music(100, 100);
    CHECK(Ducking::apply(d, {{90, 110}, {190, 260}}, p));
    CHECK_EQ(keys(d), QString("0:-12 10:-12 20:0 86:0 90:-12 99:-12"));
    // No sound on the clip: old ducking keys go (running it again after editing), the volume stays
    Clip e = music(100, 100, -2);
    Ducking::apply(e, {{130, 150}}, p);
    CHECK(!Ducking::apply(e, {{400, 420}}, p));
    CHECK(!Keys::animated(e, AnimParam::Volume));
    CHECK_EQ(e.volumeDb, -2.0);
    // Never below -∞ (-60 dB)
    Ducking::Params deep = p;
    deep.amountDb = -80;
    Clip f = music(0, 100, -10);
    Ducking::apply(f, {{40, 50}}, deep);
    CHECK(near(Keys::valueAt(f, AnimParam::Volume, 45), kMinVolumeDb, 1e-9));
    // Trimmed clip (in = 20): keyframes stay at the right timeline frames
    Clip g = music(100, 100);
    g.in = 20;
    g.out = 119;
    Ducking::apply(g, {{130, 150}}, p);
    CHECK_EQ(keys(g), QString("26:0 30:-12 50:-12 60:0"));
}

void testEditorDuckOneUndoStep()
{
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/voice.wav", "voice.wav", 300, false, true, false});
    p.addMedia({"/x/music.mp3", "music.mp3", 300, false, true, false});
    p.addMedia({"/x/music2.mp3", "music2.mp3", 300, false, true, false});
    ed.addMediaAt({"/x/voice.wav"}, 0, 0);
    ed.addMediaAt({"/x/music.mp3"}, 0, 1);
    ed.addTrack(TrackKind::Audio, 2);
    ed.addMediaAt({"/x/music2.mp3"}, 0, 2);
    const int m1 = p.timeline().audio[1].clips[0].id, m2 = p.timeline().audio[2].clips[0].id;
    ed.toggleTrackLock({TrackKind::Audio, 2});
    const int steps = p.undoStack()->count();
    const QString before = ProjectFile::toJson(p.data(), "/x/p.schneidi");
    CHECK_EQ(ed.duck({m1, m2}, {{50, 75}, {150, 175}}, Ducking::defaults(25)), 1); // the locked one stays
    CHECK_EQ(p.undoStack()->count(), steps + 1);
    CHECK_EQ(p.undoStack()->undoText(), T("Musik absenken"));
    CHECK(Keys::animated(p.timeline().audio[1].clips[0], AnimParam::Volume));
    CHECK(!Keys::animated(p.timeline().audio[2].clips[0], AnimParam::Volume));
    p.undoStack()->undo();
    CHECK_EQ(QString(ProjectFile::toJson(p.data(), "/x/p.schneidi")), before);
    CHECK_EQ(ed.duck({}, {{50, 75}}, Ducking::defaults(25)), 0);
    CHECK_EQ(ed.duck({m1}, {}, Ducking::defaults(25)), 0); // nothing to duck under: no step
    CHECK_EQ(p.undoStack()->index(), steps); // count() keeps the undone step; no new step = index stays
}

// ---- Real sound ----

QString g_voice, g_music;

Timeline voiceAndMusic(int* musicId)
{
    Timeline tl = emptyTimeline();
    Clip v;
    v.id = 1;
    v.mediaPath = g_voice;
    v.out = 249;
    tl.audio[0].clips << v;
    Clip m = v;
    m.id = 2;
    m.mediaPath = g_music;
    tl.audio[1].clips << m;
    tl.audio[1].name = "Musik";
    *musicId = 2;
    return tl;
}

void testAnalyzeRealSound()
{
    int id = 0;
    const Timeline tl = voiceAndMusic(&id);
    const auto ranges = AudioAnalysis::soundRanges(ProjectFormat{}, tl, {0}, Ducking::defaults(25));
    if (!CHECK(ranges && ranges->size() == 2)) return;
    // Voice at 2–3 s and 6–7 s (frames 50–75, 150–175)
    CHECK(std::abs((*ranges)[0].first - 50) <= 2 && std::abs((*ranges)[0].second - 75) <= 2);
    CHECK(std::abs((*ranges)[1].first - 150) <= 2 && std::abs((*ranges)[1].second - 175) <= 2);
    // The music's own sound does not count (only the 'under' tracks are measured)
    const auto onlyMusic = AudioAnalysis::soundRanges(ProjectFormat{}, tl, {1}, Ducking::defaults(25));
    CHECK(onlyMusic && onlyMusic->size() == 1 && onlyMusic->value(0) == qMakePair(0, 250));
    CHECK(!AudioAnalysis::soundRanges(ProjectFormat{}, tl, {}, Ducking::defaults(25)).has_value() ||
          AudioAnalysis::soundRanges(ProjectFormat{}, tl, {}, Ducking::defaults(25))->isEmpty());
}

void testCliDuckMatchesEditor(const QString& dir)
{
    int musicId = 0;
    const Timeline tl = voiceAndMusic(&musicId);
    // App: analysis + Editor
    Project p;
    ProjectData d;
    d.timeline = tl;
    d.lastClipId = 2;
    p.load(d);
    p.addMedia({g_voice, "voice.wav", 250, false, true, false});
    p.addMedia({g_music, "music.wav", 250, false, true, false});
    Selection sel;
    Editor ed(&p, &sel);
    const auto ranges = AudioAnalysis::soundRanges(p.format(), p.renderTimeline(), {0}, Ducking::defaults(25));
    if (!CHECK(ranges)) return;
    ed.duck({musicId}, *ranges, Ducking::defaults(25));
    const Clip* app = TimelineOps::findClip(p.timeline(), musicId);
    // CLI: same project, op duck
    const QString path = dir + "/duck.schneidi";
    {
        Project q;
        q.load(d);
        q.addMedia({g_voice, "voice.wav", 250, false, true, false});
        q.addMedia({g_music, "music.wav", 250, false, true, false});
        QString error;
        CHECK(ProjectFile::save(q.data(), path, &error));
    }
    const Cli::Command* cmd = Cli::find("edit");
    Cli::Context ctx;
    try {
        const QJsonObject r = cmd->run(
            Cli::normalize(*cmd, {{"project", path}, {"ops", R"([{"op":"duck","track":"A2","under":"A1"}])"}}), ctx);
        // The note says how long the sound counts (one range over everything = the music only lower)
        const QString note = r["notes"].toArray().at(0).toString();
        CHECK(note.contains("sound range") && note.contains(" s above threshold_db -35"));
    } catch (const Cli::Error& e) {
        CHECK(false);
        qWarning().noquote() << e.code << e.message;
    }
    ProjectData back;
    QString err;
    if (!CHECK(ProjectFile::load(path, &back, &err)) || !CHECK(app)) return;
    const Clip* cli = TimelineOps::findClip(back.timeline, musicId);
    CHECK(cli && keys(*cli) == keys(*app));
    CHECK(!keys(*app).isEmpty());
    // remove:true takes it off again (Studio page "Aus"): the keys go, the clip volume stays; no 'under' needed
    try {
        const QJsonObject r = cmd->run(
            Cli::normalize(*cmd, {{"project", path}, {"ops", R"([{"op":"duck","track":"A2","remove":true}])"}}), ctx);
        CHECK(r["notes"].toArray().at(0).toString().contains("of 1 clip"));
    } catch (const Cli::Error& e) {
        CHECK(false);
        qWarning().noquote() << e.code << e.message;
    }
    ProjectData off;
    CHECK(ProjectFile::load(path, &off, &err));
    const Clip* offClip = TimelineOps::findClip(off.timeline, musicId);
    CHECK(offClip && keys(*offClip).isEmpty() && offClip->volumeDb == cli->volumeDb);
    // by: a list (["A1"]) like 'under'; without it the other audible tracks (here A1) duck it - the same curve
    for (const char* ops : {R"([{"op":"duck","track":"A2","by":["A1"]}])", R"([{"op":"duck","track":"A2"}])"}) {
        try {
            const QJsonObject r = cmd->run(Cli::normalize(*cmd, {{"project", path}, {"ops", ops}}), ctx);
            CHECK(r["notes"].toArray().at(0).toString().contains("of A1"));
        } catch (const Cli::Error& e) {
            CHECK(false);
            qWarning().noquote() << ops << e.code << e.message;
        }
        ProjectData by;
        CHECK(ProjectFile::load(path, &by, &err));
        const Clip* byClip = TimelineOps::findClip(by.timeline, musicId);
        CHECK(byClip && keys(*byClip) == keys(*app));
    }
    // 'by' and 'under' together, a track that is not there: refused
    for (const char* ops : {R"([{"op":"duck","track":"A2","by":["A1"],"under":"A1"}])",
                            R"([{"op":"duck","track":"A2","by":["A7"]}])"}) {
        bool refused = false;
        try {
            cmd->run(Cli::normalize(*cmd, {{"project", path}, {"ops", ops}}), ctx);
        } catch (const Cli::Error& e) {
            refused = e.code == "BAD_ARGUMENT";
        }
        CHECK(refused);
    }
}

// Music ducked under speech from start to end (a podcast without pauses) has no key at its own level: a new volume
// moves the curve from the clip's volume, not from the ducked level (an AI lowering the music to -8 from -4 made it
// 8 dB louder: the flat -16 became the "loudest key")
void testVolumeOfDuckedClip(const QString& dir)
{
    Clip c = music(0, 100, -4);
    Ducking::apply(c, {{0, 100}}, Ducking::defaults(25));
    CHECK_EQ(keys(c), QString("0:-16 99:-16"));
    Clip lower = c;
    CHECK_EQ(Keys::setVolumeLevel(lower, -8), -4.0);
    CHECK_EQ(keys(lower), QString("0:-20 99:-20"));
    CHECK_EQ(lower.volumeDb, -8.0); // moves along: a new ducking dips from it
    // Partly ducked: the loudest key is the clip's level as before; a drawn curve above the static volume too
    Clip part = music(0, 100, -4);
    Ducking::apply(part, {{40, 60}}, Ducking::defaults(25));
    Keys::setVolumeLevel(part, -8);
    CHECK_EQ(Keys::valueAt(part, AnimParam::Volume, 0), -8.0);
    CHECK_EQ(Keys::valueAt(part, AnimParam::Volume, 50), -20.0);
    Clip drawn = music(0, 100);
    Keys::setKey(drawn, AnimParam::Volume, 0, -30);
    Keys::setKey(drawn, AnimParam::Volume, 50, -10);
    Keys::setVolumeLevel(drawn, -6);
    CHECK_EQ(keys(drawn), QString("0:-26 50:-6"));

    // Normalize in the app and the CLI op `volume` use it
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    d.timeline.audio[0].clips << c;
    d.lastClipId = 1;
    p.load(d);
    Selection sel;
    Editor ed(&p, &sel);
    ed.normalizeAudio({{1, -2.0}}, -6.0, false); // the file -2 -> the clip at -4: as it is
    CHECK_EQ(keys(*TimelineOps::findClip(p.timeline(), 1)), QString("0:-16 99:-16"));
    const QString path = dir + "/ducked.schneidi";
    QString error;
    if (!CHECK(ProjectFile::save(d, path, &error))) return;
    const Cli::Command* cmd = Cli::find("edit");
    Cli::Context ctx;
    try {
        const QJsonObject r = cmd->run(
            Cli::normalize(*cmd, {{"project", path}, {"ops", R"([{"op":"volume","clips":[1],"db":-8}])"}}), ctx);
        CHECK(r["notes"].toArray().at(0).toString().contains("moved down by 4.0 dB"));
    } catch (const Cli::Error& e) {
        CHECK(false);
        qWarning().noquote() << e.code << e.message;
    }
    ProjectData back;
    if (!CHECK(ProjectFile::load(path, &back, &error))) return;
    const Clip* saved = TimelineOps::findClip(back.timeline, 1);
    if (CHECK(saved)) {
        CHECK_EQ(keys(*saved), QString("0:-20 99:-20"));
        CHECK_EQ(saved->volumeDb, -8.0);
    }
}

// ---- Dialog ----

void testDialogDefaultsAndTracks()
{
    QSettings().remove("ducking");
    Project p;
    Selection sel;
    Editor ed(&p, &sel);
    p.addMedia({"/x/obs.mkv", "obs.mkv", 300, true, true, false, -1, -1, 0, {}, {},
                {{2, "Mix"}, {2, "Discord"}, {2, "Mikrofon"}}});
    p.addMedia({"/x/music.mp3", "music.mp3", 300, false, true, false});
    ed.addMediaAt({"/x/obs.mkv"}, 0, 0);   // A1 Mix, A2 Discord, A3 Mikrofon
    ed.addTrack(TrackKind::Audio, 3);
    ed.addMediaAt({"/x/music.mp3"}, 0, 3); // A4
    const int musicId = p.timeline().audio[3].clips[0].id;

    DuckingDialog dlg(&ed, {musicId});
    CHECK(dlg.findChild<QCheckBox*>("under_A1") && dlg.findChild<QCheckBox*>("under_A2") && dlg.findChild<QCheckBox*>("under_A3"));
    CHECK(!dlg.findChild<QCheckBox*>("under_A4")); // the music's own track
    auto* discord = dlg.findChild<QCheckBox*>("under_A2");
    if (!CHECK(discord)) return;
    CHECK(discord->text().contains("Discord")); // track names (stream titles) shown
    auto* ok = dlg.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
    CHECK(!discord->isChecked());
    CHECK(!ok->isEnabled()); // nothing to duck under yet
    discord->setChecked(true);
    dlg.findChild<QCheckBox*>("under_A3")->setChecked(true);
    CHECK(ok->isEnabled());
    CHECK_EQ(dlg.underTracks(), QVector<int>({1, 2}));
    const Ducking::Params def = Ducking::defaults(p.fps());
    const Ducking::Params shown = dlg.params();
    CHECK(shown.amountDb == def.amountDb && shown.thresholdDb == def.thresholdDb && shown.attack == def.attack
          && shown.release == def.release && shown.hold == def.hold);
    auto* amount = dlg.findChild<QLineEdit*>("amount");
    if (!CHECK(amount && dlg.findChild<QLineEdit*>("threshold") && dlg.findChild<QLineEdit*>("attack")
               && dlg.findChild<QLineEdit*>("release") && dlg.findChild<QLineEdit*>("hold")))
        return;
    amount->selectAll();
    QTest::keyClicks(amount, "-18");
    QTest::keyClick(amount, Qt::Key_Return);
    CHECK_EQ(dlg.params().amountDb, -18.0);
    dlg.accept();

    // Next time: the same tracks (by name) and settings
    DuckingDialog again(&ed, {musicId});
    CHECK(again.findChild<QCheckBox*>("under_A2")->isChecked() && again.findChild<QCheckBox*>("under_A3")->isChecked());
    CHECK(!again.findChild<QCheckBox*>("under_A1")->isChecked());
    CHECK_EQ(again.params().amountDb, -18.0);
    QSettings().remove("ducking");

    // Music = nothing usable (only video selected): the dialog says so and cannot be confirmed
    DuckingDialog none(&ed, {});
    CHECK(!none.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled());
}

} // namespace

// Game sound never goes quiet: its threshold comes from its own loud moments (2 dB under the 90th percentile of the
// frames with sound, -35..-6 dBFS); silence (below -70) does not count; both range lists as one
void testGameThreshold()
{
    std::vector<float> v(1000, float(std::pow(10.0, -25 / 20.0))); // steps, ambience: -25 dBFS
    for (int i = 0; i < 1000; i += 10) v[size_t(i)] = float(std::pow(10.0, -6 / 20.0)); // a shot every 10th frame
    for (int i = 1; i < 1000; i += 10) v[size_t(i)] = float(std::pow(10.0, -6 / 20.0));
    CHECK(near(Ducking::gameThreshold(v), -8, 0.05)); // the loudest tenth at -6: from -8
    std::vector<float> withGaps = v;
    withGaps.resize(3000, 0.f); // two thirds silence: the same
    CHECK(near(Ducking::gameThreshold(withGaps), -8, 0.05));
    CHECK_EQ(Ducking::gameThreshold(std::vector<float>(100, 0.f)), -35.0);           // no sound
    CHECK_EQ(Ducking::gameThreshold(std::vector<float>(100, 0.001f)), -35.0);        // quiet: never under -35
    CHECK_EQ(Ducking::gameThreshold(std::vector<float>(100, 1.f)), -6.0);            // never above -6
    CHECK_EQ(Ducking::unite({{0, 10}, {50, 60}}, {{12, 20}, {100, 110}}, 5), Ranges({{0, 20}, {50, 60}, {100, 110}}));
    CHECK_EQ(Ducking::unite({}, {{5, 9}}, 5), Ranges({{5, 9}}));
}

// op duck's default threshold 'auto': the same sound - ambience at about -24 dBFS all the time, four shots - is game
// sound in a game's folder (cs2/…: ducked only under the shots) and talk elsewhere (-35 dBFS: every sound counts, as
// for speech); a given threshold_db applies as before. (CS2 highlights: -35 counted 43 of 51 s as loud.)
void testDuckGameSound(const QString& dir)
{
    QDir().mkpath(dir + "/cs2");
    const QStringList sound{"-f", "lavfi", "-i",
                            "aevalsrc=0.06*(random(0)*2-1)+(between(t\\,1\\,1.4)+between(t\\,3\\,3.4)+between(t\\,5\\,5.4)+"
                            "between(t\\,7\\,7.4))*0.9*sin(2*PI*150*t)"
                            ":s=48000:d=10",
                            "-ac", "2"};
    const QString game = Check::makeMedia(dir + "/cs2/cs2_clip_round.wav", sound);
    const QString talk = Check::makeMedia(dir + "/room.wav", sound);
    if (!CHECK(!game.isEmpty() && !talk.isEmpty())) return;
    // streams > 0: the file has that many untitled audio streams, the clip plays `stream`
    const auto covered = [&](const QString& file, const char* ops, int stream = 0, int streams = 0) {
        int id = 0;
        Timeline tl = voiceAndMusic(&id);
        tl.audio[0].clips[0].mediaPath = file;
        tl.audio[0].clips[0].audioStream = stream;
        ProjectData d;
        d.timeline = tl;
        d.lastClipId = 2;
        Project q;
        q.load(d);
        MediaInfo media{file, QFileInfo(file).fileName(), 250, false, true, false};
        media.audioStreams.resize(streams);
        q.addMedia(media);
        q.addMedia({g_music, "music.wav", 250, false, true, false});
        const QString path = dir + "/game-duck.schneidi";
        QString error;
        CHECK(ProjectFile::save(q.data(), path, &error));
        Cli::Context ctx;
        try {
            const Cli::Command* cmd = Cli::find("edit");
            const QJsonObject r = cmd->run(Cli::normalize(*cmd, {{"project", path}, {"ops", ops}}), ctx);
            const QString note = r["notes"].toArray().at(0).toString();
            static const QRegularExpression s(R"(: ([0-9.]+) s of)");
            const auto m = s.match(note);
            std::printf("       %s\n", qPrintable(note));
            return std::make_pair(m.hasMatch() ? m.captured(1).toDouble() : -1.0, note);
        } catch (const Cli::Error& e) {
            CHECK(false);
            qWarning().noquote() << e.code << e.message;
            return std::make_pair(-1.0, QString());
        }
    };
    const auto [gameS, gameNote] = covered(game, R"([{"op":"duck","track":"A2","under":"A1"}])");
    CHECK(gameNote.contains("auto: the loud moments of the game sound A1"));
    CHECK(gameS > 1 && gameS < 3.5); // the four shots (0.4 s each), not the ambience
    const auto [talkS, talkNote] = covered(talk, R"([{"op":"duck","track":"A2","under":"A1"}])");
    CHECK(talkNote.contains("threshold_db -35") && !talkNote.contains("auto"));
    CHECK(talkS > 9);
    const auto [fixedS, fixedNote] = covered(game, R"([{"op":"duck","track":"A2","under":"A1","threshold_db":-35}])");
    CHECK(fixedS > 9);
    CHECK(covered(game, R"([{"op":"duck","track":"A2","under":"A1","threshold_db":"auto"}])").first == gameS);

    // A game recording with two untitled streams: the first is the game, the second may be the microphone - its
    // speech must duck the music at every word (-35 dBFS), not only at loud moments
    const QString two = Check::makeMedia(
        dir + "/cs2/cs2_clip_two.mkv",
        QStringList{"-f", "lavfi", "-i",
                    "aevalsrc=0.06*(random(0)*2-1)+(between(t\\,1\\,1.4)+between(t\\,5\\,5.4))*0.9*sin(2*PI*150*t):s=48000:d=10",
                    "-f", "lavfi", "-i",
                    "aevalsrc=(between(t\\,2\\,3)+between(t\\,6\\,7))*0.05*sin(2*PI*300*t):s=48000:d=10",
                    "-map", "0:a", "-map", "1:a", "-ac", "2", "-c:a", "pcm_s16le"});
    if (CHECK(!two.isEmpty())) {
        const auto [micS, micNote] = covered(two, R"([{"op":"duck","track":"A2","under":"A1"}])", 1, 2);
        CHECK(micNote.contains("threshold_db -35") && !micNote.contains("auto"));
        CHECK(micS > 1.5 && micS < 4); // the words at 2-3 and 6-7 s (quiet: -26 dBFS), not missed
        const auto [firstS, firstNote] = covered(two, R"([{"op":"duck","track":"A2","under":"A1"}])", 0, 2);
        CHECK(firstNote.contains("auto: the loud moments"));
    }
    // Talk next to the game sound that is above -35 nearly all the time: the hint about a higher threshold_db is
    // about the talk track alone; the game sound alone never shows it
    CHECK(!gameNote.contains("higher threshold_db"));
    const auto mixed = [&](const QString& talkFile) {
        int id = 0;
        Timeline tl = voiceAndMusic(&id);
        tl.audio[0].clips[0].mediaPath = talkFile;
        while (tl.audio.size() < 3) tl.audio << Track{};
        Clip g = tl.audio[0].clips[0];
        g.id = 3;
        g.mediaPath = game;
        tl.audio[2].clips = {g};
        ProjectData d;
        d.timeline = tl;
        d.lastClipId = 3;
        Project q;
        q.load(d);
        q.addMedia({talkFile, QFileInfo(talkFile).fileName(), 250, false, true, false});
        q.addMedia({game, QFileInfo(game).fileName(), 250, false, true, false});
        q.addMedia({g_music, "music.wav", 250, false, true, false});
        const QString path = dir + "/mixed-duck.schneidi";
        QString error;
        CHECK(ProjectFile::save(q.data(), path, &error));
        Cli::Context ctx;
        try {
            const Cli::Command* cmd = Cli::find("edit");
            const QJsonObject r = cmd->run(
                Cli::normalize(*cmd, {{"project", path}, {"ops", R"([{"op":"duck","track":"A2","under":"A1,A3"}])"}}), ctx);
            const QString note = r["notes"].toArray().at(0).toString();
            std::printf("       %s\n", qPrintable(note));
            return note;
        } catch (const Cli::Error& e) {
            CHECK(false);
            qWarning().noquote() << e.code << e.message;
            return QString();
        }
    };
    const QString noisyTalk = mixed(talk); // the room's noise above -35 all the time
    CHECK(noisyTalk.contains("threshold_db -35 for A1") && noisyTalk.contains("for the game sound A3"));
    CHECK(noisyTalk.contains("A1 is above -35 dBFS most of the time"));
    const QString words = mixed(g_voice); // two words: no hint
    CHECK(words.contains("for the game sound A3") && !words.contains("higher threshold_db"));
}

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("ducking");
    QTemporaryDir dir;

    testDefaults();
    testSoundRanges();
    testApplyKeys();
    testEditorDuckOneUndoStep();
    testVolumeOfDuckedClip(dir.path());
    testDialogDefaultsAndTracks();
    testGameThreshold();

    if (!Check::haveFfmpeg()) return Check::result();
    // Voice: noise bursts at 2–3 s and 6–7 s; music: a quiet tone all the time
    g_voice = Check::makeMedia(dir.filePath("voice.wav"),
                               {"-f", "lavfi", "-i",
                                "aevalsrc=(between(t\\,2\\,3)+between(t\\,6\\,7))*0.4*sin(2*PI*300*t):s=48000:d=10", "-ac", "2"});
    g_music = Check::makeMedia(dir.filePath("music.wav"),
                               {"-f", "lavfi", "-i", "aevalsrc=0.1*sin(2*PI*440*t):s=48000:d=10", "-ac", "2"});
    if (!CHECK(!g_voice.isEmpty() && !g_music.isEmpty())) return Check::result();
    Check::initEngine();
    std::setlocale(LC_NUMERIC, "C");
    testAnalyzeRealSound();
    testCliDuckMatchesEditor(dir.path());
    testDuckGameSound(dir.path());
    return Check::result();
}
