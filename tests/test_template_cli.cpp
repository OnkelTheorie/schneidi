// SPDX-License-Identifier: AGPL-3.0-only
// `templates` for content templates (cli/CommandsTemplate.cpp): the list with start and content templates (own and
// built-in), info (slots with their rules, where they are, brand references), fill into a new project (values in it,
// what was adjusted, the brand kit applied or not, unfilled slots warned / strict errors, dry_run writes nothing, no
// overwrite without asking), the edit ops slot/fill, save/remove/rename, ZIP export -> import round trip (stored and
// deflated archives), hostile ZIPs refused, the CLI alias `template info`, and filling the app's project live as one
// undo step; the template's files copied after the checks and never over a user's file, dry_run reporting an existing
// project, a render error after the save.

#include "check.h"

#include "cli/Commands.h"
#include "cli/Live.h"
#include "core/BrandKit.h"
#include "core/LiveHost.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TemplateLibrary.h"
#include "core/TimelineOps.h"
#include "core/Zip.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>

#include <chrono>
#include <functional>
#include <thread>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        return {};
    }
}

QString errorOf(const QString& name, const QJsonObject& raw, QString* message = nullptr)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        if (message) *message = e.message;
        return e.code;
    }
    return "none";
}

QJsonObject cli(const QStringList& args)
{
    QProcess p;
    p.start(Check::cliPath(), args);
    p.closeWriteChannel();
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && !t.hasExpired(120000)) {
        p.waitForFinished(5);
        QCoreApplication::processEvents();
    }
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(1000);
    }
    QCoreApplication::processEvents();
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (!o["ok"].toBool()) std::printf("       %s %s\n", out.constData(), p.readAllStandardError().constData());
    return o;
}

ProjectData load(const QString& path)
{
    ProjectData d;
    QString err;
    if (!ProjectFile::load(path, &d, &err)) std::printf("       cannot load %s: %s\n", qPrintable(path), qPrintable(err));
    return d;
}

const Clip* clipWithSlot(const ProjectData& d, const QString& id)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : d.timeline.tracks(k))
            for (const Clip& c : t.clips)
                if (c.slot.id == id) return &c;
    return nullptr;
}

const Clip* firstTitle(const ProjectData& d)
{
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle()) return &c;
    return nullptr;
}

QJsonObject find(const QJsonArray& list, const QString& key, const QString& value)
{
    for (const QJsonValue& v : list)
        if (v[key].toString() == value) return v.toObject();
    return {};
}

bool hasAdjusted(const QJsonObject& report, const QString& slot, const QString& what)
{
    for (const QJsonValue& r : report["results"].toArray())
        if (r["slot"].toString() == slot && r["adjusted"].toArray().contains(what)) return true;
    return false;
}

class FakeApp : public LiveHost {
public:
    Project project;
    Selection selection;
    QString path;
    Project* liveProject() override { return &project; }
    QString liveProjectPath() const override { return path; }
    Selection* liveSelection() override { return &selection; }
    int livePlayhead() const override { return 0; }
    bool livePlaying() const override { return false; }
    void liveSeek(int) override {}
    void livePlay(bool) override {}
    bool liveSave(const QString& to, QString* error) override
    {
        if (!to.isEmpty()) path = to;
        return ProjectFile::save(project.data(), path, error);
    }
    void liveGenerateProxies(const QStringList&) override {}
    QJsonObject liveProxyState(const QString&) const override { return {}; }
    void liveConnectionChanged(bool, const QString&) override {}
    void liveChanged(const LiveChange&) override {}
    void liveActivity(const LiveActivity&) override {}
    bool liveOpen(const QStringList&, bool, QString*) override { return true; }
};

struct Media {
    QString a, longClip, shortClip, other;
};

// A template project: clip1 (3 s, trim) and clip2 (3 s, speed) on V1 with their sound, a title "Hook" with brand
// colour 1 over both (text slot, at most 20 characters), saved as own template "Test Short"
QString makeTemplate(const QString& dir, const Media& m)
{
    const QString project = dir + "/tpl.schneidi";
    run("new", {{"project", project}, {"fps", 25}, {"media", QJsonArray{m.a}}});
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"append","media":"%1","duration":"3s"},{"op":"append","media":"%1","duration":"3s"},
                                     {"op":"title","text":"Hook","at":0,"duration":"6s","track":"V2","color":"brand1"}])")
                             .arg(m.a)}});
    ProjectData d = load(project);
    if (!CHECK(d.timeline.video.size() >= 2 && d.timeline.video[0].clips.size() == 2)) return {};
    const int c1 = d.timeline.video[0].clips[0].id, c2 = d.timeline.video[0].clips[1].id;
    const Clip* title = firstTitle(d);
    if (!CHECK(title)) return {};
    const QJsonObject r = run("edit", {{"project", project},
                                       {"ops", QString(R"([{"op":"slot","clip":%1,"id":"clip1","hint":"best moment"},
                                                           {"op":"slot","clip":%2,"id":"clip2","fit":"speed"},
                                                           {"op":"slot","clip":%3,"id":"hook","max_chars":20}])")
                                                   .arg(c1)
                                                   .arg(c2)
                                                   .arg(title->id)}});
    CHECK(!r.isEmpty());
    d = load(project);
    CHECK_EQ(TimelineOps::findClip(d.timeline, c1)->slot.id, QString("clip1"));
    CHECK(TimelineOps::findClip(d.timeline, c2)->slot.fit == SlotFit::Speed);
    CHECK(firstTitle(d)->slot.kind == SlotKind::Text); // a title becomes a text slot by itself
    CHECK_EQ(firstTitle(d)->slot.maxChars, 20);
    // info shows the slot on the clip
    const QJsonObject info = run("info", {{"project", project}});
    const QJsonArray clips = info["timeline"]["tracks"].toArray()[0].toObject()["clips"].toArray();
    CHECK_EQ(clips[0]["slot"]["id"].toString(), QString("clip1"));
    CHECK_EQ(clips[0]["slot"]["hint"].toString(), QString("best moment"));
    CHECK_EQ(clips[1]["slot"]["fit"].toString(), QString("speed"));
    // Save as template
    const QJsonObject saved = run("templates", {{"save", project}, {"name", "Test Short"}, {"description", "two clips"},
                                                {"tags", "test, short"}});
    CHECK(saved["saved"].toBool());
    CHECK_EQ(saved["slots"].toArray().size(), 3);
    CHECK_EQ(errorOf("templates", {{"save", project}, {"name", "Test Short"}}), QString("EXISTS"));
    return project;
}

