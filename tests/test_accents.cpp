// Accents on hits (core/Accents, Editor::addAccents, op 'accent'): a zoom punch, an exposure flash, a position shake
// and their mixes are ordinary keyframes on top of the clip's values, close hits add up, the curve between the
// written keyframes is the accent's own shape; the clips under the times are found without a list, one undo step;
// op 'keyframe' takes param zoom (both axes) and `info` shows matching zoom tracks once; `effects` lists the presets.
#include "check.h"

#include "cli/Commands.h"
#include "cli/CommandsDetail.h"
#include "core/Accents.h"
#include "core/Editor.h"
#include "core/EffectRegistry.h"
#include "core/Keyframes.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/Selection.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QUndoStack>

#include <cmath>
#include <utility>

namespace {

constexpr double kFps = 25;
const QSizeF kFrame(1080, 1920);

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

Clip clip(int length = 100)
{
    Clip c;
    c.id = 1;
    c.mediaPath = "/x/a.mp4";
    c.in = 20; // keyframes count from the source: a clip that does not start at the file start
    c.out = c.in + length - 1;
    return c;
}

void testPunch()
{
    // 25 fps: rises one frame before the hit, peaks on it, lands after 0.25 s = 6 frames (Ease In = slow landing)
    Clip c = clip();
    const Accents::Amounts punch = Accents::find("punch")->amounts;
    CHECK_EQ(Accents::apply(c, {30}, punch, kFps, kFrame), 1);
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::ZoomY}) {
        CHECK_EQ(Keys::keyTimes(c, {p}), (QVector<int>{29, 30, 36}));
        CHECK(near(Keys::valueAt(c, p, 29), 1.0));
        CHECK(near(Keys::valueAt(c, p, 30), 1.15));
        CHECK(near(Keys::valueAt(c, p, 36), 1.0));
        CHECK(Keys::keyAt(c, p, 36)->ease == KeyEase::EaseIn);
        CHECK(Keys::keyAt(c, p, 30)->ease == KeyEase::Linear);
    }
    // Between the keyframes: falls fast, lands slowly (1 - sin), as the accent's envelope says
    CHECK(near(Keys::valueAt(c, AnimParam::ZoomX, 33), 1 + 0.15 * (1 - std::sin(0.5 * M_PI / 2)), 1e-3));
    CHECK(Keys::valueAt(c, AnimParam::ZoomX, 31) - 1 > (Keys::valueAt(c, AnimParam::ZoomX, 35) - 1) * 3);
    // Outside the hit nothing changed
    CHECK(near(Keys::valueAt(c, AnimParam::ZoomX, 0), 1.0) && near(Keys::valueAt(c, AnimParam::ZoomX, 99), 1.0));

    // On top of the clip's zoom: 2 -> 2.3; strength scales; a hit outside the clip is left out
    Clip z = clip();
    z.transform.zoomX = z.transform.zoomY = 2;
    CHECK_EQ(Accents::apply(z, {50, 150, -3}, punch.scaled(2), kFps, kFrame), 1);
    CHECK(near(Keys::valueAt(z, AnimParam::ZoomY, 50), 2 * 1.3));
    CHECK(near(Keys::valueAt(z, AnimParam::ZoomY, 10), 2.0));

    // An animated zoom keeps its course around the hit (1 -> 2 over the clip)
    Clip a = clip();
    Keys::setKey(a, AnimParam::ZoomX, 0, 1);
    Keys::setKey(a, AnimParam::ZoomX, 99, 2);
    Accents::apply(a, {50}, punch, kFps, kFrame);
    CHECK(near(Keys::valueAt(a, AnimParam::ZoomX, 50), (1 + 50.0 / 99) * 1.15, 1e-6));
    CHECK(near(Keys::valueAt(a, AnimParam::ZoomX, 80), 1 + 80.0 / 99, 1e-6));
    CHECK(near(Keys::valueAt(a, AnimParam::ZoomX, 99), 2.0));

    // Close hits add up: the second lands on the first one's fall (u = 2/6 -> 1 - sin(30°) = 0.5)
    Clip two = clip();
    Accents::apply(two, {10, 12}, punch, kFps, kFrame);
    CHECK(near(Keys::valueAt(two, AnimParam::ZoomX, 12), 1 + 0.15 * 1.5, 1e-6));
    CHECK(near(Keys::valueAt(two, AnimParam::ZoomX, 18), 1.0));

    // A hit on the first frame peaks there; one near the end is cut at the clip's last frame (still falling)
    Clip edge = clip(20);
    Accents::apply(edge, {0, 17}, punch, kFps, kFrame);
    CHECK(near(Keys::valueAt(edge, AnimParam::ZoomX, 0), 1.15));
    CHECK(Keys::keyAt(edge, AnimParam::ZoomX, 19));
    CHECK(Keys::valueAt(edge, AnimParam::ZoomX, 19) > 1.0);
    CHECK(Keys::keyTimes(edge, {AnimParam::ZoomX}).last() == 19);
}

