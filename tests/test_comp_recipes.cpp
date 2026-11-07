// SPDX-License-Identifier: AGPL-3.0-only
// Compositing recipes (0.19 package I, core/CompRecipes.h): every built-in recipe applies, validates and renders
// (pixels of synthetic pictures), slots fill node values, errors for unknown recipes/slots and wrong values,
// save -> list -> apply, ZIP export/import, one undo step.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CompGraph.h"
#include "core/CompRecipes.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"
#include "core/TimelineOps.h"
#include "core/Zip.h"
#include "engine/picture/CompNodes.h"

#include "check.h"

#include <QDir>
#include <QFile>
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
QString editError(const QJsonObject& op) { return messageOf("edit", {{"project", g_project}, {"ops", QJsonArray{op}}}); }

ProjectData load()
{
    ProjectData d;
    ProjectFile::load(g_project, &d, nullptr);
    return d;
}

const EffectInstance* compOf(const ProjectData& d, int clip)
{
    const Clip* c = TimelineOps::findClip(d.timeline, clip);
    return c ? EffectRegistry::instance(*c, Comp::kEffectId) : nullptr;
}

const CompGraph::KindInfo kKinds = [](const QString& t, QStringList* in) { return Picture::Comp::kindInfo(t, in); };

QJsonObject recipeOp(int clip, const QString& recipe, const QJsonObject& with = {})
{
    QJsonObject o{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", recipe}};
    if (!with.isEmpty()) o["with"] = with;
    return o;
}

// Only `clip` on the timeline (no other clips under it), frame 0 at the project size
QImage frameOf(int clip)
{
    ProjectData d = load();
    int start = 0;
    for (Track& t : d.timeline.video) {
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.id != clip; }),
                      t.clips.end());
        for (const Clip& c : t.clips) start = c.start;
    }
    return Check::renderFrame(d.timeline, d.format, start);
}

bool near(QRgb p, int r, int g, int b, int tol = 30)
{
    return std::abs(qRed(p) - r) <= tol && std::abs(qGreen(p) - g) <= tol && std::abs(qBlue(p) - b) <= tol;
}

void printPixel(const char* what, QRgb p) { std::printf("       %s: %d %d %d\n", what, qRed(p), qGreen(p), qBlue(p)); }

