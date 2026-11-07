// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graph model (0.19, core/CompGraph): storage form, validation (unknown kinds, inputs, cycles), evaluation
// order, node parameter keys, the effect "comp" in the registry and the render order, media files of "media" nodes
// in the project file (relative paths, relink, "in use"), the single-node runner of engine/picture/CompNodes; the
// core tables of node files and pixel values (Comp::fileKeys/pixelKeys) and everything that follows them: grade LUTs and
// text textures in the project file (relative, LUT fallback), templates (save, ZIP export/import), the media pool's
// "in use", the render cache key, scaling with a resolution change.
#include "check.h"

#include "core/CompGraph.h"
#include "core/EffectFolders.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/TemplateZip.h"
#include "core/TimelineOps.h"
#include "engine/RenderCache.h"
#include "engine/picture/CompNodes.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

// Kinds for validation without the engine: merge(bg, fg, mask), blur(src), media(), clip()
bool fakeKinds(const QString& type, QStringList* inputs)
{
    static const QHash<QString, QStringList> k{
        {"merge", {"bg", "fg", "mask"}}, {"blur", {"src"}}, {"media", {}}, {"clip", {}}};
    if (!k.contains(type)) return false;
    *inputs = k.value(type);
    return true;
}

CompGraph sample()
{
    CompGraph g;
    g.nodes = {{1, "clip", {0, 0}}, {2, "media", {0, 100}}, {3, "blur", {150, 100}}, {4, "merge", {300, 50}}};
    g.links = {{2, 3, "src"}, {3, 4, "bg"}, {1, 4, "fg"}};
    g.out = 4;
    return g;
}

void testModel()
{
    const CompGraph g = sample();
    QString err;
    CHECK(g.validate(fakeKinds, &err));
    CHECK_EQ(g.nextId(), 5);
    CHECK(g.linkInto(4, "fg") && g.linkInto(4, "fg")->from == 1);
    CHECK(!g.linkInto(4, "mask"));

    // Storage form round trip (also through JSON, which turns ints into doubles/longs)
    CompGraph back;
    CHECK(CompGraph::fromVariant(g.toVariant(), &back, &err));
    CHECK(back == g);
    const QJsonObject json = QJsonObject::fromVariantMap(g.toVariant());
    CompGraph viaJson;
    CHECK(CompGraph::fromVariant(json.toVariantMap(), &viaJson, &err));
    CHECK(viaJson == g);

    // Evaluation order: inputs before the node that reads them, out last, unreachable nodes left out
    CompGraph h = g;
    h.nodes << CompNode{9, "blur", {}};
    const QVector<int> order = h.evalOrder();
    CHECK_EQ(order.size(), 4);
    CHECK_EQ(order.last(), 4);
    CHECK(order.indexOf(2) < order.indexOf(3));
    CHECK(!order.contains(9));

    // Broken forms
    CHECK(!CompGraph::fromVariant(QVariant(), &back, &err));
    QVariantMap twice = g.toVariant();
    QVariantList ns = twice["nodes"].toList();
    ns << ns.first();
    twice["nodes"] = ns;
    CHECK(!CompGraph::fromVariant(twice, &back, &err) && err.contains("twice"));
    QVariantMap dangling = g.toVariant();
    QVariantList ls = dangling["links"].toList();
    ls << QVariant(QVariantList{7, 4, "mask"});
    dangling["links"] = ls;
    CHECK(!CompGraph::fromVariant(dangling, &back, &err));
    // Links as objects are read too
    QVariantMap objects = g.toVariant();
    objects["links"] = QVariantList{QVariantMap{{"from", 2}, {"to", 3}, {"input", "src"}}};
    CHECK(CompGraph::fromVariant(objects, &back, &err) && back.links.size() == 1 && back.links[0].input == "src");

    // Validation errors
    CompGraph bad = g;
    bad.nodes << CompNode{5, "sparkle", {}};
    CHECK(!bad.validate(fakeKinds, &err) && err.contains("unknown type"));
    CHECK(bad.validate(fakeKinds, &err, true)); // a newer version's kind: skipped, not an error
    bad = g;
    bad.links << CompLink{2, 4, "src"};
    CHECK(!bad.validate(fakeKinds, &err) && err.contains("no input"));
    bad = g;
    bad.links << CompLink{2, 4, "fg"};
    CHECK(!bad.validate(fakeKinds, &err) && err.contains("twice"));
    bad = g;
    bad.links << CompLink{4, 3, "src"}; // 3 -> 4 -> 3 (src of 3 is then linked twice too; use mask)
    bad.links.removeIf([](const CompLink& l) { return l.from == 2 && l.to == 3; });
    CHECK(!bad.validate(fakeKinds, &err) && err.contains("cycle"));
    CHECK(bad.evalOrder().isEmpty());
    bad = g;
    bad.out = 0;
    CHECK(!bad.validate(fakeKinds, &err));
    CHECK(CompGraph{}.validate(fakeKinds, &err)); // empty graph = nothing to do

    // Parameter keys
    CHECK_EQ(CompGraph::paramKey(3, "size"), QString("n3.size"));
    int id = 0;
    QString key;
    CHECK(CompGraph::splitParamKey("n12.w1.x", &id, &key) && id == 12 && key == "w1.x");
    CHECK(!CompGraph::splitParamKey("w1.x", &id, &key));
    CHECK(!CompGraph::splitParamKey("n.x", &id, &key));
    CHECK(!CompGraph::splitParamKey("n0.x", &id, &key));
    CHECK(!CompGraph::splitParamKey("n3.", &id, &key));

    // Big input: refused, no crash
    QVariantList many;
    for (int i = 1; i <= 5000; ++i) many << QVariantMap{{"id", i}, {"type", "blur"}};
    CHECK(!CompGraph::fromVariant(QVariantMap{{"nodes", many}}, &back, &err));
}