void testFlashShake()
{
    // Flash: the Color page exposure, the grade comes with it; on top of an existing exposure
    Clip c = clip();
    CHECK(!EffectRegistry::has(c, "grade"));
    Accents::apply(c, {40}, Accents::find("flash")->amounts, kFps, kFrame);
    CHECK(EffectRegistry::has(c, "grade"));
    CHECK(near(Keys::valueAt(c, AnimParam::GradeExposure, 40), 2.0));
    CHECK(near(Keys::valueAt(c, AnimParam::GradeExposure, 60), 0.0));
    Clip g = clip();
    EffectRegistry::add(g, "grade");
    Keys::setStaticValue(g, AnimParam::GradeExposure, 0.5);
    Accents::apply(g, {40}, Accents::find("flash")->amounts, kFps, kFrame);
    CHECK(near(Keys::valueAt(g, AnimParam::GradeExposure, 40), 2.5));
    CHECK(near(Keys::valueAt(g, AnimParam::GradeExposure, 70), 0.5));
    CHECK(!Keys::animated(g, AnimParam::ZoomX)); // a flash only

    // Shake: x/y jumps around the clip's position, the largest one 3 % of the shorter side, back at the end
    Clip s = clip();
    s.transform.posX = 100;
    Accents::apply(s, {20}, Accents::find("shake")->amounts, kFps, kFrame);
    CHECK(Keys::animated(s, AnimParam::PosX) && Keys::animated(s, AnimParam::PosY));
    double most = 0;
    for (int t = 0; t < 100; ++t) most = std::max(most, std::abs(Keys::valueAt(s, AnimParam::PosX, t) - 100));
    CHECK(near(most, 0.03 * 1080, 1e-6));
    CHECK(near(Keys::valueAt(s, AnimParam::PosX, 19), 100) && near(Keys::valueAt(s, AnimParam::PosX, 30), 100));
    CHECK(near(Keys::valueAt(s, AnimParam::PosY, 40), 0));
    // Same edit, same shake
    Clip s2 = clip();
    s2.transform.posX = 100;
    Accents::apply(s2, {20}, Accents::find("shake")->amounts, kFps, kFrame);
    CHECK(s2.keys == s.keys);

    // hit = all three; every preset does something and has a description for AIs
    Clip h = clip();
    Accents::apply(h, {20}, Accents::find("hit")->amounts, kFps, kFrame);
    for (AnimParam p : {AnimParam::ZoomX, AnimParam::GradeExposure, AnimParam::PosX}) CHECK(Keys::animated(h, p));
    for (const Accents::Preset& p : Accents::presets()) CHECK(p.amounts.any() && *p.description && *p.name);
    CHECK(!Accents::find("nope"));
}