void testListAndInfo(const QString& tmp)
{
    std::printf("list, info\n");
    // A built-in one next to the own one
    const QString root = tmp + "/builtin";
    QDir().mkpath(root + "/demo");
    QFile::copy(TemplateLibrary::dir() + "/Test Short/template.schneidi", root + "/demo/template.schneidi");
    TemplateLibrary::setBuiltInRoot(root);
    const QJsonObject list = run("templates", {});
    const QJsonArray all = list["templates"].toArray();
    CHECK_EQ(find(all, "id", "neutral")["kind"].toString(), QString("start"));
    const QJsonObject own = find(all, "id", "Test Short");
    CHECK_EQ(own["kind"].toString(), QString("content"));
    CHECK(!own["built_in"].toBool());
    CHECK_EQ(own["aspect"].toString(), QString("16:9"));
    CHECK_EQ(own["length_s"].toDouble(), 6.0);
    CHECK_EQ(own["description"].toString(), QString("two clips"));
    CHECK_EQ(own["tags"].toArray().size(), 2);
    CHECK_EQ(own["slots"].toArray().size(), 3);
    CHECK(find(all, "id", "demo")["built_in"].toBool());
    TemplateLibrary::setBuiltInRoot({});

    const QJsonObject info = run("templates", {{"info", "test short"}});
    const QJsonArray slotList = info["slots"].toArray();
    const QJsonObject clip1 = find(slotList, "id", "clip1"), clip2 = find(slotList, "id", "clip2"), hook = find(slotList, "id", "hook");
    CHECK_EQ(clip1["kind"].toString(), QString("clip"));
    CHECK_EQ(clip1["length"].toString(), QString("fixed"));
    CHECK_EQ(clip1["fit"].toString(), QString("trim"));
    CHECK(clip1["reframe"].toBool());
    CHECK_EQ(clip1["hint"].toString(), QString("best moment"));
    CHECK_EQ(clip2["fit"].toString(), QString("speed"));
    CHECK_EQ(hook["kind"].toString(), QString("text"));
    CHECK_EQ(hook["max_chars"].toInt(), 20);
    const QJsonObject inst = clip2["instances"].toArray()[0].toObject();
    CHECK_EQ(inst["track"].toString(), QString("V1"));
    CHECK_EQ(inst["start_s"].toDouble(), 3.0);
    CHECK_EQ(inst["length_s"].toDouble(), 3.0);
    CHECK(inst["media"].toString().contains("/Test Short/media/")); // its bundled copy
    CHECK_EQ(hook["instances"].toArray()[0].toObject()["text"].toString(), QString("Hook"));
    const QJsonArray refs = info["brand_refs"].toArray();
    CHECK_EQ(refs.size(), 1);
    CHECK_EQ(refs[0]["refs"]["color"].toString(), QString("brand1"));
    // `template` for a content template gives the same info; unknown = NOT_FOUND
    CHECK_EQ(run("templates", {{"template", "Test Short"}})["slots"].toArray().size(), 3);
    CHECK_EQ(errorOf("templates", {{"info", "nothing"}}), QString("NOT_FOUND"));
    CHECK_EQ(errorOf("templates", {{"info", "Test Short"}, {"fill", "Test Short"}}), QString("BAD_ARGUMENT"));
}

