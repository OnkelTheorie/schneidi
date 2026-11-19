// SPDX-License-Identifier: AGPL-3.0-only
// What was left of the editing duels of 17./18.11.2026 (docs/testdaten/opus-duell-2026-11-17, plan 0.25 › F):
//  - `check` warns when a clip's sound is raised by +15 dB or more (the noise of a phone recording comes up with it)
//  - op 'solid': a colour matte over the whole frame (a background in a 9:16 is no safe-area finding)
//  - `frames` guides:true draws title/action safe and in 9:16 the phone apps' UI zones
//  - new_timeline into another frame shape: a matte fills the new frame, a logo keeps its size to its plate, both
//    move together out of the apps' zones
#include "check.h"

#include "cli/Commands.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return cmd->run(Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return QJsonObject{{"failed", e.code}, {"message", e.message}};
    }
}

QJsonObject edit(const QString& project, const QJsonArray& ops) { return run("edit", {{"project", project}, {"ops", ops}}); }

QJsonObject issueOf(const QJsonObject& r, const QString& check)
{
    for (const QJsonValue& v : r["issues"].toArray())
        if (v["check"].toString() == check) return v.toObject();
    return {};
}

// --- check: a big boost (clip + track fader + master) is a warning with a suggestion, +12 dB is not
void testBoost(const QString& dir)
{
    const QString clip = Check::makeMedia(dir + "/phone.mp4",
                                          {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=3", "-f", "lavfi", "-i",
                                           "sine=f=440:d=3", "-af", "volume=0.02", "-c:v", "libx264", "-pix_fmt",
                                           "yuv420p", "-c:a", "aac", "-shortest"});
    if (!CHECK(!clip.isEmpty())) return;
    const QString project = dir + "/boost.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    CHECK(!edit(project, {QJsonObject{{"op", "append"}, {"media", clip}}}).contains("failed"));
    QString track;
    int id = 0;
    const QJsonObject info = run("info", {{"project", project}});
    for (const QJsonValue& t : info["timeline"]["tracks"].toArray())
        if (t["track"].toString().startsWith('A') && !t["clips"].toArray().isEmpty())
            track = t["track"].toString(), id = t["clips"].toArray().first()["id"].toInt();
    if (!CHECK(id > 0)) return;
    edit(project, {QJsonObject{{"op", "volume"}, {"clips", QJsonArray{id}}, {"db", 12}}});
    QJsonObject r = run("check", {{"source", project}});
    CHECK(issueOf(r, "audio_boost").isEmpty()); // +12 dB: no warning
    // + 9 dB on the track fader: +21 dB in all
    edit(project, {QJsonObject{{"op", "mixer"}, {"track", track}, {"volume_db", 9}}});
    r = run("check", {{"source", project}});
    const QJsonObject boost = issueOf(r, "audio_boost");
    if (!CHECK(!boost.isEmpty())) std::printf("       %s\n", QJsonDocument(r["issues"].toArray()).toJson(QJsonDocument::Compact).constData());
    CHECK_EQ(boost["severity"].toString(), QString("warning"));
    CHECK(boost["message"].toString().contains("21") && boost["message"].toString().contains("clean_voice"));
    CHECK_EQ(r["errors"].toInt(), 0);
    // A muted track is not heard: no warning
    edit(project, {QJsonObject{{"op", "track"}, {"track", track}, {"mute", true}}});
    CHECK(issueOf(run("check", {{"source", project}}), "audio_boost").isEmpty());
}

// The clips of the timeline by id
QJsonObject clipById(const QString& project, int id)
{
    const QJsonObject info = run("info", {{"project", project}});
    for (const QJsonValue& t : info["timeline"]["tracks"].toArray())
        for (const QJsonValue& c : t["clips"].toArray())
            if (c["id"].toInt() == id) return c.toObject();
    return {};
}

