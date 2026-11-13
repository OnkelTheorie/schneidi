// SPDX-License-Identifier: AGPL-3.0-only
// The five built-in content templates (assets/templates/<id>/, made by tools/make_builtin_templates.py): listed by
// `templates` with their format, length, slotIds and preview, names shown in the UI language; each fills without errors
// from synthetic files (check + real fill, one rendered); their titles follow the brand kit (and look neutral without
// one); a logo slot without a value takes the brand kit's logo; a size guard keeps the resources small (they are
// compiled into the program).

#include "check.h"

#include "cli/Commands.h"
#include "core/AppPaths.h"
#include "core/I18n.h"
#include "core/ProjectFile.h"
#include "core/TemplateLibrary.h"
#include "core/TemplateZip.h"
#include "core/Zip.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
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

ProjectData load(const QString& path)
{
    ProjectData d;
    QString err;
    if (!ProjectFile::load(path, &d, &err)) std::printf("       cannot load %s: %s\n", qPrintable(path), qPrintable(err));
    return d;
}

struct Expected {
    const char* id;
    const char* aspect;
    double length;
    QStringList slotIds;
};

const QVector<Expected> kTemplates{
    {"short-hook", "9:16", 12, {"clip1", "clip2", "clip3", "hook", "music"}},
    {"gaming-highlight", "16:9", 15, {"clip1", "clip2", "clip3", "clip4", "clip5", "title", "music"}},
    {"product-15s", "1:1", 15, {"product1", "product2", "product3", "name", "claim", "cta", "logo", "music"}},
    {"podcast-clip", "9:16", 20, {"speaker", "topic", "logo"}},
    {"intro-outro", "16:9", 4, {"background", "logo", "name", "cta"}},
};

QJsonObject findId(const QJsonArray& list, const QString& id)
{
    for (const QJsonValue& v : list)
        if (v.toObject()["id"].toString() == id) return v.toObject();
    return {};
}

void testListed()
{
    std::printf("listed\n");
    const QJsonArray all = run("templates", {})["templates"].toArray();
    for (const Expected& e : kTemplates) {
        const QJsonObject t = findId(all, e.id);
        if (!CHECK(!t.isEmpty())) {
            std::printf("       missing: %s\n", e.id);
            continue;
        }
        CHECK_EQ(t["kind"].toString(), QString("content"));
        CHECK(t["built_in"].toBool());
        CHECK_EQ(t["aspect"].toString(), QString(e.aspect));
        CHECK_EQ(t["length_s"].toDouble(), e.length);
        QStringList slotIds;
        for (const QJsonValue& v : t["slots"].toArray()) slotIds << v.toString();
        QStringList want = e.slotIds;
        slotIds.sort();
        want.sort();
        CHECK_EQ(slotIds, want);
        CHECK(!t["description"].toString().isEmpty());
    }
    // Each has a small preview, its media inside its folder (relative paths), and info shows the slotIds' rules
    for (const ContentTemplate& t : TemplateLibrary::builtIn()) {
        CHECK(QFileInfo(t.preview).fileName() == "preview.mp4");
        CHECK(QFileInfo(t.preview).size() <= 400 * 1024);
        ProjectData d;
        CHECK(TemplateLibrary::load(t, &d));
        for (const MediaInfo& m : d.media) CHECK(m.path.startsWith(t.dir + "/media/") && QFileInfo::exists(m.path));
    }
    const QJsonObject info = run("templates", {{"info", "short-hook"}});
    const QJsonObject hook = findId(info["slots"].toArray(), "hook");
    CHECK_EQ(hook["kind"].toString(), QString("text"));
    CHECK_EQ(hook["max_chars"].toInt(), 40);
    CHECK_EQ(findId(info["slots"].toArray(), "clip1")["fit"].toString(), QString("freeze"));
    CHECK(info.contains("brand_refs"));
}

void testNamesTranslated()
{
    std::printf("names in the UI language\n");
    I18n::install("en");
    const std::optional<ContentTemplate> en = TemplateLibrary::find("short-hook");
    CHECK(en && en->name == "Short with hook");
    CHECK(en && en->description.startsWith("Vertical short"));
    // Found by its shown name too
    CHECK(TemplateLibrary::find("Podcast excerpt").has_value());
    // Exported as ZIP: the archive carries the shown name, not the German one it is stored with
    QTemporaryDir tmp;
    const QString zip = tmp.filePath("hook.zip");
    QString err;
    CHECK(TemplateLibrary::exportZip("short-hook", zip, false, &err));
    CHECK(Zip::extract(zip, tmp.filePath("out"), "short-hook", &err));
    ProjectData d;
    CHECK(ProjectFile::load(tmp.filePath("out/template.schneidi"), &d, &err));
    CHECK(d.templateInfo.has_value());
    if (d.templateInfo) {
        CHECK_EQ(d.templateInfo->name, QString("Short with hook"));
        CHECK(d.templateInfo->description.startsWith("Vertical short"));
    }
}