QString saveImage(const QString& path, const QColor& fill, bool figure = false)
{
    QImage img(320, 180, QImage::Format_RGB32);
    img.fill(fill);
    if (figure) // a red figure on the green screen
        for (int y = 60; y < 120; ++y)
            for (int x = 120; x < 200; ++x) img.setPixel(x, y, qRgb(255, 0, 0));
    img.save(path);
    return path;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("comp_recipes");
    Check::initEngine();
    QDir(CompRecipes::dir()).removeRecursively(); // no own recipes of an earlier run
    QTemporaryDir dir;

    const QString figure = saveImage(dir.filePath("figure.png"), QColor(0, 255, 0), true);
    const QString blue = saveImage(dir.filePath("background.png"), QColor(0, 0, 255));
    const QString yellow = saveImage(dir.filePath("sticker.png"), QColor(255, 255, 0));
    g_project = dir.filePath("recipes.schneidi");
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{figure}}});
    // Six clips of the figure one after the other, one per recipe
    QJsonArray place;
    for (int i = 0; i < 6; ++i) place << QJsonObject{{"op", "append"}, {"media", figure}, {"duration", "1s"}};
    edit(place);
    QVector<int> clips;
    for (const Clip& c : load().timeline.video.value(0).clips) clips << c.id;
    if (!CHECK_EQ(clips.size(), 6)) return Check::result();

    // The list: six built-in recipes with their slots
    const QStringList builtIn{"blur_face", "glitch_title", "greenscreen", "pip_frame", "sticker_track", "text_behind"};
    {
        const QJsonObject r = run("effects", {{"kind", "recipes"}});
        QStringList ids;
        for (const QJsonValue& v : r["recipes"].toArray()) {
            ids << v["recipe"].toString();
            CHECK(v["built_in"].toBool());
            CHECK(!v["slots"].toArray().isEmpty());
        }
        CHECK(ids == builtIn);
        for (const QJsonValue& v : r["recipes"].toArray())
            if (v["recipe"] == "greenscreen")
                for (const QJsonValue& s : v["slots"].toArray()) {
                    if (s["slot"] == "background") CHECK(s["type"] == "media" && !s.toObject().contains("default"));
                    if (s["slot"] == "tolerance") CHECK(s["default"].toDouble() == 0.3); // the key's own default
                    if (s["slot"] == "color") CHECK(s["default"] == "#ff00ff00");
                }
    }

    // Every built-in recipe validates against the node kinds and applies with its defaults (sticker needs its file)
    for (const CompRecipe& r : CompRecipes::builtIn()) {
        QString err;
        if (!CHECK(r.graph.validate(kKinds, &err))) std::printf("       %s: %s\n", qPrintable(r.id), qPrintable(err));
        CompGraph g;
        QHash<int, QVariantMap> values;
        QVariantMap with;
        if (r.id == "sticker_track") with["sticker"] = yellow;
        if (!CHECK(CompRecipes::apply(r, with, &g, &values, &err))) std::printf("       %s: %s\n", qPrintable(r.id), qPrintable(err));
        // Neutral: no glitch above mild, nothing switched on the name does not say
        for (const CompNode& n : g.nodes)
            if (n.type == "glitch") CHECK(values[n.id].value("strength").toDouble() <= 0.3);
    }

    // greenscreen: the background where the screen was, the figure stays; slots fill the node values
    {
        edit({recipeOp(clips[0], "greenscreen", {{"background", blue}, {"tolerance", 0.35}, {"grow", -1}})});
        const ProjectData d = load();
        const EffectInstance* e = compOf(d, clips[0]);
        if (CHECK(e)) {
            CHECK(e->params.value("n2.tolerance").toDouble() == 0.35);
            CHECK(e->params.value("n3.grow").toDouble() == -1);
            CHECK(QFileInfo(e->params.value("n4.path").toString()) == QFileInfo(blue));
        }
        const QImage img = frameOf(clips[0]);
        if (!CHECK(!img.isNull())) return Check::result();
        if (!CHECK(near(img.pixel(20, 20), 0, 0, 255))) printPixel("screen area", img.pixel(20, 20));
        if (!CHECK(near(img.pixel(160, 90), 255, 0, 0))) printPixel("figure", img.pixel(160, 90));
    }

    // text_behind: the words over the background, the figure in front of them
    {
        edit({recipeOp(clips[1], "text_behind",
                       {{"background", blue}, {"text", "MMMMMMMM"}, {"color", "#ffffff"}, {"size", 80}, {"y", 0.5}})});
        const QImage img = frameOf(clips[1]);
        if (!CHECK(near(img.pixel(160, 90), 255, 0, 0))) printPixel("figure over the text", img.pixel(160, 90));
        CHECK(near(img.pixel(5, 5), 0, 0, 255));
        int white = 0;
        for (int y = 60; y < 120; ++y)
            for (int x = 0; x < 320; ++x) white += near(img.pixel(x, y), 255, 255, 255);
        CHECK(white > 200); // the text shows beside the figure
        for (int y = 62; y < 118; ++y)
            for (int x = 122; x < 198; ++x)
                if (near(img.pixel(x, y), 255, 255, 255)) {
                    CHECK(!"text over the figure");
                    y = 999;
                    break;
                }
    }

    // blur_face (point mode, no tracking): the ellipse on the figure's left edge is blurred, the rest sharp
    {
        edit({recipeOp(clips[2], "blur_face", {{"x", 0.375}, {"y", 0.5}, {"size", 0.15}, {"radius", 20}, {"track", "point"}})});
        const ProjectData d = load();
        const EffectInstance* e = compOf(d, clips[2]);
        if (CHECK(e)) {
            CHECK(e->params.value("n2.mode").toString() == "point");
            CHECK(e->params.value("n2.ax").toDouble() == 0.375);
            CHECK(qAbs(e->params.value("n3.h").toDouble() - 0.15 * 1.3333) < 1e-9);
        }
        const QImage img = frameOf(clips[2]);
        const QRgb edge = img.pixel(121, 90);
        if (!CHECK(!near(edge, 255, 0, 0, 10) && !near(edge, 0, 255, 0, 10))) printPixel("blurred edge", edge);
        CHECK(img.pixel(199, 90) == qRgb(255, 0, 0) || near(img.pixel(199, 90), 255, 0, 0, 3)); // the other edge sharp
        CHECK(near(img.pixel(5, 5), 0, 255, 0, 3));
    }

    // pip_frame: the clip small around (0.72, 0.72) in a white frame, nothing around it
    {
        edit({recipeOp(clips[3], "pip_frame", {{"shadow", 0}})});
        const QImage img = frameOf(clips[3]);
        const int cx = qRound(0.72 * 320), cy = qRound(0.72 * 180);
        if (!CHECK(near(img.pixel(cx, cy), 255, 0, 0))) printPixel("pip centre", img.pixel(cx, cy));
        if (!CHECK(near(img.pixel(cx + 60, cy), 0, 255, 0))) printPixel("pip screen", img.pixel(cx + 60, cy));
        if (!CHECK(near(img.pixel(cx + 65, cy), 255, 255, 255, 60))) printPixel("frame", img.pixel(cx + 65, cy));
        CHECK(near(img.pixel(10, 10), 0, 0, 0, 3)); // transparent: the black under the timeline
    }

    // glitch_title: a title over the clip (the clip shows around it)
    {
        edit({recipeOp(clips[4], "glitch_title", {{"text", "HELLO"}, {"color", "#ffffff"}, {"size", 60}})});
        const QImage img = frameOf(clips[4]);
        int white = 0;
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) white += near(img.pixel(x, y), 255, 255, 255, 40);
        CHECK(white > 100);
        CHECK(near(img.pixel(5, 5), 0, 255, 0, 3));
    }

    // sticker_track: the sticker without tracking at x, y with its size
    {
        CHECK(editError(recipeOp(clips[5], "sticker_track")).contains("needs with.sticker"));
        edit({recipeOp(clips[5], "sticker_track", {{"sticker", yellow}, {"x", 0.25}, {"y", 0.5}, {"size", 0.2}})});
        const ProjectData d = load();
        const EffectInstance* e = compOf(d, clips[5]);
        if (CHECK(e)) {
            CHECK(qAbs(e->params.value("n2.ax").toDouble() - 0.20) < 1e-9);
            CHECK(qAbs(e->params.value("n2.bx").toDouble() - 0.30) < 1e-9);
        }
        const QImage img = frameOf(clips[5]);
        if (!CHECK(near(img.pixel(80, 90), 255, 255, 0))) printPixel("sticker", img.pixel(80, 90));
        CHECK(near(img.pixel(160, 90), 255, 0, 0)); // the figure beside it
        CHECK(near(img.pixel(80, 20), 0, 255, 0));  // 0.2 wide: 64 x 36 pixels
    }

    // Errors: unknown recipe, unknown slot (the slots listed), wrong type, out of range, with without recipe; nothing
    // saved
    {
        const QByteArray before = [] {
            QFile f(g_project);
            f.open(QIODevice::ReadOnly);
            return f.readAll();
        }();
        CHECK(editError(recipeOp(clips[0], "wobble")).contains("no recipe 'wobble'"));
        const QString slot = editError(recipeOp(clips[0], "greenscreen", {{"bogus", 1}}));
        if (!CHECK(slot.contains("no slot 'bogus'") && slot.contains("background"))) std::printf("       %s\n", qPrintable(slot));
        CHECK(editError(recipeOp(clips[0], "greenscreen", {{"tolerance", "x"}})).contains("must be a number"));
        CHECK(editError(recipeOp(clips[0], "greenscreen", {{"tolerance", 5}})).contains("between"));
        CHECK(editError(recipeOp(clips[0], "blur_face", {{"track", "nose"}})).contains("must be one of"));
        CHECK(editError(recipeOp(clips[0], "greenscreen", {{"color", "nope"}})).contains("must be a color"));
        CHECK(editError(recipeOp(clips[0], "greenscreen", {{"9.size", 1}})).contains("no node 9"));
        CHECK(editError({{"op", "comp"}, {"clips", QJsonArray{clips[0]}}, {"with", QJsonObject{{"a", 1}}}}).contains("give recipe"));
        QFile f(g_project);
        f.open(QIODevice::ReadOnly);
        CHECK(f.readAll() == before);
    }
    // A node value directly as "<id>.<key>"
    {
        edit({recipeOp(clips[0], "greenscreen", {{"background", blue}, {"5.opacity", 0.5}})});
        CHECK(compOf(load(), clips[0])->params.value("n5.opacity").toDouble() == 0.5);
    }

    // save -> list -> apply: the clip's graph as own recipe, its file copied along
    {
        const QJsonObject save{{"op", "comp"}, {"clips", QJsonArray{clips[0]}}, {"save_recipe", "My Key"},
                               {"description", "blue behind"}};
        edit({save}, true); // dry run: nothing written
        CHECK(!QFileInfo::exists(QDir(CompRecipes::dir()).filePath("My Key")));
        const QJsonObject r = edit({save});
        const QJsonObject note = r["notes"].toArray().first().toObject()["save_recipe"].toObject();
        CHECK(note["id"] == "My Key");
        const QString folder = QDir(CompRecipes::dir()).filePath("My Key");
        CHECK(QFileInfo::exists(QDir(folder).filePath("recipe.json")));
        CHECK(QFileInfo::exists(QDir(folder).filePath("media/background.png")));
        CHECK(editError(save).startsWith("EXISTS"));
        const QJsonObject list = run("effects", {{"kind", "recipes"}});
        bool listed = false;
        for (const QJsonValue& v : list["recipes"].toArray())
            if (v["recipe"] == "My Key") {
                listed = !v["built_in"].toBool() && v["description"] == "blue behind";
                CHECK(v["slots"].toArray().first()["slot"] == "media");
            }
        CHECK(listed);
        // Applied to another clip: the same graph and values, the file from the recipe's folder
        edit({recipeOp(clips[1], "My Key")});
        const ProjectData d = load();
        CompGraph a, b;
        Comp::graphOf(*compOf(d, clips[0]), &a);
        Comp::graphOf(*compOf(d, clips[1]), &b);
        CHECK(a == b);
        CHECK(compOf(d, clips[1])->params.value("n2.tolerance") == compOf(d, clips[0])->params.value("n2.tolerance"));
        CHECK(compOf(d, clips[1])->params.value("n5.opacity").toDouble() == 0.5);
        CHECK(QFileInfo(compOf(d, clips[1])->params.value("n4.path").toString()).absoluteFilePath()
              == QFileInfo(QDir(folder).filePath("media/background.png")).absoluteFilePath());
        const QImage img = frameOf(clips[1]);
        CHECK(near(img.pixel(20, 20), 0, 0, 255)); // the background where the screen was
        if (!CHECK(near(img.pixel(160, 90), 128, 0, 128, 20))) printPixel("figure at half opacity", img.pixel(160, 90));
    }

    // ZIP: export, delete, import again (with its file); a second import gets its own name; a built-in one exports too
    {
        const QString zip = dir.filePath("key.zip");
        const QJsonObject ex = run("effects", {{"kind", "recipes"}, {"export", "My Key"}, {"to", zip}});
        CHECK_EQ(ex["files"].toInt(), 2);
        CHECK(messageOf("effects", {{"kind", "recipes"}, {"export", "My Key"}, {"to", zip}}).startsWith("EXISTS"));
        run("effects", {{"kind", "recipes"}, {"remove", "My Key"}});
        CHECK(!QFileInfo::exists(QDir(CompRecipes::dir()).filePath("My Key")));
        CHECK(messageOf("effects", {{"kind", "recipes"}, {"remove", "greenscreen"}}).startsWith("NOT_FOUND"));
        const QJsonObject im = run("effects", {{"kind", "recipes"}, {"import", zip}});
        CHECK(im["imported"] == "My Key");
        const std::optional<CompRecipe> r = CompRecipes::find("my key");
        if (CHECK(r)) {
            CHECK(r->values.value(4).value("path").toString() == QDir(r->dir).filePath("media/background.png"));
            CHECK(QFileInfo::exists(r->values.value(4).value("path").toString()));
        }
        const QJsonObject again = run("effects", {{"kind", "recipes"}, {"import", zip}});
        CHECK(again["name"] == "My Key 2");
        const QString builtZip = dir.filePath("gs.zip");
        run("effects", {{"kind", "recipes"}, {"export", "greenscreen"}, {"to", builtZip}});
        const QJsonObject gs = run("effects", {{"kind", "recipes"}, {"import", builtZip}});
        CHECK(gs["name"] == "Green screen");
        const std::optional<CompRecipe> imported = CompRecipes::find(gs["imported"].toString());
        const std::optional<CompRecipe> original = CompRecipes::find("greenscreen");
        CHECK(imported && original && imported->graph == original->graph);
        if (imported && original)
            for (auto it = original->values.begin(); it != original->values.end(); ++it)
                if (!CHECK(imported->values.value(it.key()) == it.value()))
                    std::printf("       node %d: %s vs %s\n", it.key(),
                                QJsonDocument(QJsonObject::fromVariantMap(imported->values.value(it.key()))).toJson(QJsonDocument::Compact).constData(),
                                QJsonDocument(QJsonObject::fromVariantMap(it.value())).toJson(QJsonDocument::Compact).constData());
        CHECK(imported && original && imported->slotList.size() == original->slotList.size());
        // An archive without a recipe, a recipe reaching outside its folder
        const QString empty = dir.filePath("empty.zip");
        QFile txt(dir.filePath("x.txt"));
        txt.open(QIODevice::WriteOnly);
        txt.write("x");
        txt.close();
        Zip::write(empty, {{"x.txt", txt.fileName()}});
        CHECK(messageOf("effects", {{"kind", "recipes"}, {"import", empty}}).startsWith("BAD_RECIPE"));
        QFile evil(dir.filePath("recipe.json"));
        evil.open(QIODevice::WriteOnly);
        evil.write(R"({"name":"evil","graph":{"nodes":[{"id":1,"type":"media","params":{"path":"../../secret.png"}}],"links":[],"out":1}})");
        evil.close();
        const QString evilZip = dir.filePath("evil.zip");
        Zip::write(evilZip, {{"evil/recipe.json", evil.fileName()}});
        CHECK(messageOf("effects", {{"kind", "recipes"}, {"import", evilZip}}).contains("outside the recipe"));
        CHECK(!QFileInfo::exists(QDir(CompRecipes::dir()).filePath("evil")));
    }

    // One undo step for a recipe, undone exactly
    {
        Project p;
        p.load(load());
        Selection sel;
        Editor ed(&p, &sel);
        const QString before = Check::dump(p.timeline());
        const int steps = p.undoStack()->count();
        CompGraph g;
        QHash<int, QVariantMap> values;
        CHECK(CompRecipes::apply(*CompRecipes::find("pip_frame"), {{"size", 0.5}}, &g, &values));
        CHECK(values[4].value("size").toDouble() == 0.52);
        CHECK(ed.setCompGraph({clips[4]}, g, values, kKinds));
        CHECK_EQ(p.undoStack()->count(), steps + 1);
        p.undoStack()->undo();
        CHECK(Check::dump(p.timeline()) == before);
    }
    QDir(CompRecipes::dir()).removeRecursively();
    return Check::result();
}