void testEffect()
{
    const EffectDescriptor* d = EffectRegistry::find(Comp::kEffectId);
    if (!CHECK(d)) return;
    CHECK(d->fixed && d->video && !d->library);
    Clip c;
    c.effects << EffectInstance{Comp::kEffectId, {}, true} << EffectInstance{"frei0r.glow", {}, true}
              << EffectInstance{"grade", {}, true};
    const auto order = EffectRegistry::renderOrder(c);
    CHECK_EQ(order.first()->effectId, QString("grade"));
    CHECK_EQ(order.last()->effectId, QString(Comp::kEffectId)); // after the movable effects
    CHECK(!EffectRegistry::movable(Comp::kEffectId));

    // Graph in the instance, orphan values dropped
    EffectInstance& e = c.effects[0];
    CompGraph g = sample();
    Comp::setGraph(e, g);
    e.params[CompGraph::paramKey(3, "radius")] = 4.0;
    e.params[CompGraph::paramKey(8, "radius")] = 9.0;
    CompGraph read;
    CHECK(Comp::graphOf(e, &read) && read == g);
    Comp::dropOrphanParams(e);
    CHECK(e.params.contains("n3.radius") && !e.params.contains("n8.radius"));
}

void testProjectFile(const QString& dir)
{
    QDir().mkpath(dir + "/a/media");
    const QString clipFile = dir + "/a/media/clip.png", bgFile = dir + "/a/media/bg.png";
    for (const QString& f : {clipFile, bgFile}) {
        QFile out(f);
        out.open(QIODevice::WriteOnly);
        out.write("x");
    }
    ProjectData data;
    data.media << MediaInfo{} << MediaInfo{};
    data.media[0].path = clipFile;
    data.media[1].path = bgFile;
    Track t;
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Media;
    c.mediaPath = clipFile;
    c.start = 0;
    c.in = 0;
    c.out = 50;
    EffectInstance e{Comp::kEffectId, {}, true};
    Comp::setGraph(e, sample());
    e.params[CompGraph::paramKey(2, Comp::kPathKey)] = bgFile;
    e.params[CompGraph::paramKey(3, "radius")] = 4.5;
    c.effects << e;
    t.clips << c;
    data.timeline.video << t;

    CHECK(Comp::mediaPaths(e) == QStringList{bgFile});
    CHECK(TimelineOps::usesMediaOnVideo(data.timeline, bgFile)); // a file only a node reads counts as used

    const QString path = dir + "/a/p.schneidi";
    QString err;
    CHECK(ProjectFile::save(data, path, &err));
    const QByteArray json = ProjectFile::toJson(data, path);
    CHECK(json.contains("\"n2.path\": \"media/bg.png\"")); // relative path next to the absolute one
    ProjectData back;
    CHECK(ProjectFile::load(path, &back, &err));
    if (CHECK(!back.timeline.video.isEmpty() && !back.timeline.video[0].clips.isEmpty()))
        CHECK(back.timeline.video[0].clips[0].effects == data.timeline.video[0].clips[0].effects); // unchanged

    // Folder moved: the node's file is found relative to the project like media
    CHECK(QDir().rename(dir + "/a", dir + "/b"));
    ProjectData moved;
    CHECK(ProjectFile::load(dir + "/b/p.schneidi", &moved, &err));
    if (CHECK(!moved.timeline.video.isEmpty() && !moved.timeline.video[0].clips.isEmpty()))
        CHECK(Comp::mediaPaths(moved.timeline.video[0].clips[0].effects[0])
              == QStringList{QDir::cleanPath(dir + "/b/media/bg.png")});

    // Relink: the pool item and the node follow the found file
    QDir().mkpath(dir + "/c/elsewhere");
    CHECK(QFile::copy(dir + "/b/media/bg.png", dir + "/c/elsewhere/bg.png"));
    CHECK(ProjectFile::relink(&data, dir + "/c"));
    CHECK(Comp::mediaPaths(data.timeline.video[0].clips[0].effects[0])
          == QStringList{QDir(dir + "/c/elsewhere").filePath("bg.png")});
}

