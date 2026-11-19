// SPDX-License-Identifier: AGPL-3.0-only
// What was left of the editing duels of 17./18.11.2026 (docs/testdaten/opus-duell-2026-11-17, plan 0.25 › F):
//  - `check` warns when a clip's sound is raised by +15 dB or more (the noise of a phone recording comes up with it)
//  - op 'solid': a colour matte over the whole frame (a background in a 9:16 is no safe-area finding)
//  - `frames` guides:true draws title/action safe and in 9:16 the phone apps' UI zones
//  - new_timeline into another frame shape: a matte fills the new frame, a logo keeps its size to its plate, both
//    move together out of the apps' zones
#include "check.h"

#include "cli/Commands.h"
#include "core/OverlayFit.h"
#include "core/ProjectFormat.h"

#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include "core/TimelineOps.h"

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
    edit(project, {QJsonObject{{"op", "track"}, {"track", track}, {"mute", false}},
                   QJsonObject{{"op", "mixer"}, {"track", track}, {"volume_db", 0}}});
    // The master fader does not count (check's own loudness fix raises it): +12 clip, +12 master = no warning
    edit(project, {QJsonObject{{"op", "mixer"}, {"track", "master"}, {"volume_db", 12}}});
    if (!CHECK(issueOf(run("check", {{"source", project}}), "audio_boost").isEmpty())) std::printf("       master counted\n");
    edit(project, {QJsonObject{{"op", "mixer"}, {"track", "master"}, {"volume_db", 0}}});
    // A keyframed volume: its largest key (0 static, +12 on a key) + 4 on the fader = +16
    edit(project, {QJsonObject{{"op", "volume"}, {"clips", QJsonArray{id}}, {"db", 0}},
                   QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{id}}, {"param", "volume"}, {"at", 10}, {"value", 12}},
                   QJsonObject{{"op", "mixer"}, {"track", track}, {"volume_db", 4}}});
    QJsonObject b = issueOf(run("check", {{"source", project}}), "audio_boost");
    if (!CHECK(b["message"].toString().contains("16"))) std::printf("       %s\n", QJsonDocument(b).toJson(QJsonDocument::Compact).constData());
    // The track's volume curve over the clip counts too: no key, fader +4, curve +12 = +16
    edit(project, {QJsonObject{{"op", "keyframe"}, {"clips", QJsonArray{id}}, {"param", "volume"}, {"clear", true}},
                   QJsonObject{{"op", "track_volume"}, {"track", track},
                               {"points", QJsonArray{QJsonObject{{"at", 0}, {"db", 12}}, QJsonObject{{"at", 70}, {"db", 12}}}}}});
    b = issueOf(run("check", {{"source", project}}), "audio_boost");
    if (!CHECK(b["message"].toString().contains("16"))) std::printf("       %s\n", QJsonDocument(b).toJson(QJsonDocument::Compact).constData());
    CHECK_EQ(b["to"].toInt(), 75);
    edit(project, {QJsonObject{{"op", "track_volume"}, {"track", track}, {"clear", true}},
                   QJsonObject{{"op", "mixer"}, {"track", track}, {"volume_db", 0}}});
    // Built-in effect sounds are no recording: an SFX track at +12 with a +12 sound is left alone
    edit(project, {QJsonObject{{"op", "sfx"}, {"at", "1.5s"}, {"kind", "hit"}, {"volume_db", 6}}});
    edit(project, {QJsonObject{{"op", "mixer"}, {"track", "SFX"}, {"volume_db", 12}}});
    if (!CHECK(issueOf(run("check", {{"source", project}}), "audio_boost").isEmpty())) std::printf("       sfx counted\n");
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
                                   QJsonObject{{"op", "shape"}, {"kind", "rect"}, {"width", 460}, {"height", 260}, {"x", 600},
                                               {"y", -300}, {"fill", "#ffffff"}, {"at", 0}, {"duration", "3s"}, {"track", "V3"}},
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
    edit(project, {QJsonObject{{"op", "transform"}, {"clips", QJsonArray{logoId}}, {"zoom", 0.2}, {"x", 600}, {"y", -300}}});
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

// --- OverlayFit by itself: crop, safe areas per target, edges on purpose, a stroked frame border
Clip shapeClip(int id, double w, double h, double x, double y)
{
    Clip c;
    c.id = id;
    c.kind = ClipKind::Title;
    c.title.kind = TitleKind::Shape;
    c.title.shape.kind = ShapeKind::Rect;
    c.title.shape.width = w;
    c.title.shape.height = h;
    c.title.posX = x;
    c.title.posY = y;
    c.out = 49;
    return c;
}

