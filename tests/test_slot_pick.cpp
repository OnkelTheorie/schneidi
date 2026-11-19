// SPDX-License-Identifier: AGPL-3.0-only
// Filling a template's clip slots by itself (0.25): core/SlotPick on a fake index (long enough, sharpness, what the
// slot's label/hint asks for, no shot twice, the longest one when none is long enough, the material's order without
// index data) and `templates fill auto:true` / in place with real files (the index built, the choice per slot with its
// reason, given values stay).
#include "check.h"

#include "cli/Commands.h"
#include "core/AppPaths.h"
#include "core/ProjectFile.h"
#include "core/SlotPick.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

SlotPick::Shot shot(const QString& file, int n, double from, double to, double sharp, double bright, const QString& desc = {})
{
    SlotPick::Shot s;
    s.shot.file = file;
    s.shot.n = n;
    s.shot.from = from;
    s.shot.to = to;
    s.shot.desc = desc;
    s.sharp = sharp;
    s.bright = bright;
    s.shot.sharp = sharp >= 0.6;
    s.shot.blurry = sharp >= 0 && sharp < 0.3;
    s.shot.dark = bright >= 0 && bright < 0.2;
    return s;
}

void testCore()
{
    using namespace SlotPick;
    std::printf("core\n");
    QVector<Shot> shots;
    shots << shot("a.mp4", 1, 0, 1.0, 0.9, 0.5)              // 0: sharp but short
          << shot("a.mp4", 2, 1.0, 5.0, 0.2, 0.5)            // 1: blurry
          << shot("b.mp4", 1, 0, 4.0, 0.8, 0.1)              // 2: sharp, dark
          << shot("b.mp4", 2, 4.0, 9.0, 0.7, 0.5)            // 3: sharp, well exposed
          << shot("c.mp4", 1, 0, 3.0, 0.4, 0.5, "die Ladentheke von vorne"); // 4: the shop, so-so
    // Slot 1 asks for the shop: the match wins over sharper shots; slot 2: the best quality long enough (not the
    // short sharp one); slot 3: what is left, best first
    const QVector<Slot> slotList{{"shop", "Laden", {}, 2.0}, {"clip2", {}, {}, 2.0}, {"clip3", {}, {}, 2.0}};
    const QVector<Pick> p = pick(slotList, shots);
    if (!CHECK_EQ(p.size(), 3)) return;
    CHECK_EQ(p[0].shot, 4);
    CHECK(!p[0].matched.isEmpty());
    CHECK(p[0].why.contains("matches"));
    CHECK_EQ(p[1].shot, 3);
    CHECK(p[1].quality.contains("sharp"));
    CHECK(p[1].quality.contains("well exposed"));
    CHECK_EQ(p[2].shot, 2); // sharp but dark beats blurry
    // No shot twice; once all are used nothing is left
    QVector<Slot> many;
    for (int i = 0; i < 7; ++i) many << Slot{QString("s%1").arg(i), {}, {}, 0};
    const QVector<Pick> all = pick(many, shots);
    QSet<int> seen;
    for (const Pick& x : all)
        if (x.shot >= 0) CHECK(!seen.contains(x.shot)), seen.insert(x.shot);
    CHECK_EQ(seen.size(), 5);
    CHECK_EQ(all[5].shot, -1);
    CHECK_EQ(all[6].shot, -1);
    // Nothing long enough: the longest one left, said so
    const QVector<Pick> longOne = pick({Slot{"long", {}, {}, 20.0}}, shots);
    CHECK_EQ(longOne[0].shot, 3);
    CHECK(longOne[0].shortShot);
    // Without index data: the material's order (the first free one long enough)
    QVector<Shot> plain;
    plain << shot("x.mp4", 1, 0, 1.0, -1, -1) << shot("y.mp4", 1, 0, 5.0, -1, -1) << shot("z.mp4", 1, 0, 5.0, -1, -1);
    const QVector<Pick> order = pick({Slot{"a", {}, {}, 2.0}, Slot{"b", {}, {}, 2.0}, Slot{"c", {}, {}, 2.0}}, plain);
    CHECK_EQ(order[0].shot, 1);
    CHECK(order[0].inOrder);
    CHECK_EQ(order[1].shot, 2);
    CHECK_EQ(order[2].shot, 0); // only the short one is left
    CHECK(order[2].shortShot);
    // A shot without a picture never fills a clip slot
    Shot sound = shot("s.wav", 1, 0, 9.0, -1, -1);
    sound.video = false;
    CHECK_EQ(pick({Slot{"a", {}, {}, 0}}, {sound})[0].shot, -1);
}

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, raw), ctx);
}