void testRunner()
{
    using namespace Picture::Comp;
    CHECK(!runNode("no-such-kind", {}, {}, {}, nullptr));
    QStringList inputs;
    CHECK(!kindInfo("no-such-kind", &inputs));
    // Every registered kind: unique id, a name, an eval, parameters with keys
    QSet<QString> ids;
    for (const NodeType& k : all()) {
        CHECK(!k.id.isEmpty() && !ids.contains(k.id));
        ids.insert(k.id);
        CHECK(!k.name.isEmpty());
        CHECK(bool(k.eval));
        for (const EffectParam& p : k.params) CHECK(!p.key.isEmpty() && !p.key.contains('.'));
    }
}


void writeFile(const QString& path, const QByteArray& bytes = "x")
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(bytes);
}

// A media clip whose graph reads three files: media node 2 (path), grade node 3 (lut), text node 4 (texture; its
// words in "text", a Path parameter that is no file). The title clip next to it shows a texture picture.
ProjectData filesProject(const QString& bg, const QString& lut, const QString& tex, const QString& titleTex)
{
    ProjectData data;
    data.media << MediaInfo{} << MediaInfo{};
    data.media[0].path = QFileInfo(bg).absolutePath() + "/clip.mp4";
    data.media[1].path = bg;
    writeFile(data.media[0].path);
    Track t;
    Clip c;
    c.id = 1;
    c.kind = ClipKind::Media;
    c.mediaPath = data.media[0].path;
    c.out = 50;
    CompGraph g;
    g.nodes = {{1, "clip", {}}, {2, "media", {}}, {3, "grade", {}}, {4, "text", {}}, {5, "merge", {}}};
    g.links = {{2, 3, "src"}, {3, 5, "bg"}, {4, 5, "fg"}};
    g.out = 5;
    EffectInstance e{Comp::kEffectId, {}, true};
    Comp::setGraph(e, g);
    e.params["n2.path"] = bg;
    e.params["n3.lut"] = lut;
    e.params["n4.texture"] = tex;
    e.params["n4.text"] = "Hello";
    c.effects << e;
    t.clips << c;
    Clip title;
    title.id = 2;
    title.kind = ClipKind::Title;
    title.start = 60;
    title.out = 50;
    title.title.text = "Title";
    title.title.look.fill = TitleFill::Texture;
    title.title.look.texture = titleTex;
    t.clips << title;
    data.timeline.video << t;
    data.sequences = {Sequence{1, "Timeline 1", false, 0, data.timeline}};
    data.currentSequence = 1;
    data.lastClipId = 2;
    return data;
}

const EffectInstance& compOf(const ProjectData& d) { return d.timeline.video[0].clips[0].effects[0]; }

