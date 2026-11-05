// SPDX-License-Identifier: AGPL-3.0-only
// `templates` for content templates (cli/CommandsTemplate.cpp): the list with start and content templates (own and
// built-in), info (slots with their rules, where they are, brand references), fill into a new project (values in it,
// what was adjusted, the brand kit applied or not, unfilled slots warned / strict errors, dry_run writes nothing, no
// overwrite without asking), the edit ops slot/fill, save/remove/rename, ZIP export -> import round trip (stored and
// deflated archives), hostile ZIPs refused, the CLI alias `template info`, and filling the app's project live as one
// undo step.

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
#include <QProcess>
#include <QTemporaryDir>

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
    p.start(SCHNEIDI_CLI, args);
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
    const QJsonArray clips = info["timeline"]["tracks"].toArray()[0]["clips"].toArray();
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
    const QJsonArray slots = info["slots"].toArray();
    const QJsonObject clip1 = find(slots, "id", "clip1"), clip2 = find(slots, "id", "clip2"), hook = find(slots, "id", "hook");
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
    CHECK_EQ(hook["instances"].toArray()[0]["text"].toString(), QString("Hook"));
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
    CHECK_EQ(dry["errors"].toArray()[0]["code"].toString(), QString("FILE_MISSING"));
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
                                                   {"render", QJsonObject{{"out", video}, {"width", 320}, {"height", 180}}}});
    CHECK(rendered.contains("render"));
    CHECK(QFileInfo(video).size() > 0);
    CHECK(Brand::setGlobal({}));
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
    CHECK(!run("info", {{"project", project}})["timeline"]["tracks"].toArray()[0]["clips"].toArray()[0].toObject().contains("slot"));
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
    CHECK(find(info["slots"].toArray(), "id", "clip2")["instances"].toArray()[0]["media"].toString().startsWith(
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
        const QJsonObject d = run("templates", {{"import", tmp + "/share/deflated.zip"}, {"replace", true}});
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
    };
    for (const Case& c : cases) {
        const QString hostile = tmp + "/share/hostile.zip";
        QFile::remove(hostile);
        CHECK(Zip::write(hostile, c.files));
        QString msg;
        const QString code = errorOf("templates", {{"import", hostile}}, &msg);
        if (!CHECK_EQ(code, QString("BAD_ZIP"))) std::printf("       %s: %s\n", c.what, qPrintable(msg));
    }
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

void testAlias()
{
    std::printf("CLI alias\n");
    const QJsonObject o = cli({"template", "info", "Test Short"});
    CHECK(o["ok"].toBool());
    CHECK_EQ(o["slots"].toArray().size(), 3);
    CHECK(cli({"template", "list"})["templates"].toArray().size() > 4);
}

void testLive(const QString& tmp, const Media& m)
{
    std::printf("live fill: one undo step\n");
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
    if (makeTemplate(tmp.path(), m).isEmpty()) return Check::result();
    testListAndInfo(tmp.path());
    testFill(tmp.path(), m);
    testEditOps(tmp.path(), m);
    testAlias();
    testLive(tmp.path(), m);
    testZip(tmp.path(), m);
    return Check::result();
}