void testEditor()
{
    // Without clips: every enabled clip with a picture on a visible video track under each time, one undo step
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    d.timeline.video.resize(3);
    p.load(d);
    p.edit("setup", [&](Timeline& tl) {
        Clip a = clip(50);
        a.id = 1;
        Clip b = clip(50);
        b.id = 2;
        b.start = 50;
        tl.video[0].clips << a << b;
        Clip t;
        t.id = 3;
        t.kind = ClipKind::Title;
        t.out = 99;
        tl.video[1].clips << t;
        Clip off = clip(50);
        off.id = 4;
        off.enabled = false;
        tl.video[2].clips << off;
    });
    Selection sel;
    Editor ed(&p, &sel);
    const int before = p.undoStack()->index();
    const QVector<int> landed = ed.addAccents({}, {10, 60, 300}, Accents::find("punch")->amounts, "Accent");
    CHECK_EQ(landed, (QVector<int>{10, 60}));
    CHECK_EQ(p.undoStack()->index(), before + 1);
    const auto find = [&](int id) { return *TimelineOps::findClip(p.timeline(), id); };
    CHECK(near(Keys::valueAt(find(1), AnimParam::ZoomX, 10), 1.15));
    CHECK(near(Keys::valueAt(find(2), AnimParam::ZoomX, 10), 1.15)); // timeline 60 = clip frame 10
    CHECK(!Keys::hasKeys(find(3)) && !Keys::hasKeys(find(4)));
    p.undoStack()->undo();
    CHECK(!Keys::hasKeys(find(1)) && !Keys::hasKeys(find(2)));
    // Given clips: only those (a title too, if asked for)
    CHECK_EQ(ed.addAccents({3}, {10, 60}, Accents::find("pulse")->amounts, "Accent"), (QVector<int>{10, 60}));
    CHECK(Keys::animated(find(3), AnimParam::ZoomX) && !Keys::hasKeys(find(1)));
    CHECK(ed.addAccents({2}, {10}, Accents::find("pulse")->amounts, "Accent").isEmpty());
}

QString g_path;

const QJsonObject run(const QString& name, const QJsonObject& args)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    return cmd->run(Cli::normalize(*cmd, args), ctx);
}

QString errorOf(const QString& ops)
{
    try {
        run("edit", {{"project", g_path}, {"ops", ops}});
    } catch (const Cli::Error& e) {
        return e.code;
    }
    return "none";
}

QJsonObject firstClip() { return run("info", {{"project", g_path}})["timeline"]["tracks"][0]["clips"][0].toObject(); }