void testFileKeys()
{
    CHECK(Comp::fileKeys("media") == QStringList{"path"});
    CHECK(Comp::fileKeys("grade") == QStringList{"lut"});
    CHECK(Comp::fileKeys("text") == QStringList{"texture"});
    CHECK(Comp::fileKeys("merge").isEmpty() && Comp::fileKeys("no-such-kind").isEmpty());
    CHECK(Comp::pixelKeys("blur") == QStringList{"radius"});
    CHECK(Comp::pixelKeys("transform").isEmpty() && Comp::pixelKeys("no-such-kind").isEmpty());
    // Every key of both tables is a parameter of that engine kind (a renamed parameter shows here)
    using namespace Picture::Comp;
    for (const NodeType& k : all())
        for (const QStringList& keys : {Comp::fileKeys(k.id), Comp::pixelKeys(k.id)})
            for (const QString& key : keys) {
                const bool has = std::any_of(k.params.begin(), k.params.end(),
                                             [&](const EffectParam& p) { return p.key == key; });
                if (!CHECK(has)) qWarning() << "missing" << k.id << key;
            }
    for (const char* kind : {"media", "grade", "text", "blur", "crop", "mask", "shadow", "glitch", "matte_tune"})
        CHECK(find(QString::fromLatin1(kind)) != nullptr);
}

// Project file: LUT and textures relative to the project file (folder moved), the LUT by name in the own LUTs folder
void testNodeFiles(const QString& dir)
{
    const QString a = dir + "/files/a";
    const ProjectData data = filesProject(a + "/media/bg.png", a + "/luts/look.cube", a + "/media/tex.png",
                                          a + "/media/title.png");
    for (const QString& f : {a + "/media/bg.png", a + "/luts/look.cube", a + "/media/tex.png", a + "/media/title.png"})
        writeFile(f);
    CHECK(Comp::mediaPaths(compOf(data))
          == QStringList({a + "/media/bg.png", a + "/luts/look.cube", a + "/media/tex.png"})); // not the words
    CHECK(TimelineOps::usesMediaOnVideo(data.timeline, a + "/luts/look.cube"));
    QString err;
    CHECK(ProjectFile::save(data, a + "/p.schneidi", &err));
    const QByteArray json = ProjectFile::toJson(data, a + "/p.schneidi");
    CHECK(json.contains("\"n3.lut\": \"luts/look.cube\""));
    CHECK(json.contains("\"n4.texture\": \"media/tex.png\""));
    CHECK_EQ(json.count("\"n4.text\""), 1); // its value only: the words of a text node are no file (no relPath)
    CHECK(QDir().rename(dir + "/files/a", dir + "/files/b"));
    ProjectData moved;
    const QString b = dir + "/files/b";
    CHECK(ProjectFile::load(b + "/p.schneidi", &moved, &err));
    if (CHECK(!moved.timeline.video.isEmpty() && moved.timeline.video[0].clips.size() == 2)) {
        CHECK(Comp::mediaPaths(compOf(moved))
              == QStringList({b + "/media/bg.png", b + "/luts/look.cube", b + "/media/tex.png"}));
        CHECK_EQ(compOf(moved).params.value("n4.text").toString(), QString("Hello"));
        CHECK_EQ(moved.timeline.video[0].clips[1].title.look.texture, b + "/media/title.png");
    }

    // LUT neither at its place nor relative to the project: a file of the same name in the own LUTs folder
    EffectFolders::ensure();
    const QString own = QDir(EffectFolders::lutDir()).filePath("comp-fallback-test.cube");
    writeFile(own);
    ProjectData other = filesProject(b + "/media/bg.png", "C:\\Users\\someone\\LUTs\\comp-fallback-test.cube",
                                     b + "/media/tex.png", b + "/media/title.png");
    CHECK(ProjectFile::save(other, dir + "/files/other.schneidi", &err));
    ProjectData back;
    CHECK(ProjectFile::load(dir + "/files/other.schneidi", &back, &err));
    if (CHECK(!back.timeline.video.isEmpty() && !back.timeline.video[0].clips.isEmpty()))
        CHECK_EQ(compOf(back).params.value("n3.lut").toString(), own);
    QFile::remove(own);

    // Relink: the pool item moved -> the node that reads it follows; title textures likewise
    QDir().mkpath(dir + "/files/c/elsewhere");
    CHECK(QFile::copy(b + "/media/bg.png", dir + "/files/c/elsewhere/bg.png"));
    ProjectData offline = filesProject(dir + "/files/gone/bg.png", b + "/luts/look.cube", b + "/media/tex.png",
                                       b + "/media/title.png");
    CHECK(ProjectFile::relink(&offline, dir + "/files/c") == 1);
    CHECK_EQ(compOf(offline).params.value("n2.path").toString(), QDir(dir + "/files/c/elsewhere").filePath("bg.png"));
}

