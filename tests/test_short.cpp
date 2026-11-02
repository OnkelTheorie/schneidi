// SPDX-License-Identifier: AGPL-3.0-only
// From recordings to a finished short: the moments it picks (ShortPick), the Shorts/Reels/TikTok render presets
// (ids, frame rate up to 60), `check` with a platform (too_long), the limiter ceiling under the check's -1 dBTP, new
// timelines keeping the master limiter, `extensions` ready_for, and the `short` command end to end on a synthetic
// recording (no network: without whisper it renders without captions and says how to get them)
#include "cli/Commands.h"
#include "core/ExportCheck.h"
#include "core/Project.h"
#include "core/ProjectFile.h"
#include "core/RenderJob.h"
#include "core/ShortPick.h"

#include "check.h"

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <cmath>

namespace {

QJsonObject run(const QString& name, const QJsonObject& raw)
{
    const Cli::Command* cmd = Cli::find(name);
    Cli::Context ctx;
    try {
        return Cli::execute(*cmd, Cli::normalize(*cmd, raw), ctx);
    } catch (const Cli::Error& e) {
        std::printf("       %s: %s %s\n", qPrintable(name), qPrintable(e.code), qPrintable(e.message));
        return {{"error", e.code}};
    }
}

bool near(double a, double b, double eps = 0.05) { return std::abs(a - b) <= eps; }

} // namespace