void testOverlayFitUnit()
{
    const QSizeF hd(1920, 1080), vert(1080, 1920);
    // A cropped picture overlay (a 2:1 image at zoom 1, cropped to its left quarter): its block is what is left, not
    // the whole fitted picture (that would group everything and centre it)
    {
        Timeline tl;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        Clip pip;
        pip.id = 1;
        pip.kind = ClipKind::Media;
        pip.mediaPath = "/x/logo.png";
        pip.pictureAspect = 2.0; // fitted: 1920 x 960
        pip.out = 49;
        pip.transform.cropRight = 1440; // 480 px wide left
        pip.transform.cropBottom = 480; // 480 px high left
        tl.video[1].clips << pip;
        const QVector<OverlayFit::Before> b = OverlayFit::collect(tl, hd, [](const Clip&) { return true; });
        if (CHECK_EQ(b.size(), qsizetype(1)))
            if (!CHECK(std::abs(b[0].block.width() - 480) < 1 && std::abs(b[0].block.height() - 480) < 1 && !b[0].fullFrame))
                std::printf("       block %.0f x %.0f\n", b[0].block.width(), b[0].block.height());
    }
    // Safe rect: 16:9 = action safe; 9:16 = title safe without the top/bottom zones, the right strip only lower down
    {
        const QRectF a = OverlayFit::safeRect(hd);
        CHECK(std::abs(a.width() - 1920 * 0.93) < 1 && std::abs(a.height() - 1080 * 0.93) < 1);
        double fromY = -1, rightX = -1;
        const QRectF v = OverlayFit::safeRect(vert, {}, &fromY, &rightX);
        CHECK(std::abs(v.right() - 1080 * 0.95) < 1 && std::abs(v.bottom() - 1920 * 0.8) < 1);
        CHECK(fromY > 600 && rightX < v.right());
        const QRectF p = OverlayFit::safeRect(QSizeF(1080, 1350), {}, &fromY, &rightX); // 4:5: title safe, no app zones
        CHECK(std::abs(p.bottom() - 1350 * 0.95) < 1 && fromY < 0);
    }
    const auto fit = [&](Timeline tl, QSizeF from, QSizeF to) {
        const QVector<OverlayFit::Before> b = OverlayFit::collect(tl, from, {});
        scaleTimeline(tl, from.toSize(), to.toSize());
        OverlayFit::apply(tl, b, from, to);
        return tl;
    };
    // A plate in the top right of a 16:9 frame (inside title safe) stays at the right in 9:16: the buttons' strip
    // starts lower down
    {
        Timeline tl;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        tl.video[1].clips << shapeClip(1, 300, 100, 700, 400);
        const Timeline out = fit(tl, hd, vert);
        const Clip& c = out.video[1].clips[0];
        const double right = 540 + c.title.posX + 150, top = 960 - c.title.posY - 50;
        if (!CHECK(right > 1080 * 0.9 && right <= 1080 * 0.95 + 1 && top >= 1920 * 0.08 - 1))
            std::printf("       right %.0f top %.0f\n", right, top);
    }
    // A bar along the bottom edge on purpose (outside title safe): it is not pushed up out of the bottom zone
    {
        Timeline tl;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        tl.video[1].clips << shapeClip(1, 1920, 40, 0, -520); // y 1020..1060 of 1080 (inside the frame)
        tl.video[1].clips[0].title.shape.width = 1000;
        const Timeline out = fit(tl, hd, vert);
        const double bottom = 960 - out.video[1].clips[0].title.posY + 20;
        if (!CHECK(bottom > 1920 * 0.9)) std::printf("       bottom %.0f\n", bottom);
    }
    // A frame border (1920 x 1080, no fill, a 20 px stroke): in 9:16 it is 1080 x 1920, its stroke inside the frame
    {
        Timeline tl;
        TimelineOps::ensureTracks(tl, TrackKind::Video, 2);
        Clip border = shapeClip(1, 1920, 1080, 0, 0);
        border.title.shape.fill = ShapeFill::None;
        border.title.outlineOn = true;
        border.title.outlineWidth = 20;
        tl.video[1].clips << border;
        const Timeline out = fit(tl, hd, vert);
        const ShapeStyle& sh = out.video[1].clips[0].title.shape;
        if (!CHECK(std::abs(sh.width - 1080) < 0.5 && std::abs(sh.height - 1920) < 0.5))
            std::printf("       border %.1f x %.1f\n", sh.width, sh.height);
    }
}

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("duel_leftovers");
    QTemporaryDir dir;
    testOverlayFitUnit();
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