void testFill(const QString& tmp, const Media& m)
{
    std::printf("fill\n");
    BrandKit kit;
    kit.colors = {QColor("#ff0000")};
    CHECK(Brand::setGlobal(kit));
    const QString out = tmp + "/out/filled.schneidi";
    const QJsonObject values{{"clip1", m.longClip}, {"clip2", m.shortClip}, {"hook", "Hello"}};
    // dry_run: checks, writes nothing
    QJsonObject dry = run("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"clip1", tmp + "/missing.mp4"}}},
                                        {"out_project", out}, {"dry_run", true}});
    CHECK(dry["dry_run"].toBool());
    CHECK(!dry["ok"].toBool());
    CHECK_EQ(dry["errors"].toArray()[0].toObject()["code"].toString(), QString("FILE_MISSING"));
    CHECK_EQ(dry["unfilled"].toArray().size(), 2);
    CHECK(!QFileInfo::exists(out));
    dry = run("templates", {{"fill", "Test Short"}, {"values", values}, {"dry_run", true}});
    CHECK(dry["ok"].toBool());
    CHECK_EQ(run("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"hook", QString(30, 'x')}}},
                               {"dry_run", true}})["ok"].toBool(),
             true); // shrink: a warning, not an error

    const QJsonObject r = run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", out}});
    CHECK(r["ok"].toBool());
    CHECK_EQ(r["project"].toString(), out);
    CHECK(hasAdjusted(r, "clip1", "trimmed"));
    CHECK(hasAdjusted(r, "clip2", "slowed"));
    CHECK(r["unfilled"].toArray().isEmpty());
    CHECK(r["brand"]["applied"].toBool());
    ProjectData d = load(out);
    CHECK_EQ(clipWithSlot(d, "clip1")->mediaPath, m.longClip);
    CHECK_EQ(clipWithSlot(d, "clip1")->length(), 75);
    CHECK_EQ(clipWithSlot(d, "clip2")->mediaPath, m.shortClip);
    CHECK(clipWithSlot(d, "clip2")->isRetimed());
    CHECK_EQ(firstTitle(d)->title.text, QString("Hello"));
    CHECK_EQ(firstTitle(d)->title.color.name(), QString("#ff0000")); // brand colour 1 of the global kit
    CHECK(!d.templateInfo.has_value());
    // No overwrite without asking; with it the old state goes into the backups
    CHECK_EQ(errorOf("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", out}}), QString("EXISTS"));
    QJsonObject again = run("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"clip1", m.other}}},
                                          {"out_project", out}, {"overwrite", true}, {"brand", "none"}});
    CHECK(!again["backup"].toString().isEmpty());
    // Unfilled: a warning, the placeholder content stays; template media copied next to the project
    CHECK_EQ(again["unfilled"].toArray().size(), 2);
    CHECK(again["notes"].toArray()[0].toString().contains("not filled"));
    d = load(out);
    CHECK(clipWithSlot(d, "clip2")->mediaPath.startsWith(tmp + "/out/filled media/"));
    CHECK(QFileInfo::exists(clipWithSlot(d, "clip2")->mediaPath));
    CHECK(firstTitle(d)->title.color.name() != QString("#ff0000")); // brand:none
    // strict: unfilled = error, nothing written
    const QString strictOut = tmp + "/out/strict.schneidi";
    CHECK_EQ(errorOf("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"clip1", m.other}}},
                                   {"out_project", strictOut}, {"strict", true}}),
             QString("UNFILLED"));
    CHECK(!QFileInfo::exists(strictOut));
    // A failing value: an error with its code, nothing written
    QString msg;
    CHECK_EQ(errorOf("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"clip1", tmp + "/missing.mp4"}}},
                                   {"out_project", strictOut}},
                     &msg),
             QString("FILL_FAILED"));
    CHECK(msg.contains("FILE_MISSING"));
    CHECK_EQ(errorOf("templates", {{"fill", "Test Short"}, {"values", QJsonObject{{"nope", m.other}}},
                                   {"out_project", strictOut}}),
             QString("FILL_FAILED"));
    CHECK(!QFileInfo::exists(strictOut));
    // An own kit for the filled project
    const QString branded = tmp + "/out/branded.schneidi";
    const QJsonObject b = run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", branded},
                                            {"brand", QJsonObject{{"colors", QJsonArray{"#00ff00"}}}}});
    CHECK_EQ(b["brand"]["kit"].toString(), QString("own"));
    d = load(branded);
    CHECK_EQ(firstTitle(d)->title.color.name(), QString("#00ff00"));
    CHECK(d.brand.has_value());
    // Rendered right away
    const QString video = tmp + "/out/branded2.mp4";
    const QJsonObject rendered = run("templates", {{"fill", "Test Short"}, {"values", values},
                                                   {"out_project", tmp + "/out/branded2.schneidi"},
                                                   {"render", QJsonObject{{"out", video}, {"height", 180}}}});
    CHECK(rendered.contains("render"));
    CHECK(QFileInfo(video).size() > 0);
    CHECK(Brand::setGlobal({}));
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

void writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
}

// The template's own files the filled project uses: copied next to it only once nothing stops the fill, never over a
// file of the user (same name, other content: the copy gets a free name; the same content is used again); dry_run
// reports an existing project; a failed render after the save reports the saved project
void testFillFiles(const QString& tmp, const Media& m)
{
    std::printf("fill: template files, existing projects, render errors\n");
    const std::optional<ContentTemplate> t = TemplateLibrary::find("Test Short");
    if (!CHECK(t)) return;
    const QString name = QFileInfo(m.a).fileName();
    const QString tplFile = t->dir + "/media/" + name;
    if (!CHECK(QFileInfo::exists(tplFile))) return;
    const QString dir = tmp + "/files";
    const QJsonObject values{{"clip1", m.longClip}, {"hook", "Hi"}}; // clip2 keeps the template's clip
    // A different file of the same size where the copy would go
    QByteArray other = readFile(tplFile);
    other[other.size() / 2] = char(other[other.size() / 2] ^ 0x55);
    writeFile(dir + "/user media/media/" + name, other);
    QJsonObject r = run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", dir + "/user.schneidi"}});
    CHECK(r["ok"].toBool());
    CHECK(readFile(dir + "/user media/media/" + name) == other); // the user's file stays
    const QString copy = clipWithSlot(load(dir + "/user.schneidi"), "clip2")->mediaPath;
    CHECK_EQ(copy, dir + "/user media/media/" + QFileInfo(m.a).completeBaseName() + "-2.mp4");
    CHECK(readFile(copy) == readFile(tplFile));
    // Again (overwrite): the copy with the same content is used, no third file
    r = run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", dir + "/user.schneidi"}, {"overwrite", true}});
    CHECK_EQ(clipWithSlot(load(dir + "/user.schneidi"), "clip2")->mediaPath, copy);
    CHECK_EQ(QDir(dir + "/user media/media").entryList(QDir::Files).size(), 2);
    // A file of another size there stays too
    writeFile(dir + "/small media/media/" + name, "mine");
    run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", dir + "/small.schneidi"}});
    CHECK(readFile(dir + "/small media/media/" + name) == "mine");
    CHECK(readFile(clipWithSlot(load(dir + "/small.schneidi"), "clip2")->mediaPath) == readFile(tplFile));

    // dry_run says that the project exists (as the fill itself would), writes nothing
    const QJsonObject dry = run("templates", {{"fill", "Test Short"}, {"values", values},
                                              {"out_project", dir + "/user.schneidi"}, {"dry_run", true}});
    CHECK(!dry["ok"].toBool());
    CHECK_EQ(dry["errors"][0]["code"].toString(), QString("EXISTS"));
    CHECK(run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", dir + "/user.schneidi"},
                            {"dry_run", true}, {"overwrite", true}})["ok"].toBool());

    // A fill that stops (project locked by someone else) leaves no copies behind
    const QString locked = dir + "/locked.schneidi";
    writeFile(locked, readFile(dir + "/user.schneidi"));
    {
        const std::unique_ptr<QLockFile> lock = ProjectFile::lock(locked);
        CHECK(lock != nullptr);
        CHECK_EQ(errorOf("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", locked}, {"overwrite", true}}),
                 QString("PROJECT_LOCKED"));
    }
    CHECK(!QFileInfo::exists(dir + "/locked media"));

    // The render fails after the project was saved: the answer names the saved project and the render's error
    const QString rendered = dir + "/render.schneidi";
    r = run("templates", {{"fill", "Test Short"}, {"values", values}, {"out_project", rendered},
                          {"render", QJsonObject{{"preset", "no such preset"}}}});
    CHECK_EQ(r["project"].toString(), rendered);
    CHECK(QFileInfo::exists(rendered));
    CHECK(!r["render_error"].toObject()["code"].toString().isEmpty());
    CHECK(!r["ok"].toBool());
}

