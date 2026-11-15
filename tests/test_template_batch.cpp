// SPDX-License-Identifier: AGPL-3.0-only
// `templates` fill + rows (cli/CommandsTemplate.cpp): a template rendered many times from a table - a CSV (";", paths
// relative to it, extra columns warned once, duplicate names get "-2") gives one project per row, a bad row is
// reported and the others are done (or the batch stops with continue_on_error:false), dry_run writes nothing,
// existing projects are not replaced without overwrite, a JSON file / inline rows, the render of each row and the
// progress stages "row i/N"; the template's files in one folder per batch, JSON numbers as written, --map on row paths.

#include "check.h"

#include "cli/Commands.h"
#include "cli/Sandbox.h"
#include "core/AppPaths.h"
#include "core/ProjectFile.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw, QStringList* stages = nullptr)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    if (stages) ctx.stage = [stages](const QString& s) { *stages << s; };
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        return {};
    }
}

QString errorOf(const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find("templates");
        Cli::Context ctx;
        Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

ProjectData load(const QString& path)
{
    ProjectData d;
    QString err;
    if (!ProjectFile::load(path, &d, &err)) std::printf("       cannot load %s: %s\n", qPrintable(path), qPrintable(err));
    return d;
}

const Clip* withSlot(const ProjectData& d, const QString& id)
{
    for (TrackKind k : {TrackKind::Video, TrackKind::Audio})
        for (const Track& t : d.timeline.tracks(k))
            for (const Clip& c : t.clips)
                if (c.slot.id == id) return &c;
    return nullptr;
}

QString write(const QString& path, const QByteArray& data)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    return path;
}

QStringList filesIn(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files, QDir::Name);
}

// A small template "Batch": clip slot "clip" (2 s) and a text slot "hook" over it, 320x180 so renders are quick
bool makeTemplate(const QString& dir, const QString& media)
{
    const QString project = dir + "/tpl.schneidi";
    run("new", {{"project", project}, {"fps", 25}, {"width", 320}, {"height", 180}, {"media", QJsonArray{media}}});
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"append","media":"%1","duration":"2s"},
                                     {"op":"title","text":"Hook","at":0,"duration":"2s","track":"V2"}])")
                             .arg(media)}});
    ProjectData d = load(project);
    if (!CHECK(d.timeline.video.size() >= 2 && !d.timeline.video[0].clips.isEmpty() && !d.timeline.video[1].clips.isEmpty()))
        return false;
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"slot","clip":%1,"id":"clip"},{"op":"slot","clip":%2,"id":"hook"}])")
                             .arg(d.timeline.video[0].clips[0].id)
                             .arg(d.timeline.video[1].clips[0].id)}});
    return CHECK(run("templates", {{"save", project}, {"name", "Batch"}})["saved"].toBool());
}