void testCli(const QString& dir)
{
    g_path = dir + "/a.schneidi";
    Project p;
    ProjectData d;
    d.timeline = emptyTimeline();
    p.load(d);
    p.addMedia({"/x/a.mp4", "a.mp4", 200, true, false, false});
    int id = 0;
    p.edit("setup", [&](Timeline& tl) {
        Clip v;
        v.id = id = p.newClipId();
        v.mediaPath = "/x/a.mp4";
        v.out = 99;
        tl.video[0].clips << v;
        tl.markers = {70, 150};
    });
    QString error;
    CHECK(ProjectFile::save(p.data(), g_path, &error));

    // A list of times (seconds or frames), the default preset is the kill hit
    const QJsonObject r = run("edit", {{"project", g_path}, {"ops", R"([{"op":"accent","at":["1s",50,"20s"]}])"}});
    bool noted = false;
    for (const QJsonValue& n : r["notes"].toArray()) noted |= n.toString().contains("accent 'hit' at 2 of 3 times");
    CHECK(noted);
    // The answer counts the keyframes of a changed clip (all of them for 20 clips were 30-40 KB)
    const QJsonArray changed = r["changes"]["changed"].toArray();
    if (CHECK(!changed.isEmpty())) {
        const QJsonObject counts = changed[0].toObject().value("keyframes").toObject();
        CHECK(counts.value("zoom").isDouble() && counts.value("zoom").toInt() >= 3);
        CHECK(!changed[0].toObject().contains("media"));
    }
    QJsonObject c = firstClip();
    const QJsonObject keys = std::as_const(c)["keyframes"].toObject();
    CHECK(keys.contains("zoom") && !keys.contains("zoom_x") && !keys.contains("zoom_y")); // both axes alike: one
    CHECK(keys.contains("grade.exposure") && keys.contains("x") && keys.contains("y"));
    CHECK_EQ(keys["zoom"][1]["at"].toInt(), 25);
    CHECK(near(keys["zoom"][1]["value"].toDouble(), 1.12, 1e-4));
    {
        // A zoom (or place) set afterwards without `at` moves the whole curve: the punch stays a punch around it
        const int first = std::as_const(c)["id"].toInt();
        const double x0 = keys["x"][0]["value"].toDouble(), x1 = keys["x"][1]["value"].toDouble();
        run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"transform","clips":[%1],"zoom":2,"x":100}])")
                                                      .arg(first)}});
        const QJsonObject after = firstClip();
        const QJsonObject moved = after["keyframes"].toObject();
        CHECK(near(moved["zoom"][0]["value"].toDouble(), 2.0, 1e-4));
        CHECK(near(moved["zoom"][1]["value"].toDouble(), 2.24, 1e-4));
        CHECK_EQ(moved["zoom"].toArray().size(), keys["zoom"].toArray().size()); // no extra keyframe
        CHECK(near(moved["x"][1]["value"].toDouble() - moved["x"][0]["value"].toDouble(), x1 - x0, 1e-4));
        CHECK(near(moved["x"][0]["value"].toDouble(), 100, 1e-4));
        CHECK(near(after["transform"]["zoom_x"].toDouble(), 2.0, 1e-4)); // the value without keyframes follows
        run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"transform","clips":[%1],"zoom":1,"x":0}])")
                                                      .arg(first)}});
    }

    // Markers, strength, length, one part set directly
    run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"zoom","clear":true},
        {"op":"accent","markers":true,"clips":[%1],"preset":"punch","strength":2,"length":"0.4s","flash":0}])").arg(id)}});
    c = firstClip();
    const QJsonArray zoom = std::as_const(c)["keyframes"]["zoom"].toArray();
    CHECK_EQ(zoom.size(), 3); // marker 150 is behind the clip
    CHECK_EQ(zoom[1]["at"].toInt(), 70);
    CHECK(near(zoom[1]["value"].toDouble(), 1.3, 1e-4));
    CHECK_EQ(zoom[2]["at"].toInt(), 80);
    CHECK_EQ(zoom[2]["ease"].toString(), QString("ease_in"));

    // Errors: unknown preset, no time, nothing under the times, nothing to do
    CHECK_EQ(errorOf(R"([{"op":"accent","at":10,"preset":"wobble"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"accent","preset":"punch"}])"), QString("BAD_ARGUMENT"));
    CHECK_EQ(errorOf(R"([{"op":"accent","at":"30s"}])"), QString("NOT_FOUND"));
    CHECK_EQ(errorOf(R"([{"op":"accent","at":10,"preset":"punch","punch":0}])"), QString("BAD_ARGUMENT"));

    // op 'keyframe' with zoom sets both axes; different axes show apart in `info`
    run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"zoom","clear":true},
        {"op":"keyframe","clips":[%1],"param":"zoom","keys":[{"at":0,"value":1},{"at":40,"value":2,"ease":"ease_out"}]}])").arg(id)}});
    c = firstClip();
    CHECK_EQ(std::as_const(c)["keyframes"]["zoom"].toArray().size(), 2);
    CHECK_EQ(std::as_const(c)["keyframes"]["zoom"][1]["ease"].toString(), QString("ease_out"));
    run("edit", {{"project", g_path}, {"ops", QString(R"([{"op":"keyframe","clips":[%1],"param":"zoom_y","at":40,"value":3},
        {"op":"keyframe","clips":[%1],"param":"zoom","at":0,"move_to":5}])").arg(id)}});
    c = firstClip();
    CHECK(!std::as_const(c)["keyframes"].toObject().contains("zoom"));
    CHECK_EQ(std::as_const(c)["keyframes"]["zoom_y"][1]["value"].toDouble(), 3.0);
    CHECK_EQ(std::as_const(c)["keyframes"]["zoom_x"][0]["at"].toInt(), 5);
    CHECK_EQ(std::as_const(c)["keyframes"]["zoom_y"][0]["at"].toInt(), 5);

    // `effects` lists the presets with what they do; the op help points to them
    const QJsonArray accents = run("effects", {})["accents"].toArray();
    CHECK_EQ(accents.size(), Accents::presets().size());
    CHECK_EQ(accents[0]["preset"].toString(), QString("punch"));
    CHECK(!accents[0]["description"].toString().isEmpty());
    bool helped = false;
    for (const QJsonValue& v : Cli::detail::editOpsHelp()) helped |= v["op"].toString() == "accent";
    CHECK(helped);
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QCoreApplication app(argc, argv);
    Check::initApp("accents");
    testPunch();
    testFlashShake();
    testEditor();
    QTemporaryDir dir;
    if (!CHECK(dir.isValid())) return Check::result();
    testCli(dir.path());
    return Check::result();
}