void testEditOps(const QString& tmp, const Media& m)
{
    std::printf("edit ops slot/fill, fill in place\n");
    const QString project = tmp + "/ops.schneidi";
    QFile::remove(project);
    run("new", {{"project", project}, {"fps", 25}, {"media", QJsonArray{m.a}}});
    run("edit", {{"project", project}, {"ops", QString(R"([{"op":"append","media":"%1","duration":"2s"}])").arg(m.a)}});
    int id = load(project).timeline.video[0].clips[0].id;
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"slot","clip":%1,"id":"shot","length":"flexible","max":"5s"}])").arg(id)}});
    const Clip* c = clipWithSlot(load(project), "shot");
    if (!CHECK(c)) return;
    CHECK(c->slot.length == SlotLength::Flexible);
    CHECK_EQ(c->slot.maxLen, 125);
    CHECK_EQ(errorOf("edit", {{"project", project}, {"ops", QString(R"([{"op":"slot","clip":%1,"fit":"wobble"}])").arg(id)}}),
             QString("BAD_ARGUMENT"));
    // Fill: flexible takes the value's length up to max (10 s file -> 5 s)
    const QJsonObject r = run("edit", {{"project", project},
                                       {"ops", QString(R"([{"op":"fill","slot":"shot","value":"%1"}])").arg(m.longClip)}});
    CHECK(!r.isEmpty());
    ProjectData d = load(project);
    CHECK_EQ(clipWithSlot(d, "shot")->mediaPath, m.longClip);
    CHECK_EQ(clipWithSlot(d, "shot")->length(), 125);
    // In place with templates project+values, dry_run first
    const QJsonObject dry = run("templates", {{"project", project}, {"values", QJsonObject{{"shot", m.shortClip}}}, {"dry_run", true}});
    CHECK(dry["ok"].toBool());
    const QJsonObject inPlace = run("templates", {{"project", project}, {"values", QJsonObject{{"shot", QJsonObject{{"file", m.longClip}, {"from", "1s"}, {"to", "3s"}}}}}});
    CHECK(inPlace["ok"].toBool());
    CHECK(!inPlace["backup"].toString().isEmpty());
    d = load(project);
    CHECK_EQ(clipWithSlot(d, "shot")->in, 25);
    CHECK_EQ(clipWithSlot(d, "shot")->length(), 50);
    // Unmark
    run("edit", {{"project", project}, {"ops", QString(R"([{"op":"slot","clip":%1,"remove":true}])").arg(id)}});
    CHECK(!clipWithSlot(load(project), "shot"));
    CHECK(!run("info", {{"project", project}}).value("timeline")["tracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject().contains("slot"));
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

void testZip(const QString& tmp, const Media& m)
{
    std::printf("zip\n");
    const QString zip = tmp + "/share/short.zip";
    const QJsonObject e = run("templates", {{"export", "Test Short"}, {"zip", zip}});
    CHECK_EQ(e["zip"].toString(), zip);
    CHECK(e["files"].toInt() >= 2);
    CHECK_EQ(errorOf("templates", {{"export", "Test Short"}, {"zip", zip}}), QString("EXISTS"));
    // Round trip: removed, imported, fillable from its new folder
    CHECK_EQ(run("templates", {{"remove", "Test Short"}})["removed"].toString(), QString("Test Short"));
    CHECK_EQ(errorOf("templates", {{"info", "Test Short"}}), QString("NOT_FOUND"));
    const QJsonObject imp = run("templates", {{"import", zip}});
    CHECK(imp["imported"].toBool());
    CHECK_EQ(imp["id"].toString(), QString("Test Short"));
    CHECK_EQ(imp["slots"].toArray().size(), 3);
    const QJsonObject info = run("templates", {{"info", "Test Short"}});
    CHECK(find(info["slots"].toArray(), "id", "clip2")["instances"].toArray()[0].toObject()["media"].toString().startsWith(
        TemplateLibrary::dir() + "/Test Short/media/"));
    // Again: a name of its own
    const QJsonObject second = run("templates", {{"import", zip}});
    CHECK_EQ(second["id"].toString(), QString("Test Short 2"));
    CHECK_EQ(second["name"].toString(), QString("Test Short 2"));
    CHECK_EQ(run("templates", {{"import", zip}, {"replace", true}})["id"].toString(), QString("Test Short"));
    CHECK(run("templates", {{"rename", "Test Short 2"}, {"name", "Other"}})["name"].toString() == "Other");
    CHECK(run("templates", {{"remove", "Other"}})["removed"].toString() == "Other");
    CHECK_EQ(errorOf("templates", {{"remove", "../x"}}), QString("NOT_FOUND"));

    // A ZIP made by another tool with compression (deflate), the folder at the archive's root
    if (!QStandardPaths::findExecutable("zip").isEmpty()) {
        const QString text = tmp + "/deflate/notes.txt";
        QDir().mkpath(tmp + "/deflate");
        QByteArray big;
        for (int i = 0; i < 20000; ++i) big += QByteArray::number(i % 97) + " lorem ipsum ";
        QFile f(text);
        f.open(QIODevice::WriteOnly);
        f.write(big);
        f.close();
        QFile::copy(TemplateLibrary::dir() + "/Test Short/template.schneidi", tmp + "/deflate/template.schneidi");
        QDir().mkpath(tmp + "/deflate/media");
        for (const QFileInfo& fi : QDir(TemplateLibrary::dir() + "/Test Short/media").entryInfoList(QDir::Files))
            QFile::copy(fi.absoluteFilePath(), tmp + "/deflate/media/" + fi.fileName());
        QProcess p;
        p.setWorkingDirectory(tmp + "/deflate");
        p.start("zip", {"-q", "-r", "-9", tmp + "/share/deflated.zip", "."});
        CHECK(p.waitForFinished(60000) && p.exitCode() == 0);
        const QJsonObject d = run("templates", {{"import", tmp + "/share/deflated.zip"}});
        CHECK_EQ(d["id"].toString(), QString("Test Short 2"));
        CHECK_EQ(d["slots"].toArray().size(), 3);
        CHECK_EQ(readAll(QDir(d["dir"].toString()).filePath("notes.txt")), big);
        run("templates", {{"remove", d["id"].toString()}});
    } else {
        std::printf("  (no zip program: deflate import not checked)\n");
    }

    // Hostile archives: nothing lands outside, nothing is imported
    const QString payload = tmp + "/payload.txt";
    QFile pf(payload);
    pf.open(QIODevice::WriteOnly);
    pf.write("x");
    pf.close();
    const QString tpl = TemplateLibrary::dir() + "/Test Short/template.schneidi";
    const int before = int(TemplateLibrary::own().size());
    struct Case {
        const char* what;
        QVector<Zip::Source> files;
    };
    const QVector<Case> cases{
        {"../ entry", {{"t/template.schneidi", tpl}, {"t/../../evil.txt", payload}}},
        {"absolute entry", {{"template.schneidi", tpl}, {"/tmp/evil-schneidi.txt", payload}}},
        {"drive letter", {{"template.schneidi", tpl}, {"C:/evil.txt", payload}}},
        {"backslash ..", {{"template.schneidi", tpl}, {"..\\evil.txt", payload}}},
        // Names Windows reads as another one: ".. " is "..", "a. " is "a", "name." is "name"
        {"'.. ' part", {{"t/template.schneidi", tpl}, {"t/.. /.. /evil.txt", payload}}},
        {"'..\\' + space", {{"template.schneidi", tpl}, {".. \\evil.txt", payload}}},
        {"'a. ' folder", {{"template.schneidi", tpl}, {"a. /evil.txt", payload}}},
        {"trailing dot", {{"template.schneidi", tpl}, {"media/name.", payload}}},
        {"trailing space", {{"template.schneidi", tpl}, {"media/name.mp4 ", payload}}},
        {"three dots", {{"template.schneidi", tpl}, {".../evil.txt", payload}}},
        // Windows device names, also with an extension or a space
        {"CON", {{"template.schneidi", tpl}, {"media/CON", payload}}},
        {"nul.txt", {{"template.schneidi", tpl}, {"media/nul.txt", payload}}},
        {"Aux folder", {{"template.schneidi", tpl}, {"Aux/x.txt", payload}}},
        {"com1.mp4", {{"template.schneidi", tpl}, {"media/com1.mp4", payload}}},
        {"LPT9 .png", {{"template.schneidi", tpl}, {"media/LPT9 .png", payload}}},
        // Characters Windows refuses, an NTFS stream
        {"question mark", {{"template.schneidi", tpl}, {"media/what?.mp4", payload}}},
        {"pipe", {{"template.schneidi", tpl}, {"media/a|b.mp4", payload}}},
        {"control char", {{"template.schneidi", tpl}, {QString("media/a") + QChar(0x01) + "b.mp4", payload}}},
        {"NTFS stream", {{"template.schneidi", tpl}, {"media/x.mp4:evil", payload}}},
    };
    for (const Case& c : cases) {
        const QString hostile = tmp + "/share/hostile.zip";
        QFile::remove(hostile);
        CHECK(Zip::write(hostile, c.files));
        QString msg;
        const QString code = errorOf("templates", {{"import", hostile}}, &msg);
        if (!CHECK_EQ(code, QString("BAD_ZIP"))) std::printf("       %s: %s\n", c.what, qPrintable(msg));
    }
    // Ordinary names stay allowed
    for (const char* ok : {"media/clip 1.mp4", "a.b/c.d", ".hidden", "./x.png", "CONSOLE.mp4", "com10.mp4", "nulled.txt",
                           "Test Short/template.schneidi", "media/Ölfass – Szene ¹.mp4"})
        if (!CHECK(!Zip::safeName(QString::fromUtf8(ok)).isEmpty())) std::printf("       refused: %s\n", ok);
    CHECK(!QFileInfo::exists(TemplateLibrary::dir() + "/../evil.txt"));
    CHECK(!QFileInfo::exists(QDir(TemplateLibrary::dir()).filePath("../../evil.txt")));
    // A symbolic link entry (external attributes of a Unix link)
    {
        const QString hostile = tmp + "/share/link.zip";
        CHECK(Zip::write(hostile, {{"template.schneidi", tpl}, {"media/link.mp4", payload}}));
        QFile f(hostile);
        f.open(QIODevice::ReadWrite);
        QByteArray bytes = f.readAll();
        const qsizetype cd = bytes.lastIndexOf(QByteArray("\x50\x4b\x01\x02", 4));
        if (!CHECK(cd >= 0)) return;
        const quint32 link = quint32(0120777) << 16;
        for (int i = 0; i < 4; ++i) bytes[cd + 38 + i] = char((link >> (8 * i)) & 0xff);
        f.seek(0);
        f.write(bytes);
        f.close();
        QString msg;
        CHECK_EQ(errorOf("templates", {{"import", hostile}}, &msg), QString("BAD_ZIP"));
        CHECK(msg.contains("symbolic link"));
    }
    // Not a template, not a ZIP
    {
        const QString none = tmp + "/share/none.zip";
        CHECK(Zip::write(none, {{"readme.txt", payload}}));
        CHECK_EQ(errorOf("templates", {{"import", none}}), QString("BAD_TEMPLATE"));
        CHECK_EQ(errorOf("templates", {{"import", payload}}), QString("BAD_ZIP"));
    }
    CHECK_EQ(int(TemplateLibrary::own().size()), before);
    for (const QFileInfo& fi : QDir(TemplateLibrary::dir()).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        CHECK(!fi.fileName().startsWith(".import")); // no staging folder left
    (void)m;
}

// Patches the uncompressed size of the first central directory entry (a "zip bomb" announcing more than it holds)
void announceSize(const QString& zip, quint32 size)
{
    QFile f(zip);
    if (!f.open(QIODevice::ReadWrite)) return;
    QByteArray bytes = f.readAll();
    const qsizetype cd = bytes.indexOf(QByteArray("\x50\x4b\x01\x02", 4));
    if (cd < 0) return;
    for (int i = 0; i < 4; ++i) bytes[cd + 24 + i] = char((size >> (8 * i)) & 0xff);
    f.seek(0);
    f.write(bytes);
}

QStringList entryNames(const QString& zip)
{
    QStringList names;
    if (const std::optional<QVector<Zip::Entry>> list = Zip::entries(zip))
        for (const Zip::Entry& e : *list) names << e.name;
    return names;
}

// Archives of other systems and names that are not valid everywhere
void testZipNames(const QString& tmp, const Media& m)
{
    std::printf("zip: names of other systems\n");
    const QString tpl = TemplateLibrary::dir() + "/Test Short/template.schneidi";
    const int before = int(TemplateLibrary::own().size());
    QString msg;

    // macOS adds "__MACOSX/" and ".DS_Store" next to the folder: the template is still found, the extras stay out
    {
        const QString mac = tmp + "/share/mac.zip";
        CHECK(Zip::write(mac, {{"Mac Short/template.schneidi", tpl}, {"Mac Short/.DS_Store", m.a}, {"__MACOSX/._Mac Short", m.a},
                               {"__MACOSX/Mac Short/._template.schneidi", m.a}, {".DS_Store", m.a}}));
        const QJsonObject imp = run("templates", {{"import", mac}});
        CHECK(imp["imported"].toBool());
        const QString folder = TemplateLibrary::dir() + "/" + imp["id"].toString();
        CHECK(QFileInfo::exists(folder + "/template.schneidi"));
        CHECK(!QFileInfo::exists(folder + "/.DS_Store"));
        CHECK(!QFileInfo::exists(folder + "/__MACOSX"));
        run("templates", {{"remove", imp["id"].toString()}});
    }

    // Media names legal on Linux but not on Windows: portable names in the archive, the project follows them
    // (Windows cannot create such files at all)
#ifndef Q_OS_WIN
    {
        QDir().mkpath(tmp + "/odd");
        const QString colon = tmp + "/odd/take:1.mp4", backslash = tmp + "/odd/cam\\2.mp4", device = tmp + "/odd/nul.mp4";
        for (const QString& f : {colon, backslash, device}) CHECK(QFile::copy(m.a, f));
        const QString project = tmp + "/odd/odd.schneidi";
        run("new", {{"project", project}, {"fps", 25}, {"media", QJsonArray{colon, backslash, device}}});
        run("edit", {{"project", project},
                     {"ops", QJsonArray{QJsonObject{{"op", "append"}, {"media", colon}, {"duration", "1s"}},
                                        QJsonObject{{"op", "append"}, {"media", backslash}, {"duration", "1s"}},
                                        QJsonObject{{"op", "append"}, {"media", device}, {"duration", "1s"}}}}});
        CHECK(run("templates", {{"save", project}, {"name", "Odd Names"}})["saved"].toBool());
        const QString zip = tmp + "/share/odd.zip";
        CHECK(run("templates", {{"export", "Odd Names"}, {"zip", zip}})["files"].toInt() >= 4);
        const QStringList names = entryNames(zip);
        for (const QString& n : names)
            if (!CHECK(!Zip::safeName(n).isEmpty())) std::printf("       not portable: %s\n", qPrintable(n));
        CHECK(names.contains("Odd Names/media/take_1.mp4"));
        CHECK(names.contains("Odd Names/media/cam_2.mp4"));
        CHECK(names.contains("Odd Names/media/nul_.mp4"));
        run("templates", {{"remove", "Odd Names"}});
        const QJsonObject imp = run("templates", {{"import", zip}});
        CHECK_EQ(imp["id"].toString(), QString("Odd Names"));
        const std::optional<ContentTemplate> t = TemplateLibrary::find("Odd Names");
        if (CHECK(t.has_value())) {
            ProjectData d;
            CHECK(TemplateLibrary::load(*t, &d));
            QStringList used;
            for (const Clip& c : d.timeline.video[0].clips) used << QFileInfo(c.mediaPath).fileName();
            CHECK_EQ(used, (QStringList{"take_1.mp4", "cam_2.mp4", "nul_.mp4"}));
            for (const Clip& c : d.timeline.video[0].clips) CHECK(QFileInfo::exists(c.mediaPath) && c.mediaPath.startsWith(t->dir));
            for (const MediaInfo& mi : d.media) CHECK(QFileInfo::exists(mi.path) && mi.path.startsWith(t->dir));
        }
        run("templates", {{"remove", "Odd Names"}});
    }
#endif

    // Two files that are one on Windows and macOS, a file where a folder is: refused
    for (const QVector<Zip::Source>& files :
         {QVector<Zip::Source>{{"template.schneidi", tpl}, {"media/Clip.mp4", m.a}, {"media/clip.mp4", m.a}},
          QVector<Zip::Source>{{"template.schneidi", tpl}, {"media/a.mp4", m.a}, {"media/a.mp4", m.a}},
          QVector<Zip::Source>{{"template.schneidi", tpl}, {"media", m.a}, {"Media/a.mp4", m.a}}}) {
        const QString clash = tmp + "/share/clash.zip";
        QFile::remove(clash);
        CHECK(Zip::write(clash, files));
        if (!CHECK_EQ(errorOf("templates", {{"import", clash}}, &msg), QString("BAD_ZIP")))
            std::printf("       %s\n", qPrintable(files[1].name));
    }

    // A "zip bomb": announces far more than 20 times its size -> refused before anything is written
    {
        const QString bomb = tmp + "/share/bomb.zip";
        CHECK(Zip::write(bomb, {{"template.schneidi", tpl}, {"media/big.mp4", m.a}}));
        announceSize(bomb, 0xF0000000u); // 3.75 GiB
        CHECK_EQ(errorOf("templates", {{"import", bomb}}, &msg), QString("BAD_ZIP"));
        CHECK(msg.contains("too big"));
    }

    // A project that does not load: nothing shows up
    {
        const QString broken = tmp + "/share/broken.zip";
        QDir().mkpath(tmp + "/odd");
        const QString junkProject = tmp + "/odd/template.schneidi";
        QFile f(junkProject);
        f.open(QIODevice::WriteOnly);
        f.write("{ not a project");
        f.close();
        CHECK(Zip::write(broken, {{"Broken/template.schneidi", junkProject}}));
        CHECK_EQ(errorOf("templates", {{"import", broken}}), QString("BAD_TEMPLATE"));
    }

    CHECK_EQ(int(TemplateLibrary::own().size()), before);
    for (const QFileInfo& fi : QDir(TemplateLibrary::dir()).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot))
        CHECK(!fi.fileName().startsWith(".import")); // no staging folder or replaced copy left
}

void testAlias()
{
    std::printf("CLI alias\n");
#ifdef Q_OS_WIN
    std::printf("       (skipped on Windows: the schneidi-cli process does not share the test folders, XDG does not apply)\n");
    return;
#endif
    const QJsonObject o = cli({"template", "info", "Test Short"});
    CHECK(o["ok"].toBool());
    CHECK_EQ(o["slots"].toArray().size(), 3);
    CHECK(cli({"template", "list"})["templates"].toArray().size() > 4);
}

void testLive(const QString& tmp, const Media& m)
{
    std::printf("live fill: one undo step\n");
#ifdef Q_OS_WIN
    Q_UNUSED(tmp);
    Q_UNUSED(m);
    std::printf("       (skipped on Windows: the schneidi-cli process does not share the test folders, XDG does not apply)\n");
    return;
#endif
    FakeApp fake;
    ProjectData d;
    TemplateLibrary::load(*TemplateLibrary::find("Test Short"), &d);
    fake.project.load(d);
    fake.path = tmp + "/live.schneidi";
    QString error;
    CHECK(ProjectFile::save(fake.project.data(), fake.path, &error));
    fake.project.markSaved();
    Cli::Live::Server server(&fake);
    if (!CHECK(server.start(&error))) return;
    const int index = fake.project.undoStack()->index();
    const QString values = QString(R"({"clip1":"%1","hook":"Live text"})").arg(m.longClip);
    const QJsonObject r = cli({"templates", "--project", fake.path, "--values", values});
    CHECK(r["live"].toBool());
    CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    const Clip* t = firstTitle(fake.project.data());
    CHECK(t && t->title.text == "Live text");
    CHECK_EQ(clipWithSlot(fake.project.data(), "clip1")->mediaPath, m.longClip);
    fake.project.undoStack()->undo();
    CHECK_EQ(firstTitle(fake.project.data())->title.text, QString("Hook"));
    // The edit op too
    const QJsonObject e = cli({"edit", fake.path, "--ops", R"([{"op":"fill","slot":"hook","value":"Op text"}])"});
    CHECK(e["live"].toBool());
    CHECK_EQ(firstTitle(fake.project.data())->title.text, QString("Op text"));
    CHECK_EQ(fake.project.undoStack()->index(), index + 1);
    // A failing value changes nothing
    const int now = fake.project.undoStack()->index();
    cli({"templates", "--project", fake.path, "--values", QString(R"({"clip1":"%1"})").arg(tmp + "/missing.mp4")});
    CHECK_EQ(fake.project.undoStack()->index(), now);
}

} // namespace

// Another schneidi busy with `lock` (another thread here): it does `busy` while holding it for ms. true = `work`
// finished only after it let go (waited for it)
bool waitsFor(std::unique_ptr<QLockFile> lock, int ms, const std::function<void()>& work,
              const std::function<void()>& busy = {})
{
    QElapsedTimer clock;
    clock.start();
    qint64 released = 0;
    std::thread other([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        if (busy) busy();
        released = clock.elapsed();
        lock.reset();
    });
    work();
    const qint64 done = clock.elapsed();
    other.join();
    return done >= released;
}

// Several schneidi at once on the own templates (two AIs): saving, removing, renaming and importing take turns (one
// lock for the library) instead of working on each other's half-written folders; a ZIP export does not write into
// another export's temporary file
void testLibraryParallel(const QString& tmp, const QString& project)
{
    std::printf("parallel: own templates\n");
    // The other one saves a template of the same name meanwhile: this save sees it (EXISTS), does not mix into it
    const QString folder = TemplateLibrary::dir() + "/Parallel";
    QString code;
    CHECK(waitsFor(TemplateLibrary::lockLibrary(0), 1500,
                   [&] { code = errorOf("templates", {{"save", project}, {"name", "Parallel"}}); },
                   [&] {
                       QDir().mkpath(folder);
                       QFile::copy(project, folder + "/template.schneidi");
                   }));
    CHECK_EQ(code, QString("EXISTS"));
    QDir(folder).removeRecursively();
    CHECK(run("templates", {{"save", project}, {"name", "Parallel"}})["saved"].toBool());
    const QString zip = tmp + "/share/parallel.zip";
    // Another export to the same ZIP still writing its temporary file next to it: that one stays untouched
    writeFile(zip + ".part", "busy");
    CHECK_EQ(run("templates", {{"export", "Parallel"}, {"zip", zip}})["zip"].toString(), zip);
    CHECK_EQ(readAll(zip + ".part"), QByteArray("busy"));
    CHECK(waitsFor(TemplateLibrary::lockLibrary(0), 1500, [&] {
        CHECK_EQ(run("templates", {{"remove", "Parallel"}})["removed"].toString(), QString("Parallel"));
    }));
    CHECK(waitsFor(TemplateLibrary::lockLibrary(0), 1500, [&] {
        CHECK_EQ(run("templates", {{"import", zip}})["id"].toString(), QString("Parallel"));
    }));
    CHECK(waitsFor(TemplateLibrary::lockLibrary(0), 1500, [&] {
        CHECK(run("templates", {{"rename", "Parallel"}, {"name", "Parallel 2"}})["name"].toString() == "Parallel 2");
    }));
    run("templates", {{"remove", "Parallel 2"}});

    // Ids in another case find the template (an AI types "my-case", the folder is "My-Case"; Windows sees one folder
    // anyway); a remove while another schneidi holds the library says BUSY, not NOT_FOUND
    CHECK(run("templates", {{"save", project}, {"name", "My-Case"}})["saved"].toBool());
    CHECK_EQ(run("templates", {{"info", "my-case"}})["id"].toString(), QString("My-Case"));
    CHECK_EQ(run("templates", {{"rename", "MY-CASE"}, {"name", "My-Case 2"}})["name"].toString(), QString("My-Case 2"));
    CHECK_EQ(run("templates", {{"info", "my-case 2"}})["name"].toString(), QString("My-Case 2"));
    qputenv("SCHNEIDI_TEMPLATE_LOCK_MS", "200"); // not a minute
    {
        const auto held = TemplateLibrary::lockLibrary(0);
        CHECK(held != nullptr);
        CHECK_EQ(errorOf("templates", {{"remove", "my-case 2"}}), QString("BUSY"));
        CHECK_EQ(errorOf("templates", {{"rename", "my-case 2"}, {"name", "x"}}), QString("BUSY"));
    }
    qunsetenv("SCHNEIDI_TEMPLATE_LOCK_MS");
    CHECK_EQ(errorOf("templates", {{"remove", "no such template"}}), QString("NOT_FOUND"));
    CHECK_EQ(run("templates", {{"remove", "my-case 2"}})["removed"].toString(), QString("my-case 2"));
    CHECK(TemplateLibrary::own().isEmpty() || !TemplateLibrary::find("My-Case 2"));
}

// Two fills to one new name at once (two batches into one out_dir): the other one makes the file while this one runs -
// it is kept, this one says EXISTS and copies no media
void testFillParallel(const QString& tmp, const QString& project)
{
    std::printf("parallel: fill\n");
    CHECK(run("templates", {{"save", project}, {"name", "Parallel Fill"}})["saved"].toBool());
    const QString out = tmp + "/parallel/filled.schneidi";
    QDir().mkpath(tmp + "/parallel");
    QString code;
    CHECK(waitsFor(ProjectFile::lock(out, 0), 1500,
                   [&] { code = errorOf("templates", {{"fill", "Parallel Fill"}, {"out_project", out}}); },
                   [&] { writeFile(out, "the other fill's project"); }));
    CHECK_EQ(code, QString("EXISTS"));
    CHECK_EQ(readAll(out), QByteArray("the other fill's project"));
    CHECK(!QFileInfo::exists(tmp + "/parallel/filled media"));
    run("templates", {{"remove", "Parallel Fill"}});
}

int main(int argc, char* argv[])
{
    // schneidi-cli processes (alias, live) must see the same template folder: own data/cache folders for all
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    QApplication::setApplicationName("schneidi");
#ifdef Q_OS_WIN
    QStandardPaths::setTestModeEnabled(true); // XDG does not apply there: never the user's templates/backups
    // The test folders stay between runs there: start empty like a new XDG folder
    for (const auto where : {QStandardPaths::AppDataLocation, QStandardPaths::CacheLocation})
        if (const QString d = QStandardPaths::writableLocation(where); d.contains("qttest")) QDir(d).removeRecursively();
#endif
    Check::initEngine();
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    const QString dir = tmp.path() + "/media";
    QDir().mkpath(dir);
    const auto make = [&](const char* name, int seconds, const char* size) {
        return Check::makeMedia(dir + "/" + name,
                                {"-f", "lavfi", "-i", QString("testsrc=size=%1:rate=25:duration=%2").arg(size).arg(seconds),
                                 "-f", "lavfi", "-i", QString("sine=frequency=440:duration=%1").arg(seconds), "-c:v",
                                 "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    };
    Media m;
    m.a = make("a.mp4", 4, "320x180");
    m.longClip = make("long.mp4", 10, "320x180");
    m.shortClip = make("short.mp4", 1, "320x180");
    m.other = make("other.mp4", 4, "320x180");
    if (!CHECK(!m.a.isEmpty() && !m.longClip.isEmpty() && !m.shortClip.isEmpty() && !m.other.isEmpty()))
        return Check::result();
    const QString tpl = makeTemplate(tmp.path(), m);
    if (tpl.isEmpty()) return Check::result();
    testListAndInfo(tmp.path());
    testFill(tmp.path(), m);
    testFillFiles(tmp.path(), m);
    testEditOps(tmp.path(), m);
    testAlias();
    testLive(tmp.path(), m);
    testZip(tmp.path(), m);
    testZipNames(tmp.path(), m);
    testLibraryParallel(tmp.path(), tpl);
    testFillParallel(tmp.path(), tpl);
    return Check::result();
}
