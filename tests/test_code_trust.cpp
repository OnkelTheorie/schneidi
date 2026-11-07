// SPDX-License-Identifier: AGPL-3.0-only
// Trust for custom code (core/CodeTrust.h, 0.19): a filter chain written by edit renders at once; the same project
// "from someone else" (a fresh run without its trust record) renders without it, `check` lists it, render answers
// skipped_effects reason "untrusted", info marks it; trust_custom_code:true runs it and is remembered for the file
// (also in the next run), a changed code asks again. A recipe from a ZIP stays untrusted; one the user saved from
// trusted code stays trusted.
#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/CodeTrust.h"
#include "core/CompGraph.h"
#include "core/CompRecipes.h"
#include "core/EffectRegistry.h"
#include "core/PathKey.h"
#include "core/ProjectFile.h"
#include "core/TimelineOps.h"
#include "engine/RenderCache.h"
#include "engine/picture/CustomEffects.h"

#include "check.h"

#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>

#include <clocale>

namespace {

QString g_project;
QString g_out;
int g_frame = 0;

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       unexpected error %s: %s\n", qPrintable(e.code), qPrintable(e.message));
        throw;
    }
}

QJsonObject edit(const QJsonArray& ops, const QJsonObject& extra = {})
{
    QJsonObject a{{"project", g_project}, {"ops", ops}};
    for (auto it = extra.begin(); it != extra.end(); ++it) a[it.key()] = it.value();
    return run("edit", a);
}

// The picture's middle at frame 10: green (the clip) or magenta (negated)
QColor middle()
{
    const QJsonObject r = run("frames", {{"source", g_project}, {"out", QDir(g_out).filePath(QString("f%1").arg(++g_frame))},
                                         {"width", 160}, {"at", QJsonArray{10}}});
    const QImage img(r["frames"].toArray().first().toObject()["file"].toString());
    return img.isNull() ? QColor() : img.pixelColor(80, 45);
}
bool negated(const QColor& c) { return c.red() > 240 && c.green() < 15 && c.blue() > 240; }
bool plain(const QColor& c) { return c.red() < 15 && c.green() > 240 && c.blue() < 15; }

// A new run: nothing trusted in memory (the settings' records stay)
void newRun() { CodeTrust::resetForTests(); }
// Someone else's computer: no records either
void otherComputer()
{
    CodeTrust::resetForTests();
    QSettings().remove("trust");
}