QString errorOf(const QString& name, const QJsonObject& raw)
{
    try {
        run(name, raw);
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

QJsonObject pickOf(const QJsonObject& r, const QString& slot)
{
    for (const QJsonValue& v : r["auto"].toArray())
        if (v["slot"].toString() == slot) return v.toObject();
    return {};
}

void testCli(const QString& tmp)
{
    std::printf("templates fill auto\n");
    const QString dir = tmp + "/media";
    QDir().mkpath(dir);
    const auto make = [&](const char* name, const QString& filter) {
        return Check::makeMedia(dir + "/" + name, {"-f", "lavfi", "-i", filter, "-c:v", "libx264", "-preset", "ultrafast",
                                                   "-pix_fmt", "yuv420p"});
    };
    const QString placeholder = make("placeholder.mp4", "testsrc=size=320x180:rate=25:duration=4");
    const QString sharp = make("sharp.mp4", "testsrc=size=320x180:rate=25:duration=4");
    const QString blurry = make("blurry.mp4", "testsrc=size=320x180:rate=25:duration=4,gblur=sigma=12");
    const QString shop = make("shop.mp4", "testsrc2=size=320x180:rate=25:duration=4,gblur=sigma=3");
    if (!CHECK(!placeholder.isEmpty() && !sharp.isEmpty() && !blurry.isEmpty() && !shop.isEmpty())) return;
    // The template: two 3 s clip slots, the first asks for the shop
    const QString project = tmp + "/tpl.schneidi";
    run("new", {{"project", project}, {"fps", 25}, {"width", 320}, {"height", 180}});
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"append","media":"%1","duration":"3s"},{"op":"append","media":"%1","duration":"3s"}])")
                             .arg(placeholder)}});
    ProjectData d;
    ProjectFile::load(project, &d, nullptr);
    if (!CHECK_EQ(d.timeline.video.value(0).clips.size(), 2)) return;
    const int c1 = d.timeline.video[0].clips[0].id, c2 = d.timeline.video[0].clips[1].id;
    run("edit", {{"project", project},
                 {"ops", QString(R"([{"op":"slot","clip":%1,"id":"shop","label":"Laden"},{"op":"slot","clip":%2,"id":"clip2"}])")
                             .arg(c1)
                             .arg(c2)}});
    CHECK(run("templates", {{"save", project}, {"name", "Auto Test"}})["saved"].toBool());
    // The shop described in the index (what a person or the AI wrote)
    run("index", {{"file", shop}});
    run("index", {{"annotate", QJsonArray{QJsonObject{{"file", shop}, {"shot", 1}, {"desc", "der Laden von vorne"}}}}});

    const QString out = tmp + "/filled.schneidi";
    const QJsonObject r = run("templates", {{"fill", "Auto Test"}, {"out_project", out}, {"auto", true},
                                            {"files", QJsonArray{blurry, sharp, shop}}});
    const QJsonObject p1 = pickOf(r, "shop"), p2 = pickOf(r, "clip2");
    if (!CHECK(p1["file"].toString() == shop)) std::printf("       %s\n", QJsonDocument(r).toJson().constData());
    CHECK_EQ(p1["reason"]["key"].toString(), QString("slot_match"));
    CHECK(!p1["reason"]["values"]["matched"].toArray().isEmpty());
    CHECK(p1["reason"]["text"].toString().contains("matches"));
    CHECK_EQ(p2["file"].toString(), sharp); // sharper than the blurred one
    CHECK(p2["reason"]["values"]["quality"].toArray().contains("sharp"));
    ProjectData f;
    ProjectFile::load(out, &f, nullptr);
    if (CHECK_EQ(f.timeline.video.value(0).clips.size(), 2)) {
        CHECK_EQ(f.timeline.video[0].clips[0].mediaPath, shop);
        CHECK_EQ(f.timeline.video[0].clips[1].mediaPath, sharp);
    }
    // A value given stays; auto fills only the rest
    const QString out2 = tmp + "/filled2.schneidi";
    const QJsonObject r2 = run("templates", {{"fill", "Auto Test"}, {"out_project", out2}, {"auto", true},
                                             {"values", QJsonObject{{"shop", blurry}}}, {"files", QJsonArray{blurry, sharp, shop}}});
    CHECK(pickOf(r2, "shop").isEmpty());
    CHECK_EQ(pickOf(r2, "clip2")["file"].toString(), sharp);
    // dry_run lists the choice too and writes nothing
    const QJsonObject dry = run("templates", {{"fill", "Auto Test"}, {"dry_run", true}, {"auto", true},
                                              {"files", QJsonArray{blurry, sharp, shop}}});
    CHECK_EQ(pickOf(dry, "shop")["file"].toString(), shop);
    // In place (the slots of a project; every clip slot not given in values is chosen anew): no shop among these
    const QJsonObject inPlace = run("templates", {{"project", out}, {"auto", true}, {"files", QJsonArray{blurry, sharp}}});
    CHECK_EQ(inPlace["auto"].toArray().size(), 2);
    CHECK_EQ(pickOf(inPlace, "shop")["file"].toString(), sharp);
    CHECK_EQ(pickOf(inPlace, "clip2")["file"].toString(), blurry);
    ProjectFile::load(out, &f, nullptr);
    if (CHECK_EQ(f.timeline.video.value(0).clips.size(), 2)) CHECK_EQ(f.timeline.video[0].clips[0].mediaPath, sharp);
    // Without material
    CHECK_EQ(errorOf("templates", {{"fill", "Auto Test"}, {"out_project", tmp + "/x.schneidi"}, {"auto", true}}),
             QString("NO_MATERIAL"));
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
    AppPaths::setProgramNames();
    Check::initEngine();
    testCore();
    if (Check::haveFfmpeg()) testCli(tmp.path());
    else std::printf("no ffmpeg: CLI part skipped\n");
    return Check::result();
}