// One still of a project at `at` (frames command, full size)
QImage still(const QString& project, const QString& at, const QString& dir, const QJsonObject& extra = {})
{
    QJsonObject args{{"source", project}, {"at", QJsonArray{at}}, {"out", dir}};
    for (auto it = extra.begin(); it != extra.end(); ++it) args[it.key()] = it.value();
    const QJsonObject r = run("frames", args);
    const QJsonArray frames = r["frames"].toArray();
    if (frames.isEmpty()) {
        std::printf("       frames: %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        return {};
    }
    return QImage(frames.first()["file"].toString());
}

bool near(QColor a, QColor b, int tol = 12)
{
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol;
}

// --- op 'solid': a colour over the whole frame, corners included; in a 9:16 timeline no safe-area finding
void testSolid(const QString& dir)
{
    const QString video = Check::makeMedia(dir + "/v.mp4", {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=4", "-c:v",
                                                            "libx264", "-pix_fmt", "yuv420p"});
    if (!CHECK(!video.isEmpty())) return;
    const QString project = dir + "/solid.schneidi";
    run("new", {{"project", project}, {"fps", 25}, {"width", 1080}, {"height", 1920}});
    QJsonObject r = edit(project, {QJsonObject{{"op", "append"}, {"media", video}},
                                   QJsonObject{{"op", "solid"}, {"color", "#ff8000"}, {"at", "1s"}, {"length", "2s"}}});
    int id = 0;
    for (const QJsonValue& n : r["notes"].toArray()) id = std::max(id, n["solid"].toInt());
    if (!CHECK(id > 0)) std::printf("       %s\n", QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
    const QJsonObject c = clipById(project, id);
    CHECK_EQ(c["start"].toInt(), 25);
    CHECK_EQ(c["end"].toInt(), 75);
    CHECK_EQ(c["shape"]["kind"].toString(), QString("rect"));
    const QImage img = still(project, "2s", dir + "/solid-frames");
    if (CHECK(!img.isNull())) {
        for (const QPoint p : {QPoint(0, 0), QPoint(img.width() - 1, img.height() - 1), QPoint(img.width() / 2, img.height() / 2)})
            if (!CHECK(near(img.pixelColor(p), QColor("#ff8000")))) std::printf("       %d,%d: %s\n", p.x(), p.y(), qPrintable(img.pixelColor(p).name()));
    }
    r = run("check", {{"source", project}, {"sound", false}});
    for (const QJsonValue& v : r["issues"].toArray())
        if (!CHECK(v["check"].toString() != "safe_area")) std::printf("       %s\n", qPrintable(v["message"].toString()));
    // color and length are needed
    CHECK(edit(project, {QJsonObject{{"op", "solid"}, {"length", "1s"}}}).contains("failed"));
    CHECK(edit(project, {QJsonObject{{"op", "solid"}, {"color", "#000000"}}}).contains("failed"));
}

// --- frames guides:true: the title-safe line (yellow) at 5 % of the width; in 9:16 the bottom app zone tinted red;
// without guides the picture is untouched
void testGuides(const QString& dir)
{
    for (const bool vertical : {false, true}) {
        const QString project = dir + (vertical ? "/guides-v.schneidi" : "/guides-h.schneidi");
        run("new", {{"project", project}, {"fps", 25}, {"width", vertical ? 1080 : 1920}, {"height", vertical ? 1920 : 1080}});
        edit(project, {QJsonObject{{"op", "solid"}, {"color", "#000000"}, {"at", 0}, {"length", "2s"}}});
        const QString out = dir + (vertical ? "/guides-v" : "/guides-h");
        const QImage plain = still(project, "1s", out + "-plain", {{"width", 960}});
        const QImage img = still(project, "1s", out, {{"guides", true}, {"width", 960}});
        if (!CHECK(!img.isNull() && !plain.isNull())) continue;
        const int x = int(std::lround(img.width() * 0.05)), y = img.height() / 2;
        QColor line;
        for (int dx = -1; dx <= 1; ++dx) // the line is 1-3 px wide, JPEG blurs it a little
            if (img.pixelColor(x + dx, y).red() > line.red()) line = img.pixelColor(x + dx, y);
        if (!CHECK(line.red() > 150 && line.green() > 120 && line.blue() < 140)) std::printf("       title safe: %s\n", qPrintable(line.name()));
        CHECK(near(plain.pixelColor(x, y), Qt::black));
        CHECK(near(plain.pixelColor(img.width() / 2, int(img.height() * 0.9)), Qt::black));
        const QColor bottom = img.pixelColor(img.width() / 2, int(img.height() * 0.9));
        if (vertical) {
            if (!CHECK(bottom.red() > bottom.green() + 20)) std::printf("       bottom zone: %s\n", qPrintable(bottom.name()));
        } else {
            CHECK(near(bottom, Qt::black)); // no app zones in 16:9
        }
    }
}

// --- new_timeline 16:9 -> 9:16: overlays fitted to the new frame
void testNewFormat(const QString& dir)
{
    const QString video = Check::makeMedia(dir + "/nf.mp4", {"-f", "lavfi", "-i", "testsrc2=s=320x180:r=25:d=4", "-c:v",
                                                             "libx264", "-pix_fmt", "yuv420p"});
    const QString logo = Check::makeMedia(dir + "/logo.png", {"-f", "lavfi", "-i", "color=c=blue:s=400x200", "-frames:v", "1"});
    if (!CHECK(!video.isEmpty() && !logo.isEmpty())) return;
    const QString project = dir + "/nf.schneidi";
    run("new", {{"project", project}, {"fps", 25}});
    // A white matte at the end, a plate in the bottom right corner with a 2:1 logo on it (zoom 0.2: 384 x 192), a text
    QJsonObject r = edit(project, {QJsonObject{{"op", "append"}, {"media", video}},
                                   QJsonObject{{"op", "solid"}, {"color", "#ffffff"}, {"at", "3s"}, {"length", "1s"}, {"track", "V2"}},
                                   QJsonObject{{"op", "shape"}, {"kind", "rect"}, {"width", 460}, {"height", 260}, {"x", 650},
                                               {"y", -330}, {"fill", "#ffffff"}, {"at", 0}, {"duration", "3s"}, {"track", "V3"}},
                                   QJsonObject{{"op", "place"}, {"media", logo}, {"at", 0}, {"out", "3s"}, {"track", "V4"}},
                                   QJsonObject{{"op", "title"}, {"text", "Hello"}, {"at", 0}, {"duration", "3s"}, {"track", "V5"}}});
    if (!CHECK(!r.contains("failed"))) return;
    const auto clipOn = [&](const QString& timeline, const QString& track) {
        const QJsonObject info = run("info", {{"project", project}, {"timeline", timeline}});
        for (const QJsonValue& t : info["timeline"]["tracks"].toArray())
            if (t["track"].toString() == track && !t["clips"].toArray().isEmpty()) return t["clips"].toArray().first().toObject();
        return QJsonObject{};
    };
    const int logoId = clipOn("Timeline 1", "V4")["id"].toInt();
    edit(project, {QJsonObject{{"op", "transform"}, {"clips", QJsonArray{logoId}}, {"zoom", 0.2}, {"x", 650}, {"y", -330}}});
    r = edit(project, {QJsonObject{{"op", "new_timeline"}, {"from", "Timeline 1"}, {"aspect", "9:16"}, {"reframe", "auto"},
                                   {"name", "Short"}}});
    if (!CHECK(!r.contains("failed"))) return;
    // The matte fills the 1080 x 1920 frame
    const QJsonObject matte = clipOn("Short", "V2")["shape"].toObject();
    if (!CHECK(matte["width"].toDouble() >= 1080 && matte["width"].toDouble() < 1100 && matte["height"].toDouble() >= 1920
               && matte["height"].toDouble() < 1940 && matte["x"].toDouble() == 0 && matte["y"].toDouble() == 0))
        std::printf("       matte %s\n", QJsonDocument(matte).toJson(QJsonDocument::Compact).constData());
    // Plate and logo: still centred on each other; the logo as wide as before (384 px: zoom 384 / 1080)
    const QJsonObject plate = clipOn("Short", "V3")["shape"].toObject();
    const QJsonObject tr = clipOn("Short", "V4")["transform"].toObject();
    CHECK(std::abs(plate["x"].toDouble() - tr["x"].toDouble()) < 1 && std::abs(plate["y"].toDouble() - tr["y"].toDouble()) < 1);
    if (!CHECK(std::abs(tr["zoom_x"].toDouble() * 1080 - 384) < 2)) std::printf("       logo zoom %f\n", tr["zoom_x"].toDouble());
    // ... and out of the apps' zones: the plate (460 x 260) above the bottom 20 % and left of the right 12 %
    const double right = 540 + plate["x"].toDouble() + 230, bottom = 960 - plate["y"].toDouble() + 130;
    if (!CHECK(right <= 1080 * 0.88 + 1 && bottom <= 1920 * 0.8 + 1)) std::printf("       plate right %.0f bottom %.0f\n", right, bottom);
    // The text in the middle stays where it was put (no need to move)
    CHECK(clipOn("Short", "V5").contains("id"));
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("duel_leftovers");
    QTemporaryDir dir;
    if (Check::haveFfmpeg()) {
        testBoost(dir.path());
        testSolid(dir.path());
        testGuides(dir.path());
        testNewFormat(dir.path());
    } else {
        std::printf("  no ffmpeg: skipped\n");
    }
    return Check::result();
}