void testCsv(const QString& tmp)
{
    std::printf("CSV: one project per row, relative paths, names\n");
    const QString data = tmp + "/data";
    write(data + "/products.csv", "\xEF\xBB\xBFproduct;hook;clip;price\r\n"
                                  "Mug;\"Buy; now\";media/a.mp4;4,99\r\n"
                                  "Pen;Write it;media/b.mp4;1,50\r\n"
                                  "Mug;Again;media/a.mp4;5\r\n");
    QStringList stages;
    const QString out = tmp + "/out1";
    const QJsonObject r = run("templates", {{"fill", "Batch"}, {"rows", data + "/products.csv"}, {"out_dir", out},
                                            {"name", "{product}"}},
                              &stages);
    CHECK(r["ok"].toBool());
    CHECK_EQ(r["totals"]["ok"].toInt(), 3);
    CHECK_EQ(r["totals"]["failed"].toInt(), 0);
    CHECK_EQ(filesIn(out), (QStringList{"Mug-2.schneidi", "Mug.schneidi", "Pen.schneidi"}));
    const QJsonArray rows = r["rows"].toArray();
    CHECK_EQ(rows.size(), 3);
    CHECK_EQ(rows[2]["name"].toString(), QString("Mug-2"));
    CHECK_EQ(rows[1]["project"].toString(), out + "/Pen.schneidi");
    // The cells went in: the text, the clip relative to the CSV's folder
    const ProjectData d = load(out + "/Mug.schneidi");
    const Clip* hook = withSlot(d, "hook");
    const Clip* clip = withSlot(d, "clip");
    CHECK(hook && hook->title.text == "Buy; now");
    CHECK(clip && clip->mediaPath == data + "/media/a.mp4");
    const ProjectData pen = load(out + "/Pen.schneidi");
    const Clip* penClip = withSlot(pen, "clip");
    CHECK(penClip && penClip->mediaPath == data + "/media/b.mp4");
    // Columns that are no slot: one note, not one per row
    const QJsonArray notes = r["notes"].toArray();
    int extra = 0;
    for (const QJsonValue& n : notes) extra += n.toString().contains("price") && n.toString().contains("product");
    CHECK_EQ(extra, 1);
    // Progress: a stage per row
    CHECK(stages.contains("row 1/3") && stages.contains("row 3/3"));

    // The default name: <template>-001 …; an "out" column names the files
    const QJsonObject d1 = run("templates", {{"fill", "Batch"}, {"rows", data + "/products.csv"}, {"out_dir", tmp + "/out2"}});
    CHECK_EQ(filesIn(tmp + "/out2").value(0), QString("Batch-001.schneidi"));
    CHECK(d1["ok"].toBool());
    write(data + "/named.csv", "out,hook\nfirst: one,A\nsecond/two,B\n");
    run("templates", {{"fill", "Batch"}, {"rows", data + "/named.csv"}, {"out_dir", tmp + "/out3"}});
    CHECK_EQ(filesIn(tmp + "/out3"), (QStringList{"first - one.schneidi", "second-two.schneidi"}));

    // The template's own files (the clip slot is not filled here): one folder for the whole batch, not one per row
    CHECK_EQ(QDir(tmp + "/out3").entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{"Batch media"});
    CHECK(withSlot(load(tmp + "/out3/first - one.schneidi"), "clip")->mediaPath.startsWith(tmp + "/out3/Batch media/"));
    CHECK_EQ(withSlot(load(tmp + "/out3/first - one.schneidi"), "clip")->mediaPath,
             withSlot(load(tmp + "/out3/second-two.schneidi"), "clip")->mediaPath);

    // Existing projects stay without overwrite (each row reports it), with overwrite they are replaced
    const QJsonObject again = run("templates", {{"fill", "Batch"}, {"rows", data + "/named.csv"}, {"out_dir", tmp + "/out3"}});
    CHECK(!again["ok"].toBool());
    CHECK_EQ(again["rows"][0]["errors"][0]["code"].toString(), QString("EXISTS"));
    CHECK(run("templates", {{"fill", "Batch"}, {"rows", data + "/named.csv"}, {"out_dir", tmp + "/out3"},
                            {"overwrite", true}})["ok"].toBool());
    // A pattern with an unknown column fails before anything is written
    CHECK_EQ(errorOf({{"fill", "Batch"}, {"rows", data + "/named.csv"}, {"out_dir", tmp + "/out4"}, {"name", "{prodcut}"}}),
             QString("BAD_ARGUMENT"));
    CHECK(!QFileInfo::exists(tmp + "/out4"));
    CHECK_EQ(errorOf({{"fill", "Batch"}, {"rows", data + "/named.csv"}}), QString("BAD_ARGUMENT")); // no out_dir
    CHECK_EQ(errorOf({{"fill", "Batch"}, {"rows", data + "/none.csv"}, {"out_dir", tmp + "/out4"}}), QString("NOT_FOUND"));
    write(data + "/broken.csv", "hook,hook\nA,B\n");
    CHECK_EQ(errorOf({{"fill", "Batch"}, {"rows", data + "/broken.csv"}, {"out_dir", tmp + "/out4"}}), QString("BAD_ROWS"));
}