// Media pool: a file only a node or a title texture reads counts as used (remove_media refuses without its clips)
void testUsage(const QString& dir)
{
    const QString m = dir + "/usage";
    ProjectData data = filesProject(m + "/bg.png", m + "/look.cube", m + "/tex.png", m + "/title.png");
    data.media << MediaInfo{} << MediaInfo{};
    data.media[2].path = m + "/tex.png";
    data.media[3].path = m + "/title.png";
    Project p;
    p.load(data);
    for (const QString& f : {m + "/bg.png", m + "/tex.png", m + "/title.png"}) {
        const auto usage = p.mediaUsage({f});
        if (CHECK_EQ(usage.size(), 1)) CHECK_EQ(usage[0].second, 1);
        CHECK(!p.removeMedia({f}, false)); // used: nothing removed
    }
    CHECK_EQ(p.media().size(), 4);
    CHECK(p.removeMedia({m + "/title.png"}, true)); // with its clips: the title goes too
    CHECK_EQ(p.timeline().video[0].clips.size(), 1);
}

// Templates: save copies the node files and the title texture into the template, ZIP export/import keeps them
void testTemplates(const QString& dir)
{
    const QString src = dir + "/tpl-src";
    for (const QString& f : {src + "/bg.png", src + "/look.cube", src + "/tex.png", src + "/title.png"}) writeFile(f);
    const ProjectData data = filesProject(src + "/bg.png", src + "/look.cube", src + "/tex.png", src + "/title.png");
    QString err, code;
    const auto saved = TemplateLibrary::save(data, "Comp files test", {}, &err, &code);
    if (!CHECK(saved.has_value())) {
        qWarning() << err << code;
        return;
    }
    const auto inside = [](const ProjectData& d, const QString& folder) {
        QStringList files = Comp::mediaPaths(compOf(d));
        files << d.timeline.video[0].clips[1].title.look.texture;
        bool ok = files.size() == 4;
        for (const QString& f : files) {
            ok = ok && f.startsWith(QDir::cleanPath(folder) + '/') && QFileInfo::exists(f);
            if (!ok) qWarning() << "not in" << folder << f;
        }
        return ok;
    };
    ProjectData placed;
    CHECK(TemplateLibrary::load(*saved, &placed, &err));
    if (CHECK(!placed.timeline.video.isEmpty() && placed.timeline.video[0].clips.size() == 2))
        CHECK(inside(placed, saved->dir));
    const QString zip = dir + "/comp-files.zip";
    CHECK(TemplateLibrary::exportZip(saved->id, zip, true, &err, &code));
    CHECK(TemplateLibrary::remove(saved->id, &err));
    const auto imported = TemplateLibrary::importZip(zip, TemplateLibrary::ZipClash::Replace, &err, &code);
    if (CHECK(imported.has_value())) {
        ProjectData back;
        CHECK(TemplateLibrary::load(*imported, &back, &err));
        if (CHECK(!back.timeline.video.isEmpty() && back.timeline.video[0].clips.size() == 2))
            CHECK(inside(back, imported->dir));
        TemplateLibrary::remove(imported->id, &err);
    }
}

// Render cache: a node's LUT or texture edited under the same name -> another key
void testRenderCacheKey(const QString& dir)
{
    const QString r = dir + "/cache";
    for (const QString& f : {r + "/bg.png", r + "/look.cube", r + "/tex.png", r + "/title.png"}) writeFile(f);
    const ProjectData data = filesProject(r + "/bg.png", r + "/look.cube", r + "/tex.png", r + "/title.png");
    const Clip& c = data.timeline.video[0].clips[0];
    const ProjectFormat f;
    const QString k0 = RenderCache::key(c, f);
    CHECK_EQ(RenderCache::key(c, f), k0);
    writeFile(r + "/tex.png", "a longer texture");
    const QString k1 = RenderCache::key(c, f);
    CHECK(k1 != k0);
    writeFile(r + "/look.cube", "a longer LUT file");
    CHECK(RenderCache::key(c, f) != k1);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!CHECK(tmp.isValid())) return Check::result();
    testModel();
    testEffect();
    testProjectFile(tmp.path());
    testRunner();
    testFileKeys();
    testNodeFiles(tmp.path());
    testUsage(tmp.path());
    testTemplates(tmp.path());
    testRenderCacheKey(tmp.path());
    return Check::result();
}