struct Media {
    QString wide, tall, longTall, still, logo, song;
};

QJsonObject valuesFor(const Expected& e, const Media& m)
{
    QJsonObject v;
    for (const QString& s : e.slotIds) {
        if (s == "hook" || s == "title" || s == "name" || s == "claim" || s == "cta" || s == "topic")
            v[s] = QString("Sample %1").arg(s);
        else if (s == "music") v[s] = m.song;
        else if (s == "logo") v[s] = m.logo;
        else if (s.startsWith("product")) v[s] = m.still;
        else if (s == "speaker") v[s] = m.longTall;
        else v[s] = QString(e.aspect) == "9:16" ? m.tall : m.wide;
    }
    return v;
}

void testFill(const QString& tmp, const Media& m)
{
    std::printf("fill each\n");
    for (const Expected& e : kTemplates) {
        const QJsonObject values = valuesFor(e, m);
        const QJsonObject dry = run("templates", {{"fill", e.id}, {"values", values}, {"dry_run", true}, {"strict", true}});
        CHECK(dry["ok"].toBool());
        CHECK(dry["errors"].toArray().isEmpty());
        const QString out = tmp + "/filled/" + e.id + ".schneidi";
        QJsonObject args{{"fill", e.id}, {"values", values}, {"out_project", out}};
        const QString video = tmp + "/filled/" + e.id + ".mp4";
        if (QString(e.id) == "intro-outro") args["render"] = QJsonObject{{"out", video}, {"height", 144}};
        const QJsonObject r = run("templates", args);
        if (!CHECK(r["ok"].toBool())) std::printf("       %s failed\n", e.id);
        CHECK(r["errors"].toArray().isEmpty());
        CHECK(r["unfilled"].toArray().isEmpty());
        const ProjectData d = load(out);
        CHECK(!d.templateInfo.has_value());
        // The values' files are in the project
        QSet<QString> files;
        for (const MediaInfo& mi : d.media) files.insert(mi.path);
        for (const QJsonValue& v : values)
            if (v.toString().startsWith('/')) CHECK(files.contains(v.toString()));
        if (QString(e.id) == "podcast-clip") {
            // The speaker slot grew to the 30 s clip: the topic title and the logo still cover all of it
            const int end = d.timeline.video[0].clips.isEmpty() ? 0 : d.timeline.video[0].clips.last().end();
            CHECK_EQ(end, 900);
            for (int t = 1; t < d.timeline.video.size(); ++t)
                for (const Clip& c : d.timeline.video[t].clips) CHECK_EQ(c.end(), end);
        }
        if (QString(e.id) == "intro-outro") {
            CHECK(r.contains("render"));
            CHECK(QFileInfo(video).size() > 0);
        }
    }
}

const Clip* titleWithText(const ProjectData& d, const QString& text)
{
    for (const Track& t : d.timeline.video)
        for (const Clip& c : t.clips)
            if (c.isTitle() && c.title.text == text) return &c;
    return nullptr;
}