void testBadRow(const QString& tmp)
{
    std::printf("a bad row: reported, the others done; dry_run; continue_on_error:false\n");
    const QString data = tmp + "/data";
    write(data + "/bad.csv", "hook,clip\nOne,media/a.mp4\nTwo,media/missing.mp4\nThree,media/b.mp4\n");
    // dry_run: every row checked, nothing written
    const QString dryOut = tmp + "/dry";
    const QJsonObject dry = run("templates", {{"fill", "Batch"}, {"rows", data + "/bad.csv"}, {"out_dir", dryOut},
                                              {"dry_run", true}});
    CHECK(dry["dry_run"].toBool());
    CHECK(!dry["ok"].toBool());
    CHECK_EQ(dry["totals"]["failed"].toInt(), 1);
    CHECK_EQ(dry["rows"][1]["errors"][0]["code"].toString(), QString("FILE_MISSING"));
    CHECK_EQ(dry["rows"][1]["errors"][0]["slot"].toString(), QString("clip"));
    CHECK(dry["rows"][0]["ok"].toBool());
    CHECK(!QFileInfo::exists(dryOut));

    const QString out = tmp + "/bad";
    const QJsonObject r = run("templates", {{"fill", "Batch"}, {"rows", data + "/bad.csv"}, {"out_dir", out}});
    CHECK(!r["ok"].toBool());
    CHECK_EQ(r["totals"]["ok"].toInt(), 2);
    CHECK_EQ(r["totals"]["failed"].toInt(), 1);
    CHECK_EQ(r["rows"][1]["errors"][0]["code"].toString(), QString("FILE_MISSING"));
    CHECK(!r["rows"][1].toObject().contains("project")); // nothing written for it
    CHECK_EQ(filesIn(out), (QStringList{"Batch-001.schneidi", "Batch-003.schneidi"}));

    // Stop at the first bad row
    const QJsonObject stop = run("templates", {{"fill", "Batch"}, {"rows", data + "/bad.csv"}, {"out_dir", tmp + "/stop"},
                                               {"continue_on_error", false}});
    CHECK_EQ(stop["totals"]["ok"].toInt(), 1);
    CHECK_EQ(stop["totals"]["skipped"].toInt(), 1);
    CHECK_EQ(filesIn(tmp + "/stop"), QStringList{"Batch-001.schneidi"});
}

void testJsonAndRender(const QString& tmp)
{
    std::printf("JSON file, inline rows, render per row\n");
    const QString data = tmp + "/data";
    // A JSON file: paths relative to it, {file, from, to} and "<slot>.from"
    write(data + "/rows.json", R"({"rows":[{"hook":"J1","clip":{"file":"media/a.mp4","from":"1s","to":"3s"}},
                                          {"hook":"J2","clip":"media/b.mp4","clip.from":"0.5s"}]})");
    const QJsonObject r = run("templates", {{"fill", "Batch"}, {"rows", data + "/rows.json"}, {"out_dir", tmp + "/json"},
                                            {"name", "{hook}"}});
    CHECK_EQ(r["totals"]["ok"].toInt(), 2);
    const ProjectData j1 = load(tmp + "/json/J1.schneidi");
    const Clip* c1 = withSlot(j1, "clip");
    CHECK(c1 && c1->mediaPath == data + "/media/a.mp4" && c1->in == 25);
    const ProjectData j2 = load(tmp + "/json/J2.schneidi");
    const Clip* c2 = withSlot(j2, "clip");
    CHECK(c2 && c2->mediaPath == data + "/media/b.mp4" && (c2->in == 12 || c2->in == 13));

    // Inline rows (as the command line gives them: JSON text) and a render of each
    const QString rows = QString(R"([{"hook":"Rendered","clip":"%1"}])").arg(data + "/media/a.mp4");
    const QJsonObject v = run("templates", {{"fill", "Batch"}, {"rows", rows}, {"out_dir", tmp + "/render"},
                                            {"name", "clip {n}"}, {"render", QJsonObject{{"quality", 2}}}});
    CHECK(v["ok"].toBool());
    const QString video = tmp + "/render/clip 1.mp4";
    CHECK_EQ(v["rows"][0]["video"].toString(), video);
    CHECK(QFileInfo(video).size() > 1000);
    CHECK(QFileInfo::exists(tmp + "/render/clip 1.schneidi"));
    // The format decides the extension; a fixed render.out makes no sense for many rows
    CHECK_EQ(errorOf({{"fill", "Batch"}, {"rows", rows}, {"out_dir", tmp + "/render"},
                      {"render", QJsonObject{{"out", tmp + "/x.mp4"}}}}),
             QString("BAD_ARGUMENT"));
    const QJsonObject wav = run("templates", {{"fill", "Batch"}, {"rows", rows}, {"out_dir", tmp + "/render"}, {"name", "sound"},
                                              {"render", QJsonObject{{"format", "wav"}}}, {"dry_run", true}});
    CHECK_EQ(wav["rows"][0]["video"].toString(), tmp + "/render/sound.wav");
}

