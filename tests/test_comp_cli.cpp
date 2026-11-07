// SPDX-License-Identifier: AGPL-3.0-only
// Compositing graph through the CLI layer (0.19 package G): edit ops comp / comp_from / node / track_node with
// dry_run and errors, keyframes of node values (comp.n<id>.<key>), `effects kind:'nodes'`, the graph in `info`,
// `frames` at a node (mask and matte as gray), the check findings; the Editor ops are one undo step each.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/ExportCheck.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "engine/picture/CompNodes.h"

#include "render_check.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

QString g_project;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        qWarning().noquote() << "unexpected error" << e.code << e.message;
        throw;
    }
}

QString messageOf(const QString& name, const QJsonObject& raw)
{
    try {
        const Cli::Command* cmd = Cli::find(name);
        Cli::Context ctx;
        cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        return e.code + ": " + e.message;
    }
    return "none";
}

QJsonObject edit(const QJsonArray& ops, bool dry = false)
{
    return run("edit", {{"project", g_project}, {"ops", ops}, {"dry_run", dry}});
}
QString editError(const QJsonArray& ops) { return messageOf("edit", {{"project", g_project}, {"ops", ops}}); }

QJsonObject op(const char* json) { return QJsonDocument::fromJson(json).object(); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

const Clip* clipIn(const ProjectData& d, int id) { return TimelineOps::findClip(d.timeline, id); }

CompGraph graphOf(int clip)
{
    const ProjectData d = load();
    CompGraph g;
    const Clip* c = clipIn(d, clip);
    if (const EffectInstance* e = c ? EffectRegistry::instance(*c, Comp::kEffectId) : nullptr) Comp::graphOf(*e, &g);
    return g;
}

QByteArray fileBytes()
{
    QFile f(g_project);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

// The single image `frames` wrote for the clip at its first frame
QImage nodeFrame(const QString& dir, QJsonObject a)
{
    a["source"] = g_project;
    a["out"] = dir;
    a["width"] = 320;
    const QJsonObject r = run("frames", a);
    return QImage(r["frames"].toArray().first().toObject()["file"].toString());
}

QString answerText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_cli");
    Check::initEngine();
    QTemporaryDir dir;

    // Synthetic picture: a red square on green (a green screen with a figure)
    const QString still = dir.filePath("figure.png");
    {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(0, 255, 0));
        for (int y = 60; y < 120; ++y)
            for (int x = 120; x < 200; ++x) img.setPixel(x, y, qRgb(255, 0, 0));
        CHECK(img.save(still));
    }
    g_project = dir.filePath("comp.schneidi");
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    edit({QJsonObject{{"op", "append"}, {"media", still}, {"duration", "2s"}},
          QJsonObject{{"op", "place"}, {"media", still}, {"at", 0}, {"duration", "2s"}, {"track", "V2"}}});
    int clip = 0, upper = 0;
    {
        const ProjectData d = load();
        if (!CHECK(d.timeline.video.size() >= 2 && !d.timeline.video[0].clips.isEmpty()
                   && !d.timeline.video[1].clips.isEmpty()))
            return Check::result();
        clip = d.timeline.video[0].clips.first().id;
        upper = d.timeline.video[1].clips.first().id;
    }

    // effects kind:'nodes': every registered kind with inputs and parameters
    {
        const QJsonObject r = run("effects", {{"kind", "nodes"}});
        const QJsonArray nodes = r["nodes"].toArray();
        CHECK_EQ(nodes.size(), Picture::Comp::all().size());
        QStringList types;
        for (const QJsonValue& v : nodes) types << v["type"].toString();
        for (const char* t : {"clip", "media", "color", "merge", "mask", "matte", "key", "matte_tune", "tracker", "text"})
            if (!CHECK(types.contains(t))) std::printf("       kind %s missing\n", t);
        for (const QJsonValue& v : nodes)
            if (v["type"] == "merge") {
                CHECK(v["inputs"].toArray() == (QJsonArray{"bg", "fg", "mask"}));
                bool opacity = false;
                for (const QJsonValue& p : v["params"].toArray())
                    if (p["key"] == "opacity") opacity = p["type"] == "number" && p["min"] == 0 && p["max"] == 1 && p["default"] == 1;
                CHECK(opacity);
            }
        CHECK(messageOf("effects", {{"kind", "bogus"}}).contains("'nodes'"));
    }

    // node add: a clip without a graph gets clip -> key; dry_run changes nothing
    {
        const QByteArray before = fileBytes();
        QJsonObject add = op(R"({"op":"node","add":{"type":"key","params":{"color":"#00ff00"}}})");
        add["clip"] = clip;
        const QJsonObject dry = edit({add}, true);
        CHECK(dry["saved"] == false && dry["notes"].toArray().first().toObject()["node"].toObject()["id"] == 2);
        CHECK(fileBytes() == before);
    }
    {
        QJsonObject add = op(R"({"op":"node","add":{"type":"key","params":{"color":"#00ff00","tolerance":0.3}}})");
        add["clip"] = clip;
        const QJsonObject r = edit({add});
        const QJsonObject note = r["notes"].toArray().first().toObject()["node"].toObject();
        CHECK_EQ(note["id"].toInt(), 2);
        CHECK(note["type"] == "key");
        const CompGraph g = graphOf(clip);
        CHECK(g.nodes.size() == 2 && g.out == 2 && g.node(1) && g.node(1)->type == "clip");
        CHECK(g.linkInto(2, "src") && g.linkInto(2, "src")->from == 1);
        const ProjectData d = load();
        const EffectInstance* e = EffectRegistry::instance(*clipIn(d, clip), Comp::kEffectId);
        CHECK(e && e->params.value("n2.tolerance").toDouble() == 0.3);
    }

    // Errors: unknown kind, a value out of range, an input the kind does not have, a cycle - nothing saved
    {
        const QByteArray before = fileBytes();
        const auto with = [&](const char* json) {
            QJsonObject o = op(json);
            o["clip"] = clip;
            return editError({o});
        };
        CHECK(with(R"({"op":"node","add":{"type":"wobble"}})").contains("unknown node type 'wobble'"));
        CHECK(with(R"({"op":"node","add":{"type":"blur","params":{"radius":-4}}})").contains("between"));
        CHECK(with(R"({"op":"node","add":{"type":"blur","params":{"size":3}}})").contains("has no parameter 'size'"));
        CHECK(with(R"({"op":"node","link":[1,2,"fg"]})").contains("has no input 'fg'"));
        CHECK(with(R"({"op":"node","id":9,"remove":true})").contains("no node 9"));
        // A transform after the key, then the key reading the transform: a cycle
        QJsonObject t = op(R"({"op":"node","add":{"type":"transform"}})");
        t["clip"] = clip;
        QJsonObject cyc = op(R"({"op":"node","link":[3,2,"src"]})");
        cyc["clip"] = clip;
        const QString cycle = editError({t, cyc});
        if (!CHECK(cycle.contains("cycle"))) std::printf("       %s\n", qPrintable(cycle));
        CHECK(fileBytes() == before); // all or nothing
        QJsonObject g = op(R"({"op":"comp","graph":{"nodes":[{"id":1,"type":"clip"},{"id":2,"type":"sparkle"}],"links":[[1,2]]}})");
        g["clips"] = QJsonArray{clip};
        CHECK(editError({g}).contains("unknown node type 'sparkle'"));
        CHECK(editError({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", "bogus"}}}).contains("no recipe 'bogus'"));
    }

    // frames at a node: the clip node shows the clip, the key's matte is white on the figure and black on the screen
    {
        const QImage plain = nodeFrame(dir.filePath("f1"), {{"clip", clip}, {"node", 1}});
        if (CHECK(!plain.isNull())) {
            CHECK(qGreen(plain.pixel(10, 10)) > 200 && qRed(plain.pixel(10, 10)) < 60);
            CHECK(qRed(plain.pixel(160, 90)) > 200);
        }
        const QImage matte = nodeFrame(dir.filePath("f2"), {{"clip", clip}, {"node", 2}, {"view", "matte"}});
        if (CHECK(!matte.isNull())) {
            CHECK(qGray(matte.pixel(160, 90)) > 220); // the figure: solid
            CHECK(qGray(matte.pixel(10, 10)) < 30);   // the screen: transparent
        }
        // Without node: the output (here the key, its picture over black)
        const QImage out = nodeFrame(dir.filePath("f3"), {{"clip", clip}});
        if (CHECK(!out.isNull())) CHECK(qGreen(out.pixel(10, 10)) < 40 && qRed(out.pixel(160, 90)) > 200);
        CHECK(messageOf("frames", {{"source", g_project}, {"clip", clip}, {"node", 7}, {"out", dir.filePath("f4")}})
                  .contains("no node 7"));
        CHECK(messageOf("frames", {{"source", still}, {"node", 1}, {"out", dir.filePath("f4")}}).contains("for a project"));
    }

    // A mask added on its own (input:false) does not reach the output: shown as gray at its node, comp_unused_input in
    // check and info
    {
        QJsonObject m = op(R"({"op":"node","add":{"type":"mask","params":{"shape":"rect","w":0.5,"h":0.5}},"input":false})");
        m["clip"] = clip;
        const QJsonObject r = edit({m});
        const int mask = r["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
        CHECK_EQ(mask, 3);
        CHECK(graphOf(clip).out == 2); // only added
        const QImage gray = nodeFrame(dir.filePath("f5"), {{"clip", clip}, {"node", mask}});
        if (CHECK(!gray.isNull())) {
            CHECK(qGray(gray.pixel(160, 90)) > 220);
            CHECK(qGray(gray.pixel(10, 10)) < 30);
        }
        const QJsonObject info = run("info", {{"project", g_project}});
        const QString text = answerText(info);
        CHECK(text.contains(R"("graph":{)") && text.contains(R"("unused":[3])") && text.contains("cost_ms"));
        const QJsonObject check = run("check", {{"source", g_project}, {"sound", false}});
        const QString ct = answerText(check);
        CHECK(ct.contains("comp_unused_input"));
        CHECK(!ct.contains("key_edges")); // the key of a clean figure is clean
        CHECK(!ct.contains("comp_slow"));
    }

    // Link the mask into a blur behind the key, change values, set out, remove the blur again
    {
        QJsonObject b = op(R"({"op":"node","add":{"type":"blur","params":{"radius":4}}})");
        b["clip"] = clip;
        QJsonObject l = op(R"({"op":"node","link":[3,4,"mask"]})");
        l["clip"] = clip;
        QJsonObject p = op(R"({"op":"node","id":4,"params":{"radius":6}})");
        p["clip"] = clip;
        edit({b, l, p});
        CompGraph g = graphOf(clip);
        CHECK(g.out == 4 && g.linkInto(4, "src") && g.linkInto(4, "src")->from == 2 && g.linkInto(4, "mask")
              && g.linkInto(4, "mask")->from == 3);
        CHECK(load().timeline.video[0].clips.first().effects.size() >= 1);
        {
            const ProjectData d = load();
            const EffectInstance* e = EffectRegistry::instance(*clipIn(d, clip), Comp::kEffectId);
            CHECK(e && e->params.value("n4.radius").toDouble() == 6);
        }
        // Keyframes of a node value; a node that is not there and a non-number are errors
        QJsonObject k = op(R"({"op":"keyframe","param":"comp.n4.radius","keys":[{"at":0,"value":2},{"at":10,"value":900}]})");
        k["clips"] = QJsonArray{clip};
        edit({k});
        {
            const ProjectData d = load();
            const Clip* c = clipIn(d, clip);
            const AnimParam ap = Keys::effectParam(Comp::kEffectId, "n4.radius");
            CHECK(c && Keys::animated(*c, ap));
            CHECK(c && Keys::valueAt(*c, ap, 10) == 500); // clamped to the blur's range (0..500)
        }
        CHECK(answerText(run("info", {{"project", g_project}})).contains("comp.n4.radius"));
        QJsonObject bad = op(R"({"op":"keyframe","param":"comp.n9.radius","at":0,"value":1})");
        bad["clips"] = QJsonArray{clip};
        CHECK(editError({bad}).contains("no node 9"));
        bad["param"] = "comp.n3.shape";
        CHECK(editError({bad}).contains("only numbers"));
        bad["param"] = "comp.radius";
        CHECK(editError({bad}).contains("comp.n<node id>.<key>"));
        // Remove the blur: what read it reads its src (the key) again, its values and keyframes go
        QJsonObject rm = op(R"({"op":"node","id":4,"remove":true})");
        rm["clip"] = clip;
        edit({rm});
        g = graphOf(clip);
        CHECK(g.out == 2 && !g.node(4));
        {
            const ProjectData d = load();
            const Clip* c = clipIn(d, clip);
            const EffectInstance* e = EffectRegistry::instance(*c, Comp::kEffectId);
            CHECK(e && !e->params.contains("n4.radius"));
            CHECK(!Keys::animated(*c, Keys::effectParam(Comp::kEffectId, "n4.radius")));
        }
        QJsonObject o = op(R"({"op":"node","id":1,"out":true})");
        o["clip"] = clip;
        edit({o});
        CHECK(graphOf(clip).out == 1);
        QJsonObject un = op(R"({"op":"node","unlink":[1,2,"src"]})");
        un["clip"] = clip;
        edit({un});
        CHECK(!graphOf(clip).linkInto(2, "src"));
    }

    // Tracker: a point (the figure's corner) on the still stays where it is; keyframes of x/y on the node
    {
        QJsonObject t = op(R"({"op":"node","add":{"type":"tracker","params":{"ax":0.375,"ay":0.3333}},"input":false})");
        t["clip"] = clip;
        const QJsonObject r = edit({t});
        const int tr = r["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
        QJsonObject run1{{"op", "track_node"}, {"clip", clip}, {"node", tr}, {"from", 0}, {"to", 10}};
        const QJsonObject a = edit({run1});
        const QJsonObject note = a["notes"].toArray().first().toObject()["track_node"].toObject();
        if (!CHECK(note["points"].toInt() >= 2)) std::printf("       %s\n", qPrintable(answerText(a)));
        CHECK(note["gave_up"] == false && note["lost_at"].isNull());
        const ProjectData d = load();
        const Clip* c = clipIn(d, clip);
        const AnimParam x = Keys::effectParam(Comp::kEffectId, QString("n%1.x").arg(tr));
        CHECK(c && Keys::animated(*c, x) && std::abs(Keys::valueAt(*c, x, 5) - 0.375) < 0.01);
        CHECK(editError({QJsonObject{{"op", "track_node"}, {"clip", clip}, {"node", 2}}}).contains("not a tracker"));
    }

    // Whole graph at once, then off again
    {
        QJsonObject g = op(R"({"op":"comp","graph":{"nodes":[{"id":1,"type":"clip"},{"id":2,"type":"color","params":{"color":"#ff0000ff"}},{"id":3,"type":"merge","params":{"opacity":0.5}}],"links":[[1,3,"bg"],[2,3]]}})");
        g["clips"] = QJsonArray{clip};
        edit({g});
        const CompGraph cg = graphOf(clip);
        CHECK(cg.nodes.size() == 3 && cg.out == 3 && cg.linkInto(3, "fg") && cg.linkInto(3, "fg")->from == 2);
        {
            const ProjectData d = load();
            const EffectInstance* e = EffectRegistry::instance(*clipIn(d, clip), Comp::kEffectId);
            CHECK(e && e->params.value("n2.color").toString() == "#ff0000ff" && !e->params.contains("n4.x"));
            // the tracker's keyframes went with the old graph
            CHECK(!Keys::hasKeys(*clipIn(d, clip)) || !Keys::animated(*clipIn(d, clip), Keys::effectParam(Comp::kEffectId, "n5.x")));
        }
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});
        CHECK(!EffectRegistry::has(*clipIn(load(), clip), Comp::kEffectId));
        CHECK(editError({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}}).contains("none of the clips"));
    }

    // unlink takes only the link from `from` (round 2): without an input the one `from` feeds, a link that is not
    // there is an error and nothing else goes
    {
        QJsonObject g = op(R"({"op":"comp","graph":{"nodes":[{"id":1,"type":"clip"},{"id":2,"type":"color"},{"id":3,"type":"merge"}],"links":[[1,3,"bg"],[2,3,"fg"]],"out":3}})");
        g["clips"] = QJsonArray{clip};
        edit({g});
        QJsonObject un = op(R"({"op":"node","unlink":[2,3]})");
        un["clip"] = clip;
        edit({un});
        CompGraph cg = graphOf(clip);
        CHECK(cg.linkInto(3, "bg") && cg.linkInto(3, "bg")->from == 1 && !cg.linkInto(3, "fg"));
        un["unlink"] = QJsonArray{2, 3};
        CHECK(editError({un}).contains("not linked into node 3"));
        un["unlink"] = QJsonArray{2, 3, "bg"}; // bg is fed by node 1, not 2
        CHECK(editError({un}).contains("not linked into input 'bg'"));
        CHECK(graphOf(clip).linkInto(3, "bg"));
        un["unlink"] = QJsonArray{1, 3, true};
        CHECK(editError({un}).contains("the input is a name"));
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});
    }

    // File parameters (Comp::fileKeys): absolute, readable, a LUT by name as op 'look' (round 2)
    {
        QJsonObject add = op(R"({"op":"node","add":{"type":"grade","params":{"lut":"teal-orange"}}})");
        add["clip"] = clip;
        const QJsonObject r = edit({add});
        const QJsonObject graph = r["notes"].toArray().first().toObject()["node"].toObject()["graph"].toObject();
        const QString lut = graph["nodes"].toArray().last().toObject()["params"].toObject()["lut"].toString();
        CHECK(QFileInfo(lut).isAbsolute() && QFileInfo(lut).isFile());
        add["add"] = op(R"({"type":"grade","params":{"lut":"no-such-lut"}})");
        CHECK(editError({add}).contains("no LUT 'no-such-lut'"));
        add["add"] = QJsonObject{{"type", "text"}, {"params", QJsonObject{{"texture", dir.filePath("missing.png")}}}};
        CHECK(editError({add}).contains("cannot read the file"));
        add["add"] = QJsonObject{{"type", "text"}, {"params", QJsonObject{{"texture", still}}}};
        edit({add});
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});
    }

    // Smaller CLI points (round 2): input must be a name or false; fx values against the wrapped effect; text/font
    // of the text node are text, not files; params + out together; frames node/view need clip
    {
        QJsonObject add = op(R"({"op":"node","add":{"type":"blur"},"input":true})");
        add["clip"] = clip;
        CHECK(editError({add}).contains("input is the name"));
        add["input"] = 3;
        CHECK(editError({add}).contains("input is the name"));
        add = op(R"({"op":"node","add":{"type":"fx","params":{"effect":"frei0r.rgbnoise","e_0":5}}})");
        add["clip"] = clip;
        CHECK(editError({add}).contains("must be between"));
        add["add"] = op(R"({"type":"fx","params":{"effect":"frei0r.rgbnoise","e_nope":0.5}})");
        CHECK(editError({add}).contains("has no value 'nope'"));
        add["add"] = op(R"({"type":"fx","params":{"effect":"frei0r.rgbnoise","e_0":0.5}})");
        const QJsonObject r = edit({add});
        const int fx = r["notes"].toArray().first().toObject()["node"].toObject()["id"].toInt();
        QJsonObject set{{"op", "node"}, {"clip", clip}, {"id", fx}, {"params", QJsonObject{{"e_0", 7}}}};
        CHECK(editError({set}).contains("must be between")); // the node's effect, not given again
        set["params"] = QJsonObject{{"e_0", 0.25}};
        set["out"] = true;
        edit({set});
        CHECK(graphOf(clip).out == fx);
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});

        QHash<QString, QString> types;
        for (const QJsonValue& v : run("effects", {{"kind", "nodes"}})["nodes"].toArray())
            if (v["type"].toString() == "text")
                for (const QJsonValue& p : v["params"].toArray()) types[p["key"].toString()] = p["type"].toString();
        CHECK_EQ(types.value("text"), QString("text"));
        CHECK_EQ(types.value("font"), QString("text"));
        CHECK_EQ(types.value("texture"), QString("path"));

        CHECK(messageOf("frames", {{"source", g_project}, {"node", 2}, {"out", dir.path()}}).contains("give clip"));
    }

    // comp_from: both tracks into one comp clip (compound + graph), named in the notes
    {
        const QJsonObject r = edit({QJsonObject{{"op", "comp_from"}, {"clips", QJsonArray{clip, upper}}}});
        const QJsonObject note = r["notes"].toArray().first().toObject()["comp_from"].toObject();
        const int made = note["clip"].toInt();
        CHECK(made > 0);
        const CompGraph g = graphOf(made);
        int clips = 0, merges = 0;
        for (const CompNode& n : g.nodes) {
            clips += n.type == "clip";
            merges += n.type == "merge";
        }
        CHECK(clips == 2 && merges >= 1);
        CHECK(note["graph"].toObject()["nodes"].toArray().size() == g.nodes.size());
    }

    // comp_slow / comp_unused_input as pure checks: a big blur in 1080p is slow, the same at 320x180 is not
    {
        Timeline tl;
        tl.video.resize(1);
        Clip c;
        c.id = 1;
        c.mediaPath = still;
        c.out = 49;
        EffectRegistry::add(c, Comp::kEffectId);
        EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
        CompGraph g;
        g.nodes = {{1, "clip", {}}, {2, "blur", {}}, {3, "color", {}}};
        g.links = {{1, 2, "src"}};
        g.out = 2;
        Comp::setGraph(*e, g);
        e->params["n2.radius"] = 16.0;
        tl.video[0].clips << c;
        const ProjectFormat hd{1920, 1080, {25, 1}}, small{320, 180, {25, 1}};
        const auto ids = [](const QVector<ExportCheck::Issue>& v) {
            QStringList out;
            for (const auto& i : v) out << i.check;
            return out;
        };
        CHECK(ids(ExportCheck::comp(tl, 0, 50, hd)) == (QStringList{"comp_unused_input", "comp_slow"}));
        CHECK(ids(ExportCheck::comp(tl, 0, 50, small)) == QStringList{"comp_unused_input"});
        CHECK(ExportCheck::compCostMs(*e, hd.size()) > ExportCheck::kCompBudgetMs);
        tl.video[0].clips[0].renderCache = true; // cached: playback reads the file
        CHECK(ids(ExportCheck::comp(tl, 0, 50, hd)) == QStringList{"comp_unused_input"});
        // A tracker read only through a mask's follow is used
        {
            Timeline t2 = tl;
            EffectInstance* e2 = EffectRegistry::instance(t2.video[0].clips[0], Comp::kEffectId);
            CompGraph g2;
            g2.nodes = {{1, "clip", {}}, {2, "blur", {}}, {3, "tracker", {}}, {4, "mask", {}}};
            g2.links = {{1, 2, "src"}, {4, 2, "mask"}};
            g2.out = 2;
            Comp::setGraph(*e2, g2);
            e2->params["n4.follow"] = 3.0;
            CHECK(ids(ExportCheck::comp(t2, 0, 50, small)).isEmpty());
            e2->params.remove("n4.follow");
            CHECK(ids(ExportCheck::comp(t2, 0, 50, small)) == QStringList{"comp_unused_input"});
        }
        // key_edges: a soft matte inside the subject is found, a clean one not
        std::vector<uint8_t> alpha(100 * 100, 0);
        for (int y = 20; y < 80; ++y)
            for (int x = 20; x < 80; ++x) alpha[size_t(y) * 100 + x] = 255;
        CHECK(!ExportCheck::keyEdges(alpha.data(), 100, 100, 1, 0, 10));
        for (int y = 30; y < 70; y += 2)
            for (int x = 30; x < 70; ++x) alpha[size_t(y) * 100 + x] = 128;
        const auto soft = ExportCheck::keyEdges(alpha.data(), 100, 100, 1, 0, 10);
        CHECK(soft && soft->check == "key_edges" && soft->clip == 1);
    }

    // The Editor ops are one undo step each and undo exactly
    {
        Project p;
        p.load(load());
        Selection sel;
        Editor ed(&p, &sel);
        int id = 0;
        for (const Track& t : p.timeline().video)
            for (const Clip& c : t.clips)
                if (!id) id = c.id; // the comp clip of comp_from
        const auto kinds = [](const QString& t, QStringList* in) { return Picture::Comp::kindInfo(t, in); };
        const QString before = Check::dump(p.timeline());
        const int steps = p.undoStack()->count();
        const int n = ed.addCompNode(id, "blur", {{"radius", 3.0}}, 0, "src", kinds);
        CHECK(n > 1);
        CHECK_EQ(p.undoStack()->count(), steps + 1);
        CHECK(ed.setCompNodeValues(id, n, {{"radius", 5.0}}));
        CHECK_EQ(p.undoStack()->count(), steps + 2);
        CHECK(ed.linkCompNodes(id, {1, n, "mask"}, true, kinds));
        CHECK_EQ(p.undoStack()->count(), steps + 3);
        const int now = p.undoStack()->count();
        QString err;
        CHECK(!ed.linkCompNodes(id, {n, 1, "src"}, true, kinds, &err) && err.contains("no input")); // clip has no input
        CHECK_EQ(p.undoStack()->count(), now); // a refused change is no undo step
        CHECK(ed.removeCompNode(id, n, kinds));
        CHECK_EQ(p.undoStack()->count(), now + 1);
        while (p.undoStack()->index() > steps) p.undoStack()->undo();
        CHECK(Check::dump(p.timeline()) == before);
        // A graph with a newer version's node kind stays editable (only new nodes must be known)
        ed.modifyClips({id}, "future", [&](Clip& c) {
            EffectInstance* e = EffectRegistry::instance(c, Comp::kEffectId);
            CompGraph g;
            Comp::graphOf(*e, &g);
            g.nodes << CompNode{50, "future_kind", {}};
            Comp::setGraph(*e, g);
        });
        const int b = ed.addCompNode(id, "blur", {}, 0, "src", kinds, &err);
        CHECK(b > 0);
        CHECK(ed.setCompOut(id, 50, kinds, &err));
        CHECK(ed.removeCompNode(id, b, kinds, &err));
    }
    return Check::result();
}
