// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graph model (0.19, core/CompGraph): storage form, validation (unknown kinds, inputs, cycles), evaluation
// order, node parameter keys, the effect "comp" in the registry and the render order, media files of "media" nodes
// in the project file (relative paths, relink, "in use"), the single-node runner of engine/picture/CompNodes.
#include "check.h"

#include "core/CompGraph.h"
#include "core/EffectRegistry.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
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
    return Check::result();
}