int main(int argc, char** argv)
{
    Check::initEnv();
    QGuiApplication app(argc, argv);
    Check::initApp("short");
    Check::initEngine();
    QTemporaryDir dir;

    // ---- ShortPick: best first within max, each padded to the minimum, widened to min, the end without candidates
    {
        using ShortPick::Moment;
        ShortPick::Options o; // 15..45 s, moments at least 6 s
        // Three 3 s kills in a 60 s recording: each grows to 6 s (more after than before), together 18 s
        QVector<Moment> c{{0, 10, 13, 5}, {0, 40, 43, 3}, {0, 25, 28, 4}};
        QVector<Moment> p = ShortPick::pick(c, {60}, o);
        if (CHECK_EQ(p.size(), 3)) {
            CHECK(near(p[0].from, 8.65) && near(p[0].to, 14.65)); // 1.35 s before, 1.65 s after
            CHECK(p[0].to - p[0].from >= 5.99 && p[1].from > p[0].to && p[2].from > p[1].to);
        }
        CHECK(ShortPick::total(p) >= 15 && ShortPick::total(p) <= 45);
        // One kill: widened to the minimum length (15 s), around it
        p = ShortPick::pick({{0, 30, 33, 1}}, {60}, o);
        if (CHECK_EQ(p.size(), 1)) CHECK(near(p[0].to - p[0].from, 15) && p[0].from < 30 && p[0].to > 33);
        // Too many: the best ones within 45 s, a weaker short one still fits after a long one did not
        p = ShortPick::pick({{0, 0, 20, 9}, {0, 50, 75, 8}, {0, 100, 120, 7}, {0, 150, 153, 6}}, {200}, o);
        CHECK(ShortPick::total(p) <= 45.001);
        CHECK(std::any_of(p.begin(), p.end(), [](const Moment& m) { return m.from <= 0 && m.to >= 20; }));
        CHECK(!std::any_of(p.begin(), p.end(), [](const Moment& m) { return m.from >= 49 && m.to <= 76; }));
        CHECK(std::any_of(p.begin(), p.end(), [](const Moment& m) { return m.from >= 140; }));
        // Close moments of one file join (no 1 s jump); two files keep their order
        p = ShortPick::pick({{1, 5, 8, 2}, {0, 10, 16, 3}, {0, 17, 23, 1}}, {30, 30}, o);
        if (CHECK_EQ(p.size(), 2)) {
            CHECK(p[0].file == 0 && p[1].file == 1);
            CHECK(p[0].from <= 10 && p[0].to >= 23);
        }
        // Nothing found: the end of the recording, 30 s; a short recording: all of it
        p = ShortPick::pick({}, {100}, o);
        if (CHECK_EQ(p.size(), 1)) CHECK(near(p[0].from, 70) && near(p[0].to, 100));
        p = ShortPick::pick({{0, 2, 4, 1}}, {12}, o);
        if (CHECK_EQ(p.size(), 1)) CHECK(near(p[0].from, 0) && near(p[0].to, 12));
        // A kill at the very start: what does not fit before goes after
        p = ShortPick::pick({{0, 0, 3, 1}}, {60}, o);
        if (CHECK_EQ(p.size(), 1)) CHECK(near(p[0].from, 0) && near(p[0].to, 15));
    }

    // ---- Presets for Shorts, Reels, TikTok
    {
        for (const char* key : {"tiktok", "TikTok", "shorts", "youtube-shorts", "YouTube Shorts", "reels",
                                "Instagram Reels"}) {
            const auto p = RenderPresets::find(key);
            if (!CHECK(p.has_value())) {
                std::printf("       no preset for %s\n", key);
                continue;
            }
            CHECK_EQ(p->settings.size, QSize(1080, 1920));
            CHECK_EQ(p->settings.format, QString("h264"));
            CHECK_EQ(p->settings.quality, RenderSettings::kQualityWeb);
            CHECK_EQ(p->settings.maxFps, 60);
            CHECK(!p->hint.isEmpty());
        }
        CHECK_EQ(RenderPresets::find("tiktok")->id, QString("tiktok"));
        CHECK_EQ(RenderPresets::find("shorts")->name, QString("YouTube Shorts"));
        CHECK_EQ(RenderPresets::find("phone")->settings.maxFps, 0); // the plain phone preset keeps any rate
        CHECK(RenderPresets::find("youtube-1080p").has_value() && RenderPresets::find("YouTube 1080p").has_value());
        CHECK(!RenderPresets::find("myspace").has_value());
        // Frame rate up to 60: faster timelines divided by a whole number, slower ones as they are
        RenderSettings s = RenderPresets::find("tiktok")->settings;
        CHECK((s.outputRate({120, 1}) == FrameRate{60, 1}));
        CHECK((s.outputRate({144, 1}) == FrameRate{48, 1}));
        CHECK((s.outputRate({100, 1}) == FrameRate{50, 1}));
        CHECK((s.outputRate({60000, 1001}) == FrameRate{60000, 1001}));
        CHECK((s.outputRate({30, 1}) == FrameRate{30, 1}));
        s.rateNum = 24; // a chosen rate wins
        CHECK((s.outputRate({120, 1}) == FrameRate{24, 1}));
        // Saved with the queue/own presets and back
        const RenderSettings back = RenderSettings::fromJson(RenderPresets::find("reels")->settings.toJson());
        CHECK(back == RenderPresets::find("reels")->settings);
        CHECK_EQ(back.maxFps, 60);
        CHECK(RenderSettings::fromJson(RenderPresets::find("phone")->settings.toJson()).maxFps == 0);
        // Platforms
        CHECK(shortPlatform("TIKTOK") && shortPlatform("TIKTOK")->maxSeconds == 600);
        CHECK(shortPlatform("shorts") && shortPlatform("shorts")->maxSeconds == 180);
        CHECK(shortPlatform("Instagram Reels") && QString(shortPlatform("reels")->id) == "instagram-reels");
        CHECK(!shortPlatform("vine"));
    }

    // ---- check: too long for the platform; the limiter fix sits a little under -1 dBTP
    {
        ProjectFormat f{1080, 1920, {30, 1}};
        ExportCheck::Options o;
        CHECK(ExportCheck::length(0, 30 * 600, f, o).isEmpty()); // no platform: no limit
        o.maxLengthS = 180;
        o.platform = "YouTube Shorts";
        CHECK(ExportCheck::length(0, 30 * 180, f, o).isEmpty());
        const QVector<ExportCheck::Issue> longer = ExportCheck::length(0, 30 * 181, f, o);
        if (CHECK_EQ(longer.size(), 1)) {
            CHECK_EQ(longer[0].check, QString("too_long"));
            CHECK(longer[0].message.contains("YouTube Shorts"));
        }
        const std::vector<float> peaks(300, 0.5f);
        Timeline mix;
        auto fix = ExportCheck::soundFix(ExportCheck::sound(peaks, 0, -14, -0.5, f), -14, mix);
        CHECK(fix.limiter && fix.ceiling == -1.2);
        CHECK_EQ(kDefaultLimiterDb, -1.2);
        // On at -1.0 (an older project) and still over -1 after the encoder: the ceiling goes down a little
        mix.masterLimiter = true;
        mix.masterLimiterDb = -1.0;
        fix = ExportCheck::soundFix(ExportCheck::sound(peaks, 0, -14, -0.95, f, {}, true), -14, mix);
        CHECK(fix.limiter && fix.ceiling == -1.2);
        // On at -1.2 and the peak below -1: nothing to do
        mix.masterLimiterDb = -1.2;
        CHECK(!ExportCheck::soundFix(ExportCheck::sound(peaks, 0, -14, -1.1, f, {}, true), -14, mix).any());
    }

    // ---- New timelines keep the master limiter of the project (a start template's); `extensions` ready_for
    {
        const QString proj = dir.filePath("limiter.schneidi");
        run("new", {{"project", proj}, {"template", "game"}});
        run("edit", {{"project", proj}, {"ops", R"([{"op":"new_timeline","name":"Cut"}])"}});
        run("edit", {{"project", proj}, {"ops", R"([{"op":"new_timeline","name":"Tall","from":"Cut","aspect":"9:16"}])"}});
        for (const char* t : {"Cut", "Tall"}) {
            const QJsonObject master = run("info", {{"project", proj}, {"timeline", QString(t)}, {"summary", true}})["timeline"]["master"].toObject();
            if (!CHECK(master["limiter"].toBool())) std::printf("       timeline %s without the limiter\n", t);
            CHECK(near(master["limiter_db"].toDouble(), -1.2, 1e-9));
        }
        const QJsonObject ext = run("extensions", {});
        CHECK(ext["ready_for"].isArray());
        CHECK(!ext["ready_for"].toArray().contains(QJsonValue("transcribe"))); // test paths: nothing installed
        // render / check name the presets and platforms they know
        const Cli::Command* render = Cli::find("render");
        bool presetIds = false;
        for (const Cli::Param& p : render->params)
            if (p.name == "preset") presetIds = p.help.contains("tiktok") && p.help.contains("youtube-shorts");
        CHECK(presetIds);
        CHECK(Cli::find("short") != nullptr);
    }

    // ---- `short` end to end: a 40 s "recording" with two loud moments, its folder named like a game's
    if (!Check::haveFfmpeg()) return Check::result();
    {
        QDir().mkpath(dir.filePath("clips/cs2"));
        const QString clip = Check::makeMedia(
            dir.filePath("clips/cs2/cs2_clip_test.mp4"),
            {"-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=40", "-f", "lavfi", "-i",
             "aevalsrc='0.02*sin(2*PI*220*t)+if(between(t,9,9.6)+between(t,29,29.6),0.9*sin(2*PI*440*t),0)':d=40:s=48000",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"});
        if (!CHECK(!clip.isEmpty())) return Check::result();
        const QString out = dir.filePath("out/short.mp4");
        const QJsonObject r = run("short", {{"files", QJsonArray{clip}}, {"out", out}, {"game", "none"}});
        CHECK(!r.contains("error"));
        CHECK(QFileInfo::exists(out));
        CHECK_EQ(r["out"].toString(), out);
        const double len = r["length_s"].toDouble();
        if (!CHECK(len >= 14.9 && len <= 45.1)) std::printf("       length %.2f s\n", len);
        CHECK_EQ(r["width"].toInt(), 1080);
        CHECK_EQ(r["height"].toInt(), 1920);
        CHECK_EQ(r["timeline"].toString(), QString("Short"));
        // Both loud moments are in it
        const QJsonArray moments = r["moments"].toArray();
        const auto covers = [&](double s) {
            return std::any_of(moments.begin(), moments.end(),
                               [s](const QJsonValue& m) { return m["from_s"].toDouble() <= s && m["to_s"].toDouble() >= s; });
        };
        CHECK(covers(9.3) && covers(29.3));
        // No whisper here: no captions, a note with the install command (never installed)
        const QString notes = QJsonDocument(r["notes"].toArray()).toJson();
        CHECK(notes.contains("extensions --install whisper"));
        // The project next to the video holds the cut and the 9:16 short; the master limiter is on (check's fix or
        // the template), its ceiling under -1
        const QString proj = r["project"].toString();
        CHECK_EQ(QFileInfo(proj).absolutePath(), QFileInfo(out).absolutePath());
        const QJsonObject info = run("info", {{"project", proj}, {"timeline", "Short"}, {"summary", true}});
        CHECK_EQ(info["timeline"]["width"].toInt(), 1080);
        CHECK_EQ(info["timeline"]["height"].toInt(), 1920);
        // The rendered file passes the platform check's sound part (true peak under -1 dBTP)
        const QJsonObject fileCheck = run("check", {{"source", out}, {"platform", "shorts"}});
        bool peakOk = true;
        for (const QJsonValue& i : fileCheck["issues"].toArray())
            peakOk &= i["check"].toString() != "true_peak" && i["check"].toString() != "clipping";
        if (!CHECK(peakOk)) std::printf("       %s\n", QJsonDocument(fileCheck).toJson().constData());
        CHECK(fileCheck["sound"]["true_peak_db"].toDouble() <= -1.0);
        // Again: a new numbered file, the existing one is not replaced; a given out that exists needs overwrite
        const QJsonObject again = run("short", {{"files", QJsonArray{clip}}, {"captions", "off"}, {"max_s", 20}});
        CHECK_EQ(QFileInfo(again["out"].toString()).fileName(), QString("cs2_clip_test-short.mp4"));
        CHECK(again["length_s"].toDouble() <= 20.05);
        CHECK(QJsonDocument(again["done"].toArray()).toJson().contains("kill feed of cs2")); // game from the folder
        CHECK_EQ(run("short", {{"files", QJsonArray{clip}}, {"out", out}})["error"].toString(), QString("EXISTS"));
        // An existing timeline as the cut
        const QJsonObject fromTimeline =
            run("short", {{"project", proj}, {"timeline", "Short-Schnitt"}, {"captions", "off"}, {"platform", "tiktok"},
                          {"aspect", "1:1"}, {"out", dir.filePath("out/square.mp4")}});
        CHECK_EQ(fromTimeline["height"].toInt(), 1080);
        CHECK_EQ(fromTimeline["platform"].toString(), QString("TikTok"));
        CHECK_EQ(fromTimeline["timeline"].toString(), QString("Short 2"));
        CHECK_EQ(run("short", {{"files", QJsonArray{clip}}, {"aspect", "16:9"}})["error"].toString(), QString("BAD_ARGUMENT"));
    }
    return Check::result();
}
