// SPDX-License-Identifier: AGPL-3.0-only
// Start templates (core/StartTemplates.h): how a new project looks. "Neutral" is exactly the project schneidi always
// started with and the default; the built-in ones (game, podcast, vertical) are only offered and never mute, hide or
// lock a track. Own templates are files in the app data folder (save, list, find, rename, delete, default follows),
// projects carry the fixed format and their own import presets in optional fields, the app starts and makes new
// projects from the default (or the one chosen when it asks, own ones named like built-in ones included), the first
// clip keeps a fixed resolution, the CLI `new` takes a template and `templates` lists them. Everything stays in the test's own settings and folders.
// TEMPLATES_DUMP=<folder>: pictures of Settings › Start Templates and the File menu (settings-templates.png,
// file-menu.png, template-menu.png, save-template.png).

#include "check.h"

#include "app/InputBindings.h"
#include "app/MainWindow.h"
#include "app/ProjectSettingsDialog.h"
#include "app/SettingsDialog.h"
#include "app/StartTemplatesPage.h"
#include "app/Theme.h"
#include "cli/Commands.h"
#include "core/Editor.h"
#include "core/I18n.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/StartTemplates.h"
#include "core/StreamPresets.h"
#include "engine/Engine.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <clocale>
#include <utility>

namespace {

QByteArray projectJson(const ProjectData& d) { return ProjectFile::toJson(d, "/x/p.schneidi"); }

// What a new project gets, as the project file writes it
QByteArray newProject(const StartTemplate& t) { return projectJson(StartTemplates::apply(t)); }

StartTemplate builtIn(const QString& id)
{
    for (const StartTemplate& t : StartTemplates::builtIn())
        if (t.id == id) return t;
    CHECK(!"built-in template missing");
    return {};
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        return {};
    }
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

ProjectData loadProject(const QString& path)
{
    ProjectData d;
    QString error;
    CHECK(ProjectFile::load(path, &d, &error));
    return d;
}

void clean()
{
    QSettings().remove("templates");
    QSettings().remove("import/streamPresets");
    QDir(StartTemplates::dir()).removeRecursively();
}

// Tests never touch the user's settings or templates
void testIsolation()
{
    CHECK(QSettings().fileName().contains("schneidi-tests"));
    CHECK(StartTemplates::dir().contains("schneidi-test-start-templates"));
    CHECK(StartTemplates::dir().contains(".qttest")); // QStandardPaths test mode
}

// Neutral = the project schneidi always started with, byte for byte, and the default
void testNeutralIsToday()
{
    clean();
    ProjectData today;
    today.timeline = emptyTimeline();
    const StartTemplate neutral = builtIn(StartTemplates::NeutralId);
    CHECK(newProject(neutral) == projectJson(today));
    CHECK_EQ(StartTemplates::defaultId(), QString("neutral"));
    CHECK_EQ(StartTemplates::defaultTemplate().id, QString("neutral"));
    CHECK(!StartTemplates::askOnNew());
    // Through a Project as the app does it (load) and as it did (reset)
    Project a, b;
    a.load(StartTemplates::apply(neutral));
    b.reset();
    CHECK(projectJson(a.data()) == projectJson(b.data()));
    CHECK(!a.importPresets());
    // The project file of an ordinary project has none of the new fields
    const QJsonObject root = QJsonDocument::fromJson(projectJson(today)).object();
    CHECK(!root.contains("fixedFormat") && !root.contains("importPresets"));
}

// The offered ones: what each sets; none mutes, hides, locks or solos a track
void testBuiltIns()
{
    clean();
    const QVector<StartTemplate> list = StartTemplates::builtIn();
    QStringList ids;
    for (const StartTemplate& t : list) {
        ids << t.id;
        CHECK(t.builtIn && !t.displayName().isEmpty() && !t.displayDescription().isEmpty());
        for (const auto* tracks : {&t.data.timeline.video, &t.data.timeline.audio})
            for (const Track& tr : *tracks) CHECK(!tr.muted && !tr.hidden && !tr.locked && !tr.solo && tr.clips.isEmpty());
        Project p;
        p.load(StartTemplates::apply(t));
        CHECK(!p.isModified());
        CHECK(!p.timeline().video.isEmpty() && !p.timeline().audio.isEmpty());
    }
    CHECK_EQ(ids.join(","), QString("neutral,game,podcast,vertical"));

    // Game: Mix muted on top of the user's own rules, limiter; format from the first clip
    StreamPresets user;
    StreamRule spotify;
    spotify.title = "Spotify";
    spotify.action = StreamRule::Action::Skip;
    user.rules << spotify;
    user.save();
    const ProjectData game = StartTemplates::apply(builtIn("game"));
    CHECK(game.importPresets && game.importPresets->muteMix);
    CHECK(game.importPresets && game.importPresets->rules == user.rules);
    CHECK(game.timeline.masterLimiter);
    CHECK(!game.fixedResolution && !game.fixedFrameRate);
    // 60 fps until the first clip brings its own frame rate (titles made before it); the rest as a new project
    CHECK(game.format.rate == (FrameRate{60, 1}));
    ProjectFormat gameRest = game.format;
    gameRest.rate = ProjectFormat{}.rate;
    CHECK(gameRest == ProjectFormat{});
    CHECK_EQ(StartTemplates::summary(builtIn("game")).value(0), T("Format: Auflösung und Framerate vom ersten Clip"));
    CHECK_EQ(game.timeline.audio.size(), qsizetype(2));
    QSettings().remove("import/streamPresets");

    // Podcast: named voice and music tracks (in the UI language), limiter, the settings' import presets
    const ProjectData podcast = StartTemplates::apply(builtIn("podcast"));
    CHECK_EQ(podcast.timeline.audio.size(), qsizetype(3));
    CHECK_EQ(podcast.timeline.audio.value(0).name, T("Stimme 1"));
    CHECK_EQ(podcast.timeline.audio.value(1).name, T("Stimme 2"));
    CHECK_EQ(podcast.timeline.audio.value(2).name, T("Musik"));
    CHECK_EQ(podcast.timeline.audio.value(2).color, QString("purple"));
    CHECK(podcast.timeline.masterLimiter && !podcast.importPresets);

    // Vertical: 1080 x 1920 that the first clip keeps, frame rate from the clip
    const ProjectData vertical = StartTemplates::apply(builtIn("vertical"));
    CHECK_EQ(vertical.format.width, 1080);
    CHECK_EQ(vertical.format.height, 1920);
    CHECK(vertical.fixedResolution && !vertical.fixedFrameRate);
    CHECK(!vertical.timeline.masterLimiter && !vertical.importPresets);

    // Found by id and by name, any case
    CHECK_EQ(StartTemplates::find("PODCAST").value_or(StartTemplate{}).id, QString("podcast"));
    CHECK_EQ(StartTemplates::find(T("Hochformat 9:16")).value_or(StartTemplate{}).id, QString("vertical"));
    CHECK(!StartTemplates::find("nope"));
    CHECK(!StartTemplates::summary(builtIn("vertical")).isEmpty());
}

// The new project-file fields: written only when set, read back
void testProjectFields()
{
    ProjectData d;
    d.timeline = emptyTimeline();
    d.fixedResolution = true;
    d.fixedFrameRate = true;
    StreamPresets own;
    own.muteMix = true;
    StreamRule r;
    r.title = "Discord";
    r.action = StreamRule::Action::Mute;
    r.volumeDb = -3;
    r.trackName = "Voice chat";
    own.rules << r;
    d.importPresets = own;
    ProjectData back;
    QString error;
    CHECK(ProjectFile::fromJson(projectJson(d), "/x/p.schneidi", &back, &error));
    CHECK(back.fixedResolution && back.fixedFrameRate);
    CHECK(back.importPresets && *back.importPresets == own);
    // Through Project (data/load); setting them counts as a change, without an undo step
    Project p;
    p.load(back);
    CHECK(p.fixedResolution() && p.fixedFrameRate() && p.importPresets() == own);
    CHECK(!p.isModified());
    int emitted = 0;
    QObject::connect(&p, &Project::importPresetsChanged, [&emitted] { ++emitted; });
    p.setImportPresets(std::nullopt);
    CHECK(p.isModified() && emitted == 1 && !p.undoStack()->canUndo());
    CHECK(p.effectiveImportPresets() == StreamPresets::load()); // the settings' again
    p.setFixedFormat(false, true);
    CHECK(!p.data().fixedResolution && p.data().fixedFrameRate);
}

// A project as own template: tracks with names and states, no clips; saved, listed, found, renamed, deleted
void testOwnTemplates()
{
    clean();
    Project p;
    ProjectData start;
    start.timeline = emptyTimeline();
    start.format.width = 2560;
    start.format.height = 1440;
    start.format.rate = {60, 1};
    p.load(start);
    p.setFixedFormat(false, true);
    StreamPresets own;
    own.muteMix = true;
    p.setImportPresets(own);
    p.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
    p.edit("setup", [&](Timeline& tl) {
        tl.video[0].name = "Cam";
        tl.video[1].locked = true;
        tl.audio[0].name = "Mix";
        tl.audio[0].muted = true; // the user's "track 1 muted"
        tl.audio[1].solo = true;
        tl.audio[1].volumeDb = -6;
        tl.audio[1].pan = 20;
        Track a3;
        a3.kind = TrackKind::Audio;
        a3.name = "Musik";
        a3.color = "purple";
        a3.dynamics.on = true;
        tl.audio << a3;
        SubtitleTrack st;
        st.name = "DE";
        st.style.size = 60;
        st.cues << SubtitleCue{p.newClipId(), 0, 25, "Hallo"};
        tl.subtitles << st;
        tl.markers << 10;
        tl.markIn = 5;
        tl.masterLimiter = true;
        tl.masterLimiterDb = -2;
        Clip v;
        v.id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.out = 99;
        tl.video[0].clips << v;
    });

    StartTemplate t = StartTemplates::fromProject(p.data(), "  My Show  ", "Weekly");
    CHECK_EQ(t.name, QString("My Show"));
    QString error;
    CHECK(StartTemplates::save(t, &error));
    CHECK_EQ(t.id, QString("My Show"));
    CHECK(QFileInfo::exists(QDir(StartTemplates::dir()).filePath("My Show.json")));
    // The file is a project file without media, plus its name
    {
        QFile f(t.path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        CHECK_EQ(root["app"].toString(), QString("schneidi"));
        CHECK_EQ(root["template"]["name"].toString(), QString("My Show"));
        CHECK(!root.contains("media") && !root.contains("playhead"));
    }
    const QVector<StartTemplate> mine = StartTemplates::own();
    CHECK_EQ(mine.size(), qsizetype(1));
    const StartTemplate back = mine.value(0);
    CHECK(!back.builtIn && back.id == "My Show" && back.description == "Weekly");
    // Round trip: exactly what was saved
    CHECK(newProject(back) == newProject(t));
    const ProjectData d = StartTemplates::apply(back);
    CHECK_EQ(d.format.width, 2560);
    CHECK(d.format.rate == FrameRate({60, 1}));
    CHECK(!d.fixedResolution && d.fixedFrameRate);
    CHECK(d.importPresets && d.importPresets->muteMix);
    CHECK(d.timeline.video.value(0).name == "Cam" && d.timeline.video.value(0).clips.isEmpty());
    CHECK(d.timeline.video.value(1).locked);
    CHECK(d.timeline.audio.value(0).muted && d.timeline.audio.value(0).name == "Mix");
    CHECK(d.timeline.audio.value(1).solo && d.timeline.audio.value(1).volumeDb == -6 && d.timeline.audio.value(1).pan == 20);
    CHECK(d.timeline.audio.value(2).name == "Musik" && d.timeline.audio.value(2).color == "purple"
          && d.timeline.audio.value(2).dynamics.on);
    CHECK(d.timeline.subtitles.size() == 1 && d.timeline.subtitles.value(0).name == "DE"
          && d.timeline.subtitles.value(0).cues.isEmpty() && d.timeline.subtitles.value(0).style.size == 60);
    CHECK(d.timeline.markers.isEmpty() && d.timeline.markIn == -1);
    CHECK(d.timeline.masterLimiter && d.timeline.masterLimiterDb == -2);
    CHECK(d.media.isEmpty());
    CHECK_EQ(StartTemplates::find("my show").value_or(StartTemplate{}).id, QString("My Show"));
    CHECK(StartTemplates::all().size() == StartTemplates::builtIn().size() + 1);

    // Default: set, renamed (follows), gone (Neutral)
    StartTemplates::setDefaultId(t.id);
    CHECK_EQ(StartTemplates::defaultTemplate().name, QString("My Show"));
    CHECK(StartTemplates::rename(t.id, "Our Show", &error));
    CHECK(!QFileInfo::exists(t.path));
    CHECK_EQ(StartTemplates::defaultId(), QString("Our Show"));
    CHECK_EQ(StartTemplates::defaultTemplate().name, QString("Our Show"));
    CHECK(StartTemplates::defaultTemplate().data.timeline.audio.value(0).muted);
    // Renaming onto another one is refused
    StartTemplate other = StartTemplates::fromProject(p.data(), "Other");
    CHECK(StartTemplates::save(other, &error));
    CHECK(!StartTemplates::rename("Other", "Our Show", &error));
    CHECK(!error.isEmpty());
    CHECK(StartTemplates::remove("Our Show"));
    CHECK_EQ(StartTemplates::defaultId(), QString("neutral"));
    CHECK_EQ(StartTemplates::defaultTemplate().id, QString("neutral"));
    StartTemplates::setDefaultId("Other");
    QFile::remove(other.path); // gone outside schneidi
    CHECK_EQ(StartTemplates::defaultTemplate().id, QString("neutral"));
    CHECK(!StartTemplates::remove("neutral")); // built-in ones stay
    // A broken file is left out
    QFile broken(QDir(StartTemplates::dir()).filePath("broken.json"));
    CHECK(broken.open(QIODevice::WriteOnly));
    broken.write("{not json");
    broken.close();
    CHECK(StartTemplates::own().isEmpty());
    clean();
}

void testIdForName()
{
    CHECK_EQ(StartTemplates::idForName("Podcast"), QString("Podcast-own")); // never a built-in id
    CHECK_EQ(StartTemplates::idForName("game"), QString("game-own"));
    CHECK_EQ(StartTemplates::idForName("a/b:c*d"), QString("a_b_c_d"));
    CHECK_EQ(StartTemplates::idForName(" .hidden. "), QString("hidden"));
    CHECK_EQ(StartTemplates::idForName("   "), QString("template"));
    // Windows device names are no files there, also with an extension ("Aux.json"); very long names neither
    CHECK_EQ(StartTemplates::idForName("Aux"), QString("Aux_"));
    CHECK_EQ(StartTemplates::idForName("com1"), QString("com1_"));
    CHECK_EQ(StartTemplates::idForName("NUL.v2"), QString("NUL_.v2"));
    CHECK_EQ(StartTemplates::idForName("Console"), QString("Console"));
    const QString longId = StartTemplates::idForName(QString(300, QChar(0x00e4)) + " .");
    CHECK(longId.size() <= 200 && !longId.endsWith('.') && !longId.endsWith(' '));

    // Names that share a file: saving or renaming says which template is there
    clean();
    StartTemplate shorts = StartTemplates::fromProject(StartTemplates::apply(builtIn("vertical")), "Shorts 9:16");
    QString error;
    CHECK(StartTemplates::save(shorts, &error));
    const std::optional<StartTemplate> there = StartTemplates::sameFile("Shorts 9_16");
    CHECK(there && there->name == QString("Shorts 9:16") && there->id == shorts.id);
    CHECK(!StartTemplates::sameFile("Shorts 9-16"));
    StartTemplate other = StartTemplates::fromProject(StartTemplates::apply(builtIn("podcast")), "Anders");
    CHECK(StartTemplates::save(other, &error));
    CHECK(!StartTemplates::rename(other.id, "Shorts 9_16", &error));
    CHECK_EQ(error, T("„%1“ bekäme denselben Dateinamen wie die Vorlage „%2“.").arg("Shorts 9_16", "Shorts 9:16"));
    CHECK(!StartTemplates::rename(other.id, "Shorts 9:16", &error));
    CHECK_EQ(error, T("Eine Vorlage „%1“ gibt es schon.").arg("Shorts 9:16"));
    clean();
}

// schneidi-cli: `new` with a template (id, name, file, project), the default, `templates`
void testCli(const QString& dir)
{
    clean();
    const QString path = dir + "/cli.schneidi";
    // Default without a template: Neutral, as before
    QJsonObject r = run("new", {{"project", path}});
    CHECK_EQ(std::as_const(r)["template"]["id"].toString(), QString("neutral"));
    ProjectData plain;
    plain.timeline = emptyTimeline();
    {
        ProjectData d = loadProject(path);
        d.playhead = 0;
        CHECK(StartTemplates::apply(StartTemplates::fromProject(d, {})).timeline == plain.timeline);
        CHECK(d.format == ProjectFormat{} && !d.fixedResolution && !d.importPresets);
    }

    // podcast: in one call
    r = run("new", {{"project", path}, {"template", "podcast"}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["template"]["id"].toString(), QString("podcast"));
    const QJsonArray tracks = std::as_const(r)["timeline"]["tracks"].toArray();
    QStringList names;
    for (const QJsonValue& v : tracks) names << v["name"].toString();
    CHECK_EQ(names.join(","), QString(",,%1,%2,%3").arg(T("Stimme 1"), T("Stimme 2"), T("Musik")));
    CHECK(std::as_const(r)["timeline"]["master"]["limiter"].toBool());

    // vertical: width/height from the template, fps given; the format keeps its fixed resolution
    r = run("new", {{"project", path}, {"template", "vertical"}, {"fps", 60}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["format"]["width"].toInt(), 1080);
    CHECK_EQ(std::as_const(r)["format"]["height"].toInt(), 1920);
    CHECK_EQ(std::as_const(r)["format"]["fps"].toDouble(), 60.0);
    CHECK(std::as_const(r)["format"]["fixed_resolution"].toBool());
    CHECK(loadProject(path).fixedResolution);
    // explicit width/height win over the template's
    r = run("new", {{"project", path}, {"template", "vertical"}, {"width", 720}, {"height", 1280}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["format"]["width"].toInt(), 720);

    // The user's default applies without a template
    StartTemplates::setDefaultId("podcast");
    r = run("new", {{"project", path}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["template"]["id"].toString(), QString("podcast"));
    StartTemplates::setDefaultId("neutral");

    // Unknown template: the error names the ones there are
    CHECK_EQ(errorOf("new", {{"project", path}, {"template", "nope"}, {"overwrite", true}}), QString("NOT_FOUND"));
    CHECK_EQ(errorOf("new", {{"project", path}, {"template", dir + "/missing.json"}, {"overwrite", true}}),
             QString("NOT_FOUND"));

    // An own template by name and by file; another project as template (its tracks, no clips)
    Project src;
    ProjectData sd;
    sd.timeline = emptyTimeline();
    src.load(sd);
    src.edit("names", [](Timeline& tl) {
        tl.audio[0].name = "Game";
        tl.audio[0].muted = true;
    });
    StartTemplate mine = StartTemplates::fromProject(src.data(), "Mein Spiel");
    QString error;
    CHECK(StartTemplates::save(mine, &error));
    r = run("new", {{"project", path}, {"template", "mein spiel"}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["template"]["id"].toString(), QString("Mein Spiel"));
    CHECK(loadProject(path).timeline.audio.value(0).muted);
    r = run("new", {{"project", path}, {"template", mine.path}, {"overwrite", true}});
    CHECK_EQ(std::as_const(r)["timeline"]["tracks"].toArray().at(2)["name"].toString(), QString("Game"));
    {
        const QString other = dir + "/other.schneidi";
        src.addMedia({"/x/a.mp4", "a.mp4", 100, true, true, false});
        src.edit("clip", [&](Timeline& tl) {
            Clip c;
            c.id = src.newClipId();
            c.mediaPath = "/x/a.mp4";
            c.out = 49;
            tl.audio[0].clips << c;
        });
        CHECK(ProjectFile::save(src.data(), other, &error));
        r = run("new", {{"project", path}, {"template", other}, {"overwrite", true}});
        const ProjectData d = loadProject(path);
        CHECK(d.timeline.audio.value(0).name == "Game" && d.timeline.audio.value(0).clips.isEmpty() && d.media.isEmpty());
    }

    // templates: built-in and own, the default marked, what each sets
    r = run("templates", {});
    CHECK_EQ(std::as_const(r)["default"].toString(), QString("neutral"));
    const QJsonArray list = std::as_const(r)["templates"].toArray();
    CHECK_EQ(list.size(), qsizetype(5));
    QJsonObject game, own;
    for (const QJsonValue& v : list) {
        if (v["id"].toString() == "game") game = v.toObject();
        if (v["id"].toString() == "Mein Spiel") own = v.toObject();
        if (v["id"].toString() == "neutral") CHECK(v["default"].toBool());
    }
    CHECK(std::as_const(game)["built_in"].toBool() && std::as_const(game)["import_presets"]["mute_mix"].toBool());
    CHECK(std::as_const(game)["master"]["limiter"].toBool());
    CHECK(!std::as_const(own)["built_in"].toBool() && std::as_const(own)["file"].toString() == mine.path);
    CHECK(std::as_const(own)["tracks"].toArray().at(2)["muted"].toBool());
    CHECK(!own.contains("import_presets"));
    r = run("templates", {{"template", "vertical"}});
    CHECK_EQ(std::as_const(r)["format"]["height"].toInt(), 1920);
    CHECK(std::as_const(r)["format"]["fixed_resolution"].toBool() && !std::as_const(r)["format"]["fixed_frame_rate"].toBool());
    clean();
}

// A game project mutes the Mix through its own import presets, with the settings' switch off
void testCliGameMutesMix(const QString& dir)
{
    clean();
    CHECK(!StreamPresets::load().muteMix);
    const QString path = dir + "/game.schneidi";
    run("new", {{"project", path}, {"template", "game"}});
    {
        Project p;
        p.load(loadProject(path));
        MediaInfo m;
        m.path = "/x/obs.mkv";
        m.name = "obs.mkv";
        m.length = 100;
        m.hasVideo = m.hasAudio = true;
        for (const char* title : {"Mix", "Discord", "Mikrofon"}) m.audioStreams << AudioStreamInfo{2, title};
        p.addMedia(m);
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    const QJsonObject info = run("info", {{"project", path}});
    CHECK(info["import_presets"]["mute_mix"].toBool());
    run("edit", {{"project", path}, {"ops", R"([{"op":"keep","media":"/x/obs.mkv","ranges":[[0,50]]}])"}});
    const ProjectData d = loadProject(path);
    QStringList clips;
    for (int i = 0; i < d.timeline.audio.size(); ++i)
        for (const Clip& c : d.timeline.audio[i].clips)
            clips << QString("A%1:s%2%3").arg(i + 1).arg(c.audioStream).arg(c.enabled ? "" : "(off)");
    CHECK_EQ(clips.join(" "), QString("A1:s0(off) A2:s1 A3:s2"));
    // A neutral project with the same file: every stream plays (the settings' presets)
    run("new", {{"project", path}, {"overwrite", true}});
    {
        Project p;
        p.load(loadProject(path));
        MediaInfo m;
        m.path = "/x/obs.mkv";
        m.name = "obs.mkv";
        m.length = 100;
        m.hasVideo = m.hasAudio = true;
        for (const char* title : {"Mix", "Discord", "Mikrofon"}) m.audioStreams << AudioStreamInfo{2, title};
        p.addMedia(m);
        QString error;
        CHECK(ProjectFile::save(p.data(), path, &error));
    }
    run("edit", {{"project", path}, {"ops", R"([{"op":"keep","media":"/x/obs.mkv","ranges":[[0,50]]}])"}});
    int off = 0;
    for (const Track& t : loadProject(path).timeline.audio)
        for (const Clip& c : t.clips) off += c.enabled ? 0 : 1;
    CHECK_EQ(off, 0);
}

void testDialogs()
{
    ProjectSettingsDialog dlg(ProjectFormat{}, false);
    CHECK(!dlg.fixedResolution() && !dlg.fixedFrameRate()); // as always: the first clip may change both
    dlg.setFixed(true, false);
    CHECK(dlg.fixedResolution() && !dlg.fixedFrameRate());
    auto* res = dlg.findChild<QCheckBox*>("clip_resolution");
    if (CHECK(res)) {
        res->setChecked(true);
        CHECK(!dlg.fixedResolution());
    }

    clean();
    StartTemplate own = StartTemplates::fromProject(StartTemplates::apply(builtIn("podcast")), "Show");
    QString error;
    CHECK(StartTemplates::save(own, &error));
    StartTemplates::setDefaultId("vertical");
    StartTemplatesPage page;
    auto* combo = page.findChild<QComboBox*>("template_default");
    auto* list = page.findChild<QListWidget*>("template_list");
    auto* details = page.findChild<QLabel*>("template_details");
    if (CHECK(combo && list && details)) {
        CHECK_EQ(combo->count(), 5);
        CHECK_EQ(page.defaultId(), QString("vertical"));
        CHECK_EQ(list->currentRow(), combo->currentIndex()); // shows the default
        CHECK(details->text().contains("1080 × 1920"));
        combo->setCurrentIndex(combo->findData("Show"));
        CHECK_EQ(page.defaultId(), QString("Show"));
        CHECK_EQ(StartTemplates::defaultId(), QString("vertical")); // only on OK (the caller saves it)
        page.reload();
        CHECK_EQ(page.defaultId(), QString("Show"));
        page.reload("neutral");
        CHECK_EQ(page.defaultId(), QString("neutral"));
    }
    SaveStartTemplateDialog save(own);
    CHECK_EQ(save.name(), QString("Show"));
    CHECK(!save.makeDefault());

    // File › New Project when it asks: the default preselected, the description of the chosen one shown, chosen by
    // its place (an own template named "Neutral" is not the built-in Neutral)
    {
        StartTemplate mine = StartTemplates::fromProject(StartTemplates::apply(builtIn("vertical")), "Neutral",
                                                         "Meine Neutral-Vorlage");
        CHECK(StartTemplates::save(mine, &error));
        const QVector<StartTemplate> list = StartTemplates::all();
        ChooseStartTemplateDialog choose(list, "vertical");
        auto* box = choose.findChild<QComboBox*>("choose_template");
        auto* text = choose.findChild<QLabel*>("choose_template_details");
        if (CHECK(box && text)) {
            CHECK_EQ(choose.chosen().id, QString("vertical"));
            CHECK_EQ(box->currentText(), T("%1 (Standard)").arg(T("Hochformat 9:16")));
            CHECK(text->text().startsWith(builtIn("vertical").displayDescription()));
            int row = -1, neutral = -1;
            for (int i = 0; i < box->count(); ++i) {
                if (!box->itemData(i).isValid()) continue;
                const StartTemplate& t = list[box->itemData(i).toInt()];
                if (t.id == mine.id) row = i;
                if (t.id == StartTemplates::NeutralId) neutral = i;
            }
            CHECK(row >= 0 && neutral >= 0);
            CHECK_EQ(box->itemText(neutral), T("%1 (eingebaut)").arg(T("Neutral"))); // told apart
            CHECK_EQ(box->itemText(row), QString("Neutral"));
            box->setCurrentIndex(row);
            CHECK_EQ(choose.chosen().id, mine.id);
            CHECK(!choose.chosen().builtIn);
            CHECK(text->text().startsWith("Meine Neutral-Vorlage"));
            CHECK(text->text().contains("1080 × 1920"));
            box->setCurrentIndex(neutral);
            CHECK_EQ(choose.chosen().id, QString(StartTemplates::NeutralId));
            CHECK(text->text().startsWith(builtIn(StartTemplates::NeutralId).displayDescription()));
            if (const QString dump = qEnvironmentVariable("TEMPLATES_DUMP"); !dump.isEmpty()) {
                box->setCurrentIndex(row);
                choose.show();
                QApplication::processEvents();
                choose.grab().save(dump + "/choose-template.png");
            }
        }
        StartTemplates::remove(mine.id);
    }

    // Settings › Start Templates at the dialog's first size: the whole page, its buttons (rename, delete …) without
    // scrolling (at 560 × 560 they were below the edge)
    {
        SettingsDialog dlg(AiSettings{});
        dlg.showTab(SettingsDialog::Tab::Templates);
        dlg.show();
        QApplication::processEvents();
        int pages = 0;
        for (QScrollArea* area : dlg.findChildren<QScrollArea*>())
            if (qobject_cast<StartTemplatesPage*>(area->widget())) {
                ++pages;
                Check::report(area->verticalScrollBar()->maximum() == 0, "templates page scrolls", __FILE__, __LINE__,
                              QString("page %1, room %2, scroll %3").arg(area->widget()->heightForWidth(area->viewport()->width()))
                                  .arg(area->viewport()->height()).arg(area->verticalScrollBar()->maximum()));
            }
        CHECK_EQ(pages, 1);
    }
    clean();
}

void testMainWindow(const QString& dir)
{
    clean();
    Engine engine;
    QString err;
    if (!CHECK(engine.init(&err))) return;
    std::setlocale(LC_NUMERIC, "C");

    // Neutral: the window starts with the project as always
    {
        MainWindow w(&engine);
        w.disableAutosave();
        Project* p = w.liveProject();
        ProjectData today;
        today.timeline = emptyTimeline();
        Project ref;
        ref.load(today);
        CHECK(projectJson(p->data()) == projectJson(ref.data()));
    }

    // The default template counts from the start; New Project from Template; the editor follows the import presets
    StartTemplates::setDefaultId("vertical");
    MainWindow w(&engine);
    w.disableAutosave();
    Project* p = w.liveProject();
    auto* editor = w.findChild<Editor*>();
    if (!CHECK(editor)) return;
    CHECK_EQ(p->format().width, 1080);
    CHECK(p->fixedResolution() && !p->isModified());
    // The first clip keeps the fixed resolution, its frame rate is taken
    if (Check::haveFfmpeg()) {
        const QString clip = Check::makeMedia(dir + "/clip30.mp4", {"-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30:duration=1",
                                                                    "-c:v", "libx264", "-pix_fmt", "yuv420p"});
        if (CHECK(!clip.isEmpty())) {
            w.importFiles({clip}, true, true);
            QApplication::processEvents();
            CHECK_EQ(p->format().width, 1080);
            CHECK_EQ(p->format().height, 1920);
            CHECK(p->format().rate == FrameRate({30, 1}));
        }
    }

    w.newProjectFromTemplate(builtIn("game"));
    CHECK(p->importPresets() && editor->streamPresets().muteMix);
    CHECK(p->timeline().masterLimiter);
    w.newProjectFromTemplate(builtIn("podcast"));
    CHECK(!p->importPresets() && !editor->streamPresets().muteMix); // the settings' (off)
    CHECK_EQ(p->timeline().audio.value(2).name, T("Musik"));
    w.newProjectFromTemplate(builtIn(StartTemplates::NeutralId));
    {
        Project ref;
        ref.reset();
        CHECK(projectJson(p->data()) == projectJson(ref.data()));
    }
    // Own import presets set later: the editor uses them
    StreamPresets own;
    own.muteMix = true;
    p->setImportPresets(own);
    CHECK(editor->streamPresets().muteMix);
    p->setImportPresets(std::nullopt);
    CHECK(!editor->streamPresets().muteMix);

    // File › New Project from Template: every template, the default marked
    auto* menu = w.findChild<QMenu*>("template_menu");
    if (CHECK(menu)) {
        emit menu->aboutToShow();
        QStringList texts;
        for (QAction* a : menu->actions())
            if (!a->isSeparator()) texts << a->text();
        CHECK(texts.contains(T("%1 (Standard)").arg(T("Hochformat 9:16"))));
        CHECK(texts.contains(T("Spielaufnahme")) && texts.contains(T("Neutral")));
        CHECK(texts.contains(T("Startvorlagen verwalten…")));
    }

    // File › New Project asking for the template: an own template named like a built-in one is the one chosen
    {
        StartTemplate mine = StartTemplates::fromProject(StartTemplates::apply(builtIn(StartTemplates::NeutralId)), T("Neutral"));
        Track third;
        third.kind = TrackKind::Video;
        mine.data.timeline.video << third;
        QString error;
        CHECK(StartTemplates::save(mine, &error));
        StartTemplates::setAskOnNew(true);
        QStringList offered;
        QTimer answer;
        answer.setInterval(20);
        QObject::connect(&answer, &QTimer::timeout, [&] {
            auto* dlg = qobject_cast<ChooseStartTemplateDialog*>(QApplication::activeModalWidget());
            auto* box = dlg ? dlg->findChild<QComboBox*>("choose_template") : nullptr;
            if (!box) return;
            answer.stop();
            for (int i = 0; i < box->count(); ++i)
                if (box->itemData(i).isValid()) offered << box->itemText(i); // not the line above the own ones
            box->setCurrentIndex(box->count() - 1); // the own one (after the built-in ones)
            dlg->accept();
        });
        answer.start();
        if (QAction* action = InputBindings::instance().action("project_new"); CHECK(action)) action->trigger();
        answer.stop();
        CHECK_EQ(offered.size(), int(StartTemplates::all().size()));
        CHECK_EQ(int(QSet<QString>(offered.begin(), offered.end()).size()), int(offered.size())); // all told apart
        CHECK_EQ(p->timeline().video.size(), 3);
        StartTemplates::setAskOnNew(false);
        CHECK(StartTemplates::remove(mine.id));
    }

    if (const QString dump = qEnvironmentVariable("TEMPLATES_DUMP"); !dump.isEmpty()) {
        StartTemplate show = StartTemplates::fromProject(StartTemplates::apply(builtIn("podcast")), "Mein Podcast");
        QString error;
        StartTemplates::save(show, &error);
        StartTemplates::setDefaultId("podcast");
        SettingsDialog dlg(AiSettings{});
        dlg.resize(560, 620);
        dlg.showTab(SettingsDialog::Tab::Templates);
        dlg.show();
        QApplication::processEvents();
        dlg.grab().save(dump + "/settings-templates.png");
        dlg.hide();
        QMenu* file = nullptr;
        for (QAction* a : w.menuBar()->actions())
            if (a->menu() && a->menu()->actions().contains(menu->menuAction())) file = a->menu();
        if (CHECK(file)) {
            file->popup(QPoint(0, 0));
            QApplication::processEvents();
            file->grab().save(dump + "/file-menu.png");
            file->hide();
        }
        menu->popup(QPoint(0, 0));
        QApplication::processEvents();
        menu->grab().save(dump + "/template-menu.png");
        menu->hide();
        SaveStartTemplateDialog save(StartTemplates::fromProject(p->data(), "Mein Projekt"));
        save.show();
        QApplication::processEvents();
        save.grab().save(dump + "/save-template.png");
    }
    clean();
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QApplication app(argc, argv);
    Check::initApp("start-templates");
    Theme::apply(app); // as in the app (pictures)
    I18n::install(qEnvironmentVariable("TEMPLATES_LANG", "de"));
    QTemporaryDir dir;
    testIsolation();
    testNeutralIsToday();
    testBuiltIns();
    testProjectFields();
    testOwnTemplates();
    testIdForName();
    testCli(dir.path());
    testCliGameMutesMix(dir.path());
    testDialogs();
    testMainWindow(dir.path());
    clean();
    return Check::result();
}