// Numbers of JSON rows as they are written (not 1.23457e+06); paths of another computer through --map before a
// relative path is taken against the table's folder
void testNumbersAndMaps(const QString& tmp)
{
    std::printf("JSON numbers, --map\n");
    const QString data = tmp + "/data";
    const QString rows = QString(R"([{"hook":1234567,"clip":"%1"},{"hook":2.5,"clip":"%1"}])").arg(data + "/media/a.mp4");
    QJsonObject r = run("templates", {{"fill", "Batch"}, {"rows", rows}, {"out_dir", tmp + "/numbers"}});
    CHECK_EQ(r["totals"].toObject()["ok"].toInt(), 2);
    CHECK_EQ(withSlot(load(tmp + "/numbers/Batch-001.schneidi"), "hook")->title.text, QString("1234567"));
    CHECK_EQ(withSlot(load(tmp + "/numbers/Batch-002.schneidi"), "hook")->title.text, QString("2.5"));

    Cli::Sandbox::configure({{}, {"Z:\\Clips=" + data + "/media"}});
    write(data + "/mapped.csv", "hook,clip\nMapped,Z:\\Clips\\b.mp4\n");
    r = run("templates", {{"fill", "Batch"}, {"rows", data + "/mapped.csv"}, {"out_dir", tmp + "/mapped"}});
    Cli::Sandbox::configure({});
    CHECK(r["ok"].toBool());
    const Clip* c = withSlot(load(tmp + "/mapped/Batch-001.schneidi"), "clip");
    CHECK(c && c->mediaPath == data + "/media/b.mp4");

    // A map whose target is inside its source: row paths are mapped once, not twice (review C10) - a path of the
    // client's computer and one relative to the table (which is given as the client's path too)
    const QString client = data + "/client";
    QDir().mkpath(client);
    QFile::copy(data + "/media/b.mp4", client + "/b.mp4");
    write(client + "/nested.csv", QString("hook,clip\nAbs,%1/b.mp4\nRel,b.mp4\n").arg(data).toUtf8());
    Cli::Sandbox::configure({{}, {data + "=" + client}});
    r = run("templates", {{"fill", "Batch"}, {"rows", data + "/nested.csv"}, {"out_dir", tmp + "/nested"}});
    Cli::Sandbox::configure({});
    CHECK(r["ok"].toBool());
    for (const char* name : {"/nested/Batch-001.schneidi", "/nested/Batch-002.schneidi"}) {
        c = withSlot(load(tmp + name), "clip");
        CHECK(c && c->mediaPath == client + "/b.mp4");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/xdg-data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/xdg-cache").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/xdg-config").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    AppPaths::setProgramNames(); // the program's folders (also on Windows: with its organization)
    Check::initEngine();
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    const QString media = tmp.path() + "/data/media";
    QDir().mkpath(media);
    const auto make = [&](const char* name, int seconds) {
        return Check::makeMedia(media + "/" + name,
                                {"-f", "lavfi", "-i", QString("testsrc=size=320x180:rate=25:duration=%1").arg(seconds),
                                 "-f", "lavfi", "-i", QString("sine=frequency=440:duration=%1").arg(seconds), "-c:v",
                                 "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    };
    const QString a = make("a.mp4", 4), b = make("b.mp4", 4);
    if (!CHECK(!a.isEmpty() && !b.isEmpty())) return Check::result();
    if (!makeTemplate(tmp.path(), a)) return Check::result();
    testCsv(tmp.path());
    testBadRow(tmp.path());
    testJsonAndRender(tmp.path());
    testNumbersAndMaps(tmp.path());
    return Check::result();
}