QStringList checkIds(const QString& project = g_project)
{
    const QJsonObject r = run("check", {{"source", project}, {"sound", false}});
    QStringList ids;
    for (const QJsonValue& v : r["issues"].toArray()) ids << v["check"].toString();
    return ids;
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    Check::initApp("code_trust");
    Check::initEngine();
    if (!Picture::Custom::filterAvailable()) return Check::skip("no libavfilter in this build");
    QSettings().remove("trust");
    QDir(CompRecipes::dir()).removeRecursively();
    QTemporaryDir dir;
    g_out = dir.path();

    const QString still = dir.filePath("green.png");
    {
        QImage img(320, 180, QImage::Format_RGB32);
        img.fill(QColor(0, 255, 0));
        CHECK(img.save(still));
    }
    g_project = dir.filePath("shared.schneidi");
    run("new", {{"project", g_project}, {"width", 320}, {"height", 180}, {"fps", 25}, {"media", QJsonArray{still}}});
    edit({QJsonObject{{"op", "append"}, {"media", still}, {"duration", "2s"}}});
    ProjectData d;
    CHECK(ProjectFile::load(g_project, &d, nullptr));
    if (!CHECK(!d.timeline.video.isEmpty() && !d.timeline.video[0].clips.isEmpty())) return Check::result();
    const int clip = d.timeline.video[0].clips.first().id;

    // A project without custom code: no record written
    CHECK(!QSettings().childGroups().contains("trust"));

    // Code set by edit renders at once (and the save remembers it for this file)
    edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                      {"params", QJsonObject{{"chain", "negate"}}}}});
    CHECK(negated(middle()));
    CHECK(!checkIds().contains("custom_code_untrusted"));
    // The next run of the same user: remembered for this file
    newRun();
    CHECK(negated(middle()));

    // The same file on another computer (shared): kept, but not run; check, render and info say so
    otherComputer();
    {
        const QColor c = middle();
        if (!CHECK(plain(c))) std::printf("       %s\n", qPrintable(c.name()));
        CHECK(checkIds().contains("custom_code_untrusted"));
        const QJsonObject info = run("info", {{"project", g_project}});
        const QJsonObject fx = info["timeline"]["tracks"].toArray().first()["clips"].toArray().first()["effects"].toArray().first().toObject();
        CHECK_EQ(fx["effect"].toString(), QString("ffmpeg_filter"));
        CHECK(fx["untrusted"].toBool());
        const QJsonObject r = run("render", {{"project", g_project}, {"out", dir.filePath("a.mp4")}, {"height", 180}});
        const QJsonArray skipped = r["skipped_effects"].toArray();
        if (CHECK_EQ(skipped.size(), 1)) {
            CHECK_EQ(skipped[0]["effect"].toString(), QString("ffmpeg_filter"));
            CHECK_EQ(skipped[0]["reason"].toString(), QString("untrusted"));
        }
        // An edit that does not touch it does not trust it (nor remember it)
        edit({QJsonObject{{"op", "marker"}, {"at", 5}}});
        newRun();
        CHECK(plain(middle()));
        // Sending the same code back (copied from info) does not make it the caller's own
        edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"},
                          {"params", QJsonObject{{"chain", "negate"}}}}});
        CHECK(!CodeTrust::isTrusted("negate"));
        newRun();
        CHECK(plain(middle()));
    }
    // trust_custom_code:true on render: runs, and remembered for the file
    {
        const QJsonObject r = run("render", {{"project", g_project}, {"out", dir.filePath("b.mp4")}, {"height", 180},
                                             {"trust_custom_code", true}});
        CHECK(!r.contains("skipped_effects"));
        newRun();
        CHECK(negated(middle()));
        CHECK(!checkIds().contains("custom_code_untrusted"));
    }
    // The file changed by someone else (other code): asked again
    {
        ProjectData changed;
        CHECK(ProjectFile::load(g_project, &changed, nullptr));
        for (Track& t : changed.timeline.video)
            for (Clip& c : t.clips)
                for (EffectInstance& e : c.effects)
                    if (e.effectId == QLatin1String("ffmpeg_filter")) e.params["chain"] = "negate,hflip";
        for (Sequence& q : changed.sequences)
            if (q.id == changed.currentSequence) q.timeline = changed.timeline;
        CHECK(ProjectFile::save(changed, g_project, nullptr));
        newRun();
        CHECK(plain(middle()));
        CHECK(checkIds().contains("custom_code_untrusted"));
        // trust_custom_code:true on edit
        edit({QJsonObject{{"op", "marker"}, {"at", 7}}}, {{"trust_custom_code", true}});
        newRun();
        CHECK(negated(middle()));
    }

    // Recipes: one saved from the user's trusted code stays trusted; the same from a ZIP on another computer not
    {
        edit({QJsonObject{{"op", "effect"}, {"clips", QJsonArray{clip}}, {"effect", "ffmpeg_filter"}, {"remove", true}}});
        const QJsonObject graph{{"nodes", QJsonArray{QJsonObject{{"id", 1}, {"type", "clip"}},
                                                     QJsonObject{{"id", 2}, {"type", "fx"},
                                                                 {"params", QJsonObject{{"effect", "ffmpeg_filter"},
                                                                                        {"e_chain", "negate,vflip"}}}}}},
                                {"links", QJsonArray{QJsonArray{1, 2, "src"}}},
                                {"out", 2}};
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"graph", graph}}});
        CHECK(negated(middle()));
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"save_recipe", "Negative"}}});
        const QString zip = dir.filePath("negative.zip");
        run("effects", {{"kind", "recipes"}, {"export", "Negative"}, {"to", zip}});
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});
        CHECK(plain(middle()));
        // The user's own recipe in a later run
        newRun();
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", "Negative"}}});
        CHECK(negated(middle()));
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"remove", true}}});
        // From a ZIP on another computer: the comp node passes the picture, check lists it
        run("effects", {{"kind", "recipes"}, {"remove", "Negative"}});
        otherComputer();
        run("effects", {{"kind", "recipes"}, {"import", zip}});
        edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"recipe", "Negative"}}});
        CHECK(plain(middle()));
        CHECK(checkIds().contains("custom_code_untrusted"));
        const QJsonObject info = run("info", {{"project", g_project}});
        const QJsonArray nodes = info["timeline"]["tracks"].toArray().first()["clips"].toArray().first()["effects"].toArray().first()["graph"]["nodes"].toArray();
        bool marked = false;
        for (const QJsonValue& n : nodes) marked |= n["untrusted"].toBool();
        CHECK(marked);
        // A full `comp graph` round trip of info's graph keeps the code untrusted (and unremembered)
        {
            QJsonObject graph = info["timeline"]["tracks"].toArray().first()["clips"].toArray().first()["effects"]
                                    .toArray().first()["graph"].toObject();
            QJsonArray clean;
            for (const QJsonValue& n : graph["nodes"].toArray()) {
                QJsonObject o = n.toObject();
                o.remove("untrusted");
                clean << o;
            }
            const QJsonObject resend{{"nodes", clean}, {"links", graph["links"]}, {"out", graph["out"]}};
            edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"graph", resend}}});
            CHECK(!CodeTrust::isTrusted("negate,vflip"));
            CHECK(checkIds().contains("custom_code_untrusted"));
            newRun();
            CHECK(plain(middle()));
            // Changed code is the caller's own: it runs
            QJsonArray changed;
            for (const QJsonValue& n : clean) {
                QJsonObject o = n.toObject();
                if (o["type"] == "fx") {
                    QJsonObject params = o["params"].toObject();
                    params["e_chain"] = "negate,vflip,hflip";
                    o["params"] = params;
                }
                changed << o;
            }
            edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}},
                              {"graph", QJsonObject{{"nodes", changed}, {"links", graph["links"]}, {"out", graph["out"]}}}}});
            CHECK(CodeTrust::isTrusted("negate,vflip,hflip"));
            CHECK(negated(middle()));
            // The foreign code sent back once the project lost it: still not the caller's own (seen in this run)
            edit({QJsonObject{{"op", "comp"}, {"clips", QJsonArray{clip}}, {"graph", resend}}});
            CHECK(!CodeTrust::isTrusted("negate,vflip"));
            CHECK(plain(middle()));
        }
        // The user confirms it
        edit({QJsonObject{{"op", "marker"}, {"at", 9}}}, {{"trust_custom_code", true}});
        CHECK(negated(middle()));
    }

    // Trusting code changes the picture of a clip that has it: its render-cache key changes (no stale render);
    // a clip without custom code keeps its key. An export freezes the trust of its start (freeze)
    {
        newRun();
        ProjectFormat f;
        Clip plainClip;
        plainClip.mediaPath = still;
        plainClip.out = 49;
        Clip coded = plainClip;
        EffectInstance fx;
        fx.effectId = "ffmpeg_filter";
        fx.params["chain"] = "negate,boxblur=2";
        coded.effects << fx;
        const QString plainKey = RenderCache::key(plainClip, f);
        const QString untrustedKey = RenderCache::key(coded, f);
        Timeline tl;
        tl.video << Track{};
        tl.video[0].clips << coded;
        Timeline frozen = tl;
        CHECK(CodeTrust::freeze(frozen));
        const quint64 epoch = CodeTrust::epoch();
        CodeTrust::trust("negate,boxblur=2");
        CHECK(CodeTrust::epoch() > epoch);
        CHECK(RenderCache::key(coded, f) != untrustedKey);
        CHECK_EQ(RenderCache::key(plainClip, f), plainKey);
        // The frozen copy still has it untrusted; the marker itself can never be trusted
        const QVector<CodeTrust::Item> left = CodeTrust::itemsOf(frozen.video[0].clips[0].effects[0]);
        if (CHECK_EQ(left.size(), 1)) {
            CHECK(!CodeTrust::isTrusted(left[0].code));
            CodeTrust::trust(left[0].code);
            CHECK(!CodeTrust::isTrusted(left[0].code));
        }
        Timeline again = tl;
        CHECK(!CodeTrust::freeze(again)); // all trusted now: nothing to freeze
    }

    // check lists only code that would run: not a switched-off effect, not a hidden track, not a value left on an fx
    // node that runs another effect now
    {
        ProjectData p;
        CHECK(ProjectFile::load(g_project, &p, nullptr));
        Clip& c = p.timeline.video[0].clips[0];
        c.effects.clear();
        c.effects << EffectInstance{"ffmpeg_filter", {{"chain", "hue=h=33"}}, false};
        EffectInstance comp;
        comp.effectId = Comp::kEffectId;
        CompGraph g;
        g.nodes << CompNode{1, "clip", {}} << CompNode{2, "fx", {}};
        g.links << CompLink{1, 2, "src"};
        g.out = 2;
        Comp::setGraph(comp, g);
        comp.params[CompGraph::paramKey(2, "effect")] = "none";
        comp.params[CompGraph::paramKey(2, "e_chain")] = "hue=h=44";
        c.effects << comp;
        for (Sequence& q : p.sequences)
            if (q.id == p.currentSequence) q.timeline = p.timeline;
        const QString quiet = dir.filePath("quiet.schneidi");
        CHECK(ProjectFile::save(p, quiet, nullptr));
        CHECK(!checkIds(quiet).contains("custom_code_untrusted"));
        CHECK(CodeTrust::itemsOf(c.effects[1]).isEmpty());
        // Switched on: listed; on a hidden track: not
        c.effects[0].enabled = true;
        for (Sequence& q : p.sequences)
            if (q.id == p.currentSequence) q.timeline = p.timeline;
        CHECK(ProjectFile::save(p, quiet, nullptr));
        CHECK(checkIds(quiet).contains("custom_code_untrusted"));
        p.timeline.video[0].hidden = true;
        for (Sequence& q : p.sequences)
            if (q.id == p.currentSequence) q.timeline = p.timeline;
        CHECK(ProjectFile::save(p, quiet, nullptr));
        CHECK(!checkIds(quiet).contains("custom_code_untrusted"));
    }

    // The trust record is the settings' only: a project file cannot carry its own trust
    {
        ProjectData now;
        CHECK(ProjectFile::load(g_project, &now, nullptr));
        const QString hash = CodeTrust::hashOf(CodeTrust::codes(now));
        CHECK_EQ(QSettings().value("trust/" + QString::fromLatin1(QCryptographicHash::hash(
                                                    PathKey::of(g_project).toUtf8(), QCryptographicHash::Sha1).toHex())).toString(),
                 hash);
        QFile f(g_project);
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(!f.readAll().contains(hash.toLatin1()));
    }
    QDir(CompRecipes::dir()).removeRecursively();
    QSettings().remove("trust");
    return Check::result();
}