void testBrand(const QString& tmp, const Media& m)
{
    std::printf("brand kit\n");
    // Empty kit: the stored values (white text, dark outline)
    const QString plain = tmp + "/brand/plain.schneidi";
    CHECK(run("templates", {{"fill", "short-hook"}, {"values", QJsonObject{{"hook", "Plain"}}}, {"out_project", plain}})["ok"].toBool());
    const Clip* t = titleWithText(load(plain), "Plain");
    CHECK(t && t->title.colorRef == 1 && t->title.color.name() == "#ffffff");
    CHECK(t && t->title.outlineRef == 2 && t->title.outlineColor.name() == "#000000");
    // A kit: its colors reach the titles of every template
    const QJsonObject kit{{"colors", QJsonArray{"#ff2200", "#0022ff"}}};
    for (const Expected& e : kTemplates) {
        const QString out = tmp + "/brand/" + e.id + ".schneidi";
        const QJsonObject r = run("templates", {{"fill", e.id}, {"out_project", out}, {"brand", kit}});
        CHECK(r["brand"]["references"].toInt() > 0);
        const ProjectData d = load(out);
        int branded = 0;
        for (const Sequence& s : d.sequences)
            for (const Track& tr : (s.id == d.currentSequence ? d.timeline : s.timeline).video)
                for (const Clip& c : tr.clips) {
                    if (!c.isTitle()) continue;
                    CHECK(c.title.hasBrandRef()); // every title follows the kit
                    if (c.title.colorRef == 1) branded += c.title.color.name() == "#ff2200";
                    if (c.title.colorRef == 2) branded += c.title.color.name() == "#0022ff";
                }
        if (!CHECK(branded > 0)) std::printf("       %s: no title took the kit's colors\n", e.id);
    }
    // Podcast captions: the subtitle track's style follows the kit too
    const ProjectData pod = load(tmp + "/brand/podcast-clip.schneidi");
    CHECK(!pod.timeline.subtitles.isEmpty() && pod.timeline.subtitles[0].style.outlineRef == 2
          && pod.timeline.subtitles[0].style.outlineColor.name() == "#0022ff");

    // A logo slot without a value shows the kit's logo; "none" keeps the template's
    const QString withLogo = tmp + "/brand/logo.schneidi";
    const QJsonObject r = run("templates", {{"fill", "product-15s"}, {"out_project", withLogo}, {"brand", QJsonObject{{"logo", m.logo}}}});
    CHECK(r["unfilled"].toArray().size() == 7); // all but the logo
    bool logo = false;
    for (const MediaInfo& mi : load(withLogo).media) logo |= mi.path == m.logo;
    CHECK(logo);
    const QString without = tmp + "/brand/nologo.schneidi";
    run("templates", {{"fill", "product-15s"}, {"out_project", without}, {"brand", "none"}});
    logo = false;
    for (const MediaInfo& mi : load(without).media) logo |= mi.path == m.logo;
    CHECK(!logo);
}

void testSize()
{
    std::printf("size guard\n");
    qint64 bytes = 0;
    QDirIterator it(Check::sourceDir() + "/assets/templates", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) bytes += QFileInfo(it.next()).size();
    std::printf("       assets/templates: %.2f MB\n", bytes / 1048576.0);
    CHECK(bytes > 0);
    CHECK(bytes < qint64(3.5 * 1024 * 1024)); // compiled into the program: keep placeholder media small
}

} // namespace

int main(int argc, char* argv[])
{
    QTemporaryDir tmp;
    qputenv("XDG_DATA_HOME", (tmp.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (tmp.path() + "/cache").toUtf8());
    qputenv("XDG_CONFIG_HOME", (tmp.path() + "/config").toUtf8());
    Check::initEnv();
    QApplication app(argc, argv);
    AppPaths::setProgramNames(); // the program's folders (also on Windows: with its organization)
    Check::initEngine();
    I18n::install("en");
    testSize();
    if (!Check::haveFfmpeg()) return Check::skip("no ffmpeg");
    const QString dir = tmp.path() + "/media";
    QDir().mkpath(dir);
    const auto video = [&](const char* name, int seconds, const char* size) {
        return Check::makeMedia(dir + "/" + name,
                                {"-f", "lavfi", "-i", QString("testsrc=size=%1:rate=30:duration=%2").arg(size).arg(seconds),
                                 "-f", "lavfi", "-i", QString("sine=frequency=440:duration=%1").arg(seconds), "-c:v",
                                 "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
    };
    Media m;
    m.wide = video("wide.mp4", 5, "320x180");
    m.tall = video("tall.mp4", 2, "180x320"); // shorter than the slotIds: frozen / slowed
    m.longTall = video("long.mp4", 30, "180x320");
    m.still = Check::makeMedia(dir + "/still.png", {"-f", "lavfi", "-i", "testsrc=size=300x300", "-frames:v", "1"});
    m.logo = Check::makeMedia(dir + "/logo.png", {"-f", "lavfi", "-i", "color=c=red:s=64x64", "-frames:v", "1"});
    m.song = Check::makeMedia(dir + "/song.m4a", {"-f", "lavfi", "-i", "sine=frequency=220:duration=25", "-c:a", "aac"});
    if (!CHECK(!m.wide.isEmpty() && !m.tall.isEmpty() && !m.longTall.isEmpty() && !m.still.isEmpty()
               && !m.logo.isEmpty() && !m.song.isEmpty()))
        return Check::result();
    testListed();
    testNamesTranslated();
    testFill(tmp.path(), m);
    testBrand(tmp.path(), m);
    return Check::result();
}
